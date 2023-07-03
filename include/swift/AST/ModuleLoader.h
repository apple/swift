//===--- ModuleLoader.h - Module Loader Interface ---------------*- C++ -*-===//
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
// This file implements an abstract interface for loading modules.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_MODULE_LOADER_H
#define SWIFT_AST_MODULE_LOADER_H

#include "swift/AST/Identifier.h"
#include "swift/AST/Import.h"
#include "swift/Basic/ArrayRefView.h"
#include "swift/Basic/Fingerprint.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Located.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/VersionTuple.h"
#include <system_error>

namespace llvm {
class FileCollectorBase;
namespace vfs {
class OutputBackend;
}
}

namespace clang {
class DependencyCollector;
}

namespace swift {

class AbstractFunctionDecl;
struct AutoDiffConfig;
class ClangImporterOptions;
class ClassDecl;
class FileUnit;
class ModuleDecl;
class ModuleDependencyInfo;
class ModuleDependenciesCache;
class NominalTypeDecl;
class SourceFile;
class TypeDecl;
class CompilerInstance;
enum class ModuleDependencyKind : int8_t;

enum class KnownProtocolKind : uint8_t;

enum class Bridgeability : unsigned {
  /// This context does not permit bridging at all.  For example, the
  /// target of a C pointer.
  None,

  /// This context permits all kinds of bridging.  For example, the
  /// imported result of a method declaration.
  Full
};

/// Specifies which dependencies the intermodule dependency tracker records.
enum class IntermoduleDepTrackingMode {
  /// Records both system and non-system dependencies.
  IncludeSystem,

  /// Records only non-system dependencies.
  ExcludeSystem,
};

/// Records dependencies on files outside of the current module;
/// implemented in terms of a wrapped clang::DependencyCollector.
class DependencyTracker {
public:
  /// A representation of a first-class incremental dependency known to the
  /// Swift compiler.
  struct IncrementalDependency {
    std::string path;
    Fingerprint fingerprint;

    IncrementalDependency(std::string Path, Fingerprint FP)
        : path(std::move(Path)), fingerprint(std::move(FP)) {}
  };

  inline static std::string getPath(const IncrementalDependency &id) {
    return id.path;
  }
  typedef ArrayRefView<IncrementalDependency, std::string, getPath>
      DependencyPathArrayRef;

  std::shared_ptr<clang::DependencyCollector> clangCollector;
  SmallVector<IncrementalDependency, 8> incrementalDeps;
  llvm::StringSet<> incrementalDepsUniquer;

public:
  explicit DependencyTracker(
      IntermoduleDepTrackingMode Mode,
      std::shared_ptr<llvm::FileCollectorBase> FileCollector = {});

  /// Adds a file as a dependency.
  ///
  /// The contents of \p File are taken literally, and should be appropriate
  /// for appearing in a list of dependencies suitable for tooling like Make.
  /// No path canonicalization is done.
  void addDependency(StringRef File, bool IsSystem);

  /// Adds a file as an incremental dependency.
  ///
  /// No additional canonicalization or adulteration of the file path in
  /// \p File is performed.
  void addIncrementalDependency(StringRef File, Fingerprint FP);

  /// Fetches the list of dependencies.
  ArrayRef<std::string> getDependencies() const;

  /// Fetches the list of dependencies that are known to have incremental swift
  /// dependency information embedded inside of them.
  ArrayRef<IncrementalDependency> getIncrementalDependencies() const;

  /// A view of the paths of the dependencies known to have incremental swift
  /// dependency information embedded inside of them.
  DependencyPathArrayRef getIncrementalDependencyPaths() const {
    return DependencyPathArrayRef(getIncrementalDependencies());
  }

  /// Return the underlying clang::DependencyCollector that this
  /// class wraps.
  std::shared_ptr<clang::DependencyCollector> getClangCollector();
};

struct SubCompilerInstanceInfo {
  StringRef CompilerVersion;
  CompilerInstance* Instance;
  StringRef Hash;
  ArrayRef<StringRef> BuildArguments;
  ArrayRef<StringRef> ExtraPCMArgs;
};

/// Abstract interface for a checker of module interfaces and prebuilt modules.
class ModuleInterfaceChecker {
public:
  virtual std::vector<std::string>
  getCompiledModuleCandidatesForInterface(StringRef moduleName,
                                          StringRef interfacePath) = 0;

  /// Given a list of potential ready-to-use compiled modules for \p interfacePath,
  /// check if any one of them is up-to-date. If so, emit a forwarding module
  /// to the candidate binary module to \p outPath.
  virtual bool tryEmitForwardingModule(StringRef moduleName,
                               StringRef interfacePath,
                               ArrayRef<std::string> candidates,
                               llvm::vfs::OutputBackend &backend,
                               StringRef outPath) = 0;
  virtual ~ModuleInterfaceChecker() = default;
};

/// Abstract interface to run an action in a sub ASTContext.
struct InterfaceSubContextDelegate {
  virtual std::error_code runInSubContext(StringRef moduleName,
                                          StringRef interfacePath,
                                          StringRef outputPath,
                                          SourceLoc diagLoc,
    llvm::function_ref<std::error_code(ASTContext&, ModuleDecl*,
                                       ArrayRef<StringRef>,
                                       ArrayRef<StringRef>, StringRef)> action) = 0;
  virtual std::error_code runInSubCompilerInstance(StringRef moduleName,
                                                   StringRef interfacePath,
                                                   StringRef outputPath,
                                                   SourceLoc diagLoc,
                                                   bool silenceErrors,
    llvm::function_ref<std::error_code(SubCompilerInstanceInfo&)> action) = 0;

  virtual ~InterfaceSubContextDelegate() = default;
};

/// Abstract interface that loads named modules into the AST.
class ModuleLoader {
  virtual void anchor();

protected:
  DependencyTracker * const dependencyTracker;
  ModuleLoader(DependencyTracker *tracker) : dependencyTracker(tracker) {}

public:
  virtual ~ModuleLoader() = default;

  /// Collect visible module names.
  ///
  /// Append visible module names to \p names. Note that names are possibly
  /// duplicated, and not guaranteed to be ordered in any way.
  virtual void collectVisibleTopLevelModuleNames(
      SmallVectorImpl<Identifier> &names) const = 0;

  /// The kind of source for a module's version.
  ///
  /// NOTE: The order of the members is significant; they are declared in
  ///       ascending priority order.
  enum class ModuleVersionSourceKind {
    ClangModuleTBD,
    SwiftBinaryModule,
    SwiftInterface,
  };

  /// Represents a module version and the source it was parsed from.
  class ModuleVersionInfo {
    llvm::VersionTuple Version;
    llvm::Optional<ModuleVersionSourceKind> SourceKind;

  public:
    /// Returns true if the version has a valid source kind.
    bool isValid() const { return SourceKind.has_value(); }

    /// Returns the version, which may be empty if a version was not present or
    /// was unparsable.
    llvm::VersionTuple getVersion() const { return Version; }

    /// Returns the kind of source of the module version. Do not call if
    /// \c isValid() returns false.
    ModuleVersionSourceKind getSourceKind() const {
      return SourceKind.value();
    }

    void setVersion(llvm::VersionTuple version, ModuleVersionSourceKind kind) {
      Version = version;
      SourceKind = kind;
    }
  };

  /// Check whether the module with a given name can be imported without
  /// importing it.
  ///
  /// Note that even if this check succeeds, errors may still occur if the
  /// module is loaded in full.
  ///
  /// If a non-null \p versionInfo is provided, the module version will be
  /// parsed and populated.
  virtual bool canImportModule(ImportPath::Module named,
                               ModuleVersionInfo *versionInfo,
                               bool isTestableImport = false) = 0;

  /// Import a module with the given module path.
  ///
  /// \param importLoc The location of the 'import' keyword.
  ///
  /// \param path A sequence of (identifier, location) pairs that denote
  /// the dotted module name to load, e.g., AppKit.NSWindow.
  ///
  /// \param AllowMemoryCache Enables preserving the loaded module in the
  /// in-memory cache for the next loading attempt.
  ///
  /// \returns the module referenced, if it could be loaded. Otherwise,
  /// emits a diagnostic and returns NULL.
  virtual
  ModuleDecl *loadModule(SourceLoc importLoc, ImportPath::Module path,
                         bool AllowMemoryCache = true) = 0;

  /// Load extensions to the given nominal type.
  ///
  /// \param nominal The nominal type whose extensions should be loaded.
  ///
  /// \param previousGeneration The previous generation number. The AST already
  /// contains extensions loaded from any generation up to and including this
  /// one.
  virtual void loadExtensions(NominalTypeDecl *nominal,
                              unsigned previousGeneration) { }

  /// Load the methods within the given type that produce
  /// Objective-C class or instance methods with the given selector.
  ///
  /// \param typeDecl The type in which we are searching for @objc methods.
  /// The search only considers this type and its extensions; not any
  /// superclasses.
  ///
  /// \param selector The selector to search for.
  ///
  /// \param isInstanceMethod Whether we are looking for an instance method
  /// (vs. a class method).
  ///
  /// \param previousGeneration The previous generation with which this
  /// callback was invoked. The list of methods will already contain all of
  /// the results from generations up and including \c previousGeneration.
  ///
  /// \param methods The list of @objc methods in this class that have this
  /// selector and are instance/class methods as requested. This list will be
  /// extended with any methods found in subsequent generations.
  virtual void loadObjCMethods(
                 NominalTypeDecl *typeDecl,
                 ObjCSelector selector,
                 bool isInstanceMethod,
                 unsigned previousGeneration,
                 llvm::TinyPtrVector<AbstractFunctionDecl *> &methods) = 0;

  /// Load derivative function configurations for the given
  /// AbstractFunctionDecl.
  ///
  /// \param originalAFD The declaration whose derivative function
  /// configurations should be loaded.
  ///
  /// \param previousGeneration The previous generation number. The AST already
  /// contains derivative function configurations loaded from any generation up
  /// to and including this one.
  ///
  /// \param results The result list of derivative function configurations.
  /// This list will be extended with any methods found in subsequent
  /// generations.
  virtual void loadDerivativeFunctionConfigurations(
      AbstractFunctionDecl *originalAFD, unsigned previousGeneration,
      llvm::SetVector<AutoDiffConfig> &results) {};

  /// Verify all modules loaded by this loader.
  virtual void verifyAllModules() { }

  /// Discover overlays declared alongside this file and add information about
  /// them to it.
  void findOverlayFiles(SourceLoc diagLoc, ModuleDecl *module, FileUnit *file);

  /// Retrieve the dependencies for the given, named module, or \c None
  /// if no such module exists.
  virtual llvm::Optional<const ModuleDependencyInfo *>
  getModuleDependencies(StringRef moduleName, ModuleDependenciesCache &cache,
                        InterfaceSubContextDelegate &delegate,
                        bool isTestableImport = false) = 0;
};

} // namespace swift

#endif
