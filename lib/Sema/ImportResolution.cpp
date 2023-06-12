//===--- ImportResolution.cpp - Import Resolution -------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file performs import resolution.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "swift-import-resolution"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/ModuleNameLookup.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Statistic.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Parse/Parser.h"
#include "swift/Subsystems.h"
#include "swift/SymbolGraphGen/DocumentationCategory.h"
#include "clang/Basic/Module.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include <algorithm>
#include <system_error>
using namespace swift;

//===----------------------------------------------------------------------===//
// MARK: ImportResolver and supporting types
//===----------------------------------------------------------------------===//

namespace {
/// Represents an import which the ImportResolver knows exists, but which has
/// not yet had its options checked, module loaded, or cross-imports found.
///
/// An UnboundImport may represent a physical ImportDecl written in the
/// source, or it may represent a cross-import overlay that has been found and
/// needs to be loaded.
struct UnboundImport {
  /// Information about the import. Use this field, not \c getImportDecl(), to
  /// determine the behavior expected for this import.
  AttributedImport<UnloadedImportedModule> import;

  /// The source location to use when diagnosing errors for this import.
  SourceLoc importLoc;

  /// If this UnboundImport directly represents an ImportDecl, contains the
  /// ImportDecl it represents. This should only be used for diagnostics and
  /// for updating the AST; if you want to read information about the import,
  /// get it from the \c import field rather than from the \c ImportDecl.
  ///
  /// If this UnboundImport represents a cross-import, contains the declaring
  /// module's \c ModuleDecl.
  PointerUnion<NullablePtr<ImportDecl>, ModuleDecl *>
      importOrUnderlyingModuleDecl;

  NullablePtr<ImportDecl> getImportDecl() const {
    return importOrUnderlyingModuleDecl.is<NullablePtr<ImportDecl>>() ?
           importOrUnderlyingModuleDecl.get<NullablePtr<ImportDecl>>() : nullptr;
  }

  NullablePtr<ModuleDecl> getUnderlyingModule() const {
    return importOrUnderlyingModuleDecl.is<ModuleDecl *>() ?
           importOrUnderlyingModuleDecl.get<ModuleDecl *>() : nullptr;
  }

  /// Create an UnboundImport for a user-written import declaration.
  explicit UnboundImport(ImportDecl *ID);

  /// Create an UnboundImport for an unloaded implicit import.
  explicit UnboundImport(AttributedImport<UnloadedImportedModule> implicit);

  /// Create an UnboundImport for a cross-import overlay.
  explicit UnboundImport(ASTContext &ctx,
                         const UnboundImport &base, Identifier overlayName,
                         const AttributedImport<ImportedModule> &declaringImport,
                         const AttributedImport<ImportedModule> &bystandingImport);

  /// Diagnoses if the import would simply load the module \p SF already
  /// belongs to, with no actual effect.
  ///
  /// Some apparent self-imports do actually load a different module; this
  /// method allows them.
  bool checkNotTautological(const SourceFile &SF);

  /// Make sure the module actually loaded, and diagnose if it didn't.
  bool checkModuleLoaded(ModuleDecl *M, SourceFile &SF);

  /// Find the top-level module for this module; that is, if \p M is the
  /// module \c Foo.Bar.Baz, this finds \c Foo.
  ///
  /// Specifically, this method returns:
  ///
  /// \li \p M if \p M is a top-level module.
  /// \li \c nullptr if \p M is a submodule of \c SF's parent module. (This
  ///     corner case can occur in mixed-source frameworks, where Swift code
  ///     can import a Clang submodule of itself.)
  /// \li The top-level parent (i.e. ancestor with no parent) module above
  ///     \p M otherwise.
  NullablePtr<ModuleDecl> getTopLevelModule(ModuleDecl *M, SourceFile &SF);

  /// Diagnose any errors concerning the \c @_exported, \c @_implementationOnly,
  /// \c \@testable, or \c @_private attributes, including a 
  /// non-implementation-only import of a fragile library from a resilient one.
  void validateOptions(NullablePtr<ModuleDecl> topLevelModule, SourceFile &SF);

  /// Create an \c AttributedImport<ImportedModule> from the information in this
  /// UnboundImport.
  AttributedImport<ImportedModule>
  makeAttributedImport(ModuleDecl *module) const {
    return import.getLoaded(module);
  }

private:
  void validatePrivate(ModuleDecl *topLevelModule);

  /// Check that no import has more than one of the following modifiers:
  /// @_exported, @_implementationOnly, and @_spiOnly.
  void validateRestrictedImport(ASTContext &ctx);

  void validateTestable(ModuleDecl *topLevelModule);
  void validateResilience(NullablePtr<ModuleDecl> topLevelModule,
                          SourceFile &SF);
  void validateAllowableClient(ModuleDecl *topLevelModule, SourceFile &SF);
  void validateInterfaceWithPackageName(ModuleDecl *topLevelModule, SourceFile &SF);

  /// Diagnoses an inability to import \p modulePath in this situation and, if
  /// \p attrs is provided and has an \p attrKind, invalidates the attribute and
  /// offers a fix-it to remove it.
  void diagnoseInvalidAttr(DeclAttrKind attrKind, DiagnosticEngine &diags,
                           Diag<Identifier> diagID);
};

class ImportResolver final : public DeclVisitor<ImportResolver> {
  friend DeclVisitor<ImportResolver>;

  SourceFile &SF;
  ASTContext &ctx;

  /// Imports which still need their options checked, modules loaded, and
  /// cross-imports found.
  SmallVector<UnboundImport, 4> unboundImports;

  /// The list of fully bound imports.
  SmallVector<AttributedImport<ImportedModule>, 16> boundImports;

  /// All imported modules which should be considered when cross-importing.
  /// This is basically the transitive import graph, but with only top-level
  /// modules and without reexports from Objective-C modules.
  ///
  /// We use a \c SmallSetVector here because this doubles as the worklist for
  /// cross-importing, so we want to keep it in order; this is feasible
  /// because this set is usually fairly small.
  SmallSetVector<AttributedImport<ImportedModule>, 64> crossImportableModules;

  /// The subset of \c crossImportableModules which may declare cross-imports.
  ///
  /// This is a performance optimization. Since most modules do not register
  /// any cross-imports, we can usually compare against this list, which is
  /// much, much smaller than \c crossImportableModules.
  SmallVector<AttributedImport<ImportedModule>, 16> crossImportDeclaringModules;

  /// The index of the next module in \c visibleModules that should be
  /// cross-imported.
  size_t nextModuleToCrossImport = 0;

public:
  ImportResolver(SourceFile &SF) : SF(SF), ctx(SF.getASTContext()) {
    addImplicitImports();
  }

  void addImplicitImports();

  /// Retrieve the finalized imports.
  ArrayRef<AttributedImport<ImportedModule>> getFinishedImports() const {
    return boundImports;
  }

private:
  // We only need to visit import decls.
  void visitImportDecl(ImportDecl *ID);

  // Ignore other decls.
  void visitDecl(Decl *D) {}

  template<typename ...ArgTypes>
  InFlightDiagnostic diagnose(ArgTypes &&...Args) {
    return ctx.Diags.diagnose(std::forward<ArgTypes>(Args)...);
  }

  /// Calls \c bindImport() on unbound imports until \c boundImports is drained.
  void bindPendingImports();

  /// Check a single unbound import, bind it, add it to \c boundImports,
  /// and add its cross-import overlays to \c unboundImports.
  void bindImport(UnboundImport &&I);

  /// Adds \p I and \p M to \c boundImports and \c visibleModules.
  void addImport(const UnboundImport &I, ModuleDecl *M);

  /// Adds \p desc and everything it re-exports to \c visibleModules using
  /// the settings from \c desc.
  void addCrossImportableModules(AttributedImport<ImportedModule> desc);

  /// * If \p I is a cross-import overlay, registers \p M as overlaying
  ///   \p I.underlyingModule in \c SF.
  /// * Discovers any cross-imports between \p I and previously bound imports,
  ///   then adds them to \c unboundImports using source locations from \p I.
  void crossImport(ModuleDecl *M, UnboundImport &I);

  /// Discovers any cross-imports between \p newImport and
  /// \p oldImports and adds them to \c unboundImports, using source
  /// locations from \p I.
  void findCrossImportsInLists(
      UnboundImport &I,
      ArrayRef<AttributedImport<ImportedModule>> declaring,
      ArrayRef<AttributedImport<ImportedModule>> bystanding,
      bool shouldDiagnoseRedundantCrossImports);

  /// Discovers any cross-imports between \p declaringImport and
  /// \p bystandingImport and adds them to \c unboundImports, using source
  /// locations from \p I.
  void findCrossImports(UnboundImport &I,
      const AttributedImport<ImportedModule> &declaringImport,
      const AttributedImport<ImportedModule> &bystandingImport,
      bool shouldDiagnoseRedundantCrossImports);

  /// Load a module referenced by an import statement.
  ///
  /// Returns null if no module can be loaded.
  ModuleDecl *getModule(ImportPath::Module ModuleID);
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// MARK: performImportResolution
//===----------------------------------------------------------------------===//

/// performImportResolution - This walks the AST to resolve imports.
///
/// Before we can type-check a source file, we need to make declarations
/// imported from other modules available. This is done by processing top-level
/// \c ImportDecl nodes, along with related validation.
///
/// Import resolution operates on a parsed but otherwise unvalidated AST.
void swift::performImportResolution(SourceFile &SF) {
  // If we've already performed import resolution, bail.
  if (SF.ASTStage == SourceFile::ImportsResolved)
    return;

  FrontendStatsTracer tracer(SF.getASTContext().Stats,
                             "Import resolution");

  // If we're silencing parsing warnings, then also silence import warnings.
  // This is necessary for secondary files as they can be parsed and have their
  // imports resolved multiple times.
  auto &diags = SF.getASTContext().Diags;
  auto didSuppressWarnings = diags.getSuppressWarnings();
  auto shouldSuppress = SF.getParsingOptions().contains(
      SourceFile::ParsingFlags::SuppressWarnings);
  diags.setSuppressWarnings(didSuppressWarnings || shouldSuppress);
  SWIFT_DEFER { diags.setSuppressWarnings(didSuppressWarnings); };

  ImportResolver resolver(SF);

  // Resolve each import declaration.
  for (auto D : SF.getTopLevelDecls())
    resolver.visit(D);
  for (auto D : SF.getHoistedDecls())
    resolver.visit(D);

  SF.setImports(resolver.getFinishedImports());

  SF.ASTStage = SourceFile::ImportsResolved;
  verify(SF);
}

//===----------------------------------------------------------------------===//
// MARK: Import handling generally
//===----------------------------------------------------------------------===//

void ImportResolver::visitImportDecl(ImportDecl *ID) {
  assert(unboundImports.empty());

  unboundImports.emplace_back(ID);
  bindPendingImports();
}

void ImportResolver::bindPendingImports() {
  while(!unboundImports.empty())
    bindImport(unboundImports.pop_back_val());
}

void ImportResolver::bindImport(UnboundImport &&I) {
  auto ID = I.getImportDecl();

  if (!I.checkNotTautological(SF)) {
    // No need to process this import further.
    if (ID)
      ID.get()->setModule(SF.getParentModule());
    return;
  }

  ModuleDecl *M = getModule(I.import.module.getModulePath());
  if (!I.checkModuleLoaded(M, SF)) {
    // Can't process further. checkModuleLoaded() will have diagnosed this.
    if (ID)
      ID.get()->setModule(nullptr);
    return;
  }

  // Load more dependencies for testable imports.
  if (I.import.options.contains(ImportFlags::Testable)) {
    SourceLoc diagLoc;
    if (ID) diagLoc = ID.get()->getStartLoc();

    for (auto file: M->getFiles())
      file->loadDependenciesForTestable(diagLoc);
  }

  auto topLevelModule = I.getTopLevelModule(M, SF);

  I.validateOptions(topLevelModule, SF);

  if (topLevelModule && topLevelModule != M) {
    // If we have distinct submodule and top-level module, add both.
    addImport(I, M);
    addImport(I, topLevelModule.get());
  }
  else {
    // Add only the import itself.
    addImport(I, M);
  }

  crossImport(M, I);

  if (ID)
    ID.get()->setModule(M);
}

void ImportResolver::addImport(const UnboundImport &I, ModuleDecl *M) {
  auto importDesc = I.makeAttributedImport(M);
  addCrossImportableModules(importDesc);
  boundImports.push_back(importDesc);
}

//===----------------------------------------------------------------------===//
// MARK: Import module loading
//===----------------------------------------------------------------------===//

ModuleDecl *
ImportResolver::getModule(ImportPath::Module modulePath) {
  auto loadingModule = SF.getParentModule();

  ASTContext &ctx = loadingModule->getASTContext();

  assert(!modulePath.empty());
  auto moduleID = modulePath[0];

  // The Builtin module cannot be explicitly imported unless:
  // 1. We're in a .sil file
  // 2. '-enable-builtin-module'/'-enable-experimental-feature BuiltinModule'
  //    was passed.
  //
  // FIXME: Eventually, it would be nice to separate '-parse-stdlib' from
  // implicitly importing Builtin, but we're not there yet.
  if (SF.Kind == SourceFileKind::SIL ||
      ctx.LangOpts.hasFeature(Feature::BuiltinModule)) {
    if (moduleID.Item == ctx.TheBuiltinModule->getName()) {
      return ctx.TheBuiltinModule;
    }
  }

  // If the imported module name is the same as the current module,
  // skip the Swift module loader and use the Clang module loader instead.
  // This allows a Swift module to extend a Clang module of the same name.
  //
  // FIXME: We'd like to only use this in SIL mode, but unfortunately we use it
  // for clang overlays as well.
  if (ctx.getRealModuleName(moduleID.Item) == loadingModule->getName() &&
      modulePath.size() == 1) {
    if (auto importer = ctx.getClangModuleLoader())
      return importer->loadModule(moduleID.Loc, modulePath);
    return nullptr;
  }

  return ctx.getModule(modulePath);
}

NullablePtr<ModuleDecl>
UnboundImport::getTopLevelModule(ModuleDecl *M, SourceFile &SF) {
  if (import.module.getModulePath().size() == 1)
    return M;

  // If we imported a submodule, import the top-level module as well.
  Identifier topLevelName = import.module.getModulePath().front().Item;
  ModuleDecl *topLevelModule = SF.getASTContext().getLoadedModule(topLevelName);

  if (!topLevelModule) {
    // Clang can sometimes import top-level modules as if they were
    // submodules.
    assert(!M->getFiles().empty() &&
           isa<ClangModuleUnit>(M->getFiles().front()));
    return M;
  }

  if (topLevelModule == SF.getParentModule())
    // This can happen when compiling a mixed-source framework (or overlay)
    // that imports a submodule of its C part.
    return nullptr;

  return topLevelModule;
}

//===----------------------------------------------------------------------===//
// MARK: Implicit imports
//===----------------------------------------------------------------------===//

static void tryStdlibFixit(ASTContext &ctx,
                           StringRef moduleName,
                           SourceLoc loc) {
  if (moduleName.startswith("std")) {
    ctx.Diags.diagnose(loc, diag::did_you_mean_cxxstdlib)
      .fixItReplaceChars(loc, loc.getAdvancedLoc(3), "CxxStdlib");
  }
}

static void diagnoseNoSuchModule(ModuleDecl *importingModule,
                                 SourceLoc importLoc,
                                 ImportPath::Module modulePath,
                                 bool nonfatalInREPL) {
  ASTContext &ctx = importingModule->getASTContext();

  if (modulePath.size() == 1 &&
      importingModule->getName() == modulePath.front().Item) {
    ctx.Diags.diagnose(importLoc, diag::error_underlying_module_not_found,
                       importingModule->getName());
  } else {
    SmallString<64> modulePathStr;
    modulePath.getString(modulePathStr);

    auto diagKind = diag::sema_no_import;
    if (nonfatalInREPL && ctx.LangOpts.DebuggerSupport)
      diagKind = diag::sema_no_import_repl;
    ctx.Diags.diagnose(importLoc, diagKind, modulePathStr);
    tryStdlibFixit(ctx, modulePathStr, importLoc);
  }

  if (ctx.SearchPathOpts.getSDKPath().empty() &&
      llvm::Triple(llvm::sys::getProcessTriple()).isMacOSX()) {
    ctx.Diags.diagnose(SourceLoc(), diag::sema_no_import_no_sdk);
    ctx.Diags.diagnose(SourceLoc(), diag::sema_no_import_no_sdk_xcrun);
  }
}

ImplicitImportList
ModuleImplicitImportsRequest::evaluate(Evaluator &evaluator,
                                       ModuleDecl *module) const {
  SmallVector<AttributedImport<ImportedModule>, 4> imports;
  SmallVector<AttributedImport<UnloadedImportedModule>, 4> unloadedImports;

  auto &ctx = module->getASTContext();
  auto &importInfo = module->getImplicitImportInfo();

  // Add an implicit stdlib if needed.
  ModuleDecl *stdlib;
  switch (importInfo.StdlibKind) {
  case ImplicitStdlibKind::None:
    stdlib = nullptr;
    break;
  case ImplicitStdlibKind::Builtin:
    stdlib = ctx.TheBuiltinModule;
    break;
  case ImplicitStdlibKind::Stdlib:
    stdlib = ctx.getStdlibModule(/*loadIfAbsent*/ true);
    assert(stdlib && "Missing stdlib?");
    break;
  }

  if (stdlib)
    imports.emplace_back(ImportedModule(stdlib));

  // Add any modules we were asked to implicitly import.
  llvm::copy(importInfo.AdditionalUnloadedImports,
             std::back_inserter(unloadedImports));

  // Add any pre-loaded modules.
  llvm::copy(importInfo.AdditionalImports, std::back_inserter(imports));

  auto *clangImporter =
      static_cast<ClangImporter *>(ctx.getClangModuleLoader());

  // Implicitly import the bridging header module if needed.
  auto bridgingHeaderPath = importInfo.BridgingHeaderPath;
  if (!bridgingHeaderPath.empty() &&
      !clangImporter->importBridgingHeader(bridgingHeaderPath, module)) {
    auto *headerModule = clangImporter->getImportedHeaderModule();
    assert(headerModule && "Didn't load bridging header?");
    imports.emplace_back(
        ImportedModule(headerModule), SourceLoc(), ImportFlags::Exported);
  }

  // Implicitly import the underlying Clang half of this module if needed.
  if (importInfo.ShouldImportUnderlyingModule) {
    // An @_exported self-import is loaded from ClangImporter instead of being
    // rejected; see the special case in getModuleImpl() for details.
    ImportPath::Builder importPath(module->getName());
    unloadedImports.emplace_back(UnloadedImportedModule(importPath.copyTo(ctx),
                                                        /*isScoped=*/false),
                                 SourceLoc(), ImportFlags::Exported);
  }

  return { ctx.AllocateCopy(imports), ctx.AllocateCopy(unloadedImports) };
}

void ImportResolver::addImplicitImports() {
  auto implicitImports = SF.getParentModule()->getImplicitImports();

  // TODO: Support cross-module imports.
  for (auto &import : implicitImports.imports) {
    assert(!(SF.Kind == SourceFileKind::SIL &&
             import.module.importedModule->isStdlibModule()));
    boundImports.push_back(import);
  }

  for (auto &unloadedImport : implicitImports.unloadedImports)
    unboundImports.emplace_back(unloadedImport);

  bindPendingImports();
}

UnboundImport::UnboundImport(AttributedImport<UnloadedImportedModule> implicit)
  : import(implicit), importLoc(),
    importOrUnderlyingModuleDecl(static_cast<ImportDecl *>(nullptr)) {}

//===----------------------------------------------------------------------===//
// MARK: Import validation (except for scoped imports)
//===----------------------------------------------------------------------===//

/// Create an UnboundImport for a user-written import declaration.
UnboundImport::UnboundImport(ImportDecl *ID)
  : import(UnloadedImportedModule(ID->getImportPath(), ID->getImportKind()),
           ID->getStartLoc(), {}),
    importLoc(ID->getLoc()), importOrUnderlyingModuleDecl(ID)
{
  if (ID->isExported())
    import.options |= ImportFlags::Exported;

  if (ID->getAttrs().hasAttribute<TestableAttr>())
    import.options |= ImportFlags::Testable;

  if (ID->getAttrs().hasAttribute<ImplementationOnlyAttr>())
    import.options |= ImportFlags::ImplementationOnly;

  import.accessLevel = ID->getAccessLevel();
  if (auto attr = ID->getAttrs().getAttribute<AccessControlAttr>()) {
    import.accessLevelLoc = attr->getLocation();
  }

  if (ID->getAttrs().hasAttribute<SPIOnlyAttr>())
    import.options |= ImportFlags::SPIOnly;

  if (auto *privateImportAttr =
          ID->getAttrs().getAttribute<PrivateImportAttr>()) {
    import.options |= ImportFlags::PrivateImport;
    import.sourceFileArg = privateImportAttr->getSourceFile();
  }

  SmallVector<Identifier, 4> spiGroups;
  for (auto attr : ID->getAttrs().getAttributes<SPIAccessControlAttr>()) {
    import.options |= ImportFlags::SPIAccessControl;
    auto attrSPIs = attr->getSPIGroups();
    spiGroups.append(attrSPIs.begin(), attrSPIs.end());
  }
  import.spiGroups = ID->getASTContext().AllocateCopy(spiGroups);

  if (auto attr = ID->getAttrs().getAttribute<PreconcurrencyAttr>()) {
    import.options |= ImportFlags::Preconcurrency;
    import.preconcurrencyRange = attr->getRangeWithAt();
  }

  if (auto attr = ID->getAttrs().getAttribute<WeakLinkedAttr>())
    import.options |= ImportFlags::WeakLinked;

  import.docVisibility = swift::symbolgraphgen::documentationVisibilityForDecl(ID);
}

bool UnboundImport::checkNotTautological(const SourceFile &SF) {
  // Exit early if this is not a self-import.
  auto modulePath = import.module.getModulePath();
  if (modulePath.front().Item != SF.getParentModule()->getName() ||
      // Overlays use an @_exported self-import to load their clang module.
      import.options.contains(ImportFlags::Exported) ||
      // Imports of your own submodules are allowed in cross-language libraries.
      modulePath.size() != 1 ||
      // SIL files self-import to get decls from the rest of the module.
      SF.Kind == SourceFileKind::SIL)
    return true;

  ASTContext &ctx = SF.getASTContext();

  StringRef filename = llvm::sys::path::filename(SF.getFilename());
  if (filename.empty())
    ctx.Diags.diagnose(importLoc, diag::sema_import_current_module,
                       modulePath.front().Item);
  else
    ctx.Diags.diagnose(importLoc, diag::sema_import_current_module_with_file,
                       filename, modulePath.front().Item);

  return false;
}

bool UnboundImport::checkModuleLoaded(ModuleDecl *M, SourceFile &SF) {
  if (M)
    return true;

  diagnoseNoSuchModule(SF.getParentModule(), importLoc,
                       import.module.getModulePath(), /*nonfatalInREPL=*/true);
  return false;
}

void UnboundImport::validateOptions(NullablePtr<ModuleDecl> topLevelModule,
                                    SourceFile &SF) {
  validateRestrictedImport(SF.getASTContext());

  if (auto *top = topLevelModule.getPtrOrNull()) {
    // FIXME: Having these two calls in this if condition seems dubious.
    //
    // Here's the deal: Per getTopLevelModule(), we will only skip this block
    // if you are in a mixed-source module and trying to import a submodule from
    // your clang half. But that means you're trying to @testable import or
    // @_private import part of yourself--and, moreover, a clang part of
    // yourself--which doesn't make any sense to do. Shouldn't we diagnose that?
    //
    // I'm leaving this alone for now because I'm trying to refactor without
    // changing behavior, but it smells funny.
    validateTestable(top);
    validatePrivate(top);
    validateAllowableClient(top, SF);
    validateInterfaceWithPackageName(top, SF);
  }
  validateResilience(topLevelModule, SF);
}

void UnboundImport::validatePrivate(ModuleDecl *topLevelModule) {
  assert(topLevelModule);
  ASTContext &ctx = topLevelModule->getASTContext();

  if (!import.options.contains(ImportFlags::PrivateImport))
    return;

  if (topLevelModule->arePrivateImportsEnabled())
    return;

  diagnoseInvalidAttr(DAK_PrivateImport, ctx.Diags,
                      diag::module_not_compiled_for_private_import);
  import.sourceFileArg = StringRef();
}

void UnboundImport::validateRestrictedImport(ASTContext &ctx) {
  static llvm::SmallVector<ImportFlags, 2> flags = {ImportFlags::Exported,
                                                    ImportFlags::ImplementationOnly,
                                                    ImportFlags::SPIOnly};
  llvm::SmallVector<ImportFlags, 2> conflicts;

  for (auto flag : flags) {
    if (import.options.contains(flag))
      conflicts.push_back(flag);
  }

  // Quit if there's no conflicting attributes.
  if (conflicts.size() < 2)
    return;

  // Remove all but one flag to maintain the invariant.
  for (auto iter = conflicts.begin(); iter != std::prev(conflicts.end()); iter ++)
    import.options -= *iter;

  DeclAttrKind attrToRemove = conflicts[0] == ImportFlags::ImplementationOnly?
                                      DAK_Exported : DAK_ImplementationOnly;

  // More dense enum with some cases of ImportFlags,
  // used by import_restriction_conflict.
  enum class ImportFlagForDiag : uint8_t {
    ImplementationOnly,
    SPIOnly,
    Exported
  };
  auto flagToDiag = [](ImportFlags flag) {
    switch (flag) {
      case ImportFlags::ImplementationOnly:
        return ImportFlagForDiag::ImplementationOnly;
      case ImportFlags::SPIOnly:
        return ImportFlagForDiag::SPIOnly;
      case ImportFlags::Exported:
        return ImportFlagForDiag::Exported;
      default:
        llvm_unreachable("Unexpected ImportFlag");
    }
  };

  // Report the conflict, only the first two conflicts should be enough.
  auto diag = ctx.Diags.diagnose(import.module.getModulePath().front().Loc,
                                 diag::import_restriction_conflict,
                                 import.module.getModulePath().front().Item,
                                 (uint8_t)flagToDiag(conflicts[0]),
                                 (uint8_t)flagToDiag(conflicts[1]));

  auto *ID = getImportDecl().getPtrOrNull();
  if (!ID) return;
  auto *attr = ID->getAttrs().getAttribute(attrToRemove);
  if (!attr) return;

  diag.fixItRemove(attr->getRangeWithAt());
  attr->setInvalid();
}

void UnboundImport::validateTestable(ModuleDecl *topLevelModule) {
  assert(topLevelModule);
  ASTContext &ctx = topLevelModule->getASTContext();

  if (!import.options.contains(ImportFlags::Testable) ||
      topLevelModule->isTestingEnabled() ||
      topLevelModule->isNonSwiftModule() ||
      !ctx.LangOpts.EnableTestableAttrRequiresTestableModule)
    return;

  diagnoseInvalidAttr(DAK_Testable, ctx.Diags, diag::module_not_testable);
}

void UnboundImport::validateAllowableClient(ModuleDecl *importee,
                                            SourceFile &SF) {
  assert(importee);
  auto *importer = SF.getParentModule();
  if (!importee->allowImportedBy(importer)) {
    ASTContext &ctx = SF.getASTContext();
    ctx.Diags.diagnose(import.module.getModulePath().front().Loc,
                       diag::module_allowable_client_violation,
                       importee->getName(),
                       importer->getName());
  }
}

void UnboundImport::validateInterfaceWithPackageName(ModuleDecl *topLevelModule,
                                        SourceFile &SF) {
  assert(topLevelModule);

  // If current source file is interface, don't throw an error
  if (SF.Kind == SourceFileKind::Interface)
    return;

  // If source file is .swift or non-interface, show diags when importing an interface file
  ASTContext &ctx = topLevelModule->getASTContext();
  if (!topLevelModule->getPackageName().empty() &&
      topLevelModule->getPackageName().str() == ctx.LangOpts.PackageName &&
      topLevelModule->isBuiltFromInterface()) {
      ctx.Diags.diagnose(SourceLoc(),
                         diag::in_package_module_not_compiled_from_source,
                         topLevelModule->getBaseIdentifier(),
                         ctx.LangOpts.PackageName,
                         topLevelModule->getModuleSourceFilename()
                         );
  }
}

void UnboundImport::validateResilience(NullablePtr<ModuleDecl> topLevelModule,
                                       SourceFile &SF) {
  if (import.options.contains(ImportFlags::ImplementationOnly) ||
      import.accessLevel < AccessLevel::Public)
    return;

  // Per getTopLevelModule(), we'll only get nullptr here for non-Swift modules,
  // so these two really mean the same thing.
  if (!topLevelModule || topLevelModule.get()->isNonSwiftModule())
    return;

  ASTContext &ctx = SF.getASTContext();

  // If the module we're validating is the builtin one, then just return because
  // this module is essentially a header only import and does not concern
  // itself with resiliency. This can occur when one has passed
  // '-enable-builtin-module' and is explicitly importing the Builtin module in
  // their sources.
  if (topLevelModule.get() == ctx.TheBuiltinModule) {
    return;
  }

  if (!SF.getParentModule()->isResilient() ||
      topLevelModule.get()->isResilient())
    return;

  auto inFlight = ctx.Diags.diagnose(import.module.getModulePath().front().Loc,
                                     diag::module_not_compiled_with_library_evolution,
                                     topLevelModule.get()->getName(),
                                     SF.getParentModule()->getName());

  if (ctx.LangOpts.hasFeature(Feature::AccessLevelOnImport)) {
    SourceLoc attrLoc = import.accessLevelLoc;
    if (attrLoc.isValid())
      inFlight.fixItReplace(attrLoc, "internal");
    else
      inFlight.fixItInsert(import.importLoc, "internal ");
  } else {
    inFlight.limitBehavior(DiagnosticBehavior::Warning);
  }
}

void UnboundImport::diagnoseInvalidAttr(DeclAttrKind attrKind,
                                        DiagnosticEngine &diags,
                                        Diag<Identifier> diagID) {
  auto diag = diags.diagnose(import.module.getModulePath().front().Loc, diagID,
                             import.module.getModulePath().front().Item);

  auto *ID = getImportDecl().getPtrOrNull();
  if (!ID) return;
  auto *attr = ID->getAttrs().getAttribute(attrKind);
  if (!attr) return;

  diag.fixItRemove(attr->getRangeWithAt());
  attr->setInvalid();
}

/// Returns true if any file in the module contains an import with \c flag.
static bool moduleHasAnyImportsMatchingFlag(ModuleDecl *mod, ImportFlags flag) {
  for (const FileUnit *F : mod->getFiles()) {
    auto *SF = dyn_cast<SourceFile>(F);
    if (SF && SF->hasImportsWithFlag(flag))
      return true;
  }
  return false;
}

/// Finds all import declarations for a single file that inconsistently match
/// \c predicate and passes each pair of inconsistent imports to \c diagnose.
template <typename Pred, typename Diag>
static void findInconsistentImportsAcrossFile(
    const SourceFile *SF, Pred predicate, Diag diagnose,
    llvm::DenseMap<ModuleDecl *, const ImportDecl *> &matchingImports,
    llvm::DenseMap<ModuleDecl *, std::vector<const ImportDecl *>> &otherImports) {

  for (auto *topLevelDecl : SF->getTopLevelDecls()) {
    auto *nextImport = dyn_cast<ImportDecl>(topLevelDecl);
    if (!nextImport)
      continue;

    ModuleDecl *module = nextImport->getModule();
    if (!module)
      continue;

    if (predicate(nextImport)) {
      // We found a matching import.
      bool isNew = matchingImports.insert({module, nextImport}).second;
      if (!isNew)
        continue;

      auto seenOtherImportPosition = otherImports.find(module);
      if (seenOtherImportPosition != otherImports.end()) {
        for (auto *seenOtherImport : seenOtherImportPosition->getSecond())
          diagnose(seenOtherImport, nextImport);

        // We're done with these; keep the map small if possible.
        otherImports.erase(seenOtherImportPosition);
      }
      continue;
    }

    // We saw a non-matching import. Is that in conflict with what we've seen?
    if (auto *seenMatchingImport = matchingImports.lookup(module)) {
      diagnose(nextImport, seenMatchingImport);
      continue;
    }

    // Otherwise, record it for later.
    otherImports[module].push_back(nextImport);
  }
}

/// Finds all import declarations for a single module that inconsistently match
/// \c predicate and passes each pair of inconsistent imports to \c diagnose.
template <typename Pred, typename Diag>
static void findInconsistentImportsAcrossModule(ModuleDecl *mod, Pred predicate,
                                    Diag diagnose) {
  llvm::DenseMap<ModuleDecl *, const ImportDecl *> matchingImports;
  llvm::DenseMap<ModuleDecl *, std::vector<const ImportDecl *>> otherImports;

  for (const FileUnit *file : mod->getFiles()) {
    auto *SF = dyn_cast<SourceFile>(file);
    if (!SF)
      continue;

    findInconsistentImportsAcrossFile(SF, predicate, diagnose,
                                      matchingImports, otherImports);
  }
}

evaluator::SideEffect
CheckInconsistentImplementationOnlyImportsRequest::evaluate(
    Evaluator &evaluator, ModuleDecl *mod) const {
  if (!moduleHasAnyImportsMatchingFlag(mod, ImportFlags::ImplementationOnly))
    return {};

  auto diagnose = [mod](const ImportDecl *normalImport,
                        const ImportDecl *implementationOnlyImport) {
    auto &diags = mod->getDiags();
    {
      InFlightDiagnostic warning =
          diags.diagnose(normalImport, diag::warn_implementation_only_conflict,
                         normalImport->getModule()->getName());
      if (normalImport->getAttrs().isEmpty()) {
        // Only try to add a fix-it if there's no other annotations on the
        // import to avoid creating things like
        // `@_implementationOnly @_exported import Foo`. The developer can
        // resolve those manually.
        warning.fixItInsert(normalImport->getStartLoc(),
                            "@_implementationOnly ");
      }
    }
    diags.diagnose(implementationOnlyImport,
                   diag::implementation_only_conflict_here);
  };

  auto predicate = [](ImportDecl *decl) {
    return decl->getAttrs().hasAttribute<ImplementationOnlyAttr>();
  };

  findInconsistentImportsAcrossModule(mod, predicate, diagnose);
  return {};
}

evaluator::SideEffect
CheckInconsistentSPIOnlyImportsRequest::evaluate(
    Evaluator &evaluator, SourceFile *SF) const {

  auto mod = SF->getParentModule();
  auto diagnose = [mod](const ImportDecl *normalImport,
                        const ImportDecl *spiOnlyImport) {
    auto &diags = mod->getDiags();
    {
      diags.diagnose(normalImport, diag::spi_only_import_conflict,
                     normalImport->getModule()->getName());
    }
    diags.diagnose(spiOnlyImport,
                   diag::spi_only_import_conflict_here);
  };

  auto predicate = [](ImportDecl *decl) {
    return decl->getAttrs().hasAttribute<SPIOnlyAttr>();
  };

  llvm::DenseMap<ModuleDecl *, const ImportDecl *> matchingImports;
  llvm::DenseMap<ModuleDecl *, std::vector<const ImportDecl *>> otherImports;
  findInconsistentImportsAcrossFile(SF, predicate, diagnose,
                                    matchingImports, otherImports);
  return {};
}

evaluator::SideEffect
CheckInconsistentAccessLevelOnImport::evaluate(
    Evaluator &evaluator, SourceFile *SF) const {

  auto mod = SF->getParentModule();
  auto diagnose = [mod](const ImportDecl *implicitImport,
                        const ImportDecl *otherImport) {
    auto otherAccessLevel = otherImport->getAccessLevel();

    auto &diags = mod->getDiags();
    {
      InFlightDiagnostic error =
        diags.diagnose(implicitImport, diag::inconsistent_implicit_access_level_on_import,
                       implicitImport->getModule()->getName(), otherAccessLevel);
      error.fixItInsert(implicitImport->getStartLoc(),
                        diag::inconsistent_implicit_access_level_on_import_fixit,
                        otherAccessLevel);
    }

    SourceLoc accessLevelLoc = otherImport->getStartLoc();
    if (auto attr = otherImport->getAttrs().getAttribute<AccessControlAttr>())
      accessLevelLoc = attr->getLocation();
    diags.diagnose(accessLevelLoc,
                   diag::inconsistent_implicit_access_level_on_import_here,
                   otherAccessLevel);
  };

  auto predicate = [](ImportDecl *decl) {
    return !decl->isAccessLevelImplicit();
  };

  findInconsistentImportsAcrossModule(mod, predicate, diagnose);
  return {};
}

evaluator::SideEffect
CheckInconsistentWeakLinkedImportsRequest::evaluate(Evaluator &evaluator,
                                                    ModuleDecl *mod) const {
  if (!moduleHasAnyImportsMatchingFlag(mod, ImportFlags::WeakLinked))
    return {};

  auto diagnose = [mod](const ImportDecl *otherImport,
                        const ImportDecl *weakLinkedImport) {
    auto attr = weakLinkedImport->getAttrs().getAttribute<WeakLinkedAttr>();
    auto &diags = mod->getDiags();
    diags
        .diagnose(otherImport, diag::import_attr_conflict,
                  otherImport->getModule()->getName(), attr)
        .fixItInsert(otherImport->getStartLoc(), "@_weakLinked ");
    diags.diagnose(weakLinkedImport, diag::import_attr_conflict_here, attr);
  };

  auto predicate = [](ImportDecl *decl) {
    return decl->getAttrs().hasAttribute<WeakLinkedAttr>();
  };

  findInconsistentImportsAcrossModule(mod, predicate, diagnose);
  return {};
}

//===----------------------------------------------------------------------===//
// MARK: Scoped imports
//===----------------------------------------------------------------------===//

/// Returns true if a decl with the given \p actual kind can legally be
/// imported via the given \p expected kind.
static bool isCompatibleImportKind(ImportKind expected, ImportKind actual) {
  if (expected == actual)
    return true;
  if (expected != ImportKind::Type)
    return false;

  switch (actual) {
  case ImportKind::Module:
    llvm_unreachable("module imports do not bring in decls");
  case ImportKind::Type:
    llvm_unreachable("individual decls cannot have abstract import kind");
  case ImportKind::Struct:
  case ImportKind::Class:
  case ImportKind::Enum:
    return true;
  case ImportKind::Protocol:
  case ImportKind::Var:
  case ImportKind::Func:
    return false;
  }

  llvm_unreachable("Unhandled ImportKind in switch.");
}

static bool isNominalImportKind(ImportKind kind) {
  switch (kind) {
  case ImportKind::Module:
    llvm_unreachable("module imports do not bring in decls");
  case ImportKind::Struct:
  case ImportKind::Class:
  case ImportKind::Enum:
  case ImportKind::Protocol:
    return true;
  case ImportKind::Type:
  case ImportKind::Var:
  case ImportKind::Func:
    return false;
  }
  llvm_unreachable("unhandled kind");
}

static const char *getImportKindString(ImportKind kind) {
  switch (kind) {
  case ImportKind::Module:
    llvm_unreachable("module imports do not bring in decls");
  case ImportKind::Type:
    return "typealias";
  case ImportKind::Struct:
    return "struct";
  case ImportKind::Class:
    return "class";
  case ImportKind::Enum:
    return "enum";
  case ImportKind::Protocol:
    return "protocol";
  case ImportKind::Var:
    return "var";
  case ImportKind::Func:
    return "func";
  }

  llvm_unreachable("Unhandled ImportKind in switch.");
}

ArrayRef<ValueDecl *>
ScopedImportLookupRequest::evaluate(Evaluator &evaluator,
                                    ImportDecl *import) const {
  using namespace namelookup;

  auto importKind = import->getImportKind();
  assert(importKind != ImportKind::Module);

  // If we weren't able to load the module referenced by the import, we're done.
  // The fact that we failed to load the module has already been diagnosed by
  // import resolution.
  auto *module = import->getModule();
  if (!module)
    return ArrayRef<ValueDecl *>();

  /// Validate the scoped import.
  ///
  /// We validate the scope by making sure that the named declaration exists
  /// and is of the kind indicated by the keyword. This can't be done until
  /// we've performed import resolution, since that can introduce additional
  /// imports (such as cross-import overlays) which could provide the declaration.
  auto &ctx = module->getASTContext();
  auto accessPath = import->getAccessPath();
  auto modulePath = import->getModulePath();
  auto *topLevelModule = module->getTopLevelModule();

  // Lookup the referenced decl in the top-level module. This is necessary as
  // the Clang importer currently handles submodules by importing their decls
  // into the top-level module.
  // FIXME: Doesn't handle scoped testable imports correctly.
  assert(accessPath.size() == 1 && "can't handle sub-decl imports");
  SmallVector<ValueDecl *, 8> decls;
  lookupInModule(topLevelModule, accessPath.front().Item, decls,
                 NLKind::QualifiedLookup, ResolutionKind::Overloadable,
                 import->getDeclContext()->getModuleScopeContext(),
                 import->getLoc(), NL_QualifiedDefault);

  auto importLoc = import->getLoc();
  if (decls.empty()) {
    ctx.Diags.diagnose(importLoc, diag::decl_does_not_exist_in_module,
                       static_cast<unsigned>(importKind),
                       accessPath.front().Item, modulePath.front().Item)
      .highlight(accessPath.getSourceRange());
    return ArrayRef<ValueDecl *>();
  }

  Optional<ImportKind> actualKind = ImportDecl::findBestImportKind(decls);
  if (!actualKind.has_value()) {
    // FIXME: print entire module name?
    ctx.Diags.diagnose(importLoc, diag::ambiguous_decl_in_module,
                       accessPath.front().Item, module->getName());
    for (auto next : decls)
      ctx.Diags.diagnose(next, diag::found_candidate);

  } else if (!isCompatibleImportKind(importKind, *actualKind)) {
    Optional<InFlightDiagnostic> emittedDiag;
    if (*actualKind == ImportKind::Type && isNominalImportKind(importKind)) {
      assert(decls.size() == 1 &&
             "if we start suggesting ImportKind::Type for, e.g., a mix of "
             "structs and classes, we'll need a different message here");
      assert(isa<TypeAliasDecl>(decls.front()) &&
             "ImportKind::Type is only the best choice for a typealias");
      auto *typealias = cast<TypeAliasDecl>(decls.front());
      emittedDiag.emplace(ctx.Diags.diagnose(
          importLoc, diag::imported_decl_is_wrong_kind_typealias,
          typealias->getDescriptiveKind(),
          typealias->getDeclaredInterfaceType(),
          getImportKindString(importKind)));
    } else {
      emittedDiag.emplace(ctx.Diags.diagnose(
          importLoc, diag::imported_decl_is_wrong_kind,
          accessPath.front().Item, getImportKindString(importKind),
          static_cast<unsigned>(*actualKind)));
    }

    emittedDiag->fixItReplace(SourceRange(import->getKindLoc()),
                              getImportKindString(*actualKind));
    emittedDiag->flush();

    if (decls.size() == 1)
      ctx.Diags.diagnose(decls.front(), diag::decl_declared_here,
                         decls.front()->getName());
  }
  return ctx.AllocateCopy(decls);
}

//===----------------------------------------------------------------------===//
// MARK: Cross-import overlays
//===----------------------------------------------------------------------===//

static bool canCrossImport(const AttributedImport<ImportedModule> &import) {
  if (import.options.contains(ImportFlags::Testable))
    return false;
  if (import.options.contains(ImportFlags::PrivateImport))
    return false;

  return true;
}

static UnloadedImportedModule makeUnimportedCrossImportOverlay(
    ASTContext &ctx,
    Identifier overlayName,
    const UnboundImport &base,
    const AttributedImport<ImportedModule> &declaringImport) {
  ImportPath::Builder
      builder(overlayName, base.import.module.getModulePath()[0].Loc);

  // If the declaring import was scoped, inherit that scope in the overlay's
  // import.
  llvm::copy(declaringImport.module.accessPath, std::back_inserter(builder));

  // Cross-imports are not backed by an ImportDecl, so we need to provide
  // our own storage for their module paths.
  return UnloadedImportedModule(builder.copyTo(ctx),
      /*isScoped=*/!declaringImport.module.accessPath.empty());
}

/// Create an UnboundImport for a cross-import overlay.
UnboundImport::UnboundImport(
    ASTContext &ctx, const UnboundImport &base, Identifier overlayName,
    const AttributedImport<ImportedModule> &declaringImport,
    const AttributedImport<ImportedModule> &bystandingImport)
    : import(makeUnimportedCrossImportOverlay(ctx, overlayName, base,
                                              declaringImport), {}),
      importLoc(base.importLoc),
      importOrUnderlyingModuleDecl(declaringImport.module.importedModule)
{
  // A cross-import is never private or testable, and never comes from a private
  // or testable import.
  assert(canCrossImport(declaringImport));
  assert(canCrossImport(bystandingImport));

  auto &declaringOptions = declaringImport.options;
  auto &bystandingOptions = bystandingImport.options;

  // If both are exported, the cross-import is exported.
  if (declaringOptions.contains(ImportFlags::Exported) &&
      bystandingOptions.contains(ImportFlags::Exported))
    import.options |= ImportFlags::Exported;

  // If either are implementation-only, the cross-import is
  // implementation-only.
  if (declaringOptions.contains(ImportFlags::ImplementationOnly) ||
      bystandingOptions.contains(ImportFlags::ImplementationOnly))
    import.options |= ImportFlags::ImplementationOnly;

  // If either have a `@_documentation(visibility: <access>)` attribute, the
  // cross-import has the more restrictive of the two.
  if (declaringImport.docVisibility || bystandingImport.docVisibility) {
    auto declaringAccess = declaringImport.docVisibility.value_or(AccessLevel::Public);
    auto bystandingAccess = bystandingImport.docVisibility.value_or(AccessLevel::Public);
    import.docVisibility = std::min(declaringAccess, bystandingAccess);
  }
}

void ImportResolver::crossImport(ModuleDecl *M, UnboundImport &I) {
  // FIXME: There is a fundamental problem with this find-as-we-go approach:
  // The '@_exported import'-ed modules in this module's other files should be
  // taken into account, but they haven't been bound yet, and binding them would
  // require cross-importing. Chicken, meet egg.
  //
  // The way to fix this is probably to restructure import resolution so we
  // first bind all exported imports in all files, then bind all other imports
  // in each file. This may become simpler if we bind all ImportDecls before we
  // start computing cross-imports, but I haven't figured that part out yet.
  //
  // Fixing this is tracked within Apple by rdar://problem/59527118. I haven't
  // filed an SR because I plan to address it myself, but if this comment is
  // still here in April 2020 without an SR number, please file a Swift bug and
  // harass @brentdax to fill in the details.

  if (!SF.shouldCrossImport())
    return;

  if (I.getUnderlyingModule()) {
    auto underlying = I.getUnderlyingModule().get();

    // If this is a clang module, and it has a clang overlay, we want the
    // separately-imported overlay to sit on top of the clang overlay.
    if (underlying->isNonSwiftModule())
      underlying = underlying->getTopLevelModule(true);

    // FIXME: Should we warn if M doesn't reexport underlyingModule?
    SF.addSeparatelyImportedOverlay(M, underlying);
  }

  auto newImports = crossImportableModules.getArrayRef()
                        .drop_front(nextModuleToCrossImport);

  if (newImports.empty())
    // Nothing to do except crash when we read past the end of
    // crossImportableModules in that assert at the bottom.
    return;

  for (auto &newImport : newImports) {
    if (!canCrossImport(newImport))
      continue;

    // First we check if any of the imports of modules that have declared
    // cross-imports have declared one with this module.
    findCrossImportsInLists(I, crossImportDeclaringModules, {newImport},
                            /*shouldDiagnoseRedundantCrossImports=*/false);

    // If this module doesn't declare any cross-imports, we're done with this
    // import.
    if (!newImport.module.importedModule->mightDeclareCrossImportOverlays())
      continue;

    // Fine, we need to do the slow-but-rare thing: check if this import
    // declares a cross-import with any previous one.
    auto oldImports =
        // Slice from the start of crossImportableModules up to newImport.
        llvm::makeArrayRef(crossImportableModules.getArrayRef().data(),
                           &newImport);
    findCrossImportsInLists(I, {newImport}, oldImports,
                            /*shouldDiagnoseRedundantCrossImports=*/true);

    // Add this to the list of imports everyone needs to check against.
    crossImportDeclaringModules.push_back(newImport);
  }

  // Catch potential memory smashers
  assert(newImports.data() ==
             &crossImportableModules[nextModuleToCrossImport] &&
         "findCrossImports() should never mutate visibleModules");

  nextModuleToCrossImport = crossImportableModules.size();
}

void ImportResolver::findCrossImportsInLists(
    UnboundImport &I, ArrayRef<AttributedImport<ImportedModule>> declaring,
    ArrayRef<AttributedImport<ImportedModule>> bystanding,
    bool shouldDiagnoseRedundantCrossImports) {
  for (auto &declaringImport : declaring) {
    if (!canCrossImport(declaringImport))
      continue;

    for (auto &bystandingImport : bystanding) {
      if (!canCrossImport(bystandingImport))
        continue;

      findCrossImports(I, declaringImport, bystandingImport,
                       shouldDiagnoseRedundantCrossImports);
    }
  }
}

void ImportResolver::findCrossImports(
    UnboundImport &I,
    const AttributedImport<ImportedModule> &declaringImport,
    const AttributedImport<ImportedModule> &bystandingImport,
    bool shouldDiagnoseRedundantCrossImports) {
  assert(&declaringImport != &bystandingImport);

  LLVM_DEBUG(llvm::dbgs() << "Discovering cross-imports for '"
                          << declaringImport.module.importedModule->getName()
                          << "' -> '"
                          << bystandingImport.module.importedModule->getName()
                          << "'\n");

  if (ctx.Stats)
    ++ctx.Stats->getFrontendCounters().NumCrossImportsChecked;

  // Find modules we need to import.
  SmallVector<Identifier, 4> names;
  declaringImport.module.importedModule->findDeclaredCrossImportOverlays(
      bystandingImport.module.importedModule->getName(), names, I.importLoc);

  // If we're diagnosing cases where we cross-import in both directions, get the
  // inverse list. Otherwise, leave the list empty.
  SmallVector<Identifier, 4> oppositeNames;
  if (shouldDiagnoseRedundantCrossImports)
    bystandingImport.module.importedModule->findDeclaredCrossImportOverlays(
        declaringImport.module.importedModule->getName(), oppositeNames,
        I.importLoc);

  if (ctx.Stats && !names.empty())
    ++ctx.Stats->getFrontendCounters().NumCrossImportsFound;

  // Add import statements.
  for (auto &name : names) {
    // If we are actually compiling part of this overlay, don't try to load the
    // overlay.
    if (name == SF.getParentModule()->getName())
      continue;

    unboundImports.emplace_back(
        declaringImport.module.importedModule->getASTContext(), I, name,
        declaringImport, bystandingImport);

    if (llvm::is_contained(oppositeNames, name))
      ctx.Diags.diagnose(I.importLoc, diag::cross_imported_by_both_modules,
                         declaringImport.module.importedModule->getName(),
                         bystandingImport.module.importedModule->getName(),
                         name);

    if (ctx.LangOpts.EnableCrossImportRemarks)
      ctx.Diags.diagnose(I.importLoc, diag::cross_import_added,
                         declaringImport.module.importedModule->getName(),
                         bystandingImport.module.importedModule->getName(),
                         name);

    LLVM_DEBUG({
      auto &crossImportOptions = unboundImports.back().import.options;
      llvm::dbgs() << "  ";
      if (crossImportOptions.contains(ImportFlags::Exported))
        llvm::dbgs() << "@_exported ";
      if (crossImportOptions.contains(ImportFlags::ImplementationOnly))
        llvm::dbgs() << "@_implementationOnly ";
      llvm::dbgs() << "import " << name << "\n";
    });
  }
}

static bool isSubmodule(ModuleDecl* M) {
  auto clangMod = M->findUnderlyingClangModule();
  return clangMod && clangMod->Parent;
}

void ImportResolver::addCrossImportableModules(
    AttributedImport<ImportedModule> importDesc) {
  // FIXME: namelookup::getAllImports() doesn't quite do what we need (mainly
  // w.r.t. scoped imports), but it seems like we could extend it to do so, and
  // then eliminate most of this.

  SmallVector<ImportedModule, 16> importsWorklist = { importDesc.module };

  while (!importsWorklist.empty()) {
    auto nextImport = importsWorklist.pop_back_val();

    // If they are both scoped, and they are *differently* scoped, this import
    // cannot possibly expose anything new. Skip it.
    if (!importDesc.module.accessPath.empty() &&
        !nextImport.accessPath.empty() &&
        !importDesc.module.accessPath.isSameAs(nextImport.accessPath))
      continue;

    // If we are importing a submodule, treat it as though we imported its
    // top-level module (or rather, the top-level module's clang overlay if it
    // has one).
    if (isSubmodule(nextImport.importedModule)) {
      nextImport.importedModule =
          nextImport.importedModule->getTopLevelModule(/*overlay=*/true);

      // If the rewritten import is now for our own parent module, this was an
      // import of our own clang submodule in a mixed-language module. We don't
      // want to process our own cross-imports.
      if (nextImport.importedModule == SF.getParentModule())
        continue;
    }

    // Drop this module into the ImportDesc so we treat it as imported with the
    // same options and scope as `I`.
    importDesc.module.importedModule = nextImport.importedModule;

    // Add it to the list of cross-importable modules. If it's already there,
    // we've already done the rest of the work of this loop iteration and can
    // skip it.
    if (!crossImportableModules.insert(importDesc))
      continue;

    // We don't consider the re-exports of ObjC modules because ObjC re-exports
    // everything, so there isn't enough signal there to work from.
    if (nextImport.importedModule->isNonSwiftModule())
      continue;

    // Add the module's re-exports to worklist.
    nextImport.importedModule->getImportedModules(
        importsWorklist, ModuleDecl::ImportFilterKind::Exported);
  }
}

LLVM_ATTRIBUTE_USED static void dumpCrossImportOverlays(ModuleDecl* M) {
  llvm::dbgs() << "'" << M->getName() << "' declares cross-imports with bystanders:\n";

  SmallVector<Identifier, 4> secondaries;
  M->getDeclaredCrossImportBystanders(secondaries);

  for (auto secondary : secondaries)
    llvm::dbgs() << "  " << secondary << "\n";
}
