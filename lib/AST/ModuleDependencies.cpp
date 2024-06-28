//===--- ModuleDependencies.h - Module Dependencies -------------*- C++ -*-===//
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
//
// This file implements data structures for capturing module dependencies.
//
//===----------------------------------------------------------------------===//
#include "swift/AST/ModuleDependencies.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/MacroDefinition.h"
#include "swift/AST/PluginLoader.h"
#include "swift/AST/SourceFile.h"
#include "swift/Frontend/Frontend.h"
#include "llvm/CAS/CASProvidingFileSystem.h"
#include "llvm/CAS/CachingOnDiskFileSystem.h"
#include "llvm/Config/config.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrefixMapper.h"
#include <system_error>
using namespace swift;

ModuleDependencyInfoStorageBase::~ModuleDependencyInfoStorageBase() {}

bool ModuleDependencyInfo::isSwiftModule() const {
  return isSwiftInterfaceModule() || isSwiftSourceModule() ||
         isSwiftBinaryModule() || isSwiftPlaceholderModule();
}

bool ModuleDependencyInfo::isTextualSwiftModule() const {
  return isSwiftInterfaceModule() || isSwiftSourceModule();
}

ModuleDependencyKind &operator++(ModuleDependencyKind &e) {
  if (e == ModuleDependencyKind::LastKind) {
    llvm_unreachable(
        "Attempting to increment last enum value on ModuleDependencyKind");
  }
  e = ModuleDependencyKind(
      static_cast<std::underlying_type<ModuleDependencyKind>::type>(e) + 1);
  return e;
}

bool ModuleDependencyInfo::isSwiftInterfaceModule() const {
  return isa<SwiftInterfaceModuleDependenciesStorage>(storage.get());
}

bool ModuleDependencyInfo::isSwiftSourceModule() const {
  return isa<SwiftSourceModuleDependenciesStorage>(storage.get());
}

bool ModuleDependencyInfo::isSwiftBinaryModule() const {
  return isa<SwiftBinaryModuleDependencyStorage>(storage.get());
}

bool ModuleDependencyInfo::isSwiftPlaceholderModule() const {
  return isa<SwiftPlaceholderModuleDependencyStorage>(storage.get());
}

bool ModuleDependencyInfo::isClangModule() const {
  return isa<ClangModuleDependencyStorage>(storage.get());
}

/// Retrieve the dependencies for a Swift textual interface module.
const SwiftInterfaceModuleDependenciesStorage *
ModuleDependencyInfo::getAsSwiftInterfaceModule() const {
  return dyn_cast<SwiftInterfaceModuleDependenciesStorage>(storage.get());
}

const SwiftSourceModuleDependenciesStorage *
ModuleDependencyInfo::getAsSwiftSourceModule() const {
  return dyn_cast<SwiftSourceModuleDependenciesStorage>(storage.get());
}

/// Retrieve the dependencies for a binary Swift dependency module.
const SwiftBinaryModuleDependencyStorage *
ModuleDependencyInfo::getAsSwiftBinaryModule() const {
  return dyn_cast<SwiftBinaryModuleDependencyStorage>(storage.get());
}

/// Retrieve the dependencies for a Clang module.
const ClangModuleDependencyStorage *
ModuleDependencyInfo::getAsClangModule() const {
  return dyn_cast<ClangModuleDependencyStorage>(storage.get());
}

/// Retrieve the dependencies for a placeholder dependency module stub.
const SwiftPlaceholderModuleDependencyStorage *
ModuleDependencyInfo::getAsPlaceholderDependencyModule() const {
  return dyn_cast<SwiftPlaceholderModuleDependencyStorage>(storage.get());
}

void ModuleDependencyInfo::addTestableImport(ImportPath::Module module) {
  assert(getAsSwiftSourceModule() && "Expected source module for addTestableImport.");
  dyn_cast<SwiftSourceModuleDependenciesStorage>(storage.get())->addTestableImport(module);
}

void ModuleDependencyInfo::addMacroDependency(StringRef macroModuleName,
                                              StringRef libraryPath,
                                              StringRef executablePath) {
  if (auto swiftSourceStorage =
          dyn_cast<SwiftSourceModuleDependenciesStorage>(storage.get()))
    swiftSourceStorage->addMacroDependency(macroModuleName, libraryPath,
                                           executablePath);
  else if (auto swiftInterfaceStorage =
               dyn_cast<SwiftInterfaceModuleDependenciesStorage>(storage.get()))
    swiftInterfaceStorage->addMacroDependency(macroModuleName, libraryPath,
                                              executablePath);
  else
    llvm_unreachable("Unexpected dependency kind");
}

bool ModuleDependencyInfo::isTestableImport(StringRef moduleName) const {
  if (auto swiftSourceDepStorage = getAsSwiftSourceModule())
    return swiftSourceDepStorage->testableImports.contains(moduleName);
  else
    return false;
}

void ModuleDependencyInfo::addModuleDependency(ModuleDependencyID dependencyID) {
  storage->resolvedDirectModuleDependencies.push_back(dependencyID);
}

void ModuleDependencyInfo::addOptionalModuleImport(
    StringRef module, llvm::StringSet<> *alreadyAddedModules) {
  if (!alreadyAddedModules || alreadyAddedModules->insert(module).second)
    storage->optionalModuleImports.push_back(module.str());
}

void ModuleDependencyInfo::addModuleImport(
    StringRef module, llvm::StringSet<> *alreadyAddedModules,
    const SourceManager *sourceManager, SourceLoc sourceLocation) {
  auto scannerImportLocToDiagnosticLocInfo =
      [&sourceManager](SourceLoc sourceLocation) {
        auto lineAndColumnNumbers =
            sourceManager->getLineAndColumnInBuffer(sourceLocation);
        return ScannerImportStatementInfo::ImportDiagnosticLocationInfo(
            sourceManager->getDisplayNameForLoc(sourceLocation).str(),
            lineAndColumnNumbers.first, lineAndColumnNumbers.second);
      };
  bool validSourceLocation = sourceManager && sourceLocation.isValid() &&
                             sourceManager->isOwning(sourceLocation);

  if (alreadyAddedModules && alreadyAddedModules->contains(module)) {
    if (validSourceLocation) {
      // Find a prior import of this module and add import location
      for (auto &existingImport : storage->moduleImports) {
        if (existingImport.importIdentifier == module) {
          existingImport.addImportLocation(
              scannerImportLocToDiagnosticLocInfo(sourceLocation));
          break;
        }
      }
    }
  } else {
    if (alreadyAddedModules)
      alreadyAddedModules->insert(module);

    if (validSourceLocation)
      storage->moduleImports.push_back(ScannerImportStatementInfo(
          module.str(), scannerImportLocToDiagnosticLocInfo(sourceLocation)));
    else
      storage->moduleImports.push_back(
          ScannerImportStatementInfo(module.str()));
  }
}

void ModuleDependencyInfo::addModuleImport(
    ImportPath::Module module, llvm::StringSet<> *alreadyAddedModules,
    const SourceManager *sourceManager, SourceLoc sourceLocation) {
  std::string ImportedModuleName = module.front().Item.str().str();
  auto submodulePath = module.getSubmodulePath();
  if (submodulePath.size() > 0 && !submodulePath[0].Item.empty()) {
    auto submoduleComponent = submodulePath[0];
    // Special case: a submodule named "Foo.Private" can be moved to a top-level
    // module named "Foo_Private". ClangImporter has special support for this.
    if (submoduleComponent.Item.str() == "Private")
      addOptionalModuleImport(ImportedModuleName + "_Private",
                              alreadyAddedModules);
  }

  addModuleImport(ImportedModuleName, alreadyAddedModules,
                  sourceManager, sourceLocation);
}

void ModuleDependencyInfo::addModuleImports(
    const SourceFile &sourceFile, llvm::StringSet<> &alreadyAddedModules,
    const SourceManager *sourceManager) {
  // Add all of the module dependencies.
  SmallVector<Decl *, 32> decls;
  sourceFile.getTopLevelDecls(decls);
  for (auto decl : decls) {
    if (auto importDecl = dyn_cast<ImportDecl>(decl)) {
      ImportPath::Builder scratch;
      auto realPath = importDecl->getRealModulePath(scratch);

      // Explicit 'Builtin' import is not a part of the module's
      // dependency set, does not exist on the filesystem,
      // and is resolved within the compiler during compilation.
      SmallString<64> importedModuleName;
      realPath.getString(importedModuleName);
      if (importedModuleName == BUILTIN_NAME)
        continue;

      // Ignore/diagnose tautological imports akin to import resolution
      if (!swift::dependencies::checkImportNotTautological(
              realPath, importDecl->getLoc(), sourceFile,
              importDecl->isExported()))
        continue;

      addModuleImport(realPath, &alreadyAddedModules, sourceManager,
                      importDecl->getLoc());

      // Additionally, keep track of which dependencies of a Source
      // module are `@Testable`.
      if (getKind() == swift::ModuleDependencyKind::SwiftSource &&
          importDecl->isTestable())
        addTestableImport(realPath);
    } else if (auto macroDecl = dyn_cast<MacroDecl>(decl)) {
      auto macroDef = macroDecl->getDefinition();
      auto &ctx = macroDecl->getASTContext();
      if (macroDef.kind != MacroDefinition::Kind::External)
        continue;
      auto external = macroDef.getExternalMacro();
      PluginLoader &loader = ctx.getPluginLoader();
      auto &entry = loader.lookupPluginByModuleName(external.moduleName);
      if (entry.libraryPath.empty() && entry.executablePath.empty())
        continue;
      addMacroDependency(external.moduleName.str(), entry.libraryPath,
                         entry.executablePath);
    }
  }

  auto fileName = sourceFile.getFilename();
  if (fileName.empty())
    return;

  switch (getKind()) {
  case swift::ModuleDependencyKind::SwiftInterface: {
    // If the storage is for an interface file, the only source file we
    // should see is that interface file.
    assert(fileName ==
           cast<SwiftInterfaceModuleDependenciesStorage>(storage.get())->swiftInterfaceFile);
    break;
  }
  case swift::ModuleDependencyKind::SwiftSource: {
    // Otherwise, record the source file.
    auto swiftSourceStorage =
        cast<SwiftSourceModuleDependenciesStorage>(storage.get());
    swiftSourceStorage->sourceFiles.push_back(fileName.str());
    break;
  }
  default:
    llvm_unreachable("Unexpected dependency kind");
  }
}

std::optional<std::string> ModuleDependencyInfo::getBridgingHeader() const {
  switch (getKind()) {
  case swift::ModuleDependencyKind::SwiftInterface: {
    auto swiftInterfaceStorage =
        cast<SwiftInterfaceModuleDependenciesStorage>(storage.get());
    return swiftInterfaceStorage->textualModuleDetails.bridgingHeaderFile;
  }
  case swift::ModuleDependencyKind::SwiftSource: {
    auto swiftSourceStorage =
        cast<SwiftSourceModuleDependenciesStorage>(storage.get());
    return swiftSourceStorage->textualModuleDetails.bridgingHeaderFile;
  }
  default:
    return std::nullopt;
  }
}

std::optional<std::string> ModuleDependencyInfo::getCASFSRootID() const {
  std::string Root;
  switch (getKind()) {
  case swift::ModuleDependencyKind::SwiftInterface: {
    auto swiftInterfaceStorage =
        cast<SwiftInterfaceModuleDependenciesStorage>(storage.get());
    Root = swiftInterfaceStorage->textualModuleDetails.CASFileSystemRootID;
    break;
  }
  case swift::ModuleDependencyKind::SwiftSource: {
    auto swiftSourceStorage =
        cast<SwiftSourceModuleDependenciesStorage>(storage.get());
    Root = swiftSourceStorage->textualModuleDetails.CASFileSystemRootID;
    break;
  }
  case swift::ModuleDependencyKind::Clang: {
    auto clangModuleStorage = cast<ClangModuleDependencyStorage>(storage.get());
    Root = clangModuleStorage->CASFileSystemRootID;
    break;
  }
  default:
    return std::nullopt;
  }
  if (Root.empty())
    return std::nullopt;

  return Root;
}

std::optional<std::string> ModuleDependencyInfo::getClangIncludeTree() const {
  std::string Root;
  switch (getKind()) {
  case swift::ModuleDependencyKind::Clang: {
    auto clangModuleStorage = cast<ClangModuleDependencyStorage>(storage.get());
    Root = clangModuleStorage->CASClangIncludeTreeRootID;
    break;
  }
  default:
    return std::nullopt;
  }
  if (Root.empty())
    return std::nullopt;

  return Root;
}

std::optional<std::string>
ModuleDependencyInfo::getBridgingHeaderIncludeTree() const {
  std::string Root;
  switch (getKind()) {
  case swift::ModuleDependencyKind::SwiftInterface: {
    auto swiftInterfaceStorage =
        cast<SwiftInterfaceModuleDependenciesStorage>(storage.get());
    Root = swiftInterfaceStorage->textualModuleDetails
               .CASBridgingHeaderIncludeTreeRootID;
    break;
  }
  case swift::ModuleDependencyKind::SwiftSource: {
    auto swiftSourceStorage =
        cast<SwiftSourceModuleDependenciesStorage>(storage.get());
    Root = swiftSourceStorage->textualModuleDetails
               .CASBridgingHeaderIncludeTreeRootID;
    break;
  }
  default:
    return std::nullopt;
  }
  if (Root.empty())
    return std::nullopt;

  return Root;
}

std::string ModuleDependencyInfo::getModuleOutputPath() const {
  switch (getKind()) {
  case swift::ModuleDependencyKind::SwiftInterface: {
    auto swiftInterfaceStorage =
        cast<SwiftInterfaceModuleDependenciesStorage>(storage.get());
    return swiftInterfaceStorage->moduleOutputPath;
  }
  case swift::ModuleDependencyKind::SwiftSource: {
    return "<swiftmodule>";
  }
  case swift::ModuleDependencyKind::Clang: {
    auto clangModuleStorage = cast<ClangModuleDependencyStorage>(storage.get());
    return clangModuleStorage->pcmOutputPath;
  }
  case swift::ModuleDependencyKind::SwiftBinary: {
    auto swiftBinaryStorage =
        cast<SwiftBinaryModuleDependencyStorage>(storage.get());
    return swiftBinaryStorage->compiledModulePath;
  }
  case swift::ModuleDependencyKind::SwiftPlaceholder: {
    auto swiftPlaceholderStorage =
        cast<SwiftPlaceholderModuleDependencyStorage>(storage.get());
    return swiftPlaceholderStorage->compiledModulePath;
  }
  default:
    llvm_unreachable("Unexpected dependency kind");
  }
}

void ModuleDependencyInfo::addBridgingHeader(StringRef bridgingHeader) {
  switch (getKind()) {
  case swift::ModuleDependencyKind::SwiftInterface: {
    auto swiftInterfaceStorage =
        cast<SwiftInterfaceModuleDependenciesStorage>(storage.get());
    assert(!swiftInterfaceStorage->textualModuleDetails.bridgingHeaderFile);
    swiftInterfaceStorage->textualModuleDetails.bridgingHeaderFile = bridgingHeader.str();
    break;
  }
  case swift::ModuleDependencyKind::SwiftSource: {
    auto swiftSourceStorage =
        cast<SwiftSourceModuleDependenciesStorage>(storage.get());
    assert(!swiftSourceStorage->textualModuleDetails.bridgingHeaderFile);
    swiftSourceStorage->textualModuleDetails.bridgingHeaderFile = bridgingHeader.str();
    break;
  }
  default:
    llvm_unreachable("Unexpected dependency kind");
  }
}

/// Add source files that the bridging header depends on.
void ModuleDependencyInfo::addHeaderSourceFile(StringRef bridgingSourceFile) {
  switch (getKind()) {
  case swift::ModuleDependencyKind::SwiftInterface: {
    auto swiftInterfaceStorage =
        cast<SwiftInterfaceModuleDependenciesStorage>(storage.get());
    swiftInterfaceStorage->textualModuleDetails.bridgingSourceFiles.push_back(
        bridgingSourceFile.str());
    break;
  }
  case swift::ModuleDependencyKind::SwiftSource: {
    auto swiftSourceStorage =
        cast<SwiftSourceModuleDependenciesStorage>(storage.get());
    swiftSourceStorage->textualModuleDetails.bridgingSourceFiles.push_back(
        bridgingSourceFile.str());
    break;
  }
  case swift::ModuleDependencyKind::SwiftBinary: {
    auto swiftBinaryStorage =
        cast<SwiftBinaryModuleDependencyStorage>(storage.get());
    swiftBinaryStorage->headerSourceFiles.push_back(bridgingSourceFile.str());
    break;
  }
  default:
    llvm_unreachable("Unexpected dependency kind");
  }
}

void ModuleDependencyInfo::addBridgingHeaderIncludeTree(StringRef ID) {
  switch (getKind()) {
  case swift::ModuleDependencyKind::SwiftInterface: {
    auto swiftInterfaceStorage =
        cast<SwiftInterfaceModuleDependenciesStorage>(storage.get());
    swiftInterfaceStorage->textualModuleDetails
        .CASBridgingHeaderIncludeTreeRootID = ID.str();
    break;
  }
  case swift::ModuleDependencyKind::SwiftSource: {
    auto swiftSourceStorage =
        cast<SwiftSourceModuleDependenciesStorage>(storage.get());
    swiftSourceStorage->textualModuleDetails
        .CASBridgingHeaderIncludeTreeRootID = ID.str();
    break;
  }
  default:
    llvm_unreachable("Unexpected dependency kind");
  }
}

void ModuleDependencyInfo::addSourceFile(StringRef sourceFile) {
  switch (getKind()) {
  case swift::ModuleDependencyKind::SwiftSource: {
    auto swiftSourceStorage =
        cast<SwiftSourceModuleDependenciesStorage>(storage.get());
    swiftSourceStorage->sourceFiles.push_back(sourceFile.str());
    break;
  }
  default:
    llvm_unreachable("Unexpected dependency kind");
  }
}

/// Add (Clang) module on which the bridging header depends.
void ModuleDependencyInfo::addHeaderInputModuleDependency(
    StringRef module, llvm::StringSet<> &alreadyAddedModules) {
  switch (getKind()) {
  case swift::ModuleDependencyKind::SwiftInterface: {
    auto swiftInterfaceStorage =
        cast<SwiftInterfaceModuleDependenciesStorage>(storage.get());
    if (alreadyAddedModules.insert(module).second)
      swiftInterfaceStorage->textualModuleDetails.bridgingModuleDependencies
          .push_back(module.str());
    break;
  }
  case swift::ModuleDependencyKind::SwiftSource: {
    auto swiftSourceStorage =
        cast<SwiftSourceModuleDependenciesStorage>(storage.get());
    if (alreadyAddedModules.insert(module).second)
      swiftSourceStorage->textualModuleDetails.bridgingModuleDependencies
          .push_back(module.str());
    break;
  }
  case swift::ModuleDependencyKind::SwiftBinary: {
    auto swiftBinaryStorage =
        cast<SwiftBinaryModuleDependencyStorage>(storage.get());
    if (alreadyAddedModules.insert(module).second)
      swiftBinaryStorage->headerModuleDependencies.push_back(module.str());
    break;
  }
  default:
    llvm_unreachable("Unexpected dependency kind");
  }
}

SwiftDependencyScanningService::SwiftDependencyScanningService() {
  ClangScanningService.emplace(
      clang::tooling::dependencies::ScanningMode::DependencyDirectivesScan,
      clang::tooling::dependencies::ScanningOutputFormat::FullTree,
      clang::CASOptions(),
      /* CAS (llvm::cas::ObjectStore) */ nullptr,
      /* Cache (llvm::cas::ActionCache) */ nullptr,
      /* SharedFS */ nullptr);
}

bool
swift::dependencies::checkImportNotTautological(const ImportPath::Module modulePath,
                                                const SourceLoc importLoc,
                                                const SourceFile &SF,
                                                bool isExported) {
  if (modulePath.front().Item != SF.getParentModule()->getName() ||
      // Overlays use an @_exported self-import to load their clang module.
      isExported ||
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

void
swift::dependencies::registerCxxInteropLibraries(
    const llvm::Triple &Target,
    StringRef mainModuleName,
    bool hasStaticCxx, bool hasStaticCxxStdlib,
    std::function<void(const LinkLibrary&)> RegistrationCallback) {
  if (Target.isOSDarwin())
    RegistrationCallback(LinkLibrary("c++", LibraryKind::Library));
  else if (Target.isOSLinux())
    RegistrationCallback(LinkLibrary("stdc++", LibraryKind::Library));

  // Do not try to link Cxx with itself.
  if (mainModuleName != "Cxx") {
    RegistrationCallback(LinkLibrary(Target.isOSWindows() && hasStaticCxx
                                        ? "libswiftCxx"
                                        : "swiftCxx",
                                     LibraryKind::Library));
  }

  // Do not try to link CxxStdlib with the C++ standard library, Cxx or
  // itself.
  if (llvm::none_of(llvm::ArrayRef{"Cxx", "CxxStdlib", "std"},
                    [mainModuleName](StringRef Name) {
                      return mainModuleName == Name;
                    })) {
    // Only link with CxxStdlib on platforms where the overlay is available.
    switch (Target.getOS()) {
    case llvm::Triple::Win32: {
      RegistrationCallback(
          LinkLibrary(hasStaticCxxStdlib ? "libswiftCxxStdlib" : "swiftCxxStdlib",
                      LibraryKind::Library));
      break;
    }
    default:
      if (Target.isOSDarwin() || Target.isOSLinux())
        RegistrationCallback(LinkLibrary("swiftCxxStdlib",
                                         LibraryKind::Library));
      break;
    }
  }
}

void
swift::dependencies::registerBackDeployLibraries(
    const IRGenOptions &IRGenOpts,
    std::function<void(const LinkLibrary&)> RegistrationCallback) {
  auto addBackDeployLib = [&](llvm::VersionTuple version,
                              StringRef libraryName, bool forceLoad) {
    std::optional<llvm::VersionTuple> compatibilityVersion;
    if (libraryName == "swiftCompatibilityDynamicReplacements") {
      compatibilityVersion = IRGenOpts.
          AutolinkRuntimeCompatibilityDynamicReplacementLibraryVersion;
    } else if (libraryName == "swiftCompatibilityConcurrency") {
      compatibilityVersion =
          IRGenOpts.AutolinkRuntimeCompatibilityConcurrencyLibraryVersion;
    } else {
      compatibilityVersion = IRGenOpts.
          AutolinkRuntimeCompatibilityLibraryVersion;
    }

    if (!compatibilityVersion)
      return;

    if (*compatibilityVersion > version)
      return;

    RegistrationCallback({libraryName, LibraryKind::Library, forceLoad});
  };

#define BACK_DEPLOYMENT_LIB(Version, Filter, LibraryName, ForceLoad) \
    addBackDeployLib(llvm::VersionTuple Version, LibraryName, ForceLoad);
  #include "swift/Frontend/BackDeploymentLibs.def"
}

void SwiftDependencyTracker::addCommonSearchPathDeps(
    const SearchPathOptions &Opts) {
  // Add SDKSetting file.
  SmallString<256> SDKSettingPath;
  llvm::sys::path::append(SDKSettingPath, Opts.getSDKPath(),
                          "SDKSettings.json");
  FS->status(SDKSettingPath);

  // Add Legacy layout file.
  const std::vector<std::string> AllSupportedArches = {
      "arm64", "arm64e", "x86_64", "i386",
      "armv7", "armv7s", "armv7k", "arm64_32"};

  for (auto RuntimeLibPath : Opts.RuntimeLibraryPaths) {
    std::error_code EC;
    for (auto &Arch : AllSupportedArches) {
      SmallString<256> LayoutFile(RuntimeLibPath);
      llvm::sys::path::append(LayoutFile, "layouts-" + Arch + ".yaml");
      FS->status(LayoutFile);
    }
  }

  // Add VFSOverlay file.
  for (auto &Overlay: Opts.VFSOverlayFiles)
    FS->status(Overlay);
}

void SwiftDependencyTracker::startTracking() {
  FS->trackNewAccesses();
}

llvm::Expected<llvm::cas::ObjectProxy>
SwiftDependencyTracker::createTreeFromDependencies() {
  return FS->createTreeFromNewAccesses(
      [&](const llvm::vfs::CachedDirectoryEntry &Entry,
          llvm::SmallVectorImpl<char> &Storage) {
        if (Mapper)
          return Mapper->mapDirEntry(Entry, Storage);
        return Entry.getTreePath();
      });
}

bool SwiftDependencyScanningService::setupCachingDependencyScanningService(
    CompilerInstance &Instance) {
  if (!Instance.getInvocation().getCASOptions().EnableCaching)
    return false;

  if (CASOpts) {
    // If CASOption matches, the service is initialized already.
    if (*CASOpts == Instance.getInvocation().getCASOptions().CASOpts)
      return false;

    // CASOption mismatch, return error.
    Instance.getDiags().diagnose(
        SourceLoc(), diag::error_cas,
        "conflicting CAS options used in scanning service");
    return true;
  }

  // Setup CAS.
  CASOpts = Instance.getInvocation().getCASOptions().CASOpts;
  CAS = Instance.getSharedCASInstance();

  // Add SDKSetting file.
  SmallString<256> SDKSettingPath;
  llvm::sys::path::append(
      SDKSettingPath,
      Instance.getInvocation().getSearchPathOptions().getSDKPath(),
      "SDKSettings.json");
  CommonDependencyFiles.emplace_back(SDKSettingPath.data(),
                                     SDKSettingPath.size());

  // Add Legacy layout file (maybe just hard code instead of searching).
  for (auto RuntimeLibPath :
       Instance.getInvocation().getSearchPathOptions().RuntimeLibraryPaths) {
    auto &FS = Instance.getFileSystem();
    std::error_code EC;
    for (auto F = FS.dir_begin(RuntimeLibPath, EC);
         !EC && F != llvm::vfs::directory_iterator(); F.increment(EC)) {
      if (F->path().ends_with(".yaml"))
        CommonDependencyFiles.emplace_back(F->path().str());
    }
  }

  auto CachingFS =
      llvm::cas::createCachingOnDiskFileSystem(Instance.getObjectStore());
  if (!CachingFS) {
    Instance.getDiags().diagnose(SourceLoc(), diag::error_cas,
                                 toString(CachingFS.takeError()));
    return true;
  }
  CacheFS = std::move(*CachingFS);

  // Setup prefix mapping.
  Mapper = std::make_unique<llvm::TreePathPrefixMapper>(CacheFS);
  SmallVector<llvm::MappedPrefix, 4> Prefixes;
  if (auto E = llvm::MappedPrefix::transformJoined(
          Instance.getInvocation().getSearchPathOptions().ScannerPrefixMapper,
          Prefixes)) {
    Instance.getDiags().diagnose(SourceLoc(), diag::error_prefix_mapping,
                                 toString(std::move(E)));
    return true;
  }
  Mapper->addRange(Prefixes);
  Mapper->sort();

  UseClangIncludeTree =
      Instance.getInvocation().getClangImporterOptions().UseClangIncludeTree;
  const clang::tooling::dependencies::ScanningOutputFormat ClangScanningFormat =
      UseClangIncludeTree
          ? clang::tooling::dependencies::ScanningOutputFormat::FullIncludeTree
          : clang::tooling::dependencies::ScanningOutputFormat::FullTree;

  ClangScanningService.emplace(
      clang::tooling::dependencies::ScanningMode::DependencyDirectivesScan,
      ClangScanningFormat,
      Instance.getInvocation().getCASOptions().CASOpts,
      Instance.getSharedCASInstance(), Instance.getSharedCacheInstance(),
      UseClangIncludeTree ? nullptr : CacheFS);

  return false;
}

SwiftDependencyScanningService::ContextSpecificGlobalCacheState *
SwiftDependencyScanningService::getCacheForScanningContextHash(StringRef scanningContextHash) const {
  auto contextSpecificCache = ContextSpecificCacheMap.find(scanningContextHash);
  assert(contextSpecificCache != ContextSpecificCacheMap.end() &&
         "Global Module Dependencies Cache not configured with context-specific "
         "state.");
  return contextSpecificCache->getValue().get();
}

const ModuleNameToDependencyMap &
SwiftDependencyScanningService::getDependenciesMap(
    ModuleDependencyKind kind, StringRef scanContextHash) const {
  auto contextSpecificCache = getCacheForScanningContextHash(scanContextHash);
  auto it = contextSpecificCache->ModuleDependenciesMap.find(kind);
  assert(it != contextSpecificCache->ModuleDependenciesMap.end() &&
         "invalid dependency kind");
  return it->second;
}

ModuleNameToDependencyMap &
SwiftDependencyScanningService::getDependenciesMap(
    ModuleDependencyKind kind, StringRef scanContextHash) {
  llvm::sys::SmartScopedLock<true> Lock(ScanningServiceGlobalLock);
  auto contextSpecificCache = getCacheForScanningContextHash(scanContextHash);
  auto it = contextSpecificCache->ModuleDependenciesMap.find(kind);
  assert(it != contextSpecificCache->ModuleDependenciesMap.end() &&
         "invalid dependency kind");
  return it->second;
}

void SwiftDependencyScanningService::configureForContextHash(StringRef scanningContextHash) {
  llvm::sys::SmartScopedLock<true> Lock(ScanningServiceGlobalLock);
  auto knownContext = ContextSpecificCacheMap.find(scanningContextHash);
  if (knownContext == ContextSpecificCacheMap.end()) {
    // First time scanning with this context, initialize context-specific state.
    std::unique_ptr<ContextSpecificGlobalCacheState> contextSpecificCache =
        std::make_unique<ContextSpecificGlobalCacheState>();
    for (auto kind = ModuleDependencyKind::FirstKind;
         kind != ModuleDependencyKind::LastKind; ++kind) {
      contextSpecificCache->ModuleDependenciesMap.insert({kind, ModuleNameToDependencyMap()});
    }
    ContextSpecificCacheMap.insert({scanningContextHash.str(), std::move(contextSpecificCache)});
    AllContextHashes.push_back(scanningContextHash.str());
  }
}

std::optional<const ModuleDependencyInfo *>
SwiftDependencyScanningService::findDependency(
    StringRef moduleName, std::optional<ModuleDependencyKind> kind,
    StringRef scanningContextHash) const {
  if (!kind) {
    for (auto kind = ModuleDependencyKind::FirstKind;
         kind != ModuleDependencyKind::LastKind; ++kind) {
      auto dep = findDependency(moduleName, kind, scanningContextHash);
      if (dep.has_value())
        return dep.value();
    }
    return std::nullopt;
  }

  assert(kind.has_value() && "Expected dependencies kind for lookup.");
  const auto &map = getDependenciesMap(kind.value(), scanningContextHash);
  auto known = map.find(moduleName);
  if (known != map.end())
    return &(known->second);

  return std::nullopt;
}

bool SwiftDependencyScanningService::hasDependency(
    StringRef moduleName, std::optional<ModuleDependencyKind> kind,
    StringRef scanContextHash) const {
  return findDependency(moduleName, kind, scanContextHash).has_value();
}

const ModuleDependencyInfo *SwiftDependencyScanningService::recordDependency(
    StringRef moduleName, ModuleDependencyInfo dependencies,
    StringRef scanContextHash) {
  auto kind = dependencies.getKind();
  auto &map = getDependenciesMap(kind, scanContextHash);
  map.insert({moduleName, dependencies});
  return &(map[moduleName]);
}

const ModuleDependencyInfo *SwiftDependencyScanningService::updateDependency(
    ModuleDependencyID moduleID, ModuleDependencyInfo dependencies,
    StringRef scanningContextHash) {
  auto &map = getDependenciesMap(moduleID.Kind, scanningContextHash);
  auto known = map.find(moduleID.ModuleName);
  assert(known != map.end() && "Not yet added to map");
  known->second = std::move(dependencies);
  return &(known->second);
}

llvm::StringMap<const ModuleDependencyInfo *> &
ModuleDependenciesCache::getDependencyReferencesMap(
    ModuleDependencyKind kind) {
  auto it = ModuleDependenciesMap.find(kind);
  assert(it != ModuleDependenciesMap.end() && "invalid dependency kind");
  return it->second;
}

const llvm::StringMap<const ModuleDependencyInfo *> &
ModuleDependenciesCache::getDependencyReferencesMap(
    ModuleDependencyKind kind) const {
  auto it = ModuleDependenciesMap.find(kind);
  assert(it != ModuleDependenciesMap.end() && "invalid dependency kind");
  return it->second;
}

ModuleDependenciesCache::ModuleDependenciesCache(
    SwiftDependencyScanningService &globalScanningService,
    std::string mainScanModuleName, std::string moduleOutputPath,
    std::string scannerContextHash)
    : globalScanningService(globalScanningService),
      mainScanModuleName(mainScanModuleName),
      scannerContextHash(scannerContextHash),
      moduleOutputPath(moduleOutputPath) {
  globalScanningService.configureForContextHash(scannerContextHash);
  for (auto kind = ModuleDependencyKind::FirstKind;
       kind != ModuleDependencyKind::LastKind; ++kind) {
    ModuleDependenciesMap.insert(
        {kind, llvm::StringMap<const ModuleDependencyInfo *>()});
  }
}

std::optional<const ModuleDependencyInfo *>
ModuleDependenciesCache::findDependency(
    const ModuleDependencyID moduleID) const {
  return findDependency(moduleID.ModuleName, moduleID.Kind);
}

std::optional<const ModuleDependencyInfo *>
ModuleDependenciesCache::findDependency(
    StringRef moduleName, std::optional<ModuleDependencyKind> kind) const {
  auto optionalDep = globalScanningService.findDependency(moduleName, kind,
                                                          scannerContextHash);
  // During a scan, only produce the cached source module info for the current
  // module under scan.
  if (optionalDep) {
    auto dep = *optionalDep;
    if (dep->getAsSwiftSourceModule() &&
        moduleName != mainScanModuleName &&
        moduleName != "DummyMainModuleForResolvingCrossImportOverlays") {
      return std::nullopt;
    }
  }

  return optionalDep;
}

std::optional<const ModuleDependencyInfo *>
ModuleDependenciesCache::findDependency(StringRef moduleName) const {
  for (auto kind = ModuleDependencyKind::FirstKind;
       kind != ModuleDependencyKind::LastKind; ++kind) {
    if (auto found = findDependency(moduleName, kind))
      return found;
  }
  return std::nullopt;
}

const ModuleDependencyInfo &ModuleDependenciesCache::findKnownDependency(
    const ModuleDependencyID &moduleID) const {
  auto dep = findDependency(moduleID);
  assert(dep && "dependency unknown");
  return **dep;
}

bool ModuleDependenciesCache::hasDependency(const ModuleDependencyID &moduleID) const {
  return hasDependency(moduleID.ModuleName, moduleID.Kind);
}

bool ModuleDependenciesCache::hasDependency(
    StringRef moduleName, std::optional<ModuleDependencyKind> kind) const {
  return findDependency(moduleName, kind).has_value();
}

bool ModuleDependenciesCache::hasDependency(StringRef moduleName) const {
  for (auto kind = ModuleDependencyKind::FirstKind;
       kind != ModuleDependencyKind::LastKind; ++kind) {
    if (findDependency(moduleName, kind).has_value())
      return true;
  }
  return false;
}

void ModuleDependenciesCache::recordDependency(
    StringRef moduleName, ModuleDependencyInfo dependencies) {
  auto dependenciesKind = dependencies.getKind();
  const ModuleDependencyInfo *recordedDependencies =
        globalScanningService.recordDependency(moduleName, dependencies,
                                               scannerContextHash);
  auto &map = getDependencyReferencesMap(dependenciesKind);
  assert(map.count(moduleName) == 0 && "Already added to map");
  map.insert({moduleName, recordedDependencies});
}

void ModuleDependenciesCache::recordDependencies(
    ModuleDependencyVector moduleDependencies) {
  for (const auto &dep : moduleDependencies) {
    if (!hasDependency(dep.first))
      recordDependency(dep.first.ModuleName, dep.second);
    if (dep.second.getKind() == ModuleDependencyKind::Clang) {
      auto clangModuleDetails = dep.second.getAsClangModule();
      addSeenClangModule(clang::tooling::dependencies::ModuleID{
          dep.first.ModuleName, clangModuleDetails->contextHash});
    }
  }
}

void ModuleDependenciesCache::updateDependency(
    ModuleDependencyID moduleID, ModuleDependencyInfo dependencyInfo) {
  const ModuleDependencyInfo *updatedDependencies =
    globalScanningService.updateDependency(moduleID, dependencyInfo,
                                           scannerContextHash);
  auto &map = getDependencyReferencesMap(moduleID.Kind);
  auto known = map.find(moduleID.ModuleName);
  if (known != map.end())
    map.erase(known);
  map.insert({moduleID.ModuleName, updatedDependencies});
}

void ModuleDependenciesCache::resolveDependencyImports(ModuleDependencyID moduleID,
                                                       const ArrayRef<ModuleDependencyID> dependencyIDs) {
  auto optionalDependencyInfo = findDependency(moduleID);
  assert(optionalDependencyInfo.has_value() && "Resolving unknown dependency");
  // Copy the existing info to a mutable one we can then replace it with, after resolving its dependencies.
  auto dependencyInfo = *(optionalDependencyInfo.value());
  dependencyInfo.resolveDirectDependencies(dependencyIDs);
  updateDependency(moduleID, dependencyInfo);
}

void ModuleDependenciesCache::setSwiftOverlayDependencies(ModuleDependencyID moduleID,
                                                          const ArrayRef<ModuleDependencyID> dependencyIDs) {
  auto optionalDependencyInfo = findDependency(moduleID);
  assert(optionalDependencyInfo.has_value() && "Resolving unknown dependency");
  // Copy the existing info to a mutable one we can then replace it with, after setting its overlay dependencies.
  auto dependencyInfo = *(optionalDependencyInfo.value());
  dependencyInfo.setOverlayDependencies(dependencyIDs);
  updateDependency(moduleID, dependencyInfo);
}

std::vector<ModuleDependencyID>
ModuleDependenciesCache::getAllDependencies(const ModuleDependencyID &moduleID) const {
  const auto &moduleInfo = findDependency(moduleID);
  assert(moduleInfo.has_value());
  auto directDependenciesRef =
      moduleInfo.value()->getDirectModuleDependencies();
  auto overlayDependenciesRef =
      moduleInfo.value()->getSwiftOverlayDependencies();
  std::vector<ModuleDependencyID> result;
  result.insert(std::end(result), directDependenciesRef.begin(),
                directDependenciesRef.end());
  result.insert(std::end(result), overlayDependenciesRef.begin(),
                overlayDependenciesRef.end());
  return result;
}

ArrayRef<ModuleDependencyID>
ModuleDependenciesCache::getOnlyOverlayDependencies(const ModuleDependencyID &moduleID) const {
  const auto &moduleInfo = findDependency(moduleID);
  assert(moduleInfo.has_value());
  return moduleInfo.value()->getSwiftOverlayDependencies();
}

ArrayRef<ModuleDependencyID>
ModuleDependenciesCache::getOnlyDirectDependencies(const ModuleDependencyID &moduleID) const {
  const auto &moduleInfo = findDependency(moduleID);
  assert(moduleInfo.has_value());
  return moduleInfo.value()->getDirectModuleDependencies();
}
