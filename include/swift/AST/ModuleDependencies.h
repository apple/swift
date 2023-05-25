//===--- ModuleDependencies.h - Module Dependencies -------------*- C++ -*-===//
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
// This file defines data structures for capturing all of the source files
// and modules on which a given module depends, forming a graph of all of the
// modules that need to be present for a given module to be built.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_AST_MODULE_DEPENDENCIES_H
#define SWIFT_AST_MODULE_DEPENDENCIES_H

#include "swift/Basic/LLVM.h"
#include "swift/AST/Import.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningService.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningTool.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Mutex.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace swift {

class ClangModuleDependenciesCacheImpl;
class SourceFile;
class ASTContext;
class Identifier;
class CompilerInstance;

/// Which kind of module dependencies we are looking for.
enum class ModuleDependencyKind : int8_t {
  FirstKind,
  // Textual Swift dependency
  SwiftInterface = FirstKind,
  // Binary module Swift dependency
  SwiftBinary,
  // Clang module dependency
  Clang,
  // Used to model the translation unit's source module
  SwiftSource,
  // Placeholder dependencies are a kind of dependencies used only by the
  // dependency scanner. They are swift modules that the scanner will not be
  // able to locate in its search paths and which are the responsibility of the
  // scanner's client to ensure are provided.
  //
  // Placeholder dependencies will be specified in the scanner's output
  // dependency graph where it is the responsibility of the scanner's client to
  // ensure required post-processing takes place to "resolve" them. In order to
  // do so, the client (swift driver, or any other client build system) is
  // expected to have access to a full dependency graph of all placeholder
  // dependencies and be able to replace placeholder nodes in the dependency
  // graph with their full dependency trees, `uniquing` common dependency module
  // nodes in the process.
  //
  // One example where placeholder dependencies are employed is when using
  // SwiftPM in Explicit Module Build mode. SwiftPM constructs a build plan for
  // all targets ahead-of-time. When planning a build for a target that depends
  // on other targets, the dependency scanning action is not able to locate
  // dependency target modules, because they have not yet been built. Instead,
  // the build system treats them as placeholder dependencies and resolves them
  // with `actual` dependencies in a post-processing step once dependency graphs
  // of all targets, individually, have been computed.
  SwiftPlaceholder,
  LastKind = SwiftPlaceholder + 1
};

using ModuleDependencyID = std::pair<std::string, ModuleDependencyKind>;
using ModuleDependencyIDSetVector =
    llvm::SetVector<ModuleDependencyID, std::vector<ModuleDependencyID>,
                    std::set<ModuleDependencyID>>;

struct ModuleDependencyKindHash {
  std::size_t operator()(ModuleDependencyKind k) const {
    using UnderlyingType = std::underlying_type<ModuleDependencyKind>::type;
    return std::hash<UnderlyingType>{}(static_cast<UnderlyingType>(k));
  }
};

namespace dependencies {
  std::string createEncodedModuleKindAndName(ModuleDependencyID id);
}

/// Base class for the variant storage of ModuleDependencyInfo.
///
/// This class is mostly an implementation detail for \c ModuleDependencyInfo.
class ModuleDependencyInfoStorageBase {
public:
  const ModuleDependencyKind dependencyKind;

  ModuleDependencyInfoStorageBase(ModuleDependencyKind dependencyKind,
                                  bool resolved = false)
      : dependencyKind(dependencyKind), resolved(resolved) { }

  virtual ModuleDependencyInfoStorageBase *clone() const = 0;

  virtual ~ModuleDependencyInfoStorageBase();

  /// The set of modules on which this module depends.
  std::vector<std::string> moduleImports;

  /// The set of modules which constitute optional module
  /// dependencies for this module, such as `@_implementationOnly`
  /// or `internal` imports.
  std::vector<std::string> optionalModuleImports;

  /// The set of modules on which this module depends, resolved
  /// to Module IDs, qualified by module kind: Swift, Clang, etc.
  std::vector<ModuleDependencyID> resolvedModuleDependencies;
  bool resolved;
};

struct CommonSwiftTextualModuleDependencyDetails {
  CommonSwiftTextualModuleDependencyDetails(ArrayRef<StringRef> extraPCMArgs)
     : extraPCMArgs(extraPCMArgs.begin(), extraPCMArgs.end()) {}

  /// To build a PCM to be used by this Swift module, we need to append these
  /// arguments to the generic PCM build arguments reported from the dependency
  /// graph.
  const std::vector<std::string> extraPCMArgs;

  /// Bridging header file, if there is one.
  Optional<std::string> bridgingHeaderFile;

  /// Source files on which the bridging header depends.
  std::vector<std::string> bridgingSourceFiles;

  /// (Clang) modules on which the bridging header depends.
  std::vector<std::string> bridgingModuleDependencies;

  /// Dependencies comprised of Swift overlay modules of direct and
  /// transitive Clang dependencies.
  std::vector<ModuleDependencyID> swiftOverlayDependencies;
};

/// Describes the dependencies of a Swift module described by an Swift interface file.
///
/// This class is mostly an implementation detail for \c ModuleDependencyInfo.
class SwiftInterfaceModuleDependenciesStorage :
  public ModuleDependencyInfoStorageBase {
public:
  /// Destination output path
  const std::string moduleOutputPath;

  /// The Swift interface file to be used to generate the module file.
  const std::string swiftInterfaceFile;

  /// Potentially ready-to-use compiled modules for the interface file.
  const std::vector<std::string> compiledModuleCandidates;

  /// The Swift frontend invocation arguments to build the Swift module from the
  /// interface.
  std::vector<std::string> buildCommandLine;

  /// The hash value that will be used for the generated module
  const std::string contextHash;

  /// A flag that indicates this dependency is a framework
  const bool isFramework;

  /// Details common to Swift textual (interface or source) modules
  CommonSwiftTextualModuleDependencyDetails textualModuleDetails;

  SwiftInterfaceModuleDependenciesStorage(
      const std::string moduleOutputPath,
      const std::string swiftInterfaceFile,
      ArrayRef<std::string> compiledModuleCandidates,
      ArrayRef<StringRef> buildCommandLine,
      ArrayRef<StringRef> extraPCMArgs,
      StringRef contextHash,
      bool isFramework
  ) : ModuleDependencyInfoStorageBase(ModuleDependencyKind::SwiftInterface),
      moduleOutputPath(moduleOutputPath),
      swiftInterfaceFile(swiftInterfaceFile),
      compiledModuleCandidates(compiledModuleCandidates.begin(),
                               compiledModuleCandidates.end()),
      buildCommandLine(buildCommandLine.begin(), buildCommandLine.end()),
      contextHash(contextHash), isFramework(isFramework),
      textualModuleDetails(extraPCMArgs)
      {}

  ModuleDependencyInfoStorageBase *clone() const override {
    return new SwiftInterfaceModuleDependenciesStorage(*this);
  }

  static bool classof(const ModuleDependencyInfoStorageBase *base) {
    return base->dependencyKind == ModuleDependencyKind::SwiftInterface;
  }

  void updateCommandLine(const std::vector<std::string> &newCommandLine) {
    buildCommandLine = newCommandLine;
  }
};

/// Describes the dependencies of a Swift module
///
/// This class is mostly an implementation detail for \c ModuleDependencyInfo.
class SwiftSourceModuleDependenciesStorage :
  public ModuleDependencyInfoStorageBase {
public:

  /// Swift source files that are part of the Swift module, when known.
  std::vector<std::string> sourceFiles;

  /// Details common to Swift textual (interface or source) modules
  CommonSwiftTextualModuleDependencyDetails textualModuleDetails;

  /// Collection of module imports that were detected to be `@Testable`
  llvm::StringSet<> testableImports;

  SwiftSourceModuleDependenciesStorage(
    ArrayRef<StringRef> extraPCMArgs
  ) : ModuleDependencyInfoStorageBase(ModuleDependencyKind::SwiftSource),
      textualModuleDetails(extraPCMArgs),
      testableImports(llvm::StringSet<>()) {}

  ModuleDependencyInfoStorageBase *clone() const override {
    return new SwiftSourceModuleDependenciesStorage(*this);
  }

  static bool classof(const ModuleDependencyInfoStorageBase *base) {
    return base->dependencyKind == ModuleDependencyKind::SwiftSource;
  }

  void addTestableImport(ImportPath::Module module) {
    testableImports.insert(module.front().Item.str());
  }
};

/// Describes the dependencies of a pre-built Swift module (with no .swiftinterface).
///
/// This class is mostly an implementation detail for \c ModuleDependencyInfo.
class SwiftBinaryModuleDependencyStorage : public ModuleDependencyInfoStorageBase {
public:
  SwiftBinaryModuleDependencyStorage(const std::string &compiledModulePath,
                                     const std::string &moduleDocPath,
                                     const std::string &sourceInfoPath,
                                     const bool isFramework)
      : ModuleDependencyInfoStorageBase(ModuleDependencyKind::SwiftBinary),
        compiledModulePath(compiledModulePath),
        moduleDocPath(moduleDocPath),
        sourceInfoPath(sourceInfoPath),
        isFramework(isFramework) {}

  ModuleDependencyInfoStorageBase *clone() const override {
    return new SwiftBinaryModuleDependencyStorage(*this);
  }

  /// The path to the .swiftmodule file.
  const std::string compiledModulePath;

  /// The path to the .swiftModuleDoc file.
  const std::string moduleDocPath;

  /// The path to the .swiftSourceInfo file.
  const std::string sourceInfoPath;

  /// A flag that indicates this dependency is a framework
  const bool isFramework;

  static bool classof(const ModuleDependencyInfoStorageBase *base) {
    return base->dependencyKind == ModuleDependencyKind::SwiftBinary;
  }
};

/// Describes the dependencies of a Clang module.
///
/// This class is mostly an implementation detail for \c ModuleDependencyInfo.
class ClangModuleDependencyStorage : public ModuleDependencyInfoStorageBase {
public:
  /// Destination output path
  const std::string pcmOutputPath;
  
  /// The module map file used to generate the Clang module.
  const std::string moduleMapFile;

  /// The context hash describing the configuration options for this module.
  const std::string contextHash;

  /// Partial (Clang) command line that can be used to build this module.
  const std::vector<std::string> nonPathCommandLine;

  /// The file dependencies
  const std::vector<std::string> fileDependencies;

  /// The swift-specific PCM arguments captured by this dependencies object
  /// as found by the scanning action that discovered it
  const std::vector<std::string> capturedPCMArgs;

  ClangModuleDependencyStorage(
      const std::string &pcmOutputPath,
      const std::string &moduleMapFile,
      const std::string &contextHash,
      const std::vector<std::string> &nonPathCommandLine,
      const std::vector<std::string> &fileDependencies,
      const std::vector<std::string> &capturedPCMArgs
  ) : ModuleDependencyInfoStorageBase(ModuleDependencyKind::Clang),
      pcmOutputPath(pcmOutputPath),
      moduleMapFile(moduleMapFile),
      contextHash(contextHash),
      nonPathCommandLine(nonPathCommandLine),
      fileDependencies(fileDependencies),
      capturedPCMArgs(capturedPCMArgs) {}

  ModuleDependencyInfoStorageBase *clone() const override {
    return new ClangModuleDependencyStorage(*this);
  }

  static bool classof(const ModuleDependencyInfoStorageBase *base) {
    return base->dependencyKind == ModuleDependencyKind::Clang;
  }
};

/// Describes an placeholder Swift module dependency module stub.
///
/// This class is mostly an implementation detail for \c ModuleDependencyInfo.

class SwiftPlaceholderModuleDependencyStorage : public ModuleDependencyInfoStorageBase {
public:
  SwiftPlaceholderModuleDependencyStorage(const std::string &compiledModulePath,
                                          const std::string &moduleDocPath,
                                          const std::string &sourceInfoPath)
      : ModuleDependencyInfoStorageBase(ModuleDependencyKind::SwiftPlaceholder),
        compiledModulePath(compiledModulePath),
        moduleDocPath(moduleDocPath),
        sourceInfoPath(sourceInfoPath) {}

  ModuleDependencyInfoStorageBase *clone() const override {
    return new SwiftPlaceholderModuleDependencyStorage(*this);
  }

  /// The path to the .swiftmodule file.
  const std::string compiledModulePath;

  /// The path to the .swiftModuleDoc file.
  const std::string moduleDocPath;

  /// The path to the .swiftSourceInfo file.
  const std::string sourceInfoPath;

  static bool classof(const ModuleDependencyInfoStorageBase *base) {
    return base->dependencyKind == ModuleDependencyKind::SwiftPlaceholder;
  }
};

// MARK: Module Dependency Info
/// Describes the dependencies of a given module.
///
/// The dependencies of a module include all of the source files that go
/// into that module, as well as any modules that are directly imported
/// into the module.
class ModuleDependencyInfo {
private:
  std::unique_ptr<ModuleDependencyInfoStorageBase> storage;

  ModuleDependencyInfo(std::unique_ptr<ModuleDependencyInfoStorageBase> &&storage)
    : storage(std::move(storage)) { }

public:
  ModuleDependencyInfo() = default;
  ModuleDependencyInfo(const ModuleDependencyInfo &other)
    : storage(other.storage->clone()) { }
  ModuleDependencyInfo(ModuleDependencyInfo &&other) = default;

  ModuleDependencyInfo &operator=(const ModuleDependencyInfo &other) {
    storage.reset(other.storage->clone());
    return *this;
  }

  ModuleDependencyInfo &operator=(ModuleDependencyInfo &&other) = default;

  /// Describe the module dependencies for a Swift module that can be
  /// built from a Swift interface file (\c .swiftinterface).
  static ModuleDependencyInfo forSwiftInterfaceModule(
      const std::string &moduleOutputPath,
      const std::string &swiftInterfaceFile,
      ArrayRef<std::string> compiledCandidates,
      ArrayRef<StringRef> buildCommands,
      ArrayRef<StringRef> extraPCMArgs,
      StringRef contextHash,
      bool isFramework) {
    return ModuleDependencyInfo(
        std::make_unique<SwiftInterfaceModuleDependenciesStorage>(
          moduleOutputPath, swiftInterfaceFile, compiledCandidates, buildCommands,
          extraPCMArgs, contextHash, isFramework));
  }

  /// Describe the module dependencies for a serialized or parsed Swift module.
  static ModuleDependencyInfo forSwiftBinaryModule(
      const std::string &compiledModulePath,
      const std::string &moduleDocPath,
      const std::string &sourceInfoPath,
      bool isFramework) {
    return ModuleDependencyInfo(
        std::make_unique<SwiftBinaryModuleDependencyStorage>(
          compiledModulePath, moduleDocPath, sourceInfoPath, isFramework));
  }

  /// Describe the main Swift module.
  static ModuleDependencyInfo forSwiftSourceModule(ArrayRef<StringRef> extraPCMArgs) {
    return ModuleDependencyInfo(
        std::make_unique<SwiftSourceModuleDependenciesStorage>(extraPCMArgs));
  }

  /// Describe the module dependencies for a Clang module that can be
  /// built from a module map and headers.
  static ModuleDependencyInfo forClangModule(
      const std::string &pcmOutputPath,
      const std::string &moduleMapFile,
      const std::string &contextHash,
      const std::vector<std::string> &nonPathCommandLine,
      const std::vector<std::string> &fileDependencies,
      const std::vector<std::string> &capturedPCMArgs) {
    return ModuleDependencyInfo(
        std::make_unique<ClangModuleDependencyStorage>(
          pcmOutputPath, moduleMapFile, contextHash,
          nonPathCommandLine, fileDependencies, capturedPCMArgs));
  }

  /// Describe a placeholder dependency swift module.
  static ModuleDependencyInfo forPlaceholderSwiftModuleStub(
      const std::string &compiledModulePath,
      const std::string &moduleDocPath,
      const std::string &sourceInfoPath) {
    return ModuleDependencyInfo(
        std::make_unique<SwiftPlaceholderModuleDependencyStorage>(
          compiledModulePath, moduleDocPath, sourceInfoPath));
  }

  /// Retrieve the module-level imports.
  ArrayRef<std::string> getModuleImports() const {
    return storage->moduleImports;
  }

  /// Retrieve the module-level optional imports.
  ArrayRef<std::string> getOptionalModuleImports() const {
    return storage->optionalModuleImports;
  }

  /// Retreive the module-level dependencies.
  const ArrayRef<ModuleDependencyID> getModuleDependencies() const {
    assert(storage->resolved);
    return storage->resolvedModuleDependencies;
  }

  /// Resolve a dependency's set of `imports` with qualified Module IDs
  void resolveDependencies(const ArrayRef<ModuleDependencyID> dependencyIDs) {
    assert(!storage->resolved && "Resolving an already-resolved dependency");
    storage->resolved = true;
    storage->resolvedModuleDependencies.assign(dependencyIDs.begin(), dependencyIDs.end());
  }

  /// Set this module's set of Swift Overlay dependencies
  void setOverlayDependencies(const ArrayRef<ModuleDependencyID> dependencyIDs) {
    assert(isSwiftSourceModule() || isSwiftInterfaceModule());
    CommonSwiftTextualModuleDependencyDetails *textualModuleDetails;
    if (auto sourceDetailsStorage = dyn_cast<SwiftSourceModuleDependenciesStorage>(storage.get())) {
      textualModuleDetails = &sourceDetailsStorage->textualModuleDetails;
    } else if (auto interfaceDetailsStorage = dyn_cast<SwiftInterfaceModuleDependenciesStorage>(storage.get())) {
      textualModuleDetails = &interfaceDetailsStorage->textualModuleDetails;
    } else {
      llvm_unreachable("Unknown kind of dependency module info.");
    }
    textualModuleDetails->swiftOverlayDependencies.assign(dependencyIDs.begin(), dependencyIDs.end());
  }

  void updateCommandLine(const std::vector<std::string> &newCommandLine) {
    assert(isSwiftInterfaceModule() && "Can only update command line on Swift interface dependency");
    cast<SwiftInterfaceModuleDependenciesStorage>(storage.get())->updateCommandLine(newCommandLine);
  }

  bool isResolved() const {
    return storage->resolved;
  }
  void setIsResolved(bool isResolved) {
    storage->resolved = isResolved;
  }

  /// For a Source dependency, register a `Testable` import
  void addTestableImport(ImportPath::Module module);

  /// Whether or not a queried module name is a `@Testable` import dependency
  /// of this module. Can only return `true` for Swift source modules.
  bool isTestableImport(StringRef moduleName) const;

  /// Whether the dependencies are for a Swift module: either Textual, Source, Binary, or Placeholder.
  bool isSwiftModule() const;

  /// Whether the dependencies are for a textual Swift module.
  bool isSwiftInterfaceModule() const;

  /// Whether the dependencies are for a textual Swift module.
  bool isSwiftSourceModule() const;

  /// Whether the dependencies are for a binary Swift module.
  bool isSwiftBinaryModule() const;

  /// Whether this represents a placeholder module stub
  bool isSwiftPlaceholderModule() const;

  /// Whether the dependencies are for a Clang module.
  bool isClangModule() const;

  ModuleDependencyKind getKind() const {
    return storage->dependencyKind;
  }

  /// Retrieve the dependencies for a Swift textual-interface module.
  const SwiftInterfaceModuleDependenciesStorage *getAsSwiftInterfaceModule() const;

  /// Retrieve the dependencies for a Swift module.
  const SwiftSourceModuleDependenciesStorage *getAsSwiftSourceModule() const;

  /// Retrieve the dependencies for a binary Swift module.
  const SwiftBinaryModuleDependencyStorage *getAsSwiftBinaryModule() const;

  /// Retrieve the dependencies for a Clang module.
  const ClangModuleDependencyStorage *getAsClangModule() const;

  /// Retrieve the dependencies for a placeholder dependency module stub.
  const SwiftPlaceholderModuleDependencyStorage *
    getAsPlaceholderDependencyModule() const;

  /// Add a dependency on the given module, if it was not already in the set.
  void addOptionalModuleImport(StringRef module,
                               llvm::StringSet<> *alreadyAddedModules = nullptr);


  /// Add a dependency on the given module, if it was not already in the set.
  void addModuleImport(StringRef module,
                       llvm::StringSet<> *alreadyAddedModules = nullptr);

  /// Add a dependency on the given module, if it was not already in the set.
  void addModuleImport(ImportPath::Module module,
                       llvm::StringSet<> *alreadyAddedModules = nullptr) {
    addModuleImport(module.front().Item.str(), alreadyAddedModules);
  }

  /// Add all of the module imports in the given source
  /// file to the set of module imports.
  void addModuleImport(const SourceFile &sf,
                       llvm::StringSet<> &alreadyAddedModules);
  /// Add a kind-qualified module dependency ID to the set of
  /// module dependencies.
  void addModuleDependency(ModuleDependencyID dependencyID);

  /// Get the bridging header.
  Optional<std::string> getBridgingHeader() const;

  /// Add a bridging header to a Swift module's dependencies.
  void addBridgingHeader(StringRef bridgingHeader);

  /// Add source files
  void addSourceFile(StringRef sourceFile);

  /// Add source files that the bridging header depends on.
  void addBridgingSourceFile(StringRef bridgingSourceFile);

  /// Add (Clang) module on which the bridging header depends.
  void addBridgingModuleDependency(StringRef module,
                                   llvm::StringSet<> &alreadyAddedModules);

  /// Collect a map from a secondary module name to a list of cross-import
  /// overlays, when this current module serves as the primary module.
  llvm::StringMap<llvm::SmallSetVector<Identifier, 4>>
  collectCrossImportOverlayNames(ASTContext &ctx, StringRef moduleName) const;
};

using ModuleNameToDependencyMap = llvm::StringMap<ModuleDependencyInfo>;
using ModuleDependenciesKindMap =
    std::unordered_map<ModuleDependencyKind,
                       ModuleNameToDependencyMap,
                       ModuleDependencyKindHash>;
using ModuleDependenciesKindRefMap =
    std::unordered_map<ModuleDependencyKind,
                       llvm::StringMap<const ModuleDependencyInfo *>,
                       ModuleDependencyKindHash>;

// MARK: SwiftDependencyScanningService
/// A carrier of state shared among possibly multiple invocations of the dependency
/// scanner. Acts as a global cache of discovered module dependencies and
/// filesystem state. It is not to be queried directly, but is rather
/// meant to be wrapped in an instance of `ModuleDependenciesCache`, responsible
/// for recording new dependencies and answering cache queries in a given scan.
class SwiftDependencyScanningService {
  /// Global cache contents specific to a specific scanner invocation context
  struct ContextSpecificGlobalCacheState {
    /// All cached module dependencies, in the order in which they were
    /// encountered.
    std::vector<ModuleDependencyID> AllModules;

    /// Set containing all of the Clang modules that have already been seen.
    llvm::StringSet<> alreadySeenClangModules;

    /// Dependencies for modules that have already been computed.
    /// This maps a dependency kind to a map of a module's name to a Dependency
    /// object
    ModuleDependenciesKindMap ModuleDependenciesMap;
  };

  /// The persistent Clang dependency scanner service
  clang::tooling::dependencies::DependencyScanningService ClangScanningService;
  /// The global file system cache.
  Optional<
      clang::tooling::dependencies::DependencyScanningFilesystemSharedCache>
      SharedFilesystemCache;

  /// A map from a String representing the target triple of a scanner invocation
  /// to the corresponding cached dependencies discovered so far when using this
  /// triple.
  llvm::StringMap<std::unique_ptr<ContextSpecificGlobalCacheState>>
      ContextSpecificCacheMap;

  /// The context hashes used by scanners using this cache, in the order in
  /// which they were used
  std::vector<std::string> AllContextHashes;

  /// Shared state mutual-exclusivity lock
  llvm::sys::SmartMutex<true> ScanningServiceGlobalLock;

  /// Retrieve the dependencies map that corresponds to the given dependency
  /// kind.
  ModuleNameToDependencyMap &getDependenciesMap(ModuleDependencyKind kind,
                                                StringRef scanContextHash);
  const ModuleNameToDependencyMap &
  getDependenciesMap(ModuleDependencyKind kind,
                     StringRef scanContextHash) const;

public:
  SwiftDependencyScanningService();
  SwiftDependencyScanningService(const SwiftDependencyScanningService &) = delete;
  SwiftDependencyScanningService &
  operator=(const SwiftDependencyScanningService &) = delete;
  virtual ~SwiftDependencyScanningService() {}

  /// Query the service's filesystem cache
  clang::tooling::dependencies::DependencyScanningFilesystemSharedCache &getSharedCache() {
    assert(SharedFilesystemCache && "Expected a shared cache");
    return *SharedFilesystemCache;
  }

  /// Query the service's filesystem cache
  clang::tooling::dependencies::DependencyScanningFilesystemSharedCache &
  getSharedFilesystemCache() {
    assert(SharedFilesystemCache && "Expected a shared cache");
    return *SharedFilesystemCache;
  }

  llvm::StringSet<>& getAlreadySeenClangModules(StringRef scanningContextHash) {
    return getCacheForScanningContextHash(scanningContextHash)->alreadySeenClangModules;
  }

  /// Wrap the filesystem on the specified `CompilerInstance` with a
  /// caching `DependencyScanningWorkerFilesystem`
  void overlaySharedFilesystemCacheForCompilation(CompilerInstance &Instance);
private:
  /// Enforce clients not being allowed to query this cache directly, it must be
  /// wrapped in an instance of `ModuleDependenciesCache`.
  friend class ModuleDependenciesCache;
  friend class ModuleDependenciesCacheDeserializer;
  friend class ModuleDependenciesCacheSerializer;
  friend class DependencyScanningTool;

  /// Configure the current state of the cache to respond to queries
  /// for the specified scanning context hash.
  void configureForContextHash(StringRef scanningContextHash);

  /// Return context hashes of all scanner invocations that have used
  /// this cache instance.
  const std::vector<std::string> &getAllContextHashes() const {
    return AllContextHashes;
  }

  /// Whether we have cached dependency information for the given module.
  bool hasDependency(StringRef moduleName,
                     Optional<ModuleDependencyKind> kind,
                     StringRef scanContextHash) const;

  /// Return a pointer to the cache state of the specified context hash.
  ContextSpecificGlobalCacheState *
  getCacheForScanningContextHash(StringRef scanContextHash) const;

  /// Look for module dependencies for a module with the given name
  ///
  /// \returns the cached result, or \c None if there is no cached entry.
  Optional<const ModuleDependencyInfo*>
  findDependency(StringRef moduleName,
                 Optional<ModuleDependencyKind> kind,
                 StringRef scanContextHash) const;

  /// Record dependencies for the given module.
  const ModuleDependencyInfo *recordDependency(StringRef moduleName,
                                               ModuleDependencyInfo dependencies,
                                               StringRef scanContextHash);

  /// Update stored dependencies for the given module.
  const ModuleDependencyInfo *updateDependency(ModuleDependencyID moduleID,
                                               ModuleDependencyInfo dependencies,
                                               StringRef scanContextHash);

  /// Reference the list of all module dependency infos for a given scanning context
  const std::vector<ModuleDependencyID> &
  getAllModules(StringRef scanningContextHash) const {
    auto contextSpecificCache =
        getCacheForScanningContextHash(scanningContextHash);
    return contextSpecificCache->AllModules;
  }
};

// MARK: ModuleDependenciesCache
/// This "local" dependencies cache persists only for the duration of a given
/// scanning action, and wraps an instance of a `SwiftDependencyScanningService`
/// which may carry cached scanning information from prior scanning actions.
/// This cache maintains a store of references to all dependencies found within
/// the current scanning action (with their values stored in the global Cache).
class ModuleDependenciesCache {
private:
  SwiftDependencyScanningService &globalScanningService;
  /// References to data in the `globalScanningService` for module dependencies
  ModuleDependenciesKindRefMap ModuleDependenciesMap;
  /// Name of the module under scan
  std::string mainScanModuleName;
  /// The context hash of the current scanning invocation
  std::string scannerContextHash;
  /// The Clang dependency scanner tool
  clang::tooling::dependencies::DependencyScanningTool clangScanningTool;

  /// Retrieve the dependencies map that corresponds to the given dependency
  /// kind.
  llvm::StringMap<const ModuleDependencyInfo *> &
  getDependencyReferencesMap(ModuleDependencyKind kind);
  const llvm::StringMap<const ModuleDependencyInfo *> &
  getDependencyReferencesMap(ModuleDependencyKind kind) const;

public:
  ModuleDependenciesCache(SwiftDependencyScanningService &globalScanningService,
                          std::string mainScanModuleName,
                          std::string scanningContextHash);
  ModuleDependenciesCache(const ModuleDependenciesCache &) = delete;
  ModuleDependenciesCache &operator=(const ModuleDependenciesCache &) = delete;

public:
  /// Whether we have cached dependency information for the given module.
  bool hasDependency(StringRef moduleName,
                     Optional<ModuleDependencyKind> kind) const;

  /// Produce a reference to the Clang scanner tool associated with this cache
  clang::tooling::dependencies::DependencyScanningTool& getClangScannerTool() {
    return clangScanningTool;
  }
  llvm::StringSet<>& getAlreadySeenClangModules() {
    return globalScanningService.getAlreadySeenClangModules(scannerContextHash);
  }
  
  /// Look for module dependencies for a module with the given name
  ///
  /// \returns the cached result, or \c None if there is no cached entry.
  Optional<const ModuleDependencyInfo*>
  findDependency(StringRef moduleName,
                 Optional<ModuleDependencyKind> kind) const;

  /// Record dependencies for the given module.
  void recordDependency(StringRef moduleName,
                        ModuleDependencyInfo dependencies);

  /// Update stored dependencies for the given module.
  void updateDependency(ModuleDependencyID moduleID,
                        ModuleDependencyInfo dependencies);

  /// Resolve a dependency module's set of imports
  /// to a kind-qualified set of module IDs.
  void resolveDependencyImports(ModuleDependencyID moduleID,
                                const ArrayRef<ModuleDependencyID> dependencyIDs);

  /// Resolve a dependency module's set of Swift module dependencies
  /// that are Swift overlays of Clang module dependencies.
  void setSwiftOverlayDependencues(ModuleDependencyID moduleID,
                                   const ArrayRef<ModuleDependencyID> dependencyIDs);
  
  StringRef getMainModuleName() const {
    return mainScanModuleName;
  }
};

} // namespace swift

namespace std {
template <>
struct hash<swift::ModuleDependencyID> {
  using UnderlyingKindType = std::underlying_type<swift::ModuleDependencyKind>::type;
  std::size_t operator()(const swift::ModuleDependencyID &id) const {
    auto underlyingKindValue = static_cast<UnderlyingKindType>(id.second);

    return (hash<string>()(id.first) ^
            (hash<UnderlyingKindType>()(underlyingKindValue)));
  }
};
} // namespace std

#endif /* SWIFT_AST_MODULE_DEPENDENCIES_H */
