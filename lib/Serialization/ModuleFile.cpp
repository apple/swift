//===--- ModuleFile.cpp - Loading a serialized module ---------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "ModuleFile.h"
#include "ModuleFileCoreTableInfo.h"
#include "BCReadingExtras.h"
#include "DeserializationErrors.h"
#include "ModuleFormat.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/Subsystems.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Basic/Range.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/OnDiskHashTable.h"

using namespace swift;
using namespace swift::serialization;
using namespace llvm::support;
using llvm::Expected;

static_assert(IsTriviallyDestructible<SerializedASTFile>::value,
              "SerializedASTFiles are BumpPtrAllocated; d'tors are not called");

static bool areCompatibleArchitectures(const llvm::Triple &moduleTarget,
                                       const llvm::Triple &ctxTarget) {
  if (moduleTarget.getArch() == ctxTarget.getArch())
    return true;

  // Special case: ARM and Thumb are compatible.
  const llvm::Triple::ArchType moduleArch = moduleTarget.getArch();
  const llvm::Triple::ArchType ctxArch = ctxTarget.getArch();
  if ((moduleArch == llvm::Triple::arm && ctxArch == llvm::Triple::thumb) ||
      (moduleArch == llvm::Triple::thumb && ctxArch == llvm::Triple::arm))
    return true;
  if ((moduleArch == llvm::Triple::armeb && ctxArch == llvm::Triple::thumbeb) ||
      (moduleArch == llvm::Triple::thumbeb && ctxArch == llvm::Triple::armeb))
    return true;

  return false;
}

static bool areCompatibleOSs(const llvm::Triple &moduleTarget,
                             const llvm::Triple &ctxTarget) {
  if ((!moduleTarget.hasEnvironment() && ctxTarget.isSimulatorEnvironment()) ||
      (!ctxTarget.hasEnvironment() && moduleTarget.isSimulatorEnvironment()))
    return false;

  if (moduleTarget.getOS() == ctxTarget.getOS())
    return true;

  // Special case: macOS and Darwin are compatible.
  const llvm::Triple::OSType moduleOS = moduleTarget.getOS();
  const llvm::Triple::OSType ctxOS = ctxTarget.getOS();
  if ((moduleOS == llvm::Triple::Darwin && ctxOS == llvm::Triple::MacOSX) ||
      (moduleOS == llvm::Triple::MacOSX && ctxOS == llvm::Triple::Darwin))
    return true;

  return false;
}

static bool isTargetTooNew(const llvm::Triple &moduleTarget,
                           const llvm::Triple &ctxTarget) {
  if (moduleTarget.isMacOSX()) {
    llvm::VersionTuple osVersion;
    moduleTarget.getMacOSXVersion(osVersion);
    // TODO: Add isMacOSXVersionLT(Triple) API (or taking a VersionTuple)
    return ctxTarget.isMacOSXVersionLT(osVersion.getMajor(),
                                       osVersion.getMinor().value_or(0),
                                       osVersion.getSubminor().value_or(0));
  }
  return ctxTarget.isOSVersionLT(moduleTarget);
}

namespace swift {
namespace serialization {
bool areCompatible(const llvm::Triple &moduleTarget,
                   const llvm::Triple &ctxTarget) {
  return areCompatibleArchitectures(moduleTarget, ctxTarget) &&
         areCompatibleOSs(moduleTarget, ctxTarget);
}
} // namespace serialization
} // namespace swift

ModuleFile::ModuleFile(std::shared_ptr<const ModuleFileSharedCore> core)
    : Core(core) {
  assert(!core->hasError());

  DeclTypeCursor = core->DeclTypeCursor;
  SILCursor = core->SILCursor;
  SILIndexCursor = core->SILIndexCursor;
  DeclMemberTablesCursor = core->DeclMemberTablesCursor;

  for (const auto &coreDep : core->Dependencies) {
    Dependencies.emplace_back(coreDep);
  }

  // `ModuleFileSharedCore` has immutable data, we copy these into `ModuleFile`
  // so we can mutate the arrays and replace the offsets with AST object
  // pointers as we lazily deserialize them.
  allocateBuffer(Decls, core->Decls);
  allocateBuffer(LocalDeclContexts, core->LocalDeclContexts);
  allocateBuffer(Conformances, core->Conformances);
  allocateBuffer(PackConformances, core->PackConformances);
  allocateBuffer(SILLayouts, core->SILLayouts);
  allocateBuffer(Types, core->Types);
  allocateBuffer(ClangTypes, core->ClangTypes);
  allocateBuffer(GenericSignatures, core->GenericSignatures);
  allocateBuffer(GenericEnvironments, core->GenericEnvironments);
  allocateBuffer(SubstitutionMaps, core->SubstitutionMaps);
  allocateBuffer(Identifiers, core->Identifiers);
}

bool ModuleFile::allowCompilerErrors() const {
  return getContext().LangOpts.AllowModuleWithCompilerErrors;
}

Status
ModuleFile::loadDependenciesForFileContext(const FileUnit *file,
                                           SourceLoc diagLoc,
                                           bool forTestable) {
  ASTContext &ctx = getContext();
  auto clangImporter = static_cast<ClangImporter *>(ctx.getClangModuleLoader());
  ModuleDecl *M = file->getParentModule();

  bool missingDependency = false;
  for (auto &dependency : Dependencies) {
    if (forTestable && dependency.isLoaded())
      continue;

    assert(!dependency.isLoaded() && "already loaded?");

    if (dependency.isHeader()) {
      // The path may be empty if the file being loaded is a partial AST,
      // and the current compiler invocation is a merge-modules step.
      if (!dependency.Core.RawPath.empty()) {
        bool hadError =
            clangImporter->importHeader(dependency.Core.RawPath,
                                        file->getParentModule(),
                                        Core->importedHeaderInfo.fileSize,
                                        Core->importedHeaderInfo.fileModTime,
                                        Core->importedHeaderInfo.contents,
                                        diagLoc);
        if (hadError)
          return error(Status::FailedToLoadBridgingHeader);
      }
      ModuleDecl *importedHeaderModule = clangImporter->getImportedHeaderModule();
      dependency.Import = ImportedModule{ImportPath::Access(),
                                         importedHeaderModule};
      continue;
    }

    ModuleLoadingBehavior transitiveBehavior =
      getTransitiveLoadingBehavior(dependency, forTestable);

    if (ctx.LangOpts.EnableModuleLoadingRemarks) {
      ctx.Diags.diagnose(diagLoc,
                         diag::transitive_dependency_behavior,
                         dependency.Core.getPrettyPrintedPath(),
                         M->getName(),
                         unsigned(transitiveBehavior));
    }

    // Skip this dependency?
    if (transitiveBehavior == ModuleLoadingBehavior::Ignored)
      continue;

    ImportPath::Builder builder(ctx, dependency.Core.RawPath,
                                /*separator=*/'\0');
    for (const auto &elem : builder) {
      assert(!elem.Item.empty() && "invalid import path name");
    }

    auto importPath = builder.copyTo(ctx);
    auto modulePath = importPath.getModulePath(dependency.isScoped());
    auto accessPath = importPath.getAccessPath(dependency.isScoped());

    auto module = getModule(modulePath, /*allowLoading*/true);
    if (!module || module->failedToLoad()) {
      // If we're missing the module we're an overlay for, treat that specially.
      if (modulePath.size() == 1 &&
          modulePath.front().Item == file->getParentModule()->getName()) {
        return error(Status::MissingUnderlyingModule);
      }
      // Otherwise, continue trying to load dependencies, so that we can list
      // everything that's missing.

      // Report a missing dependency only when really needed.
      if (transitiveBehavior == ModuleLoadingBehavior::Required)
        missingDependency = true;

      continue;
    }

    dependency.Import = ImportedModule{accessPath, module};

    // SPI
    StringRef spisStr = dependency.Core.RawSPIs;
    while (!spisStr.empty()) {
      StringRef nextComponent;
      std::tie(nextComponent, spisStr) = spisStr.split('\0');
      dependency.spiGroups.push_back(ctx.getIdentifier(nextComponent));
    }

    if (!module->hasResolvedImports()) {
      // Notice that we check this condition /after/ recording the module that
      // caused the problem. Clients need to be able to track down what the
      // cycle was.
      return error(Status::CircularDependency);
    }
  }

  if (missingDependency) {
    return error(Status::MissingDependency);
  }

  return Status::Valid;
}

Status ModuleFile::associateWithFileContext(FileUnit *file, SourceLoc diagLoc,
                                            bool recoverFromIncompatibility) {
  PrettyStackTraceModuleFile stackEntry(*this);

  assert(!hasError() && "error already detected; should not call this");
  assert(!FileContext && "already associated with an AST module");
  FileContext = file;
  Status status = Status::Valid;

  ModuleDecl *M = file->getParentModule();
  // The real (on-disk) name of the module should be checked here as that's the
  // actually loaded module. In case module aliasing is used when building the main
  // module, e.g. -module-name MyModule -module-alias Foo=Bar, the loaded module
  // that maps to 'Foo' is actually Bar.swiftmodule|.swiftinterface (applies to swift
  // modules only), which is retrieved via M->getRealName(). If no module aliasing is
  // used, M->getRealName() will return the same value as M->getName(), which is 'Foo'.
  if (M->getRealName().str() != Core->Name) {
    return error(Status::NameMismatch);
  }

  ASTContext &ctx = getContext();

  llvm::Triple moduleTarget(llvm::Triple::normalize(Core->TargetTriple));
  if (!areCompatible(moduleTarget, ctx.LangOpts.Target)) {
    status = Status::TargetIncompatible;
    if (!recoverFromIncompatibility)
      return error(status);
  } else if (ctx.LangOpts.EnableTargetOSChecking && !M->isResilient() &&
             isTargetTooNew(moduleTarget, ctx.LangOpts.Target)) {
    status = Status::TargetTooNew;
    if (!recoverFromIncompatibility)
      return error(status);
  }

  StringRef SDKPath = ctx.SearchPathOpts.getSDKPath();
  if (SDKPath.empty() ||
      !Core->ModuleInputBuffer->getBufferIdentifier().startswith(SDKPath)) {
    for (const auto &searchPath : Core->SearchPaths) {
      ctx.addSearchPath(
        ctx.SearchPathOpts.SearchPathRemapper.remapPath(searchPath.Path),
        searchPath.IsFramework,
        searchPath.IsSystem);
    }
  }

  Status res = loadDependenciesForFileContext(file, diagLoc,
                                            /*forTestable=*/false);
  if (res != Status::Valid) return res;

  if (Core->Bits.HasEntryPoint) {
    FileContext->getParentModule()->registerEntryPointFile(FileContext,
                                                           SourceLoc(),
                                                           None);
  }

  return status;
}

ModuleLoadingBehavior
ModuleFile::getTransitiveLoadingBehavior(const Dependency &dependency,
    bool forTestable) const {
  ASTContext &ctx = getContext();
  ModuleDecl *mod = FileContext->getParentModule();

  // If this module file is being installed into the main module, it's treated
  // as a partial module.
  auto isPartialModule = mod->isMainModule();

  return Core->getTransitiveLoadingBehavior(dependency.Core,
                                            ctx.LangOpts.DebuggerSupport,
                                            isPartialModule,
                                            ctx.LangOpts.PackageName,
                                            forTestable);
}

bool ModuleFile::mayHaveDiagnosticsPointingAtBuffer() const {
  if (!hasError())
    return false;

  // Today, the only buffer that might have diagnostics in them is the input
  // buffer, and even then only if it has imported module contents.
  return !Core->importedHeaderInfo.contents.empty();
}

ModuleFile::~ModuleFile() { }

void ModuleFile::lookupValue(DeclName name,
                             SmallVectorImpl<ValueDecl*> &results) {
  PrettyStackTraceModuleFile stackEntry(*this);

  if (Core->TopLevelDecls) {
    // Find top-level declarations with the given name.
    // FIXME: As a bit of a hack, do lookup by the simple name, then filter
    // compound decls, to avoid having to completely redo how modules are
    // serialized.
    auto iter = Core->TopLevelDecls->find(name.getBaseName());
    if (iter != Core->TopLevelDecls->end()) {
      for (auto item : *iter) {
        Expected<Decl *> declOrError = getDeclChecked(item.second);
        if (!declOrError) {
          if (!getContext().LangOpts.EnableDeserializationRecovery)
            fatal(declOrError.takeError());
          llvm::consumeError(declOrError.takeError());
          continue;
        }
        auto VD = cast<ValueDecl>(declOrError.get());
        if (name.isSimpleName() || VD->getName().matchesRef(name))
          results.push_back(VD);
      }
    }
  }

  // If the name is an operator name, also look for operator methods.
  if (name.isOperator() && Core->OperatorMethodDecls) {
    auto iter = Core->OperatorMethodDecls->find(name.getBaseName());
    if (iter != Core->OperatorMethodDecls->end()) {
      for (auto item : *iter) {
        Expected<Decl *> declOrError = getDeclChecked(item.second);
        if (!declOrError) {
          if (!getContext().LangOpts.EnableDeserializationRecovery)
            fatal(declOrError.takeError());
          llvm::consumeError(declOrError.takeError());
          continue;
        }
        auto VD = cast<ValueDecl>(declOrError.get());
        results.push_back(VD);
      }
    }
  }
}

TypeDecl *ModuleFile::lookupLocalType(StringRef MangledName) {
  PrettyStackTraceModuleFile stackEntry(*this);

  if (!Core->LocalTypeDecls)
    return nullptr;

  auto iter = Core->LocalTypeDecls->find(MangledName);
  if (iter == Core->LocalTypeDecls->end())
    return nullptr;

  return cast<TypeDecl>(getDecl(*iter));
}

std::unique_ptr<llvm::MemoryBuffer>
ModuleFile::getModuleName(ASTContext &Ctx, StringRef modulePath,
                          std::string &Name) {
  // Open the module file
  auto &fs = *Ctx.SourceMgr.getFileSystem();
  auto moduleBuf = fs.getBufferForFile(modulePath);
  if (!moduleBuf)
    return nullptr;

  // FIXME: This goes through the full cost of creating a ModuleFile object
  // and then it keeps just the name and discards the whole object.
  // The user of this API is `ExplicitSwiftModuleLoader`, this API should
  // change to return a `ModuleFileSharedCore` object that
  // `ExplicitSwiftModuleLoader` caches.

  // Load the module file without validation.
  std::unique_ptr<llvm::MemoryBuffer> newBuf =
    llvm::MemoryBuffer::getMemBuffer(llvm::MemoryBufferRef(*moduleBuf.get()),
    /*RequiresNullTerminator=*/false);
  std::shared_ptr<const ModuleFileSharedCore> loadedModuleFile;
  bool isFramework = false;
  serialization::ValidationInfo loadInfo = ModuleFileSharedCore::load(
      "", "", std::move(newBuf), nullptr, nullptr,
      /*isFramework=*/isFramework, Ctx.SILOpts.EnableOSSAModules,
      Ctx.LangOpts.SDKName, Ctx.SearchPathOpts.DeserializedPathRecoverer,
      loadedModuleFile);
  Name = loadedModuleFile->Name.str();
  return std::move(moduleBuf.get());
}

OpaqueTypeDecl *ModuleFile::lookupOpaqueResultType(StringRef MangledName) {
  PrettyStackTraceModuleFile stackEntry(*this);

  if (!Core->OpaqueReturnTypeDecls)
    return nullptr;

  auto iter = Core->OpaqueReturnTypeDecls->find(MangledName);
  if (iter == Core->OpaqueReturnTypeDecls->end())
    return nullptr;

  return cast<OpaqueTypeDecl>(getDecl(*iter));
}

TypeDecl *ModuleFile::lookupNestedType(Identifier name,
                                       const NominalTypeDecl *parent) {
  PrettyStackTraceModuleFile stackEntry(*this);

  if (Core->NestedTypeDecls) {
    auto iter = Core->NestedTypeDecls->find(name);
    if (iter != Core->NestedTypeDecls->end()) {
      for (std::pair<DeclID, DeclID> entry : *iter) {
        assert(entry.first);
        auto declOrOffset = Decls[entry.first - 1];
        if (!declOrOffset.isComplete())
          continue;

        Decl *decl = declOrOffset;
        if (decl != parent)
          continue;
        return cast<TypeDecl>(getDecl(entry.second));
      }
    }
  }

  if (!UnderlyingModule)
    return nullptr;

  for (FileUnit *file : UnderlyingModule->getFiles())
    if (auto *nestedType = file->lookupNestedType(name, parent))
      return nestedType;

  return nullptr;
}

OperatorDecl *ModuleFile::lookupOperator(Identifier name,
                                         OperatorFixity fixity) {
  PrettyStackTraceModuleFile stackEntry(*this);

  if (!Core->OperatorDecls)
    return nullptr;

  auto iter = Core->OperatorDecls->find(name);
  if (iter == Core->OperatorDecls->end())
    return nullptr;

  for (auto item : *iter) {
    if (getStableFixity(fixity) == item.first)
      return cast<OperatorDecl>(getDecl(item.second));
  }
  return nullptr;
}

PrecedenceGroupDecl *ModuleFile::lookupPrecedenceGroup(Identifier name) {
  PrettyStackTraceModuleFile stackEntry(*this);

  if (!Core->PrecedenceGroupDecls)
    return nullptr;

  auto iter = Core->PrecedenceGroupDecls->find(name);
  if (iter == Core->PrecedenceGroupDecls->end())
    return nullptr;

  auto data = *iter;
  assert(data.size() == 1);
  return cast<PrecedenceGroupDecl>(getDecl(data[0].second));
}

void ModuleFile::getImportedModules(SmallVectorImpl<ImportedModule> &results,
                                    ModuleDecl::ImportFilter filter) {
  PrettyStackTraceModuleFile stackEntry(*this);

  for (auto &dep : Dependencies) {
    if (dep.isExported()) {
      if (!filter.contains(ModuleDecl::ImportFilterKind::Exported))
        continue;

    } else if (dep.isImplementationOnly()) {
      if (!filter.contains(ModuleDecl::ImportFilterKind::ImplementationOnly))
        continue;
      if (!dep.isLoaded()) {
        // Pretend we didn't have this import if we weren't originally asked to
        // load it.
        continue;
      }

    } else if (dep.isInternalOrBelow()) {
      if (!filter.contains(ModuleDecl::ImportFilterKind::InternalOrBelow))
        continue;

    } else if (dep.isPackageOnly()) {
      if (!filter.contains(ModuleDecl::ImportFilterKind::PackageOnly))
        continue;

    } else {
      if (!filter.contains(ModuleDecl::ImportFilterKind::Default))
        continue;
    }

    assert(dep.isLoaded());
    results.push_back(*(dep.Import));
  }
}

void ModuleFile::getImportDecls(SmallVectorImpl<Decl *> &Results) {
  if (!Bits.ComputedImportDecls) {
    ASTContext &Ctx = getContext();
    for (auto &Dep : Dependencies) {
      // FIXME: We need a better way to show headers, since they usually /are/
      // re-exported. This isn't likely to come up much, though.
      if (Dep.isHeader())
        continue;

      ImportPath::Builder importPath(Ctx, Dep.Core.RawPath, /*separator=*/'\0');

      if (importPath.size() == 1
          && importPath.front().Item == Ctx.StdlibModuleName)
        continue;

      auto modulePath = importPath.get().getModulePath(Dep.isScoped());
      ModuleDecl *M = Ctx.getLoadedModule(modulePath);

      auto Kind = ImportKind::Module;
      if (Dep.isScoped()) {
        auto ScopeID = importPath.get().getAccessPath(true).front().Item;
        assert(!ScopeID.empty() &&
               "invalid decl name (non-top-level decls not supported)");

        if (!M) {
          // The dependency module could not be loaded.  Just make a guess
          // about the import kind, we cannot do better.
          Kind = ImportKind::Func;
        } else {
          // Lookup the decl in the top-level module.
          ModuleDecl *TopLevelModule = M;
          if (importPath.size() > 1)
            TopLevelModule = Ctx.getLoadedModule(modulePath.getTopLevelPath());

          SmallVector<ValueDecl *, 8> Decls;
          TopLevelModule->lookupQualified(
              TopLevelModule, DeclNameRef(ScopeID),
              NL_QualifiedDefault, Decls);
          Optional<ImportKind> FoundKind = ImportDecl::findBestImportKind(Decls);
          assert(FoundKind.has_value() &&
                 "deserialized imports should not be ambiguous");
          Kind = *FoundKind;
        }
      }

      auto *ID = ImportDecl::create(Ctx, FileContext, SourceLoc(), Kind,
                                    SourceLoc(), importPath.get());
      ID->setModule(M);
      if (Dep.isExported())
        ID->getAttrs().add(
            new (Ctx) ExportedAttr(/*IsImplicit=*/false));
      if (Dep.isImplementationOnly())
        ID->getAttrs().add(
            new (Ctx) ImplementationOnlyAttr(/*IsImplicit=*/false));

      ImportDecls.push_back(ID);
    }
    Bits.ComputedImportDecls = true;
  }
  Results.append(ImportDecls.begin(), ImportDecls.end());
}

void ModuleFile::lookupVisibleDecls(ImportPath::Access accessPath,
                                    VisibleDeclConsumer &consumer,
                                    NLKind lookupKind) {
  PrettyStackTraceModuleFile stackEntry(*this);
  assert(accessPath.size() <= 1 && "can only refer to top-level decls");

  if (!Core->TopLevelDecls)
    return;

  auto tryImport = [this, &consumer](DeclID ID) {
    Expected<Decl *> declOrError = getDeclChecked(ID);
    if (!declOrError) {
      if (!getContext().LangOpts.EnableDeserializationRecovery)
        fatal(declOrError.takeError());
      llvm::consumeError(declOrError.takeError());
      return;
    }
    consumer.foundDecl(cast<ValueDecl>(declOrError.get()),
                       DeclVisibilityKind::VisibleAtTopLevel);
  };

  if (!accessPath.empty()) {
    auto iter = Core->TopLevelDecls->find(accessPath.front().Item);
    if (iter == Core->TopLevelDecls->end())
      return;

    for (auto item : *iter)
      tryImport(item.second);

    return;
  }

  for (auto entry : Core->TopLevelDecls->data()) {
    for (auto item : entry)
      tryImport(item.second);
  }
}

void ModuleFile::loadExtensions(NominalTypeDecl *nominal) {
  PrettyStackTraceModuleFile stackEntry(*this);
  if (!Core->ExtensionDecls)
    return;

  auto iter = Core->ExtensionDecls->find(nominal->getName());
  if (iter == Core->ExtensionDecls->end())
    return;

  if (nominal->getEffectiveAccess() < AccessLevel::Internal) {
    if (nominal->getModuleScopeContext() != getFile())
      return;
  }

  if (nominal->getParent()->isModuleScopeContext()) {
    auto parentFile = cast<FileUnit>(nominal->getParent());
    StringRef moduleName = parentFile->getExportedModuleName();

    for (auto item : *iter) {
      if (item.first != moduleName)
        continue;
      Expected<Decl *> declOrError = getDeclChecked(item.second);
      if (!declOrError) {
        if (!getContext().LangOpts.EnableDeserializationRecovery)
          fatal(declOrError.takeError());
        llvm::consumeError(declOrError.takeError());
      }
    }
  } else {
    std::string mangledName =
        Mangle::ASTMangler().mangleNominalType(nominal);
    for (auto item : *iter) {
      if (item.first != mangledName)
        continue;
      Expected<Decl *> declOrError = getDeclChecked(item.second);
      if (!declOrError) {
        if (!getContext().LangOpts.EnableDeserializationRecovery)
          fatal(declOrError.takeError());
        llvm::consumeError(declOrError.takeError());
      }
    }
  }
}

void ModuleFile::loadObjCMethods(
       NominalTypeDecl *typeDecl,
       ObjCSelector selector,
       bool isInstanceMethod,
       llvm::TinyPtrVector<AbstractFunctionDecl *> &methods) {
  // If we don't have an Objective-C method table, there's nothing to do.
  if (!Core->ObjCMethods)
    return;

  // Look for all methods in the module file with this selector.
  auto known = Core->ObjCMethods->find(selector);
  if (known == Core->ObjCMethods->end()) {
    return;
  }

  std::string ownerName = Mangle::ASTMangler().mangleNominalType(typeDecl);
  auto results = *known;
  for (const auto &result : results) {
    // If the method is the wrong kind (instance vs. class), skip it.
    if (isInstanceMethod != std::get<1>(result))
      continue;

    // If the method isn't defined in the requested class, skip it.
    if (std::get<0>(result) != ownerName)
      continue;

    // Deserialize the method and add it to the list.
    if (auto func = dyn_cast_or_null<AbstractFunctionDecl>(
                      getDecl(std::get<2>(result)))) {
      methods.push_back(func);
    }
  }
}

void ModuleFile::loadDerivativeFunctionConfigurations(
    AbstractFunctionDecl *originalAFD,
    llvm::SetVector<AutoDiffConfig> &results) {
  if (!Core->DerivativeFunctionConfigurations)
    return;
  auto &ctx = originalAFD->getASTContext();
  Mangle::ASTMangler Mangler;
  auto mangledName = Mangler.mangleDeclAsUSR(originalAFD, "");
  auto configs = Core->DerivativeFunctionConfigurations->find(mangledName);
  if (configs == Core->DerivativeFunctionConfigurations->end())
    return;
  for (auto entry : *configs) {
    auto *parameterIndices = IndexSubset::getFromString(ctx, entry.first);
    auto derivativeGenSigOrError = getGenericSignatureChecked(entry.second);
    if (!derivativeGenSigOrError) {
      if (!getContext().LangOpts.EnableDeserializationRecovery)
        fatal(derivativeGenSigOrError.takeError());
      llvm::consumeError(derivativeGenSigOrError.takeError());
    }
    auto derivativeGenSig = derivativeGenSigOrError.get();
    // NOTE(TF-1038): Result indices are currently unsupported in derivative
    // registration attributes. In the meantime, always use `{0}` (wrt the
    // first and only result).
    auto resultIndices = IndexSubset::get(ctx, 1, {0});
    results.insert({parameterIndices, resultIndices, derivativeGenSig});
  }
}

Optional<Fingerprint>
ModuleFile::loadFingerprint(const IterableDeclContext *IDC) const {
  PrettyStackTraceDecl trace("loading fingerprints for", IDC->getDecl());

  assert(IDC->wasDeserialized());
  assert(IDC->getDeclID() != 0);

  if (!Core->DeclFingerprints) {
    return None;
  }

  auto it = Core->DeclFingerprints->find(IDC->getDeclID());
  if (it == Core->DeclFingerprints->end()) {
    return None;
  }
  return *it;
}

TinyPtrVector<ValueDecl *>
ModuleFile::loadNamedMembers(const IterableDeclContext *IDC, DeclBaseName N,
                             uint64_t contextData) {
  PrettyStackTraceDecl trace("loading members for", IDC->getDecl());

  assert(IDC->wasDeserialized());
  assert(Core->DeclMemberNames);

  TinyPtrVector<ValueDecl *> results;
  auto i = Core->DeclMemberNames->find(N);
  if (i == Core->DeclMemberNames->end())
    return results;

  BitOffset subTableOffset = *i;
  std::unique_ptr<SerializedDeclMembersTable> &subTable =
    DeclMembersTables[subTableOffset];
  if (!subTable) {
    BCOffsetRAII restoreOffset(DeclMemberTablesCursor);
    if (diagnoseFatalIfNotSuccess(
            DeclMemberTablesCursor.JumpToBit(subTableOffset)))
      return results;
    llvm::BitstreamEntry entry =
        fatalIfUnexpected(DeclMemberTablesCursor.advance());
    if (entry.Kind != llvm::BitstreamEntry::Record) {
      diagnoseAndConsumeFatal();
      return results;
    }
    SmallVector<uint64_t, 64> scratch;
    StringRef blobData;
    unsigned kind = fatalIfUnexpected(
        DeclMemberTablesCursor.readRecord(entry.ID, scratch, &blobData));
    assert(kind == decl_member_tables_block::DECL_MEMBERS);
    (void)kind;
    subTable = Core->readDeclMembersTable(scratch, blobData);
  }

  assert(subTable);
  auto j = subTable->find(IDC->getDeclID());
  if (j != subTable->end()) {
    for (DeclID d : *j) {
      Expected<Decl *> mem = getDeclChecked(d);
      if (mem) {
        assert(mem.get() && "unchecked error deserializing named member");
        if (auto MVD = dyn_cast<ValueDecl>(mem.get())) {
          results.push_back(MVD);
        }
      } else {
        if (!getContext().LangOpts.EnableDeserializationRecovery)
          fatal(mem.takeError());
        consumeError(mem.takeError());
      }
    }
  }
  return results;
}

void ModuleFile::lookupClassMember(ImportPath::Access accessPath,
                                   DeclName name,
                                   SmallVectorImpl<ValueDecl*> &results) {
  PrettyStackTraceModuleFile stackEntry(*this);
  assert(accessPath.size() <= 1 && "can only refer to top-level decls");

  if (!Core->ClassMembersForDynamicLookup)
    return;

  auto iter = Core->ClassMembersForDynamicLookup->find(name.getBaseName());
  if (iter == Core->ClassMembersForDynamicLookup->end())
    return;

  if (!accessPath.empty()) {
    // As a hack to avoid completely redoing how the module is indexed, we take
    // the simple-name-based lookup then filter by the compound name if we have
    // one.
    if (name.isSimpleName()) {
      for (auto item : *iter) {
        auto declOrError = getDeclChecked(item.second);
        if (!declOrError) {
          if (!getContext().LangOpts.EnableDeserializationRecovery)
            fatal(declOrError.takeError());
          consumeError(declOrError.takeError());
          continue;
        }

        auto vd = cast<ValueDecl>(declOrError.get());
        auto dc = vd->getDeclContext();
        while (!dc->getParent()->isModuleScopeContext())
          dc = dc->getParent();
        if (auto nominal = dc->getSelfNominalTypeDecl())
          if (nominal->getName() == accessPath.front().Item)
            results.push_back(vd);
      }
    } else {
      for (auto item : *iter) {
        auto declOrError = getDeclChecked(item.second);
        if (!declOrError) {
          if (!getContext().LangOpts.EnableDeserializationRecovery)
            fatal(declOrError.takeError());
          consumeError(declOrError.takeError());
          continue;
        }

        auto vd = cast<ValueDecl>(declOrError.get());
        if (!vd->getName().matchesRef(name))
          continue;

        auto dc = vd->getDeclContext();
        while (!dc->getParent()->isModuleScopeContext())
          dc = dc->getParent();
        if (auto nominal = dc->getSelfNominalTypeDecl())
          if (nominal->getName() == accessPath.front().Item)
            results.push_back(vd);
      }
    }
    return;
  }

  for (auto item : *iter) {
    auto declOrError = getDeclChecked(item.second);
    if (!declOrError) {
      if (!getContext().LangOpts.EnableDeserializationRecovery)
        fatal(declOrError.takeError());
      consumeError(declOrError.takeError());
      continue;
    }

    auto vd = cast<ValueDecl>(declOrError.get());
    results.push_back(vd);
  }
}

void ModuleFile::lookupClassMembers(ImportPath::Access accessPath,
                                    VisibleDeclConsumer &consumer) {
  PrettyStackTraceModuleFile stackEntry(*this);
  assert(accessPath.size() <= 1 && "can only refer to top-level decls");

  if (!Core->ClassMembersForDynamicLookup)
    return;

  if (!accessPath.empty()) {
    for (const auto &list : Core->ClassMembersForDynamicLookup->data()) {
      for (auto item : list) {
        auto decl = getDeclChecked(item.second);
        if (!decl) {
          if (!getContext().LangOpts.EnableDeserializationRecovery)
            fatal(decl.takeError());
          llvm::consumeError(decl.takeError());
          continue;
        }

        auto vd = cast<ValueDecl>(decl.get());
        auto dc = vd->getDeclContext();
        while (!dc->getParent()->isModuleScopeContext())
          dc = dc->getParent();
        if (auto nominal = dc->getSelfNominalTypeDecl())
          if (nominal->getName() == accessPath.front().Item)
            consumer.foundDecl(vd, DeclVisibilityKind::DynamicLookup,
                               DynamicLookupInfo::AnyObject);
      }
    }
    return;
  }

  for (const auto &list : Core->ClassMembersForDynamicLookup->data()) {
    for (auto item : list) {
        auto decl = getDeclChecked(item.second);
        if (!decl) {
          if (!getContext().LangOpts.EnableDeserializationRecovery)
            fatal(decl.takeError());
          llvm::consumeError(decl.takeError());
          continue;
        }

      consumer.foundDecl(cast<ValueDecl>(decl.get()),
                         DeclVisibilityKind::DynamicLookup,
                         DynamicLookupInfo::AnyObject);
    }
  }
}

void ModuleFile::lookupObjCMethods(
       ObjCSelector selector,
       SmallVectorImpl<AbstractFunctionDecl *> &results) {
  // If we don't have an Objective-C method table, there's nothing to do.
  if (!Core->ObjCMethods) return;

  // Look for all methods in the module file with this selector.
  auto known = Core->ObjCMethods->find(selector);
  if (known == Core->ObjCMethods->end()) return;

  auto found = *known;
  for (const auto &result : found) {
    // Deserialize the method and add it to the list.
    auto declOrError = getDeclChecked(std::get<2>(result));
    if (!declOrError) {
        consumeError(declOrError.takeError());
        continue;
    }

    if (auto func = dyn_cast_or_null<AbstractFunctionDecl>(declOrError.get()))
      results.push_back(func);
  }
}

void
ModuleFile::collectLinkLibraries(ModuleDecl::LinkLibraryCallback callback) const {
  for (const auto &lib : Core->LinkLibraries)
    callback(lib);
  if (Core->Bits.IsFramework)
    callback(LinkLibrary(Core->Name, LibraryKind::Framework));
}

void ModuleFile::getTopLevelDecls(
       SmallVectorImpl<Decl *> &results,
       llvm::function_ref<bool(DeclAttributes)> matchAttributes) {
  PrettyStackTraceModuleFile stackEntry(*this);
  for (DeclID entry : Core->OrderedTopLevelDecls) {
    Expected<Decl *> declOrError = getDeclChecked(entry, matchAttributes);
    if (!declOrError) {
      if (declOrError.errorIsA<DeclAttributesDidNotMatch>()) {
        // Decl rejected by matchAttributes, ignore it.
        assert(matchAttributes);
        consumeError(declOrError.takeError());
        continue;
      }

      if (!getContext().LangOpts.EnableDeserializationRecovery)
        fatal(declOrError.takeError());
      consumeError(declOrError.takeError());
      continue;
    }
    results.push_back(declOrError.get());
  }
}

void ModuleFile::getExportedPrespecializations(
    SmallVectorImpl<Decl *> &results) {
  for (DeclID entry : Core->ExportedPrespecializationDecls) {
    Expected<Decl *> declOrError = getDeclChecked(entry);
    if (!declOrError) {
      if (!getContext().LangOpts.EnableDeserializationRecovery)
        fatal(declOrError.takeError());
      consumeError(declOrError.takeError());
      continue;
    }
    results.push_back(declOrError.get());
  }
}

void ModuleFile::getOperatorDecls(SmallVectorImpl<OperatorDecl *> &results) {
  PrettyStackTraceModuleFile stackEntry(*this);
  if (!Core->OperatorDecls)
    return;

  for (auto entry : Core->OperatorDecls->data()) {
    for (auto item : entry)
      results.push_back(cast<OperatorDecl>(getDecl(item.second)));
  }
}

void ModuleFile::getPrecedenceGroups(
       SmallVectorImpl<PrecedenceGroupDecl*> &results) {
  PrettyStackTraceModuleFile stackEntry(*this);
  if (Core->PrecedenceGroupDecls) {
    for (auto entry : Core->PrecedenceGroupDecls->data()) {
      for (auto item : entry)
        results.push_back(cast<PrecedenceGroupDecl>(getDecl(item.second)));
    }
  }
}

void
ModuleFile::getLocalTypeDecls(SmallVectorImpl<TypeDecl *> &results) {
  PrettyStackTraceModuleFile stackEntry(*this);
  if (!Core->LocalTypeDecls)
    return;

  for (auto DeclID : Core->LocalTypeDecls->data()) {
    auto TD = cast<TypeDecl>(getDecl(DeclID));
    results.push_back(TD);
  }
}

void
ModuleFile::getOpaqueReturnTypeDecls(SmallVectorImpl<OpaqueTypeDecl *> &results)
{
  PrettyStackTraceModuleFile stackEntry(*this);
  if (!Core->OpaqueReturnTypeDecls)
    return;

  for (auto DeclID : Core->OpaqueReturnTypeDecls->data()) {
    auto TD = cast<OpaqueTypeDecl>(getDecl(DeclID));
    results.push_back(TD);
  }
}

void ModuleFile::getDisplayDecls(SmallVectorImpl<Decl *> &results, bool recursive) {
  if (UnderlyingModule)
    UnderlyingModule->getDisplayDecls(results, recursive);

  PrettyStackTraceModuleFile stackEntry(*this);
  getImportDecls(results);
  getTopLevelDecls(results);
}

Optional<CommentInfo> ModuleFile::getCommentForDecl(const Decl *D) const {
  assert(D);

  // Keep these as assertions instead of early exits to ensure that we are not
  // doing extra work.  These cases should be handled by clients of this API.
  assert(!D->hasClangNode() &&
         "cannot find comments for Clang decls in Swift modules");
  assert(D->getDeclContext()->getModuleScopeContext() == FileContext &&
         "Decl is from a different serialized file");

  if (!Core->DeclCommentTable)
    return None;
  if (D->isImplicit())
    return None;
  // Compute the USR.
  llvm::SmallString<128> USRBuffer;
  llvm::raw_svector_ostream OS(USRBuffer);
  if (ide::printDeclUSR(D, OS))
    return None;

  return getCommentForDeclByUSR(USRBuffer.str());
}

bool ModuleFile::hasLoadedSwiftDoc() const {
  return Core->DeclCommentTable != nullptr;
}

void ModuleFile::collectSerializedSearchPath(
    llvm::function_ref<void(StringRef)> callback) const {
  for (auto path: Core->SearchPaths) {
    callback(path.Path);
  }
}

void ModuleFile::collectBasicSourceFileInfo(
    llvm::function_ref<void(const BasicSourceFileInfo &)> callback) const {
  if (Core->SourceFileListData.empty())
    return;
  assert(!Core->SourceLocsTextData.empty());

  auto *Cursor = Core->SourceFileListData.bytes_begin();
  auto *End = Core->SourceFileListData.bytes_end();
  while (Cursor < End) {
    // FilePath (byte offset in 'SourceLocsTextData').
    auto fileID = endian::readNext<uint32_t, little, unaligned>(Cursor);

    // InterfaceHashIncludingTypeMembers (fixed length string).
    auto fpStrIncludingTypeMembers = StringRef{reinterpret_cast<const char *>(Cursor),
                           Fingerprint::DIGEST_LENGTH};
    Cursor += Fingerprint::DIGEST_LENGTH;

    // InterfaceHashExcludingTypeMembers (fixed length string).
    auto fpStrExcludingTypeMembers = StringRef{reinterpret_cast<const char *>(Cursor),
                           Fingerprint::DIGEST_LENGTH};
    Cursor += Fingerprint::DIGEST_LENGTH;

    // LastModified (nanoseconds since epoch).
    auto timestamp = endian::readNext<uint64_t, little, unaligned>(Cursor);
    // FileSize (num of bytes).
    auto fileSize = endian::readNext<uint64_t, little, unaligned>(Cursor);

    assert(fileID < Core->SourceLocsTextData.size());
    auto filePath = Core->SourceLocsTextData.substr(fileID);
    size_t terminatorOffset = filePath.find('\0');
    filePath = filePath.slice(0, terminatorOffset);

    auto fingerprintIncludingTypeMembers =
      Fingerprint::fromString(fpStrIncludingTypeMembers);
    if (!fingerprintIncludingTypeMembers) {
      llvm::errs() << "Unconvertible fingerprint including type members'"
                   << fpStrIncludingTypeMembers << "'\n";
      abort();
    }
    auto fingerprintExcludingTypeMembers =
      Fingerprint::fromString(fpStrExcludingTypeMembers);
    if (!fingerprintExcludingTypeMembers) {
      llvm::errs() << "Unconvertible fingerprint excluding type members'"
                   << fpStrExcludingTypeMembers << "'\n";
      abort();
    }
    callback(BasicSourceFileInfo(filePath,
                                 fingerprintIncludingTypeMembers.value(),
                                 fingerprintExcludingTypeMembers.value(),
                                 llvm::sys::TimePoint<>(std::chrono::nanoseconds(timestamp)),
                                 fileSize));
  }
}

static StringRef readLocString(const char *&Data, StringRef StringData) {
  auto Str =
      StringData.substr(endian::readNext<uint32_t, little, unaligned>(Data));
  size_t TerminatorOffset = Str.find('\0');
  assert(TerminatorOffset != StringRef::npos && "unterminated string data");
  return Str.slice(0, TerminatorOffset);
}

static void readRawLoc(ExternalSourceLocs::RawLoc &Loc, const char *&Data,
                       StringRef StringData) {
  Loc.Offset = endian::readNext<uint32_t, little, unaligned>(Data);
  Loc.Line = endian::readNext<uint32_t, little, unaligned>(Data);
  Loc.Column = endian::readNext<uint32_t, little, unaligned>(Data);

  Loc.Directive.Offset = endian::readNext<uint32_t, little, unaligned>(Data);
  Loc.Directive.LineOffset = endian::readNext<int32_t, little, unaligned>(Data);
  Loc.Directive.Length = endian::readNext<uint32_t, little, unaligned>(Data);
  Loc.Directive.Name = readLocString(Data, StringData);
}

Optional<ExternalSourceLocs::RawLocs>
ModuleFile::getExternalRawLocsForDecl(const Decl *D) const {
  assert(D);
  // Keep these as assertions instead of early exits to ensure that we are not
  // doing extra work.  These cases should be handled by clients of this API.
  assert(!D->hasClangNode() &&
         "cannot find comments for Clang decls in Swift modules");
  assert(D->getDeclContext()->getModuleScopeContext() == FileContext &&
         "Decl is from a different serialized file");

  if (!Core->DeclUSRsTable)
    return None;
  // Future compilers may not provide BasicDeclLocsData anymore.
  if (Core->BasicDeclLocsData.empty())
    return None;
  if (D->isImplicit())
    return None;

  // Compute the USR.
  llvm::SmallString<128> USRBuffer;
  llvm::raw_svector_ostream OS(USRBuffer);
  if (ide::printDeclUSR(D, OS))
    return None;

  auto It = Core->DeclUSRsTable->find(OS.str());
  if (It == Core->DeclUSRsTable->end())
    return None;

  auto UsrId = *It;
  uint32_t RecordSize =
      4 +        // Source filename offset
      4 +        // Doc ranges offset
      4 * 3 * 7; // Loc/StartLoc/EndLoc each have 7 4-byte fields
  uint32_t RecordOffset = RecordSize * UsrId;
  assert(RecordOffset < Core->BasicDeclLocsData.size());
  assert(Core->BasicDeclLocsData.size() % RecordSize == 0);
  auto *Record = Core->BasicDeclLocsData.data() + RecordOffset;

  ExternalSourceLocs::RawLocs Result;
  Result.SourceFilePath = readLocString(Record, Core->SourceLocsTextData);

  const auto DocRangesOffset =
      endian::readNext<uint32_t, little, unaligned>(Record);
  if (DocRangesOffset) {
    assert(!Core->DocRangesData.empty());
    const auto *Data = Core->DocRangesData.data() + DocRangesOffset;
    const auto NumLocs = endian::readNext<uint32_t, little, unaligned>(Data);
    assert(NumLocs);

    for (uint32_t I = 0; I < NumLocs; ++I) {
      auto &Range =
          Result.DocRanges.emplace_back(ExternalSourceLocs::RawLoc(), 0);
      readRawLoc(Range.first, Data, Core->SourceLocsTextData);
      Range.second = endian::readNext<uint32_t, little, unaligned>(Data);
    }
  }

  readRawLoc(Result.Loc, Record, Core->SourceLocsTextData);
  readRawLoc(Result.StartLoc, Record, Core->SourceLocsTextData);
  readRawLoc(Result.EndLoc, Record, Core->SourceLocsTextData);
  return Result;
}

const static StringRef Separator = "/";

Optional<StringRef> ModuleFile::getGroupNameById(unsigned Id) const {
  if (!Core->GroupNamesMap)
    return None;
  const auto &GroupNamesMap = *Core->GroupNamesMap;
  auto it = GroupNamesMap.find(Id);
  if (it == GroupNamesMap.end())
    return None;
  StringRef Original = it->second;
  if (Original.empty())
    return None;
  auto SepPos = Original.find_last_of(Separator);
  assert(SepPos != StringRef::npos && "Cannot find Separator.");
  return StringRef(Original.data(), SepPos);
}

Optional<StringRef> ModuleFile::getSourceFileNameById(unsigned Id) const {
  if (!Core->GroupNamesMap)
    return None;
  const auto &GroupNamesMap = *Core->GroupNamesMap;
  auto it = GroupNamesMap.find(Id);
  if (it == GroupNamesMap.end())
    return None;
  StringRef Original = it->second;
  if (Original.empty())
    return None;
  auto SepPos = Original.find_last_of(Separator);
  assert(SepPos != StringRef::npos && "Cannot find Separator.");
  auto Start = Original.data() + SepPos + 1;
  auto Len = Original.size() - SepPos - 1;
  return StringRef(Start, Len);
}

Optional<StringRef> ModuleFile::getGroupNameForDecl(const Decl *D) const {
  auto Triple = getCommentForDecl(D);
  if (!Triple.has_value()) {
    return None;
  }
  return getGroupNameById(Triple.value().Group);
}


Optional<StringRef>
ModuleFile::getSourceFileNameForDecl(const Decl *D) const {
  auto Triple = getCommentForDecl(D);
  if (!Triple.has_value()) {
    return None;
  }
  return getSourceFileNameById(Triple.value().Group);
}

Optional<unsigned>
ModuleFile::getSourceOrderForDecl(const Decl *D) const {
  auto Triple = getCommentForDecl(D);
  if (!Triple.has_value()) {
    return None;
  }
  return Triple.value().SourceOrder;
}

void ModuleFile::collectAllGroups(SmallVectorImpl<StringRef> &Names) const {
  if (!Core->GroupNamesMap)
    return;
  for (auto It = Core->GroupNamesMap->begin(); It != Core->GroupNamesMap->end();
       ++It) {
    StringRef FullGroupName = It->getSecond();
    if (FullGroupName.empty())
      continue;
    auto Sep = FullGroupName.find_last_of(Separator);
    assert(Sep != StringRef::npos);
    auto Group = FullGroupName.substr(0, Sep);
    auto Found = std::find(Names.begin(), Names.end(), Group);
    if (Found != Names.end())
      continue;
    Names.push_back(Group);
  }
}

Optional<CommentInfo>
ModuleFile::getCommentForDeclByUSR(StringRef USR) const {
  if (!Core->DeclCommentTable)
    return None;

  // Use the comment cache to preserve the memory that the array of
  // `SingleRawComment`s, inside `CommentInfo`, points to, and generally avoid
  // allocating memory every time we query `Core->DeclCommentTable`.
  auto it = CommentsCache.find(USR);
  if (it != CommentsCache.end()) {
    const auto &cachePtr = it->second;
    if (!cachePtr)
      return None;
    return cachePtr->Info;
  }

  auto I = Core->DeclCommentTable->find(USR);
  if (I == Core->DeclCommentTable->end())
    return None;

  auto &cachePtr = CommentsCache[USR];
  cachePtr = *I;
  return cachePtr->Info;
}

Optional<StringRef>
ModuleFile::getGroupNameByUSR(StringRef USR) const {
  if (auto Comment = getCommentForDeclByUSR(USR)) {
    return getGroupNameById(Comment.value().Group);
  }
  return None;
}

Identifier ModuleFile::getDiscriminatorForPrivateDecl(const Decl *D) {
  Identifier discriminator = PrivateDiscriminatorsByValue.lookup(D);
  assert(!discriminator.empty() && "no discriminator found for decl");
  return discriminator;
}

void ModuleFile::verify() const {
#ifndef NDEBUG
  const auto &Context = getContext();
  for (const Serialized<Decl*> &next : Decls)
    if (next.isComplete() && swift::shouldVerify(next, Context))
      swift::verify(next);
#endif
}

bool SerializedASTFile::hasEntryPoint() const {
  return File.hasEntryPoint();
}

bool SerializedASTFile::getAllGenericSignatures(
                       SmallVectorImpl<GenericSignature> &genericSignatures) {
  genericSignatures.clear();
  for (unsigned index : indices(File.GenericSignatures)) {
    if (auto genericSig = File.getGenericSignature(index + 1))
      genericSignatures.push_back(genericSig);
  }

  return true;
}

ValueDecl *SerializedASTFile::getMainDecl() const {
  assert(hasEntryPoint());
  return cast_or_null<ValueDecl>(File.getDecl(File.getEntryPointDeclID()));
}

const version::Version &SerializedASTFile::getLanguageVersionBuiltWith() const {
  return File.getCompatibilityVersion();
}

StringRef SerializedASTFile::getModuleDefiningPath() const {
  StringRef moduleFilename = getFilename();
  StringRef parentDir = llvm::sys::path::parent_path(moduleFilename);

  if (llvm::sys::path::extension(parentDir) == ".swiftmodule")
    return parentDir;

  return moduleFilename;
}

StringRef SerializedASTFile::getExportedModuleName() const {
  auto name = File.getModuleExportAsName();
  if (!name.empty())
    return name;
  return FileUnit::getExportedModuleName();
}
