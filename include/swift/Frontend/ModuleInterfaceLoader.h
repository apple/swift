//===-------- ModuleInterfaceLoader.h - Loads .swiftinterface files -------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file This implements the logic for loading and building module
/// interfaces.
///
/// === Loading Module Interfaces ===
///
/// If there is a .swiftinterface file corresponding to a given module name
/// present in the frontend's search paths, then this module loader will look in
/// the following places for a module:
///
/// - First, look in the module cache (specified by -module-cache-path)
///   - We check here first because an existing .swiftmodule might be
///     out-of-date, necessitating a rebuild. If a cached module is out-of-date,
///     it's simply rebuilt.
/// - Next, look adjacent to the .swiftinterface. If we find a module that's
///   either loadable by this compiler, valid, and up-to-date, or totally
///   unreadable, then delegate to the serialized module loader to either load
///   or diagnose it.
/// - Finally, look in the prebuilt module cache (specified
///   by -prebuilt-module-cache-path)
///
/// If we can't find an appropriate module to load, we can always fall back and
/// recompile the .swiftinterface file.
///
/// === Dependency Checking ===
///
/// Cached modules keep track of their dependencies' last modification time and
/// file size. This means that checking if a module is up-to-date requires
/// `stat`ing the dependencies and comparing the results from the filesystem
/// with the results in the module.
///
/// Prebuilt modules, on the other hand, won't have a reliable modification
/// time, as their dependencies live in the SDK. Prebuilt modules will instead
/// keep track of the size and content hash of their dependencies.
/// In order to avoid constantly re-hashing the dependencies, however, we will
/// install a "forwarding module" in the regular cache.
/// This "forwarding module"
///  - Points to the prebuilt module on disk, and
///  - Lists modification times from the last time we verified the content
///
/// So, to recap, there are 4 kinds of modules:
/// ┌───────────────────────────────┐
/// │     ┌───┐           ┌───┐     │
/// │     │ I │           │ M │     │
/// │     └───┘           └───┘     │
/// │ .swiftinterface  .swiftmodule │
/// │     ┌───┐           ┌───┐     │
/// │     │ P │           │ F │     │
/// │     └───┘           └───┘     │
/// │   Prebuilt        Forwarding  │
/// │ .swiftmodule     .swiftmodule │
/// └───────────────────────────────┘
///
/// - Prebuilt modules have hash-based dependencies, cached modules have
///   mod-time-based dependencies
/// - Forwarding modules point to prebuilt modules and augment them with
///   modification times
///
/// === Example Cache ===
///
/// Here's an example of what's in a prebuilt cache or module cache.
///
/// Say there are 4 frameworks, each exposing a .swiftinterface file.
/// Then, we pre-build 2 of those frameworks and put them in the prebuilt cache.
/// Finally, we import all 4 of those frameworks while building a project.
///
/// For the 2 frameworks with modules in the prebuilt cache, we'll have
/// installed 2 forwarding modules. For the other 2 frameworks, we'll have
/// compiled the interfaces and put them in the module cache.
///
///                   ┌─────┐
///  ┌────────────────┤ SDK ├───────────────┐
///  │                └─────┘               │
///  │          ┌────────────────┐          │  ┌────────────────┐
///  │  ┌───────┤ Framework Dirs ├────────┐ │ ┌┤ Prebuilt Cache ├┐
///  │  │       └────────────────┘        │ │ │└────────────────┘│
///  │  │  ┌───┐   ┌───┐   ┌───┐   ┌───┐  │ │ │   ┌───┐  ┌───┐   │
///  │  │  │ I │   │ I │   │ I │   │ I │◀─┼─┼─┼───│ P │  │ P │◀═╗│
///  │  │  └───┘   └───┘   └───┘   └───┘  │ │ │   └───┘  └───┘  ║│
///  │  │    ▲       ▲       ▲            │ │ │     ▲      │    ║│
///  │  └────┼───────┼───────┼────────────┘ │ └─────╫──────┼────╫┘
///  │       │       │       └──────────────┼───────╫──────┘    ║
///  └───────┼───────┼──────────────────────┘       ║           ║
///          │       │   ┌───────────────┐          ║           ║
///          │  ┌────┼───┤ Module Cache  ├────────┐ ║           ║
///          │  │    │   └───────────────┘        │ ║           ║
///          │  │  ┌───┐   ┌───┐   ┌───┐   ┌───┐  │ ║           ║
///          │  │  │ M │   │ M │   │ F │   │ F │  │ ║           ║
///          │  │  └───┘   └───┘   └───┘   └───┘  │ ║           ║
///          │  │            │       ║       ╚════╪═╝           ║
///          │  └────────────┼───────╫────────────┘             ║
///          └───────────────┘       ╚══════════════════════════╝
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_FRONTEND_MODULEINTERFACELOADER_H
#define SWIFT_FRONTEND_MODULEINTERFACELOADER_H

#include "swift/Basic/LLVM.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/ModuleInterfaceSupport.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/YAMLTraits.h"

namespace llvm {
namespace cas {
class ObjectStore;
class ActionCache;
} // namespace cas
} // namespace llvm

namespace clang {
class CompilerInstance;
}

namespace unittest {
class ModuleInterfaceLoaderTest;
}

namespace swift {

class LangOptions;
class SearchPathOptions;
class CompilerInvocation;

/// A ModuleLoader that loads explicitly built Swift modules specified via
/// -swift-module-file or modules found in a provided
/// -explicit-swift-module-map-file JSON input.
class ExplicitSwiftModuleLoader: public SerializedModuleLoaderBase {
  explicit ExplicitSwiftModuleLoader(ASTContext &ctx, DependencyTracker *tracker,
                                     ModuleLoadingMode loadMode,
                                     bool IgnoreSwiftSourceInfoFile);

  bool findModule(ImportPath::Element moduleID,
                  SmallVectorImpl<char> *moduleInterfacePath,
                  SmallVectorImpl<char> *moduleInterfaceSourcePath,
                  std::unique_ptr<llvm::MemoryBuffer> *moduleBuffer,
                  std::unique_ptr<llvm::MemoryBuffer> *moduleDocBuffer,
                  std::unique_ptr<llvm::MemoryBuffer> *moduleSourceInfoBuffer,
                  bool skipBuildingInterface, bool isTestableDependencyLookup,
                  bool &isFramework, bool &isSystemModule) override;

  std::error_code findModuleFilesInDirectory(
      ImportPath::Element ModuleID, const SerializedModuleBaseName &BaseName,
      SmallVectorImpl<char> *ModuleInterfacePath,
      SmallVectorImpl<char> *ModuleInterfaceSourcePath,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
      bool SkipBuildingInterface, bool IsFramework,
      bool IsTestableDependencyLookup = false) override;

  bool canImportModule(ImportPath::Module named,
                       ModuleVersionInfo *versionInfo,
                       bool isTestableDependencyLookup = false) override;

  bool isCached(StringRef DepPath) override { return false; };

  struct Implementation;
  Implementation &Impl;
public:
  static std::unique_ptr<ExplicitSwiftModuleLoader>
  create(ASTContext &ctx,
         DependencyTracker *tracker, ModuleLoadingMode loadMode,
         StringRef ExplicitSwiftModuleMap,
         const std::vector<std::pair<std::string, std::string>> &ExplicitSwiftModuleInputs,
         bool IgnoreSwiftSourceInfoFile);

  /// Append visible module names to \p names. Note that names are possibly
  /// duplicated, and not guaranteed to be ordered in any way.
  void collectVisibleTopLevelModuleNames(
      SmallVectorImpl<Identifier> &names) const override;
  ~ExplicitSwiftModuleLoader();
};

class ExplicitCASModuleLoader : public SerializedModuleLoaderBase {
  explicit ExplicitCASModuleLoader(ASTContext &ctx, llvm::cas::ObjectStore &CAS,
                                   llvm::cas::ActionCache &cache,
                                   DependencyTracker *tracker,
                                   ModuleLoadingMode loadMode,
                                   bool IgnoreSwiftSourceInfoFile);

  bool findModule(ImportPath::Element moduleID,
                  SmallVectorImpl<char> *moduleInterfacePath,
                  SmallVectorImpl<char> *moduleInterfaceSourcePath,
                  std::unique_ptr<llvm::MemoryBuffer> *moduleBuffer,
                  std::unique_ptr<llvm::MemoryBuffer> *moduleDocBuffer,
                  std::unique_ptr<llvm::MemoryBuffer> *moduleSourceInfoBuffer,
                  bool skipBuildingInterface, bool isTestableDependencyLookup,
                  bool &isFramework, bool &isSystemModule) override;

  std::error_code findModuleFilesInDirectory(
      ImportPath::Element ModuleID, const SerializedModuleBaseName &BaseName,
      SmallVectorImpl<char> *ModuleInterfacePath,
      SmallVectorImpl<char> *ModuleInterfaceSourcePath,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
      bool SkipBuildingInterface, bool IsFramework,
      bool IsTestableDependencyLookup = false) override;

  bool canImportModule(ImportPath::Module named,
                       ModuleVersionInfo *versionInfo,
                       bool isTestableDependencyLookup = false) override;

  struct Implementation;
  Implementation &Impl;

public:
  static std::unique_ptr<ExplicitCASModuleLoader>
  create(ASTContext &ctx, llvm::cas::ObjectStore &CAS,
         llvm::cas::ActionCache &cache, DependencyTracker *tracker,
         ModuleLoadingMode loadMode, StringRef ExplicitSwiftModuleMap,
         const std::vector<std::pair<std::string, std::string>>
             &ExplicitSwiftModuleInputs,
         bool IgnoreSwiftSourceInfoFile);

  /// Append visible module names to \p names. Note that names are possibly
  /// duplicated, and not guaranteed to be ordered in any way.
  void collectVisibleTopLevelModuleNames(
      SmallVectorImpl<Identifier> &names) const override;

  ~ExplicitCASModuleLoader();
};

// Explicitly-specified Swift module inputs
struct ExplicitSwiftModuleInputInfo {
  ExplicitSwiftModuleInputInfo(std::string modulePath,
                               llvm::Optional<std::string> moduleDocPath,
                               llvm::Optional<std::string> moduleSourceInfoPath,
                               llvm::Optional<std::vector<std::string>> headerDependencyPaths,
                               bool isFramework = false,
                               bool isSystem = false,
                               llvm::Optional<std::string> moduleCacheKey = None)
    : modulePath(modulePath),
      moduleDocPath(moduleDocPath),
      moduleSourceInfoPath(moduleSourceInfoPath),
      headerDependencyPaths(headerDependencyPaths),
      isFramework(isFramework),
      isSystem(isSystem),
      moduleCacheKey(moduleCacheKey) {}
  // Path of the .swiftmodule file.
  std::string modulePath;
  // Path of the .swiftmoduledoc file.
  llvm::Optional<std::string> moduleDocPath;
  // Path of the .swiftsourceinfo file.
  llvm::Optional<std::string> moduleSourceInfoPath;
  // Paths of the precompiled header dependencies of this module.
  llvm::Optional<std::vector<std::string>> headerDependencyPaths;
  // A flag that indicates whether this module is a framework
  bool isFramework = false;
  // A flag that indicates whether this module is a system module
  bool isSystem = false;
  // The cache key for clang module.
  llvm::Optional<std::string> moduleCacheKey;
};

// Explicitly-specified Clang module inputs
struct ExplicitClangModuleInputInfo {
  ExplicitClangModuleInputInfo(std::string moduleMapPath,
                               std::string modulePath,
                               bool isFramework = false,
                               bool isSystem = false,
                               llvm::Optional<std::string> moduleCacheKey = None)
    : moduleMapPath(moduleMapPath),
      modulePath(modulePath),
      isFramework(isFramework),
      isSystem(isSystem),
      moduleCacheKey(moduleCacheKey) {}
  // Path of the Clang module map file.
  std::string moduleMapPath;
  // Path of a compiled Clang explicit module file (pcm).
  std::string modulePath;
  // A flag that indicates whether this module is a framework
  bool isFramework = false;
  // A flag that indicates whether this module is a system module
  bool isSystem = false;
  // The cache key for clang module.
  llvm::Optional<std::string> moduleCacheKey;
};

/// Parser of explicit module maps passed into the compiler.
//  [
//    {
//      "moduleName": "A",
//      "modulePath": "A.swiftmodule",
//      "docPath": "A.swiftdoc",
//      "sourceInfoPath": "A.swiftsourceinfo",
//      "isFramework": false,
//      "clangModuleMapPath": "A/module.modulemap",
//      "clangModulePath": "A.pcm",
//      "moduleCacheKey": "llvmcas://<hash>",
//      "clangModuleCacheKey": "llvmcas://<hash>",
//    },
//    {
//      "moduleName": "B",
//      "modulePath": "B.swiftmodule",
//      "docPath": "B.swiftdoc",
//      "sourceInfoPath": "B.swiftsourceinfo",
//      "isFramework": false,
//      "clangModuleMapPath": "B/module.modulemap",
//      "clangModulePath": "B.pcm",
//      "moduleCacheKey": "llvmcas://<hash>",
//      "clangModuleCacheKey": "llvmcas://<hash>",
//    }
//  ]
class ExplicitModuleMapParser {
public:
  ExplicitModuleMapParser(llvm::BumpPtrAllocator &Allocator) : Saver(Allocator) {}

  std::error_code
  parseSwiftExplicitModuleMap(llvm::MemoryBufferRef BufferRef,
                              llvm::StringMap<ExplicitSwiftModuleInputInfo> &swiftModuleMap,
                              llvm::StringMap<ExplicitClangModuleInputInfo> &clangModuleMap) {
    using namespace llvm::yaml;
    // Use a new source manager instead of the one from ASTContext because we
    // don't want the JSON file to be persistent.
    llvm::SourceMgr SM;
    Stream Stream(BufferRef, SM);
    for (auto DI = Stream.begin(); DI != Stream.end(); ++DI) {
      assert(DI != Stream.end() && "Failed to read a document");
      if (auto *MN = dyn_cast_or_null<SequenceNode>(DI->getRoot())) {
        for (auto &entry : *MN) {
          if (parseSingleModuleEntry(entry, swiftModuleMap, clangModuleMap)) {
            return std::make_error_code(std::errc::invalid_argument);
          }
        }
      } else {
        return std::make_error_code(std::errc::invalid_argument);
      }
    }
    return std::error_code{}; // success
  }

private:
  StringRef getScalaNodeText(llvm::yaml::Node *N) {
    SmallString<32> Buffer;
    return Saver.save(cast<llvm::yaml::ScalarNode>(N)->getValue(Buffer));
  }

  static bool parseBoolValue(StringRef val) {
    auto valStr = val.str();
    valStr.erase(std::remove(valStr.begin(), valStr.end(), '\n'), valStr.end());
    if (valStr.compare("true") == 0)
      return true;
    else if (valStr.compare("false") == 0)
      return false;
    else
      llvm_unreachable("Unexpected JSON value for isFramework");
  }

  bool parseSingleModuleEntry(llvm::yaml::Node &node,
                              llvm::StringMap<ExplicitSwiftModuleInputInfo> &swiftModuleMap,
                              llvm::StringMap<ExplicitClangModuleInputInfo> &clangModuleMap) {
    using namespace llvm::yaml;
    auto *mapNode = dyn_cast<MappingNode>(&node);
    if (!mapNode)
      return true;
    StringRef moduleName;
    llvm::Optional<std::string> swiftModulePath, swiftModuleDocPath,
                                swiftModuleSourceInfoPath, swiftModuleCacheKey,
                                clangModuleCacheKey;
    llvm::Optional<std::vector<std::string>> headerDependencyPaths;
    std::string clangModuleMapPath = "", clangModulePath = "";
    bool isFramework = false, isSystem = false;
    for (auto &entry : *mapNode) {
      auto key = getScalaNodeText(entry.getKey());
      if (key == "prebuiltHeaderDependencyPaths") {
        continue;
      } else {
        auto val = getScalaNodeText(entry.getValue());
        if (key == "moduleName") {
          moduleName = val;
        } else if (key == "modulePath") {
          swiftModulePath = val.str();
        } else if (key == "docPath") {
          swiftModuleDocPath = val.str();
        } else if (key == "sourceInfoPath") {
          swiftModuleSourceInfoPath = val.str();
        } else if (key == "isFramework") {
          isFramework = parseBoolValue(val);
        } else if (key == "isSystem") {
          isSystem = parseBoolValue(val);
        } else if (key == "clangModuleMapPath") {
          clangModuleMapPath = val.str();
        } else if (key == "clangModulePath") {
          clangModulePath = val.str();
        } else if (key == "moduleCacheKey") {
          swiftModuleCacheKey = val.str();
        } else if (key == "clangModuleCacheKey") {
          clangModuleCacheKey = val.str();
        } else {
          // Being forgiving for future fields.
          continue;
        }
      }
    }
    if (moduleName.empty())
      return true;

    if (swiftModulePath.has_value()) {
      assert((clangModuleMapPath.empty() &&
              clangModulePath.empty()) &&
             "Unexpected Clang dependency details for Swift module");
      ExplicitSwiftModuleInputInfo entry(swiftModulePath.value(),
                                         swiftModuleDocPath,
                                         swiftModuleSourceInfoPath,
                                         headerDependencyPaths,
                                         isFramework,
                                         isSystem,
                                         swiftModuleCacheKey);
      swiftModuleMap.try_emplace(moduleName, std::move(entry));
    } else {
      assert((!clangModuleMapPath.empty() ||
              !clangModulePath.empty()) &&
             "Expected Clang dependency module");
      ExplicitClangModuleInputInfo entry(clangModuleMapPath,
                                         clangModulePath,
                                         isFramework,
                                         isSystem,
                                         clangModuleCacheKey);
      clangModuleMap.try_emplace(moduleName, std::move(entry));
    }

    return false;
  }

  llvm::StringSaver Saver;
};

struct ModuleInterfaceLoaderOptions {
  FrontendOptions::ActionType requestedAction =
      FrontendOptions::ActionType::EmitModuleOnly;
  bool remarkOnRebuildFromInterface = false;
  bool disableInterfaceLock = false;
  bool disableImplicitSwiftModule = false;
  bool disableBuildingInterface = false;
  bool downgradeInterfaceVerificationError = false;
  bool strictImplicitModuleContext = false;
  std::string mainExecutablePath;
  ModuleInterfaceLoaderOptions(const FrontendOptions &Opts):
    remarkOnRebuildFromInterface(Opts.RemarkOnRebuildFromModuleInterface),
    disableInterfaceLock(Opts.DisableInterfaceFileLock),
    disableImplicitSwiftModule(Opts.DisableImplicitModules),
    disableBuildingInterface(Opts.DisableBuildingInterface),
    downgradeInterfaceVerificationError(Opts.DowngradeInterfaceVerificationError),
    strictImplicitModuleContext(Opts.StrictImplicitModuleContext),
    mainExecutablePath(Opts.MainExecutablePath)
  {
    switch (Opts.RequestedAction) {
    case FrontendOptions::ActionType::TypecheckModuleFromInterface:
      requestedAction = FrontendOptions::ActionType::Typecheck;
      break;
    case FrontendOptions::ActionType::ScanDependencies:
      requestedAction = Opts.RequestedAction;
      break;
    default:
      requestedAction = FrontendOptions::ActionType::EmitModuleOnly;
      break;
    }
  }
  ModuleInterfaceLoaderOptions() = default;
};

/// Strongly typed enum that represents if we require all SILModules to have
/// OSSA modules emitted. This is implemented by incorporating this bit into the
/// module cache hash.
struct RequireOSSAModules_t {
  enum ValueTy {
    No = 0,
    Yes = 1,
  };

  ValueTy value;

  RequireOSSAModules_t(const SILOptions &opts)
      : value(opts.EnableOSSAModules ? RequireOSSAModules_t::Yes
                                     : RequireOSSAModules_t::No) {}

  operator ValueTy() const { return value; }
  explicit operator bool() const { return bool(value); }
};

class ModuleInterfaceCheckerImpl: public ModuleInterfaceChecker {
  friend class ModuleInterfaceLoader;
  ASTContext &Ctx;
  std::string CacheDir;
  std::string PrebuiltCacheDir;
  std::string BackupInterfaceDir;
  ModuleInterfaceLoaderOptions Opts;
  RequireOSSAModules_t RequiresOSSAModules;

public:
  explicit ModuleInterfaceCheckerImpl(ASTContext &Ctx, StringRef cacheDir,
                                      StringRef prebuiltCacheDir,
                                      StringRef BackupInterfaceDir,
                                      ModuleInterfaceLoaderOptions opts,
                                      RequireOSSAModules_t requiresOSSAModules)
      : Ctx(Ctx), CacheDir(cacheDir), PrebuiltCacheDir(prebuiltCacheDir),
        BackupInterfaceDir(BackupInterfaceDir),
        Opts(opts), RequiresOSSAModules(requiresOSSAModules) {}
  explicit ModuleInterfaceCheckerImpl(ASTContext &Ctx, StringRef cacheDir,
                                      StringRef prebuiltCacheDir,
                                      ModuleInterfaceLoaderOptions opts,
                                      RequireOSSAModules_t requiresOSSAModules):
    ModuleInterfaceCheckerImpl(Ctx, cacheDir, prebuiltCacheDir, StringRef(),
                               opts, requiresOSSAModules) {}
  std::vector<std::string>
  getCompiledModuleCandidatesForInterface(StringRef moduleName,
                                          StringRef interfacePath) override;

  /// Given a list of potential ready-to-use compiled modules for \p interfacePath,
  /// check if any one of them is up-to-date. If so, emit a forwarding module
  /// to the candidate binary module to \p outPath.
  bool tryEmitForwardingModule(StringRef moduleName,
                               StringRef interfacePath,
                               ArrayRef<std::string> candidates,
                               llvm::vfs::OutputBackend &backend,
                               StringRef outPath) override;
  bool isCached(StringRef DepPath);
};

/// A ModuleLoader that runs a subordinate \c CompilerInvocation and
/// \c CompilerInstance to convert .swiftinterface files to .swiftmodule
/// files on the fly, caching the resulting .swiftmodules in the module cache
/// directory, and loading the serialized .swiftmodules from there.
class ModuleInterfaceLoader : public SerializedModuleLoaderBase {
  friend class unittest::ModuleInterfaceLoaderTest;
  explicit ModuleInterfaceLoader(
      ASTContext &ctx, ModuleInterfaceCheckerImpl &InterfaceChecker,
      DependencyTracker *tracker, ModuleLoadingMode loadMode,
      ArrayRef<std::string> PreferInterfaceForModules,
      bool IgnoreSwiftSourceInfoFile)
  : SerializedModuleLoaderBase(ctx, tracker, loadMode, IgnoreSwiftSourceInfoFile),
    InterfaceChecker(InterfaceChecker),
    PreferInterfaceForModules(PreferInterfaceForModules){}

  ModuleInterfaceCheckerImpl &InterfaceChecker;
  ArrayRef<std::string> PreferInterfaceForModules;

  std::error_code findModuleFilesInDirectory(
      ImportPath::Element ModuleID, const SerializedModuleBaseName &BaseName,
      SmallVectorImpl<char> *ModuleInterfacePath,
      SmallVectorImpl<char> *ModuleInterfaceSourcePath,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
      std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
      bool SkipBuildingInterface, bool IsFramework,
      bool IsTestableDependencyLookup = false) override;

  bool isCached(StringRef DepPath) override;
public:
  static std::unique_ptr<ModuleInterfaceLoader>
  create(ASTContext &ctx, ModuleInterfaceCheckerImpl &InterfaceChecker,
         DependencyTracker *tracker, ModuleLoadingMode loadMode,
         ArrayRef<std::string> PreferInterfaceForModules = {},
         bool IgnoreSwiftSourceInfoFile = false) {
    return std::unique_ptr<ModuleInterfaceLoader>(
      new ModuleInterfaceLoader(ctx, InterfaceChecker, tracker, loadMode,
                                PreferInterfaceForModules,
                                IgnoreSwiftSourceInfoFile));
  }

  /// Append visible module names to \p names. Note that names are possibly
  /// duplicated, and not guaranteed to be ordered in any way.
  void collectVisibleTopLevelModuleNames(
      SmallVectorImpl<Identifier> &names) const override;

  /// Unconditionally build \p InPath (a swiftinterface file) to \p OutPath (as
  /// a swiftmodule file).
  ///
  /// A simplified version of the core logic in #openModuleFiles.
  static bool buildSwiftModuleFromSwiftInterface(
      SourceManager &SourceMgr, DiagnosticEngine &Diags,
      const SearchPathOptions &SearchPathOpts, const LangOptions &LangOpts,
      const ClangImporterOptions &ClangOpts, StringRef CacheDir,
      StringRef PrebuiltCacheDir, StringRef BackupInterfaceDir,
      StringRef ModuleName, StringRef InPath,
      StringRef OutPath, StringRef ABIOutputPath,
      bool SerializeDependencyHashes,
      bool TrackSystemDependencies, ModuleInterfaceLoaderOptions Opts,
      RequireOSSAModules_t RequireOSSAModules,
      bool silenceInterfaceDiagnostics);

  /// Unconditionally build \p InPath (a swiftinterface file) to \p OutPath (as
  /// a swiftmodule file).
  ///
  /// Unlike the above `buildSwiftModuleFromSwiftInterface`, this method
  /// bypasses the instantiation of a `CompilerInstance` from the compiler
  /// configuration flags in the interface and instead directly uses the
  /// supplied \p Instance
  static bool buildExplicitSwiftModuleFromSwiftInterface(
      CompilerInstance &Instance, const StringRef moduleCachePath,
      const StringRef backupInterfaceDir, const StringRef prebuiltCachePath,
      const StringRef ABIDescriptorPath, StringRef interfacePath,
      StringRef outputPath, bool ShouldSerializeDeps,
      ArrayRef<std::string> CompiledCandidates,
      DependencyTracker *tracker = nullptr);
};

struct SwiftInterfaceInfo {
  /// The compiler arguments that were encoded in the swiftinterface.
  SmallVector<const char *, 64> Arguments;

  /// The string following `swift-compiler-version:` in the swiftinterface.
  std::string CompilerVersion;

  /// The tools version of the compiler (e.g. 5.8) that emitted the
  /// swiftinterface. This is extracted from the `CompilerVersion` string.
  llvm::Optional<version::Version> CompilerToolsVersion;
};

struct InterfaceSubContextDelegateImpl: InterfaceSubContextDelegate {
private:
  SourceManager &SM;
public:
  DiagnosticEngine *Diags;
private:
  llvm::BumpPtrAllocator Allocator;
  llvm::StringSaver ArgSaver;
  std::vector<StringRef> GenericArgs;
  CompilerInvocation genericSubInvocation;
  llvm::Triple ParentInvocationTarget;

  template<typename ...ArgTypes>
  InFlightDiagnostic diagnose(StringRef interfacePath,
                              SourceLoc diagnosticLoc,
                              Diag<ArgTypes...> ID,
                              typename detail::PassArgument<ArgTypes>::type... Args) {
    return InterfaceSubContextDelegateImpl::diagnose(interfacePath, diagnosticLoc, SM, Diags, ID, std::move(Args)...);
  }
  void
  inheritOptionsForBuildingInterface(const SearchPathOptions &SearchPathOpts,
                                     const LangOptions &LangOpts,
                                     const ClangImporterOptions &clangImporterOpts,
                                     bool suppressRemarks,
                                     RequireOSSAModules_t requireOSSAModules);
  bool extractSwiftInterfaceVersionAndArgs(CompilerInvocation &subInvocation,
                                           SwiftInterfaceInfo &interfaceInfo,
                                           StringRef interfacePath,
                                           SourceLoc diagnosticLoc);
public:
  InterfaceSubContextDelegateImpl(
      SourceManager &SM, DiagnosticEngine *Diags,
      const SearchPathOptions &searchPathOpts, const LangOptions &langOpts,
      const ClangImporterOptions &clangImporterOpts,
      ModuleInterfaceLoaderOptions LoaderOpts, bool buildModuleCacheDirIfAbsent,
      StringRef moduleCachePath, StringRef prebuiltCachePath,
      StringRef backupModuleInterfaceDir,
      bool serializeDependencyHashes, bool trackSystemDependencies,
      RequireOSSAModules_t requireOSSAModules);

  template<typename ...ArgTypes>
  static InFlightDiagnostic diagnose(StringRef interfacePath,
                                     SourceLoc diagnosticLoc,
                                     SourceManager &SM,
                                     DiagnosticEngine *Diags,
                                     Diag<ArgTypes...> ID,
                                     typename detail::PassArgument<ArgTypes>::type... Args) {
    SourceLoc loc = diagnosticLoc;
    if (diagnosticLoc.isInvalid()) {
      // Diagnose this inside the interface file, if possible.
      loc = SM.getLocFromExternalSource(interfacePath, 1, 1);
    }
    return Diags->diagnose(loc, ID, std::move(Args)...);
  }

  std::error_code runInSubContext(StringRef moduleName,
                                  StringRef interfacePath,
                                  StringRef outputPath,
                                  SourceLoc diagLoc,
    llvm::function_ref<std::error_code(ASTContext&, ModuleDecl*,
                                       ArrayRef<StringRef>, ArrayRef<StringRef>,
                                       StringRef)> action) override;
  std::error_code runInSubCompilerInstance(StringRef moduleName,
                                           StringRef interfacePath,
                                           StringRef outputPath,
                                           SourceLoc diagLoc,
                                           bool silenceErrors,
    llvm::function_ref<std::error_code(SubCompilerInstanceInfo&)> action) override;

  ~InterfaceSubContextDelegateImpl() = default;

  /// includes a hash of relevant key data.
  StringRef computeCachedOutputPath(StringRef moduleName,
                                    StringRef UseInterfacePath,
                                    llvm::SmallString<256> &OutPath,
                                    StringRef &CacheHash);
  std::string getCacheHash(StringRef useInterfacePath);
};
}

#endif
