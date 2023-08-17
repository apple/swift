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

#include "swift/AST/Import.h"
#include "swift/AST/SearchPathOptions.h"
#include "swift/Basic/LLVM.h"
#include "clang/CAS/CASOptions.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningService.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningTool.h"
#include "clang/Tooling/DependencyScanning/ModuleDepCollector.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/CAS/CASProvidingFileSystem.h"
#include "llvm/CAS/CASReference.h"
#include "llvm/CAS/CachingOnDiskFileSystem.h"
#include "llvm/CAS/ObjectStore.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <string>
#include <unordered_map>
#include <vector>

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
                                  StringRef moduleCacheKey = "")
      : dependencyKind(dependencyKind), moduleCacheKey(moduleCacheKey.str()),
        resolved(false), finalized(false) {}

  ModuleDependencyInfoStorageBase(ModuleDependencyKind dependencyKind,
                                  const std::vector<std::string> &moduleImports,
                                  StringRef moduleCacheKey = "")
      : dependencyKind(dependencyKind), moduleImports(moduleImports),
        moduleCacheKey(moduleCacheKey.str()), resolved(false), finalized(false)  {}

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

  /// The cache key for the produced module.
  std::string moduleCacheKey;

  /// The direct dependency of the module is resolved by scanner.
  bool resolved;
  /// ModuleDependencyInfo is finalized (with all transitive dependencies
  /// and inputs).
  bool finalized;
};

struct CommonSwiftTextualModuleDependencyDetails {
  CommonSwiftTextualModuleDependencyDetails(
      ArrayRef<StringRef> extraPCMArgs, ArrayRef<StringRef> buildCommandLine,
      const std::string &CASFileSystemRootID)
      : extraPCMArgs(extraPCMArgs.begin(), extraPCMArgs.end()),
        buildCommandLine(buildCommandLine.begin(), buildCommandLine.end()),
        CASFileSystemRootID(CASFileSystemRootID) {}

  /// To build a PCM to be used by this Swift module, we need to append these
  /// arguments to the generic PCM build arguments reported from the dependency
  /// graph.
  const std::vector<std::string> extraPCMArgs;

  /// Bridging header file, if there is one.
  llvm::Optional<std::string> bridgingHeaderFile;

  /// Source files on which the bridging header depends.
  std::vector<std::string> bridgingSourceFiles;

  /// (Clang) modules on which the bridging header depends.
  std::vector<std::string> bridgingModuleDependencies;

  /// Dependencies comprised of Swift overlay modules of direct and
  /// transitive Clang dependencies.
  std::vector<ModuleDependencyID> swiftOverlayDependencies;

  /// The Swift frontend invocation arguments to build the Swift module from the
  /// interface.
  std::vector<std::string> buildCommandLine;

  /// CASID for the Root of CASFS. Empty if CAS is not used.
  std::string CASFileSystemRootID;

  /// CASID for the Root of bridgingHeaderClangIncludeTree. Empty if not used.
  std::string CASBridgingHeaderIncludeTreeRootID;
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

  /// The hash value that will be used for the generated module
  const std::string contextHash;

  /// A flag that indicates this dependency is a framework
  const bool isFramework;

  /// Details common to Swift textual (interface or source) modules
  CommonSwiftTextualModuleDependencyDetails textualModuleDetails;

  SwiftInterfaceModuleDependenciesStorage(
      const std::string &moduleOutputPath,
      const std::string &swiftInterfaceFile,
      ArrayRef<std::string> compiledModuleCandidates,
      ArrayRef<StringRef> buildCommandLine, ArrayRef<StringRef> extraPCMArgs,
      StringRef contextHash, bool isFramework, const std::string &RootID,
      const std::string &moduleCacheKey)
      : ModuleDependencyInfoStorageBase(ModuleDependencyKind::SwiftInterface,
                                        moduleCacheKey),
        moduleOutputPath(moduleOutputPath),
        swiftInterfaceFile(swiftInterfaceFile),
        compiledModuleCandidates(compiledModuleCandidates.begin(),
                                 compiledModuleCandidates.end()),
        contextHash(contextHash), isFramework(isFramework),
        textualModuleDetails(extraPCMArgs, buildCommandLine, RootID) {}

  ModuleDependencyInfoStorageBase *clone() const override {
    return new SwiftInterfaceModuleDependenciesStorage(*this);
  }

  static bool classof(const ModuleDependencyInfoStorageBase *base) {
    return base->dependencyKind == ModuleDependencyKind::SwiftInterface;
  }

  void updateCommandLine(const std::vector<std::string> &newCommandLine) {
    textualModuleDetails.buildCommandLine = newCommandLine;
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

  /// The Swift frontend invocation arguments to build bridging header.
  std::vector<std::string> bridgingHeaderBuildCommandLine;

  SwiftSourceModuleDependenciesStorage(
      const std::string &RootID, ArrayRef<StringRef> buildCommandLine,
      ArrayRef<StringRef> bridgingHeaderBuildCommandLine,
      ArrayRef<StringRef> extraPCMArgs)
      : ModuleDependencyInfoStorageBase(ModuleDependencyKind::SwiftSource),
        textualModuleDetails(extraPCMArgs, buildCommandLine, RootID),
        testableImports(llvm::StringSet<>()),
        bridgingHeaderBuildCommandLine(bridgingHeaderBuildCommandLine.begin(),
                                       bridgingHeaderBuildCommandLine.end()) {}

  ModuleDependencyInfoStorageBase *clone() const override {
    return new SwiftSourceModuleDependenciesStorage(*this);
  }

  static bool classof(const ModuleDependencyInfoStorageBase *base) {
    return base->dependencyKind == ModuleDependencyKind::SwiftSource;
  }

  void updateCommandLine(const std::vector<std::string> &newCommandLine) {
    textualModuleDetails.buildCommandLine = newCommandLine;
  }

  void updateBridgingHeaderCommandLine(
      const std::vector<std::string> &newCommandLine) {
    bridgingHeaderBuildCommandLine = newCommandLine;
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
                                     const std::vector<std::string> &moduleImports,
                                     const std::vector<std::string> &headerImports,
                                     const bool isFramework,
                                     const std::string &moduleCacheKey)
      : ModuleDependencyInfoStorageBase(ModuleDependencyKind::SwiftBinary,
                                        moduleImports, moduleCacheKey),
        compiledModulePath(compiledModulePath), moduleDocPath(moduleDocPath),
        sourceInfoPath(sourceInfoPath), preCompiledBridgingHeaderPaths(headerImports),
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

  /// The paths of all the .pch dependencies of this module.
  const std::vector<std::string> preCompiledBridgingHeaderPaths;

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
  std::vector<std::string> buildCommandLine;

  /// The file dependencies
  const std::vector<std::string> fileDependencies;

  /// The swift-specific PCM arguments captured by this dependencies object
  /// as found by the scanning action that discovered it
  const std::vector<std::string> capturedPCMArgs;

  /// CASID for the Root of CASFS. Empty if CAS is not used.
  std::string CASFileSystemRootID;

  /// CASID for the Root of ClangIncludeTree. Empty if not used.
  std::string CASClangIncludeTreeRootID;

  ClangModuleDependencyStorage(const std::string &pcmOutputPath,
                               const std::string &moduleMapFile,
                               const std::string &contextHash,
                               const std::vector<std::string> &buildCommandLine,
                               const std::vector<std::string> &fileDependencies,
                               const std::vector<std::string> &capturedPCMArgs,
                               const std::string &CASFileSystemRootID,
                               const std::string &clangIncludeTreeRoot,
                               const std::string &moduleCacheKey)
      : ModuleDependencyInfoStorageBase(ModuleDependencyKind::Clang,
                                        moduleCacheKey),
        pcmOutputPath(pcmOutputPath), moduleMapFile(moduleMapFile),
        contextHash(contextHash), buildCommandLine(buildCommandLine),
        fileDependencies(fileDependencies), capturedPCMArgs(capturedPCMArgs),
        CASFileSystemRootID(CASFileSystemRootID),
        CASClangIncludeTreeRootID(clangIncludeTreeRoot) {}

  ModuleDependencyInfoStorageBase *clone() const override {
    return new ClangModuleDependencyStorage(*this);
  }

  static bool classof(const ModuleDependencyInfoStorageBase *base) {
    return base->dependencyKind == ModuleDependencyKind::Clang;
  }

  void updateCommandLine(const std::vector<std::string> &newCommandLine) {
    buildCommandLine = newCommandLine;
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
  static ModuleDependencyInfo
  forSwiftInterfaceModule(const std::string &moduleOutputPath,
                          const std::string &swiftInterfaceFile,
                          ArrayRef<std::string> compiledCandidates,
                          ArrayRef<StringRef> buildCommands,
                          ArrayRef<StringRef> extraPCMArgs,
                          StringRef contextHash, bool isFramework,
                          const std::string &CASFileSystemRootID,
                          const std::string &moduleCacheKey) {
    return ModuleDependencyInfo(
        std::make_unique<SwiftInterfaceModuleDependenciesStorage>(
            moduleOutputPath, swiftInterfaceFile, compiledCandidates,
            buildCommands, extraPCMArgs, contextHash, isFramework,
            CASFileSystemRootID, moduleCacheKey));
  }

  /// Describe the module dependencies for a serialized or parsed Swift module.
  static ModuleDependencyInfo forSwiftBinaryModule(
      const std::string &compiledModulePath,
      const std::string &moduleDocPath,
      const std::string &sourceInfoPath,
      const std::vector<std::string> &moduleImports,
      const std::vector<std::string> &headerImports,
      bool isFramework, const std::string &moduleCacheKey) {
    return ModuleDependencyInfo(
        std::make_unique<SwiftBinaryModuleDependencyStorage>(
          compiledModulePath, moduleDocPath, sourceInfoPath,
          moduleImports, headerImports, isFramework, moduleCacheKey));
  }

  /// Describe the main Swift module.
  static ModuleDependencyInfo
  forSwiftSourceModule(const std::string &CASFileSystemRootID,
                       ArrayRef<StringRef> buildCommands,
                       ArrayRef<StringRef> bridgingHeaderBuildCommands,
                       ArrayRef<StringRef> extraPCMArgs) {
    return ModuleDependencyInfo(
        std::make_unique<SwiftSourceModuleDependenciesStorage>(
            CASFileSystemRootID, buildCommands, bridgingHeaderBuildCommands,
            extraPCMArgs));
  }

  /// Describe the module dependencies for a Clang module that can be
  /// built from a module map and headers.
  static ModuleDependencyInfo forClangModule(
      const std::string &pcmOutputPath,
      const std::string &moduleMapFile,
      const std::string &contextHash,
      const std::vector<std::string> &nonPathCommandLine,
      const std::vector<std::string> &fileDependencies,
      const std::vector<std::string> &capturedPCMArgs,
      const std::string &CASFileSystemRootID,
      const std::string &clangIncludeTreeRoot,
      const std::string &moduleCacheKey) {
    return ModuleDependencyInfo(
        std::make_unique<ClangModuleDependencyStorage>(
          pcmOutputPath, moduleMapFile, contextHash,
          nonPathCommandLine, fileDependencies, capturedPCMArgs,
          CASFileSystemRootID, clangIncludeTreeRoot,  moduleCacheKey));
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

  std::string getModuleCacheKey() const {
    assert(storage->resolved);
    return storage->moduleCacheKey;
  }

  void updateModuleCacheKey(const std::string &key) {
    storage->moduleCacheKey = key;
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

  std::vector<std::string> getCommandline() const {
    if (auto *detail = getAsClangModule())
      return detail->buildCommandLine;
    else if (auto *detail = getAsSwiftInterfaceModule())
      return detail->textualModuleDetails.buildCommandLine;
    else if (auto *detail = getAsSwiftSourceModule())
      return detail->textualModuleDetails.buildCommandLine;
    return {};
  }

  void updateCommandLine(const std::vector<std::string> &newCommandLine) {
    if (isSwiftInterfaceModule())
      return cast<SwiftInterfaceModuleDependenciesStorage>(storage.get())
          ->updateCommandLine(newCommandLine);
    else if (isSwiftSourceModule())
      return cast<SwiftSourceModuleDependenciesStorage>(storage.get())
          ->updateCommandLine(newCommandLine);
    else if (isClangModule())
      return cast<ClangModuleDependencyStorage>(storage.get())
          ->updateCommandLine(newCommandLine);
    llvm_unreachable("Unexpected type");
  }

  std::vector<std::string> getBridgingHeaderCommandline() const {
    if (auto *detail = getAsSwiftSourceModule())
      return detail->bridgingHeaderBuildCommandLine;
    return {};
  }

  void updateBridgingHeaderCommandLine(
      const std::vector<std::string> &newCommandLine) {
    if (isSwiftSourceModule())
      return cast<SwiftSourceModuleDependenciesStorage>(storage.get())
          ->updateBridgingHeaderCommandLine(newCommandLine);
    llvm_unreachable("Unexpected type");
  }

  bool isResolved() const {
    return storage->resolved;
  }
  void setIsResolved(bool isResolved) {
    storage->resolved = isResolved;
  }

  bool isFinalized() const {
    return storage->finalized;
  }
  void setIsFinalized(bool isFinalized) {
    storage->finalized = isFinalized;
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
    std::string ImportedModuleName = module.front().Item.str().str();
    auto submodulePath = module.getSubmodulePath();
    if (submodulePath.size() > 0 && !submodulePath[0].Item.empty()) {
      assert(submodulePath.size() == 1 && "Unsupported Clang submodule import");
      auto submoduleComponent = submodulePath[0];
      // Special case: a submodule named "Foo.Private" can be moved to a top-level
      // module named "Foo_Private". ClangImporter has special support for this.
      if (submoduleComponent.Item.str() == "Private")
        ImportedModuleName = ImportedModuleName + "_Private";
    }

    addModuleImport(ImportedModuleName, alreadyAddedModules);
  }

  /// Add all of the module imports in the given source
  /// file to the set of module imports.
  void addModuleImport(const SourceFile &sf,
                       llvm::StringSet<> &alreadyAddedModules);
  /// Add a kind-qualified module dependency ID to the set of
  /// module dependencies.
  void addModuleDependency(ModuleDependencyID dependencyID);

  /// Get the bridging header.
  llvm::Optional<std::string> getBridgingHeader() const;

  /// Get CAS Filesystem RootID.
  llvm::Optional<std::string> getCASFSRootID() const;

  /// Get Clang Include Tree ID.
  llvm::Optional<std::string> getClangIncludeTree() const;

  /// Get bridging header Include Tree ID.
  llvm::Optional<std::string> getBridgingHeaderIncludeTree() const;

  /// Get module output path.
  std::string getModuleOutputPath() const;

  /// Add a bridging header to a Swift module's dependencies.
  void addBridgingHeader(StringRef bridgingHeader);

  /// Add source files
  void addSourceFile(StringRef sourceFile);

  /// Add source files that the bridging header depends on.
  void addBridgingSourceFile(StringRef bridgingSourceFile);

  /// Add (Clang) module on which the bridging header depends.
  void addBridgingModuleDependency(StringRef module,
                                   llvm::StringSet<> &alreadyAddedModules);

  /// Add bridging header include tree.
  void addBridgingHeaderIncludeTree(StringRef ID);

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

// MARK: SwiftDependencyTracker
/// Track swift dependency
class SwiftDependencyTracker {
public:
  SwiftDependencyTracker(llvm::cas::CachingOnDiskFileSystem &FS)
      : FS(FS.createProxyFS()) {}

  void startTracking();
  void addCommonSearchPathDeps(const SearchPathOptions& Opts);
  void trackFile(const Twine &path) { (void)FS->status(path); }
  llvm::Expected<llvm::cas::ObjectProxy> createTreeFromDependencies();

private:
  llvm::IntrusiveRefCntPtr<llvm::cas::CachingOnDiskFileSystem> FS;
};

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

    /// Dependencies for modules that have already been computed.
    /// This maps a dependency kind to a map of a module's name to a Dependency
    /// object
    ModuleDependenciesKindMap ModuleDependenciesMap;
  };

  /// The CASOption created the Scanning Service if used.
  llvm::Optional<clang::CASOptions> CASOpts;

  /// The persistent Clang dependency scanner service
  llvm::Optional<clang::tooling::dependencies::DependencyScanningService>
      ClangScanningService;

  /// CachingOnDiskFileSystem for dependency tracking.
  llvm::IntrusiveRefCntPtr<llvm::cas::CachingOnDiskFileSystem> CacheFS;

  /// If use clang include tree.
  bool UseClangIncludeTree = false;

  /// CAS ObjectStore Instance.
  std::shared_ptr<llvm::cas::ObjectStore> CAS;

  /// The common dependencies that is needed for every swift compiler instance.
  std::vector<std::string> CommonDependencyFiles;

  /// The global file system cache.
  llvm::Optional<
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

  bool usingCachingFS() const { return !UseClangIncludeTree && (bool)CacheFS; }

  llvm::cas::CachingOnDiskFileSystem &getSharedCachingFS() const {
    assert(CacheFS && "Expect CachingOnDiskFileSystem");
    return *CacheFS;
  }

  llvm::Optional<SwiftDependencyTracker> createSwiftDependencyTracker() const {
    if (!CacheFS)
      return llvm::None;

    return SwiftDependencyTracker(*CacheFS);
  }

  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> getClangScanningFS() const {
    if (usingCachingFS())
      return CacheFS->createProxyFS();

    if (UseClangIncludeTree)
      return llvm::cas::createCASProvidingFileSystem(
          CAS, llvm::vfs::createPhysicalFileSystem());

    return llvm::vfs::createPhysicalFileSystem();
  }

  /// Wrap the filesystem on the specified `CompilerInstance` with a
  /// caching `DependencyScanningWorkerFilesystem`
  void overlaySharedFilesystemCacheForCompilation(CompilerInstance &Instance);

  /// Setup caching service.
  bool setupCachingDependencyScanningService(CompilerInstance &Instance);
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
                     llvm::Optional<ModuleDependencyKind> kind,
                     StringRef scanContextHash) const;

  /// Return a pointer to the cache state of the specified context hash.
  ContextSpecificGlobalCacheState *
  getCacheForScanningContextHash(StringRef scanContextHash) const;

  /// Look for module dependencies for a module with the given name
  ///
  /// \returns the cached result, or \c None if there is no cached entry.
  llvm::Optional<const ModuleDependencyInfo *>
  findDependency(StringRef moduleName,
                 llvm::Optional<ModuleDependencyKind> kind,
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
  /// Set containing all of the Clang modules that have already been seen.
  llvm::DenseSet<clang::tooling::dependencies::ModuleID> alreadySeenClangModules;
  /// Name of the module under scan
  std::string mainScanModuleName;
  /// The context hash of the current scanning invocation
  std::string scannerContextHash;
  /// The location of where the built modules will be output to
  std::string moduleOutputPath;
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
                          std::string moduleOutputPath,
                          std::string scanningContextHash);
  ModuleDependenciesCache(const ModuleDependenciesCache &) = delete;
  ModuleDependenciesCache &operator=(const ModuleDependenciesCache &) = delete;

public:
  /// Whether we have cached dependency information for the given module.
  bool hasDependency(StringRef moduleName,
                     llvm::Optional<ModuleDependencyKind> kind) const;

  /// Produce a reference to the Clang scanner tool associated with this cache
  clang::tooling::dependencies::DependencyScanningTool& getClangScannerTool() {
    return clangScanningTool;
  }
  SwiftDependencyScanningService &getScanService() {
    return globalScanningService;
  }
  llvm::DenseSet<clang::tooling::dependencies::ModuleID>& getAlreadySeenClangModules() {
    return alreadySeenClangModules;
  }
  void addSeenClangModule(clang::tooling::dependencies::ModuleID newModule) {
    alreadySeenClangModules.insert(newModule);
  }
  std::string getModuleOutputPath() {
    return moduleOutputPath;
  }

  /// Look for module dependencies for a module with the given name
  ///
  /// \returns the cached result, or \c None if there is no cached entry.
  llvm::Optional<const ModuleDependencyInfo *>
  findDependency(StringRef moduleName,
                 llvm::Optional<ModuleDependencyKind> kind) const;

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
