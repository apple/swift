//===------ ModuleInterfaceLoader.cpp - Loads .swiftinterface files -------===//
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

#define DEBUG_TYPE "textual-module-interface"

#include "swift/Frontend/ModuleInterfaceLoader.h"
#include "ModuleInterfaceBuilder.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/FileSystem.h"
#include "swift/AST/Module.h"
#include "swift/Basic/Platform.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/ModuleInterfaceSupport.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/Serialization/Validation.h"
#include "swift/Strings.h"
#include "clang/Basic/Module.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/xxhash.h"

using namespace swift;
using FileDependency = SerializationOptions::FileDependency;

#pragma mark - Forwarding Modules

namespace {

/// Describes a "forwarding module", that is, a .swiftmodule that's actually
/// a YAML file inside, pointing to a the original .swiftmodule but describing
/// a different dependency resolution strategy.
struct ForwardingModule {
  /// The path to the original .swiftmodule in the prebuilt cache.
  std::string underlyingModulePath;

  /// Describes a set of file-based dependencies with their size and
  /// modification time stored. This is slightly different from
  /// \c SerializationOptions::FileDependency, because this type needs to be
  /// serializable to and from YAML.
  struct Dependency {
    std::string path;
    uint64_t size;
    uint64_t lastModificationTime;
    bool isSDKRelative;
  };
  std::vector<Dependency> dependencies;
  unsigned version = 1;

  ForwardingModule() = default;
  ForwardingModule(StringRef underlyingModulePath)
  : underlyingModulePath(underlyingModulePath) {}

  /// Loads the contents of the forwarding module whose contents lie in
  /// the provided buffer, and returns a new \c ForwardingModule, or an error
  /// if the YAML could not be parsed.
  static llvm::ErrorOr<ForwardingModule> load(const llvm::MemoryBuffer &buf);

  /// Adds a given dependency to the dependencies list.
  void addDependency(StringRef path, bool isSDKRelative, uint64_t size,
                     uint64_t modTime) {
    dependencies.push_back({path.str(), size, modTime, isSDKRelative});
  }
};

} // end anonymous namespace

#pragma mark - YAML Serialization

namespace llvm {
  namespace yaml {
    template <>
    struct MappingTraits<ForwardingModule::Dependency> {
      static void mapping(IO &io, ForwardingModule::Dependency &dep) {
        io.mapRequired("mtime", dep.lastModificationTime);
        io.mapRequired("path", dep.path);
        io.mapRequired("size", dep.size);
        io.mapOptional("sdk_relative", dep.isSDKRelative, /*default*/false);
      }
    };

    template <>
    struct SequenceElementTraits<ForwardingModule::Dependency> {
      static const bool flow = false;
    };

    template <>
    struct MappingTraits<ForwardingModule> {
      static void mapping(IO &io, ForwardingModule &module) {
        io.mapRequired("path", module.underlyingModulePath);
        io.mapRequired("dependencies", module.dependencies);
        io.mapRequired("version", module.version);
      }
    };
  }
} // end namespace llvm

llvm::ErrorOr<ForwardingModule>
ForwardingModule::load(const llvm::MemoryBuffer &buf) {
  llvm::yaml::Input yamlIn(buf.getBuffer());
  ForwardingModule fwd;
  yamlIn >> fwd;
  if (yamlIn.error())
    return yamlIn.error();

  // We only currently support Version 1 of the forwarding module format.
  if (fwd.version != 1)
    return std::make_error_code(std::errc::not_supported);
  return std::move(fwd);
}

#pragma mark - Module Discovery

namespace {

/// The result of a search for a module either alongside an interface, in the
/// module cache, or in the prebuilt module cache.
class DiscoveredModule {
  /// The kind of module we've found.
  enum class Kind {
    /// A module that's either alongside the swiftinterface or in the
    /// module cache.
    Normal,

    /// A module that resides in the prebuilt cache, and has hash-based
    /// dependencies.
    Prebuilt,

    /// A 'forwarded' module. This is a module in the prebuilt cache, but whose
    /// dependencies live in a forwarding module.
    /// \sa ForwardingModule.
    Forwarded
  };

  /// The kind of module that's been discovered.
  const Kind kind;

  DiscoveredModule(StringRef path, Kind kind,
    std::unique_ptr<llvm::MemoryBuffer> moduleBuffer)
    : kind(kind), moduleBuffer(std::move(moduleBuffer)), path(path) {}

public:
  /// The contents of the .swiftmodule, if we've read it while validating
  /// dependencies.
  std::unique_ptr<llvm::MemoryBuffer> moduleBuffer;

  /// The path to the discovered serialized .swiftmodule on disk.
  const std::string path;

  /// Creates a \c Normal discovered module.
  static DiscoveredModule normal(StringRef path,
      std::unique_ptr<llvm::MemoryBuffer> moduleBuffer) {
    return { path, Kind::Normal, std::move(moduleBuffer) };
  }

  /// Creates a \c Prebuilt discovered module.
  static DiscoveredModule prebuilt(
      StringRef path, std::unique_ptr<llvm::MemoryBuffer> moduleBuffer) {
    return { path, Kind::Prebuilt, std::move(moduleBuffer) };
  }

  /// Creates a \c Forwarded discovered module, whose dependencies have been
  /// externally validated by a \c ForwardingModule.
  static DiscoveredModule forwarded(
      StringRef path, std::unique_ptr<llvm::MemoryBuffer> moduleBuffer) {
    return { path, Kind::Forwarded, std::move(moduleBuffer) };
  }

  bool isNormal() const { return kind == Kind::Normal; }
  bool isPrebuilt() const { return kind == Kind::Prebuilt; }
  bool isForwarded() const { return kind == Kind::Forwarded; }
};

} // end anonymous namespace

#pragma mark - Common utilities

namespace path = llvm::sys::path;

static bool serializedASTLooksValid(const llvm::MemoryBuffer &buf) {
  auto VI = serialization::validateSerializedAST(buf.getBuffer());
  return VI.status == serialization::Status::Valid;
}

#pragma mark - Module Loading

namespace {

/// Keeps track of the various reasons the module interface loader needed to
/// fall back and rebuild a module from its interface.
struct ModuleRebuildInfo {
  enum class ModuleKind {
    Normal,
    Cached,
    Forwarding,
    Prebuilt
  };
  struct OutOfDateModule {
    std::string path;
    Optional<serialization::Status> serializationStatus;
    ModuleKind kind;
    SmallVector<std::string, 10> outOfDateDependencies;
    SmallVector<std::string, 10> missingDependencies;
  };
  SmallVector<OutOfDateModule, 3> outOfDateModules;

  OutOfDateModule &getOrInsertOutOfDateModule(StringRef path) {
    for (auto &mod : outOfDateModules) {
      if (mod.path == path) return mod;
    }
    outOfDateModules.push_back({path.str(), None, ModuleKind::Normal, {}, {}});
    return outOfDateModules.back();
  }

  /// Sets the kind of a module that failed to load.
  void setModuleKind(StringRef path, ModuleKind kind) {
    getOrInsertOutOfDateModule(path).kind = kind;
  }

  /// Sets the serialization status of the module at \c path. If this is
  /// anything other than \c Valid, a note will be added stating why the module
  /// was invalid.
  void setSerializationStatus(StringRef path, serialization::Status status) {
    getOrInsertOutOfDateModule(path).serializationStatus = status;
  }

  /// Registers an out-of-date dependency at \c depPath for the module
  /// at \c modulePath.
  void addOutOfDateDependency(StringRef modulePath, StringRef depPath) {
    getOrInsertOutOfDateModule(modulePath)
        .outOfDateDependencies.push_back(depPath.str());
  }

  /// Registers a missing dependency at \c depPath for the module
  /// at \c modulePath.
  void addMissingDependency(StringRef modulePath, StringRef depPath) {
    getOrInsertOutOfDateModule(modulePath)
        .missingDependencies.push_back(depPath.str());
  }

  /// Determines if we saw the given module path and registered is as out of
  /// date.
  bool sawOutOfDateModule(StringRef modulePath) {
    for (auto &mod : outOfDateModules)
      if (mod.path == modulePath)
        return true;
    return false;
  }

  const char *invalidModuleReason(serialization::Status status) {
    using namespace serialization;
    switch (status) {
    case Status::FormatTooOld:
      return "compiled with an older version of the compiler";
    case Status::FormatTooNew:
      return "compiled with a newer version of the compiler";
    case Status::Malformed:
      return "malformed";
    case Status::TargetIncompatible:
      return "compiled for a different target platform";
    case Status::TargetTooNew:
      return "target platform newer than current platform";
    default: return nullptr;
    }
  }

  /// Emits a diagnostic for all out-of-date compiled or forwarding modules
  /// encountered while trying to load a module.
  template<typename... DiagArgs>
  void diagnose(ASTContext &ctx, DiagnosticEngine &diags,
                StringRef prebuiltCacheDir, SourceLoc loc,
                DiagArgs &&...diagArgs) {
    diags.diagnose(loc, std::forward<DiagArgs>(diagArgs)...);
    auto SDKVer = getSDKBuildVersion(ctx.SearchPathOpts.SDKPath);
    llvm::SmallString<64> buffer = prebuiltCacheDir;
    llvm::sys::path::append(buffer, "SystemVersion.plist");
    auto PBMVer = getSDKBuildVersionFromPlist(buffer.str());
    if (!SDKVer.empty() && !PBMVer.empty()) {
      // Remark the potential version difference.
      diags.diagnose(loc, diag::sdk_version_pbm_version, SDKVer,
                         PBMVer);
    }
    // We may have found multiple failing modules, that failed for different
    // reasons. Emit a note for each of them.
    for (auto &mod : outOfDateModules) {
      diags.diagnose(loc, diag::out_of_date_module_here,
                         (unsigned)mod.kind, mod.path);

      // Diagnose any out-of-date dependencies in this module.
      for (auto &dep : mod.outOfDateDependencies) {
        diags.diagnose(loc, diag::module_interface_dependency_out_of_date,
                           dep);
      }

      // Diagnose any missing dependencies in this module.
      for (auto &dep : mod.missingDependencies) {
        diags.diagnose(loc, diag::module_interface_dependency_missing, dep);
      }

      // If there was a compiled module that wasn't able to be read, diagnose
      // the reason we couldn't read it.
      if (auto status = mod.serializationStatus) {
        if (auto reason = invalidModuleReason(*status)) {
          diags.diagnose(loc, diag::compiled_module_invalid_reason,
              mod.path, reason);
        } else {
          diags.diagnose(loc, diag::compiled_module_invalid, mod.path);
        }
      }
    }
  }
};

/// Handles the details of loading module interfaces as modules, and will
/// do the necessary lookup to determine if we should be loading from the
/// normal cache, the prebuilt cache, a module adjacent to the interface, or
/// a module that we'll build from a module interface.
class ModuleInterfaceLoaderImpl {
  friend class swift::ModuleInterfaceLoader;
  friend class swift::ModuleInterfaceCheckerImpl;
  ASTContext &ctx;
  llvm::vfs::FileSystem &fs;
  DiagnosticEngine &diags;
  ModuleRebuildInfo rebuildInfo;
  const StringRef modulePath;
  const std::string interfacePath;
  const StringRef moduleName;
  const StringRef prebuiltCacheDir;
  const StringRef backupInterfaceDir;
  const StringRef cacheDir;
  const SourceLoc diagnosticLoc;
  DependencyTracker *const dependencyTracker;
  const ModuleLoadingMode loadMode;
  ModuleInterfaceLoaderOptions Opts;
  RequireOSSAModules_t requiresOSSAModules;

  ModuleInterfaceLoaderImpl(
      ASTContext &ctx, StringRef modulePath, StringRef interfacePath,
      StringRef moduleName, StringRef cacheDir, StringRef prebuiltCacheDir,
      StringRef backupInterfaceDir,
      SourceLoc diagLoc, ModuleInterfaceLoaderOptions Opts,
      RequireOSSAModules_t requiresOSSAModules,
      DependencyTracker *dependencyTracker = nullptr,
      ModuleLoadingMode loadMode = ModuleLoadingMode::PreferSerialized)
      : ctx(ctx), fs(*ctx.SourceMgr.getFileSystem()), diags(ctx.Diags),
        modulePath(modulePath), interfacePath(interfacePath),
        moduleName(moduleName),
        prebuiltCacheDir(prebuiltCacheDir),
        backupInterfaceDir(backupInterfaceDir),
        cacheDir(cacheDir), diagnosticLoc(diagLoc),
        dependencyTracker(dependencyTracker), loadMode(loadMode), Opts(Opts),
        requiresOSSAModules(requiresOSSAModules) {}

  /// Constructs the full path of the dependency \p dep by prepending the SDK
  /// path if necessary.
  StringRef getFullDependencyPath(const FileDependency &dep,
                                  SmallVectorImpl<char> &scratch) const {
    if (!dep.isSDKRelative())
      return dep.getPath();

    path::native(ctx.SearchPathOpts.SDKPath, scratch);
    llvm::sys::path::append(scratch, dep.getPath());
    return StringRef(scratch.data(), scratch.size());
  }

  enum class DependencyStatus {
    UpToDate,
    OutOfDate,
    Missing
  };

  // Checks that a dependency read from the cached module is up to date compared
  // to the interface file it represents.
  DependencyStatus checkDependency(StringRef modulePath,
                                   const FileDependency &dep,
                                   StringRef fullPath) {
    auto status = fs.status(fullPath);
    if (!status)
      return DependencyStatus::Missing;

    // If the sizes differ, then we know the file has changed.
    if (status->getSize() != dep.getSize())
      return DependencyStatus::OutOfDate;

    // Otherwise, if this dependency is verified by modification time, check
    // it vs. the modification time of the file.
    if (dep.isModificationTimeBased()) {
      uint64_t mtime =
        status->getLastModificationTime().time_since_epoch().count();
      return mtime == dep.getModificationTime() ?
          DependencyStatus::UpToDate :
          DependencyStatus::OutOfDate;
    }

    // Slow path: if the dependency is verified by content hash, check it vs.
    // the hash of the file.
    auto buf = fs.getBufferForFile(fullPath, /*FileSize=*/-1,
                                   /*RequiresNullTerminator=*/false);
    if (!buf)
      return DependencyStatus::Missing;

    return xxHash64(buf.get()->getBuffer()) == dep.getContentHash() ?
        DependencyStatus::UpToDate :
        DependencyStatus::OutOfDate;
  }

  std::string getBackupPublicModuleInterfacePath() {
    return getBackupPublicModuleInterfacePath(ctx.SourceMgr, backupInterfaceDir,
                                              moduleName, interfacePath);
  }

  static std::string getBackupPublicModuleInterfacePath(SourceManager &SM,
                                                        StringRef backupInterfaceDir,
                                                        StringRef moduleName,
                                                        StringRef interfacePath) {
    if (backupInterfaceDir.empty())
      return std::string();
    auto &fs = *SM.getFileSystem();
    auto fileName = llvm::sys::path::filename(interfacePath);
    {
      llvm::SmallString<256> path(backupInterfaceDir);
      llvm::sys::path::append(path, llvm::Twine(moduleName) + ".swiftmodule");
      llvm::sys::path::append(path, fileName);
      if (fs.exists(path.str())) {
        return path.str().str();
      }
    }
    {
      llvm::SmallString<256> path(backupInterfaceDir);
      llvm::sys::path::append(path, fileName);
      if (fs.exists(path.str())) {
        return path.str().str();
      }
    }
    return std::string();
  }

  // Check if all the provided file dependencies are up-to-date compared to
  // what's currently on disk.
  bool dependenciesAreUpToDate(StringRef modulePath,
                               ArrayRef<FileDependency> deps,
                               bool skipSystemDependencies) {
    SmallString<128> SDKRelativeBuffer;
    for (auto &in : deps) {
      if (skipSystemDependencies && in.isSDKRelative() &&
          in.isModificationTimeBased()) {
        continue;
      }
      StringRef fullPath = getFullDependencyPath(in, SDKRelativeBuffer);
      switch (checkDependency(modulePath, in, fullPath)) {
      case DependencyStatus::UpToDate:
        LLVM_DEBUG(llvm::dbgs() << "Dep " << fullPath << " is up to date\n");
        break;
      case DependencyStatus::OutOfDate:
        LLVM_DEBUG(llvm::dbgs() << "Dep " << fullPath << " is out of date\n");
        rebuildInfo.addOutOfDateDependency(modulePath, fullPath);
        return false;
      case DependencyStatus::Missing:
        LLVM_DEBUG(llvm::dbgs() << "Dep " << fullPath << " is missing\n");
        rebuildInfo.addMissingDependency(modulePath, fullPath);
        return false;
      }
    }
    return true;
  }

  // Check that the output .swiftmodule file is at least as new as all the
  // dependencies it read when it was built last time.
  bool serializedASTBufferIsUpToDate(
      StringRef path, const llvm::MemoryBuffer &buf,
      SmallVectorImpl<FileDependency> &allDeps) {

    // Clear the existing dependencies, because we're going to re-fill them
    // in validateSerializedAST.
    allDeps.clear();

    LLVM_DEBUG(llvm::dbgs() << "Validating deps of " << path << "\n");
    auto validationInfo = serialization::validateSerializedAST(
        buf.getBuffer(), /*ExtendedValidationInfo=*/nullptr, &allDeps);

    if (validationInfo.status != serialization::Status::Valid) {
      rebuildInfo.setSerializationStatus(path, validationInfo.status);
      return false;
    }

    bool skipCheckingSystemDependencies =
        ctx.SearchPathOpts.DisableModulesValidateSystemDependencies;
    return dependenciesAreUpToDate(path, allDeps,
                                   skipCheckingSystemDependencies);
  }

  // Check that the output .swiftmodule file is at least as new as all the
  // dependencies it read when it was built last time.
  bool swiftModuleIsUpToDate(
    StringRef modulePath, SmallVectorImpl<FileDependency> &AllDeps,
    std::unique_ptr<llvm::MemoryBuffer> &moduleBuffer) {
    auto OutBuf = fs.getBufferForFile(modulePath);
    if (!OutBuf)
      return false;
    moduleBuffer = std::move(*OutBuf);
    return serializedASTBufferIsUpToDate(modulePath, *moduleBuffer, AllDeps);
  }

  // Check that a "forwarding" .swiftmodule file is at least as new as all the
  // dependencies it read when it was built last time. Requires that the
  // forwarding module has been loaded from disk.
  bool forwardingModuleIsUpToDate(
      StringRef path, const ForwardingModule &fwd,
      SmallVectorImpl<FileDependency> &deps,
      std::unique_ptr<llvm::MemoryBuffer> &moduleBuffer) {

    // Clear the existing dependencies, because we're going to re-fill them
    // from the forwarding module.
    deps.clear();

    LLVM_DEBUG(llvm::dbgs() << "Validating deps of " << path << "\n");

    // First, make sure the underlying module path exists and is valid.
    auto modBuf = fs.getBufferForFile(fwd.underlyingModulePath);
    if (!modBuf || !serializedASTLooksValid(*modBuf.get()))
      return false;

    // Next, check the dependencies in the forwarding file.
    for (auto &dep : fwd.dependencies) {
      deps.push_back(
        FileDependency::modTimeBased(
          dep.path, dep.isSDKRelative, dep.size, dep.lastModificationTime));
    }

    bool skipCheckingSystemDependencies =
        ctx.SearchPathOpts.DisableModulesValidateSystemDependencies;
    if (!dependenciesAreUpToDate(path, deps, skipCheckingSystemDependencies))
      return false;

    moduleBuffer = std::move(*modBuf);
    return true;
  }

  bool canInterfaceHavePrebuiltModule() {
    StringRef sdkPath = ctx.SearchPathOpts.SDKPath;
    if (!sdkPath.empty() &&
        hasPrefix(path::begin(interfacePath), path::end(interfacePath),
                  path::begin(sdkPath), path::end(sdkPath))) {
      return !StringRef(interfacePath).endswith(".private.swiftinterface");
    }
    return false;
  }

  Optional<StringRef>
  computePrebuiltModulePath(llvm::SmallString<256> &scratch) {
    namespace path = llvm::sys::path;

    // Check if this is a public interface file from the SDK.
    if (!canInterfaceHavePrebuiltModule())
      return None;

    // Assemble the expected path: $PREBUILT_CACHE/Foo.swiftmodule or
    // $PREBUILT_CACHE/Foo.swiftmodule/arch.swiftmodule. Note that there's no
    // cache key here.
    scratch = prebuiltCacheDir;

    // FIXME: Would it be possible to only have architecture-specific names
    // here? Then we could skip this check.
    StringRef inParentDirName =
      path::filename(path::parent_path(interfacePath));
    if (path::extension(inParentDirName) == ".swiftmodule") {
      assert(path::stem(inParentDirName) == moduleName);
      path::append(scratch, inParentDirName);
    }
    path::append(scratch, path::filename(modulePath));

    // If there isn't a file at this location, skip returning a path.
    if (!fs.exists(scratch))
      return None;

    return scratch.str();
  }

  /// Hack to deal with build systems (including the Swift standard library, at
  /// the time of this comment) that aren't yet using target-specific names for
  /// multi-target swiftmodules, in case the prebuilt cache is.
  Optional<StringRef>
  computeFallbackPrebuiltModulePath(llvm::SmallString<256> &scratch) {
    namespace path = llvm::sys::path;
    StringRef sdkPath = ctx.SearchPathOpts.SDKPath;

    // Check if this is a public interface file from the SDK.
    if (sdkPath.empty() ||
        !hasPrefix(path::begin(interfacePath), path::end(interfacePath),
                   path::begin(sdkPath), path::end(sdkPath)) ||
        StringRef(interfacePath).endswith(".private.swiftinterface"))
      return None;

    // If the module isn't target-specific, there's no fallback path.
    StringRef inParentDirName =
        path::filename(path::parent_path(interfacePath));
    if (path::extension(inParentDirName) != ".swiftmodule")
      return None;

    // If the interface is already using the target-specific name, there's
    // nothing else to try.
    auto normalizedTarget = getTargetSpecificModuleTriple(ctx.LangOpts.Target);
    if (path::stem(modulePath) == normalizedTarget.str())
      return None;

    // Assemble the expected path:
    // $PREBUILT_CACHE/Foo.swiftmodule/target.swiftmodule. Note that there's no
    // cache key here.
    scratch = prebuiltCacheDir;
    path::append(scratch, inParentDirName);
    path::append(scratch, normalizedTarget.str());
    scratch += ".swiftmodule";

    // If there isn't a file at this location, skip returning a path.
    if (!fs.exists(scratch))
      return None;

    return scratch.str();
  }

  bool isInResourceDir(StringRef path) {
    StringRef resourceDir = ctx.SearchPathOpts.RuntimeResourcePath;
    if (resourceDir.empty()) return false;
    return path.startswith(resourceDir);
  }

  std::pair<std::string, std::string> getCompiledModuleCandidates() {
    // If we require ossa modules, then we /always/ rebuild the module interface
    // regardless of the module loading mode.
    if (requiresOSSAModules) {
      return {};
    }

    std::pair<std::string, std::string> result;
    // Keep track of whether we should attempt to load a .swiftmodule adjacent
    // to the .swiftinterface.
    bool shouldLoadAdjacentModule = true;

    switch (loadMode) {
    case ModuleLoadingMode::OnlyInterface:
      // Always skip both the caches and adjacent modules, and always build the
      // module interface.
      return {};
    case ModuleLoadingMode::PreferInterface:
      // If we're in the load mode that prefers .swiftinterfaces, specifically
      // skip the module adjacent to the interface, but use the caches if
      // they're present.
      shouldLoadAdjacentModule = false;
      break;
    case ModuleLoadingMode::PreferSerialized:
      // The rest of the function should be covered by this.
      break;
    case ModuleLoadingMode::OnlySerialized:
      llvm_unreachable("module interface loader should not have been created");
    }
    // [NOTE: ModuleInterfaceLoader-defer-to-ImplicitSerializedModuleLoader]
    // If there's a module adjacent to the .swiftinterface that we can
    // _likely_ load (it validates OK and is up to date), bail early with
    // errc::not_supported, so the next (serialized) loader in the chain will
    // load it.
    // Alternately, if there's a .swiftmodule present but we can't even
    // read it (for whatever reason), we should let the other module loader
    // diagnose it.

    if (shouldLoadAdjacentModule) {
      if (fs.exists(modulePath)) {
        result.first = modulePath.str();
      }
    }

    // If we have a prebuilt cache path, check that too if the interface comes
    // from the SDK.
    if (!prebuiltCacheDir.empty()) {
      llvm::SmallString<256> scratch;
      std::unique_ptr<llvm::MemoryBuffer> moduleBuffer;
      Optional<StringRef> path = computePrebuiltModulePath(scratch);
      if (!path) {
        // Hack: deal with prebuilds of modules that still use the target-based
        // names.
        path = computeFallbackPrebuiltModulePath(scratch);
      }
      if (path) {
        if (fs.exists(*path)) {
          result.second = path->str();
        }
      }
    }

    return result;
  }

  llvm::ErrorOr<DiscoveredModule>
  discoverUpToDateCompiledModuleForInterface(SmallVectorImpl<FileDependency> &deps,
                                             std::string &UsableModulePath) {
    std::string adjacentMod, prebuiltMod;
    std::tie(adjacentMod, prebuiltMod) = getCompiledModuleCandidates();
    if (!adjacentMod.empty()) {
      auto adjacentModuleBuffer = fs.getBufferForFile(adjacentMod);
      if (adjacentModuleBuffer) {
        if (serializedASTBufferIsUpToDate(adjacentMod, *adjacentModuleBuffer.get(),
                                          deps)) {
          LLVM_DEBUG(llvm::dbgs() << "Found up-to-date module at "
                                  << adjacentMod
                                  << "; deferring to serialized module loader\n");
          UsableModulePath = adjacentMod;
          return std::make_error_code(std::errc::not_supported);
        } else if (isInResourceDir(adjacentMod) &&
                   loadMode == ModuleLoadingMode::PreferSerialized) {
          // Special-case here: If we're loading a .swiftmodule from the resource
          // dir adjacent to the compiler, defer to the serialized loader instead
          // of falling back. This is mainly to support development of Swift,
          // where one might change the module format version but forget to
          // recompile the standard library. If that happens, don't fall back
          // and silently recompile the standard library -- instead, error like
          // we used to.
          LLVM_DEBUG(llvm::dbgs() << "Found out-of-date module in the "
                                     "resource-dir at "
                                  << adjacentMod
                                  << "; deferring to serialized module loader "
                                     "to diagnose\n");
          return std::make_error_code(std::errc::not_supported);
        } else {
          LLVM_DEBUG(llvm::dbgs() << "Found out-of-date module at "
                                  << adjacentMod << "\n");
          rebuildInfo.setModuleKind(adjacentMod,
                                    ModuleRebuildInfo::ModuleKind::Normal);
        }
      } else {
        LLVM_DEBUG(llvm::dbgs() << "Found unreadable module at "
                                << adjacentMod
                                << "; deferring to serialized module loader\n");
        return std::make_error_code(std::errc::not_supported);
      }
    }

    if(!prebuiltMod.empty()) {
      std::unique_ptr<llvm::MemoryBuffer> moduleBuffer;
      if (swiftModuleIsUpToDate(prebuiltMod, deps, moduleBuffer)) {
        LLVM_DEBUG(llvm::dbgs() << "Found up-to-date prebuilt module at "
                                << prebuiltMod << "\n");
        UsableModulePath = prebuiltMod;
        return DiscoveredModule::prebuilt(prebuiltMod, std::move(moduleBuffer));
      } else {
        LLVM_DEBUG(llvm::dbgs() << "Found out-of-date prebuilt module at "
                                << prebuiltMod << "\n");
        rebuildInfo.setModuleKind(prebuiltMod,
                                  ModuleRebuildInfo::ModuleKind::Prebuilt);
      }
    }
    // We cannot find any proper compiled module to use.
    return std::make_error_code(std::errc::no_such_file_or_directory);
  }

  /// Finds the most appropriate .swiftmodule, whose dependencies are up to
  /// date, that we can load for the provided .swiftinterface file.
  llvm::ErrorOr<DiscoveredModule> discoverUpToDateModuleForInterface(
    StringRef cachedOutputPath,
    SmallVectorImpl<FileDependency> &deps) {

    // First, check the cached module path. Whatever's in this cache represents
    // the most up-to-date knowledge we have about the module.
    if (auto cachedBufOrError = fs.getBufferForFile(cachedOutputPath)) {
      auto buf = std::move(*cachedBufOrError);

      // Check to see if the module is a serialized AST. If it's not, then we're
      // probably dealing with a Forwarding Module, which is a YAML file.
      bool isForwardingModule =
        !serialization::isSerializedAST(buf->getBuffer());

      // If it's a forwarding module, load the YAML file from disk and check
      // if it's up-to-date.
      if (isForwardingModule) {
        if (auto forwardingModule = ForwardingModule::load(*buf)) {
          std::unique_ptr<llvm::MemoryBuffer> moduleBuffer;
          if (forwardingModuleIsUpToDate(cachedOutputPath,
                                         *forwardingModule, deps,
                                         moduleBuffer)) {
            LLVM_DEBUG(llvm::dbgs() << "Found up-to-date forwarding module at "
                                    << cachedOutputPath << "\n");
            return DiscoveredModule::forwarded(
              forwardingModule->underlyingModulePath, std::move(moduleBuffer));
          }

          LLVM_DEBUG(llvm::dbgs() << "Found out-of-date forwarding module at "
                     << cachedOutputPath << "\n");
          rebuildInfo.setModuleKind(cachedOutputPath,
                                    ModuleRebuildInfo::ModuleKind::Forwarding);
        }
      // Otherwise, check if the AST buffer itself is up to date.
      } else if (serializedASTBufferIsUpToDate(cachedOutputPath, *buf, deps)) {
        LLVM_DEBUG(llvm::dbgs() << "Found up-to-date cached module at "
                                << cachedOutputPath << "\n");
        return DiscoveredModule::normal(cachedOutputPath, std::move(buf));
      } else {
        LLVM_DEBUG(llvm::dbgs() << "Found out-of-date cached module at "
                   << cachedOutputPath << "\n");
        rebuildInfo.setModuleKind(cachedOutputPath,
                                  ModuleRebuildInfo::ModuleKind::Cached);
      }
    }
    std::string usableModulePath;
    return discoverUpToDateCompiledModuleForInterface(deps, usableModulePath);
  }

  /// Writes the "forwarding module" that will forward to a module in the
  /// prebuilt cache.
  ///
  /// Since forwarding modules track dependencies separately from the module
  /// they point to, we'll need to grab the up-to-date file status while doing
  /// this. If the write was successful, it also updates the
  /// list of dependencies to match what was written to the forwarding file.
  bool writeForwardingModuleAndUpdateDeps(
      const DiscoveredModule &mod, StringRef outputPath,
      SmallVectorImpl<FileDependency> &deps) {
    assert(mod.isPrebuilt() &&
           "cannot write forwarding file for non-prebuilt module");
    ForwardingModule fwd(mod.path);

    SmallVector<FileDependency, 16> depsAdjustedToMTime;

    // FIXME: We need to avoid re-statting all these dependencies, otherwise
    //        we may record out-of-date information.
    SmallString<128> SDKRelativeBuffer;
    auto addDependency = [&](FileDependency dep) -> FileDependency {
      auto status = fs.status(getFullDependencyPath(dep, SDKRelativeBuffer));
      uint64_t mtime =
        status->getLastModificationTime().time_since_epoch().count();
      fwd.addDependency(dep.getPath(), dep.isSDKRelative(), status->getSize(),
                        mtime);

      // Construct new FileDependency matching what we've added to the
      // forwarding module.
      return FileDependency::modTimeBased(dep.getPath(), dep.isSDKRelative(),
                                          status->getSize(), mtime);
    };

    // Add the prebuilt module as a dependency of the forwarding module, but
    // don't add it to the outer dependency list.
    (void)addDependency(FileDependency::hashBased(fwd.underlyingModulePath,
                                                  /*SDKRelative*/false,
                                                  /*size*/0, /*hash*/0));

    // Add all the dependencies from the prebuilt module, and update our list
    // of dependencies to reflect what's recorded in the forwarding module.
    for (auto dep : deps) {
      auto adjustedDep = addDependency(dep);
      depsAdjustedToMTime.push_back(adjustedDep);
    }

    // Create the module cache if we haven't created it yet.
    StringRef parentDir = path::parent_path(outputPath);
    (void)llvm::sys::fs::create_directories(parentDir);

    auto hadError = withOutputFile(diags, outputPath,
      [&](llvm::raw_pwrite_stream &out) {
        llvm::yaml::Output yamlWriter(out);
        yamlWriter << fwd;
        return false;
      });

    if (hadError)
      return true;

    // If and only if we succeeded writing the forwarding file, update the
    // provided list of dependencies.
    deps = depsAdjustedToMTime;
    return false;
  }

  /// Looks up the best module to load for a given interface, and returns a
  /// buffer of the module's contents. Also reports the module's dependencies
  /// to the parent \c dependencyTracker if it came from the cache, or was built
  /// from the given interface. See the main comment in
  /// \c ModuleInterfaceLoader.h for an explanation of the module
  /// loading strategy.
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
  findOrBuildLoadableModule() {

    // Track system dependencies if the parent tracker is set to do so.
    bool trackSystemDependencies = false;
    if (dependencyTracker) {
      auto ClangDependencyTracker = dependencyTracker->getClangCollector();
      trackSystemDependencies = ClangDependencyTracker->needSystemDependencies();
    }
    InterfaceSubContextDelegateImpl astDelegate(
        ctx.SourceMgr, &ctx.Diags, ctx.SearchPathOpts, ctx.LangOpts,
        ctx.ClangImporterOpts, Opts,
        /*buildModuleCacheDirIfAbsent*/ true, cacheDir, prebuiltCacheDir,
        backupInterfaceDir,
        /*serializeDependencyHashes*/ false, trackSystemDependencies,
        requiresOSSAModules);

    // Compute the output path if we're loading or emitting a cached module.
    llvm::SmallString<256> cachedOutputPath;
    StringRef CacheHash;
    astDelegate.computeCachedOutputPath(moduleName, interfacePath,
                                        cachedOutputPath, CacheHash);

    // Try to find the right module for this interface, either alongside it,
    // in the cache, or in the prebuilt cache.
    SmallVector<FileDependency, 16> allDeps;
    auto moduleOrErr =
      discoverUpToDateModuleForInterface(cachedOutputPath, allDeps);

    // If we errored with anything other than 'no such file or directory',
    // fail this load and let the other module loader diagnose it.
    if (!moduleOrErr &&
        moduleOrErr.getError() != std::errc::no_such_file_or_directory)
      return moduleOrErr.getError();

    // We discovered a module! Return that module's buffer so we can load it.
    if (moduleOrErr) {
      auto module = std::move(moduleOrErr.get());

      // If it's prebuilt, use this time to generate a forwarding module and
      // update the dependencies to use modification times.
      if (module.isPrebuilt())
        if (writeForwardingModuleAndUpdateDeps(module, cachedOutputPath,
                                               allDeps))
          return std::make_error_code(std::errc::not_supported);

      // Report the module's dependencies to the dependencyTracker
      if (dependencyTracker) {
        SmallString<128> SDKRelativeBuffer;
        for (auto &dep: allDeps) {
          StringRef fullPath = getFullDependencyPath(dep, SDKRelativeBuffer);
          dependencyTracker->addDependency(fullPath,
                                           /*IsSystem=*/dep.isSDKRelative());
        }
      }

      return std::move(module.moduleBuffer);
    }
    // If building from interface is disabled, return error.
    if (Opts.disableBuildingInterface) {
      return std::make_error_code(std::errc::not_supported);
    }

    std::unique_ptr<llvm::MemoryBuffer> moduleBuffer;

    // We didn't discover a module corresponding to this interface.
    // Diagnose that we didn't find a loadable module, if we were asked to.
    //
    // Note that we use `diags` so that we emit this remark even when we're
    // emitting other messages to `emptyDiags` (see below); these act as status
    // messages to explain what's taking so long.
    auto remarkRebuildAll = [&]() {
      rebuildInfo.diagnose(ctx, diags, prebuiltCacheDir, diagnosticLoc,
                           diag::rebuilding_module_from_interface, moduleName,
                           interfacePath);
    };
    // Diagnose only for the standard library; it should be prebuilt in typical
    // workflows, but if it isn't, building it may take several minutes and a
    // lot of memory, so users may think the compiler is busy-hung.
    auto remarkRebuildStdlib = [&]() {
      if (moduleName != "Swift")
        return;
      
      auto moduleTriple = getTargetSpecificModuleTriple(ctx.LangOpts.Target);
      rebuildInfo.diagnose(ctx, diags, prebuiltCacheDir, SourceLoc(),
                           diag::rebuilding_stdlib_from_interface,
                           moduleTriple.str());
    };
    auto remarkRebuild = Opts.remarkOnRebuildFromInterface
                       ? llvm::function_ref<void()>(remarkRebuildAll)
                       : remarkRebuildStdlib;

    bool failed = false;
    std::string backupPath = getBackupPublicModuleInterfacePath();
    {
      DiagnosticEngine emptyDiags(ctx.SourceMgr);
      std::unique_ptr<llvm::SaveAndRestore<DiagnosticEngine*>> saver;
      DiagnosticEngine *diagsToUse = &ctx.Diags;
      // Avoid emitting diagnostics if we have a backup interface to use.
      // If we succeed in building this canonical interface, it's not interesting
      // to see those diagnostics.
      // If we failed in build, we will use the back up interface and it's interesting
      // to see diagnostics there.
      if (!backupPath.empty()) {
        diagsToUse = &emptyDiags;
        saver = std::make_unique<llvm::SaveAndRestore<DiagnosticEngine*>>(
          astDelegate.Diags, diagsToUse);
      }
      // Set up a builder if we need to build the module. It'll also set up
      // the genericSubInvocation we'll need to use to compute the cache paths.
      ModuleInterfaceBuilder builder(
        ctx.SourceMgr, diagsToUse,
        astDelegate, interfacePath, moduleName, cacheDir,
        prebuiltCacheDir, backupInterfaceDir,
        Opts.disableInterfaceLock, diagnosticLoc,
        dependencyTracker);
      // If we found an out-of-date .swiftmodule, we still want to add it as
      // a dependency of the .swiftinterface. That way if it's updated, but
      // the .swiftinterface remains the same, we invalidate the cache and
      // check the new .swiftmodule, because it likely has more information
      // about the state of the world.
      if (rebuildInfo.sawOutOfDateModule(modulePath))
        builder.addExtraDependency(modulePath);
      failed = builder.buildSwiftModule(cachedOutputPath,
                                        /*shouldSerializeDeps*/true,
                                        &moduleBuffer, remarkRebuild);
    }
    if (!failed) {
      // If succeeded, we are done.
      assert(moduleBuffer &&
             "failed to write module buffer but returned success?");
      return std::move(moduleBuffer);
    } else if (backupPath.empty()) {
      // If failed and we don't have a backup interface file, return error code.
      return std::make_error_code(std::errc::invalid_argument);
    }
    assert(failed);
    assert(!backupPath.empty());
    while (1) {
      diags.diagnose(diagnosticLoc, diag::interface_file_backup_used,
                     interfacePath, backupPath);
      // Set up a builder if we need to build the module. It'll also set up
      // the genericSubInvocation we'll need to use to compute the cache paths.
      ModuleInterfaceBuilder fallbackBuilder(
        ctx.SourceMgr, &ctx.Diags, astDelegate, backupPath, moduleName, cacheDir,
        prebuiltCacheDir, backupInterfaceDir,
        Opts.disableInterfaceLock, diagnosticLoc,
        dependencyTracker);
      if (rebuildInfo.sawOutOfDateModule(modulePath))
        fallbackBuilder.addExtraDependency(modulePath);
      // Add the canonical interface path as a dependency of this module.
      // This ensures that after the user manually fixed the canonical interface
      // file and removed the fallback interface file, we can rebuild the cache.
      fallbackBuilder.addExtraDependency(interfacePath);
      // Use cachedOutputPath as the output file path. This output path was
      // calcualted using the canonical interface file path to make sure we
      // can find it from the canonical interface file.
      auto failedAgain = fallbackBuilder.buildSwiftModule(cachedOutputPath,
          /*shouldSerializeDeps*/true, &moduleBuffer, remarkRebuild);
      if (failedAgain)
        return std::make_error_code(std::errc::invalid_argument);
      assert(moduleBuffer);
      return std::move(moduleBuffer);
    }
  }
};

} // end anonymous namespace

bool ModuleInterfaceCheckerImpl::isCached(StringRef DepPath) {
  if (!CacheDir.empty() && DepPath.startswith(CacheDir))
    return true;
  return !PrebuiltCacheDir.empty() && DepPath.startswith(PrebuiltCacheDir);
}

bool ModuleInterfaceLoader::isCached(StringRef DepPath) {
  return InterfaceChecker.isCached(DepPath);
}

/// Load a .swiftmodule associated with a .swiftinterface either from a
/// cache or by converting it in a subordinate \c CompilerInstance, caching
/// the results.
std::error_code ModuleInterfaceLoader::findModuleFilesInDirectory(
  ImportPath::Element ModuleID,
  const SerializedModuleBaseName &BaseName,
  SmallVectorImpl<char> *ModuleInterfacePath,
  std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
  std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
  std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
  bool IsFramework) {

  // If running in OnlySerialized mode, ModuleInterfaceLoader
  // should not have been constructed at all.
  assert(LoadMode != ModuleLoadingMode::OnlySerialized);

  llvm::SmallString<256>
  ModPath{ BaseName.getName(file_types::TY_SwiftModuleFile) },
  InPath{  BaseName.getName(file_types::TY_SwiftModuleInterfaceFile) },
  PrivateInPath{BaseName.getName(file_types::TY_PrivateSwiftModuleInterfaceFile)};

  // First check to see if the .swiftinterface exists at all. Bail if not.
  auto &fs = *Ctx.SourceMgr.getFileSystem();
  if (!fs.exists(InPath)) {
    if (fs.exists(ModPath)) {
      LLVM_DEBUG(llvm::dbgs()
        << "No .swiftinterface file found adjacent to module file "
        << ModPath.str() << "\n");
      return std::make_error_code(std::errc::not_supported);
    }
    return std::make_error_code(std::errc::no_such_file_or_directory);
  }

  // If present, use the private interface instead of the public one.
  if (fs.exists(PrivateInPath)) {
    InPath = PrivateInPath;
  }

  // Create an instance of the Impl to do the heavy lifting.
  auto ModuleName = ModuleID.Item.str();
  ModuleInterfaceLoaderImpl Impl(
      Ctx, ModPath, InPath, ModuleName, InterfaceChecker.CacheDir,
      InterfaceChecker.PrebuiltCacheDir, InterfaceChecker.BackupInterfaceDir,
      ModuleID.Loc, InterfaceChecker.Opts,
      InterfaceChecker.RequiresOSSAModules, dependencyTracker,
      llvm::is_contained(PreferInterfaceForModules, ModuleName)
          ? ModuleLoadingMode::PreferInterface
          : LoadMode);

  // Ask the impl to find us a module that we can load or give us an error
  // telling us that we couldn't load it.
  auto ModuleBufferOrErr = Impl.findOrBuildLoadableModule();
  if (!ModuleBufferOrErr)
    return ModuleBufferOrErr.getError();

  if (ModuleBuffer) {
    *ModuleBuffer = std::move(*ModuleBufferOrErr);
    if (ModuleInterfacePath)
      *ModuleInterfacePath = InPath;
  }

  // Open .swiftsourceinfo file if it's present.
  if (auto SourceInfoError = openModuleSourceInfoFileIfPresent(ModuleID,
                                                               BaseName,
                                                       ModuleSourceInfoBuffer))
    return SourceInfoError;

  // Delegate back to the serialized module loader to load the module doc.
  if (auto DocLoadErr = openModuleDocFileIfPresent(ModuleID, BaseName,
                                                   ModuleDocBuffer))
    return DocLoadErr;

  return std::error_code();
}

std::vector<std::string>
ModuleInterfaceCheckerImpl::getCompiledModuleCandidatesForInterface(
    StringRef moduleName, StringRef interfacePath) {
  // Derive .swiftmodule path from the .swiftinterface path.
  auto newExt = file_types::getExtension(file_types::TY_SwiftModuleFile);
  llvm::SmallString<32> modulePath = interfacePath;
  llvm::sys::path::replace_extension(modulePath, newExt);
  ModuleInterfaceLoaderImpl Impl(Ctx, modulePath, interfacePath, moduleName,
                                 CacheDir, PrebuiltCacheDir, BackupInterfaceDir,
                                 SourceLoc(), Opts,
                                 RequiresOSSAModules, nullptr,
                                 ModuleLoadingMode::PreferSerialized);
  std::vector<std::string> results;
  auto pair = Impl.getCompiledModuleCandidates();
  // Add compiled module candidates only when they are non-empty.
  if (!pair.first.empty())
    results.push_back(pair.first);
  if (!pair.second.empty())
    results.push_back(pair.second);
  return results;
}

bool ModuleInterfaceCheckerImpl::tryEmitForwardingModule(
    StringRef moduleName, StringRef interfacePath,
    ArrayRef<std::string> candidates, StringRef outputPath) {
  // Derive .swiftmodule path from the .swiftinterface path.
  auto newExt = file_types::getExtension(file_types::TY_SwiftModuleFile);
  llvm::SmallString<32> modulePath = interfacePath;
  llvm::sys::path::replace_extension(modulePath, newExt);
  ModuleInterfaceLoaderImpl Impl(Ctx, modulePath, interfacePath, moduleName,
                                 CacheDir, PrebuiltCacheDir,
                                 BackupInterfaceDir, SourceLoc(), Opts,
                                 RequiresOSSAModules, nullptr,
                                 ModuleLoadingMode::PreferSerialized);
  SmallVector<FileDependency, 16> deps;
  std::unique_ptr<llvm::MemoryBuffer> moduleBuffer;
  for (auto mod: candidates) {
    // Check if the candidate compiled module is still up-to-date.
    if (Impl.swiftModuleIsUpToDate(mod, deps, moduleBuffer)) {
      // If so, emit a forwarding module to the candidate.
      ForwardingModule FM(mod);
      auto hadError = withOutputFile(Ctx.Diags, outputPath,
        [&](llvm::raw_pwrite_stream &out) {
          llvm::yaml::Output yamlWriter(out);
          yamlWriter << FM;
          return false;
        });
      if (!hadError)
        return true;
    }
  }
  return false;
}

bool ModuleInterfaceLoader::buildSwiftModuleFromSwiftInterface(
    SourceManager &SourceMgr, DiagnosticEngine &Diags,
    const SearchPathOptions &SearchPathOpts, const LangOptions &LangOpts,
    const ClangImporterOptions &ClangOpts, StringRef CacheDir,
    StringRef PrebuiltCacheDir, StringRef BackupInterfaceDir,
    StringRef ModuleName, StringRef InPath,
    StringRef OutPath, bool SerializeDependencyHashes,
    bool TrackSystemDependencies, ModuleInterfaceLoaderOptions LoaderOpts,
    RequireOSSAModules_t RequireOSSAModules) {
  InterfaceSubContextDelegateImpl astDelegate(
      SourceMgr, &Diags, SearchPathOpts, LangOpts, ClangOpts, LoaderOpts,
      /*CreateCacheDirIfAbsent*/ true, CacheDir, PrebuiltCacheDir,
      BackupInterfaceDir,
      SerializeDependencyHashes, TrackSystemDependencies, RequireOSSAModules);
  ModuleInterfaceBuilder builder(SourceMgr, &Diags, astDelegate, InPath,
                                 ModuleName, CacheDir, PrebuiltCacheDir,
                                 BackupInterfaceDir,
                                 LoaderOpts.disableInterfaceLock);
  // FIXME: We really only want to serialize 'important' dependencies here, if
  //        we want to ship the built swiftmodules to another machine.
  auto failed = builder.buildSwiftModule(OutPath, /*shouldSerializeDeps*/true,
                                         /*ModuleBuffer*/nullptr, nullptr,
                                         SearchPathOpts.CandidateCompiledModules);
  if (!failed)
    return false;
  auto backInPath =
    ModuleInterfaceLoaderImpl::getBackupPublicModuleInterfacePath(SourceMgr,
      BackupInterfaceDir, ModuleName, InPath);
  if (backInPath.empty())
    return true;
  assert(failed);
  assert(!backInPath.empty());
  ModuleInterfaceBuilder backupBuilder(SourceMgr, &Diags, astDelegate, backInPath,
                                       ModuleName, CacheDir, PrebuiltCacheDir,
                                       BackupInterfaceDir,
                                       LoaderOpts.disableInterfaceLock);
  // Ensure we can rebuild module after user changed the original interface file.
  backupBuilder.addExtraDependency(InPath);
  // FIXME: We really only want to serialize 'important' dependencies here, if
  //        we want to ship the built swiftmodules to another machine.
  return backupBuilder.buildSwiftModule(OutPath, /*shouldSerializeDeps*/true,
                                        /*ModuleBuffer*/nullptr, nullptr,
                                        SearchPathOpts.CandidateCompiledModules);
}

void ModuleInterfaceLoader::collectVisibleTopLevelModuleNames(
    SmallVectorImpl<Identifier> &names) const {
  collectVisibleTopLevelModuleNamesImpl(
      names,
      file_types::getExtension(file_types::TY_SwiftModuleInterfaceFile));
}

void InterfaceSubContextDelegateImpl::inheritOptionsForBuildingInterface(
    const SearchPathOptions &SearchPathOpts, const LangOptions &LangOpts,
    RequireOSSAModules_t RequireOSSAModules) {
  GenericArgs.push_back("-frontend");
  // Start with a genericSubInvocation that copies various state from our
  // invoking ASTContext.
  GenericArgs.push_back("-compile-module-from-interface");
  genericSubInvocation.setTargetTriple(LangOpts.Target);

  auto triple = ArgSaver.save(genericSubInvocation.getTargetTriple());
  if (!triple.empty()) {
    GenericArgs.push_back("-target");
    GenericArgs.push_back(triple);
  }

  if (LangOpts.ClangTarget.hasValue()) {
    genericSubInvocation.getLangOptions().ClangTarget = LangOpts.ClangTarget;
    auto triple = ArgSaver.save(genericSubInvocation.getLangOptions()
      .ClangTarget->getTriple());
    assert(!triple.empty());
    // In explicit module build, all PCMs will be built using the given clang target.
    // So the Swift interface should know that as well to load these PCMs properly.
    GenericArgs.push_back("-clang-target");
    GenericArgs.push_back(triple);
  }

  // Inherit the Swift language version
  genericSubInvocation.getLangOptions().EffectiveLanguageVersion =
    LangOpts.EffectiveLanguageVersion;
  GenericArgs.push_back("-swift-version");
  GenericArgs.push_back(ArgSaver.save(genericSubInvocation.getLangOptions()
    .EffectiveLanguageVersion.asAPINotesVersionString()));

  genericSubInvocation.setImportSearchPaths(SearchPathOpts.ImportSearchPaths);
  genericSubInvocation.setFrameworkSearchPaths(SearchPathOpts.FrameworkSearchPaths);
  if (!SearchPathOpts.SDKPath.empty()) {
    genericSubInvocation.setSDKPath(SearchPathOpts.SDKPath);
  }

  genericSubInvocation.getFrontendOptions().InputMode
      = FrontendOptions::ParseInputMode::SwiftModuleInterface;
  if (!SearchPathOpts.RuntimeResourcePath.empty()) {
    genericSubInvocation.setRuntimeResourcePath(SearchPathOpts.RuntimeResourcePath);
  }

  // Inhibit warnings from the genericSubInvocation since we are assuming the user
  // is not in a position to fix them.
  genericSubInvocation.getDiagnosticOptions().SuppressWarnings = true;
  GenericArgs.push_back("-suppress-warnings");

  // Inherit this setting down so that it can affect error diagnostics (mostly
  // by making them non-fatal).
  genericSubInvocation.getLangOptions().DebuggerSupport = LangOpts.DebuggerSupport;
  if (LangOpts.DebuggerSupport) {
    GenericArgs.push_back("-debugger-support");
  }

  // Disable this; deinitializers always get printed with `@objc` even in
  // modules that don't import Foundation.
  genericSubInvocation.getLangOptions().EnableObjCAttrRequiresFoundation = false;
  GenericArgs.push_back("-disable-objc-attr-requires-foundation-module");

  // If we are supposed to use RequireOSSAModules, do so.
  genericSubInvocation.getSILOptions().EnableOSSAModules =
      bool(RequireOSSAModules);
}

bool InterfaceSubContextDelegateImpl::extractSwiftInterfaceVersionAndArgs(
    CompilerInvocation &subInvocation,
    SmallVectorImpl<const char *> &SubArgs,
    std::string &CompilerVersion,
    StringRef interfacePath,
    SourceLoc diagnosticLoc) {
  llvm::vfs::FileSystem &fs = *SM.getFileSystem();
  auto FileOrError = swift::vfs::getFileOrSTDIN(fs, interfacePath);
  if (!FileOrError) {
    // Don't use this->diagnose() because it'll just try to re-open
    // interfacePath.
    Diags->diagnose(diagnosticLoc, diag::error_open_input_file,
                    interfacePath, FileOrError.getError().message());
    return true;
  }
  auto SB = FileOrError.get()->getBuffer();
  auto VersRe = getSwiftInterfaceFormatVersionRegex();
  auto CompRe = getSwiftInterfaceCompilerVersionRegex();
  SmallVector<StringRef, 1> VersMatches, CompMatches;

  if (!VersRe.match(SB, &VersMatches)) {
    diagnose(interfacePath, diagnosticLoc,
             diag::error_extracting_version_from_module_interface);
    return true;
  }
  if (extractCompilerFlagsFromInterface(interfacePath, SB, ArgSaver, SubArgs)) {
    diagnose(interfacePath, diagnosticLoc,
             diag::error_extracting_version_from_module_interface);
    return true;
  }
  assert(VersMatches.size() == 2);
  // FIXME We should diagnose this at a location that makes sense:
  auto Vers = swift::version::Version(VersMatches[1], SourceLoc(), Diags);

  if (CompRe.match(SB, &CompMatches)) {
    assert(CompMatches.size() == 2);
    CompilerVersion = ArgSaver.save(CompMatches[1]).str();
  }
  else {
    // Don't diagnose; handwritten module interfaces don't include this field.
    CompilerVersion = "(unspecified, file possibly handwritten)";
  }

  // For now: we support anything with the same "major version" and assume
  // minor versions might be interesting for debugging, or special-casing a
  // compatible field variant.
  if (Vers.asMajorVersion() != InterfaceFormatVersion.asMajorVersion()) {
    diagnose(interfacePath, diagnosticLoc,
             diag::unsupported_version_of_module_interface, interfacePath, Vers);
    return true;
  }

  SmallString<32> ExpectedModuleName = subInvocation.getModuleName();
  if (subInvocation.parseArgs(SubArgs, *Diags)) {
    return true;
  }

  if (subInvocation.getModuleName() != ExpectedModuleName) {
    auto DiagKind = diag::serialization_name_mismatch;
    if (subInvocation.getLangOptions().DebuggerSupport)
      DiagKind = diag::serialization_name_mismatch_repl;
    diagnose(interfacePath, diagnosticLoc,
             DiagKind, subInvocation.getModuleName(), ExpectedModuleName);
    return true;
  }

  return false;
}

InterfaceSubContextDelegateImpl::InterfaceSubContextDelegateImpl(
    SourceManager &SM, DiagnosticEngine *Diags,
    const SearchPathOptions &searchPathOpts, const LangOptions &langOpts,
    const ClangImporterOptions &clangImporterOpts,
    ModuleInterfaceLoaderOptions LoaderOpts, bool buildModuleCacheDirIfAbsent,
    StringRef moduleCachePath, StringRef prebuiltCachePath,
    StringRef backupModuleInterfaceDir,
    bool serializeDependencyHashes, bool trackSystemDependencies,
    RequireOSSAModules_t requireOSSAModules)
    : SM(SM), Diags(Diags), ArgSaver(Allocator) {
  genericSubInvocation.setMainExecutablePath(LoaderOpts.mainExecutablePath);
  inheritOptionsForBuildingInterface(searchPathOpts, langOpts,
                                     requireOSSAModules);
  // Configure front-end input.
  auto &SubFEOpts = genericSubInvocation.getFrontendOptions();
  SubFEOpts.RequestedAction = LoaderOpts.requestedAction;
  if (!moduleCachePath.empty()) {
    genericSubInvocation.setClangModuleCachePath(moduleCachePath);
  }
  if (!prebuiltCachePath.empty()) {
    genericSubInvocation.getFrontendOptions().PrebuiltModuleCachePath =
      prebuiltCachePath.str();
  }
  if (!backupModuleInterfaceDir.empty()) {
    genericSubInvocation.getFrontendOptions().BackupModuleInterfaceDir =
      backupModuleInterfaceDir.str();
  }
  if (trackSystemDependencies) {
    genericSubInvocation.getFrontendOptions().IntermoduleDependencyTracking =
        IntermoduleDepTrackingMode::IncludeSystem;
    GenericArgs.push_back("-track-system-dependencies");
  } else {
    // Always track at least the non-system dependencies for interface building.
    genericSubInvocation.getFrontendOptions().IntermoduleDependencyTracking =
        IntermoduleDepTrackingMode::ExcludeSystem;
  }
  if (LoaderOpts.disableImplicitSwiftModule) {
    genericSubInvocation.getFrontendOptions().DisableImplicitModules = true;
    GenericArgs.push_back("-disable-implicit-swift-modules");
  }
  genericSubInvocation.getSearchPathOptions().ExplicitSwiftModules =
    searchPathOpts.ExplicitSwiftModules;
  // Pass down -explicit-swift-module-map-file
  // FIXME: we shouldn't need this. Remove it?
  StringRef explictSwiftModuleMap = searchPathOpts.ExplicitSwiftModuleMap;
  genericSubInvocation.getSearchPathOptions().ExplicitSwiftModuleMap =
    explictSwiftModuleMap.str();
  auto &subClangImporterOpts = genericSubInvocation.getClangImporterOptions();
  // Respect the detailed-record preprocessor setting of the parent context.
  // This, and the "raw" clang module format it implicitly enables, are
  // required by sourcekitd.
  subClangImporterOpts.DetailedPreprocessingRecord =
    clangImporterOpts.DetailedPreprocessingRecord;

  // We need to add these extra clang flags because explict module building
  // related flags are all there: -fno-implicit-modules, -fmodule-map-file=,
  // and -fmodule-file=.
  // If we don't add these flags, the interface will be built with implicit
  // PCMs.
  // FIXME: With Implicit Module Builds, if sub-invocations inherit `-fmodule-map-file=` options,
  // those modulemaps become File dependencies of all downstream PCMs and their depending Swift
  // modules, triggering unnecessary re-builds. We work around this by only inheriting these options
  // when building with explicit modules. While this problem will not manifest with Explicit Modules
  // (which do not use the ClangImporter to build PCMs), we may still need a better way to
  // decide which options must be inherited here.
  if (LoaderOpts.disableImplicitSwiftModule) {
    subClangImporterOpts.ExtraArgs = clangImporterOpts.ExtraArgs;
    for (auto arg : subClangImporterOpts.ExtraArgs) {
      GenericArgs.push_back("-Xcc");
      GenericArgs.push_back(ArgSaver.save(arg));
    }
  }

  // Tell the genericSubInvocation to serialize dependency hashes if asked to do
  // so.
  auto &frontendOpts = genericSubInvocation.getFrontendOptions();
  frontendOpts.SerializeModuleInterfaceDependencyHashes =
    serializeDependencyHashes;
  if (serializeDependencyHashes) {
    GenericArgs.push_back("-serialize-module-interface-dependency-hashes");
  }

  // Tell the genericSubInvocation to remark on rebuilds from an interface if asked
  // to do so.
  frontendOpts.RemarkOnRebuildFromModuleInterface =
    LoaderOpts.remarkOnRebuildFromInterface;
  if (LoaderOpts.remarkOnRebuildFromInterface) {
    GenericArgs.push_back("-Rmodule-interface-rebuild");
  }

  // Note that we don't assume cachePath is the same as the Clang
  // module cache path at this point.
  if (buildModuleCacheDirIfAbsent && !moduleCachePath.empty())
    (void)llvm::sys::fs::create_directories(moduleCachePath);
}

/// Calculate an output filename in \p genericSubInvocation's cache path that
/// includes a hash of relevant key data.
StringRef InterfaceSubContextDelegateImpl::computeCachedOutputPath(
                             StringRef moduleName,
                             StringRef useInterfacePath,
                             llvm::SmallString<256> &OutPath,
                             StringRef &CacheHash) {
  OutPath = genericSubInvocation.getClangModuleCachePath();
  llvm::sys::path::append(OutPath, moduleName);
  OutPath.append("-");
  auto hashStart = OutPath.size();
  OutPath.append(getCacheHash(useInterfacePath));
  CacheHash = OutPath.str().substr(hashStart);
  OutPath.append(".");
  auto OutExt = file_types::getExtension(file_types::TY_SwiftModuleFile);
  OutPath.append(OutExt);
  return OutPath.str();
}

/// Construct a cache key for the .swiftmodule being generated. There is a
/// balance to be struck here between things that go in the cache key and
/// things that go in the "up to date" check of the cache entry. We want to
/// avoid fighting over a single cache entry too much when (say) running
/// different compiler versions on the same machine or different inputs
/// that happen to have the same short module name, so we will disambiguate
/// those in the key. But we want to invalidate and rebuild a cache entry
/// -- rather than making a new one and potentially filling up the cache
/// with dead entries -- when other factors change, such as the contents of
/// the .swiftinterface input or its dependencies.
std::string
InterfaceSubContextDelegateImpl::getCacheHash(StringRef useInterfacePath) {
  auto normalizedTargetTriple =
      getTargetSpecificModuleTriple(genericSubInvocation.getLangOptions().Target);

  llvm::hash_code H = hash_combine(
      // Start with the compiler version (which will be either tag names or
      // revs). Explicitly don't pass in the "effective" language version --
      // this would mean modules built in different -swift-version modes would
      // rebuild their dependencies.
      swift::version::getSwiftFullVersion(),

      // Simplest representation of input "identity" (not content) is just a
      // pathname, and probably all we can get from the VFS in this regard
      // anyways.
      useInterfacePath,

      // Include the normalized target triple. In practice, .swiftinterface
      // files will be in target-specific subdirectories and would have
      // target-specific pieces #if'd out. However, it doesn't hurt to include
      // it, and it guards against mistakenly reusing cached modules across
      // targets. Note that this normalization explicitly doesn't include the
      // minimum deployment target (e.g. the '12.0' in 'ios12.0').
      normalizedTargetTriple.str(),

      // The SDK path is going to affect how this module is imported, so
      // include it.
      genericSubInvocation.getSDKPath(),

      // Whether or not we're tracking system dependencies affects the
      // invalidation behavior of this cache item.
      genericSubInvocation.getFrontendOptions().shouldTrackSystemDependencies(),

      // Whether or not OSSA modules are enabled.
      //
      // If OSSA modules are enabled, we use a separate namespace of modules to
      // ensure that we compile all swift interface files with the option set.
      unsigned(genericSubInvocation.getSILOptions().EnableOSSAModules));

  return llvm::APInt(64, H).toString(36, /*Signed=*/false);
}

std::error_code
InterfaceSubContextDelegateImpl::runInSubContext(StringRef moduleName,
                                                 StringRef interfacePath,
                                                 StringRef outputPath,
                                                 SourceLoc diagLoc,
    llvm::function_ref<std::error_code(ASTContext&, ModuleDecl*, ArrayRef<StringRef>,
                            ArrayRef<StringRef>, StringRef)> action) {
  return runInSubCompilerInstance(moduleName, interfacePath, outputPath, diagLoc,
                                  [&](SubCompilerInstanceInfo &info){
    return action(info.Instance->getASTContext(),
                  info.Instance->getMainModule(),
                  info.BuildArguments,
                  info.ExtraPCMArgs,
                  info.Hash);
  });
}

std::error_code
InterfaceSubContextDelegateImpl::runInSubCompilerInstance(StringRef moduleName,
                                                          StringRef interfacePath,
                                                          StringRef outputPath,
                                                          SourceLoc diagLoc,
                  llvm::function_ref<std::error_code(SubCompilerInstanceInfo&)> action) {
  // We are about to mess up the compiler invocation by using the compiler
  // arguments in the textual interface file. So copy to use a new compiler
  // invocation.
  CompilerInvocation subInvocation = genericSubInvocation;
  std::vector<StringRef> BuildArgs(GenericArgs.begin(), GenericArgs.end());
  assert(BuildArgs.size() == GenericArgs.size());
  // Configure inputs
  subInvocation.getFrontendOptions().InputsAndOutputs
    .addInputFile(interfacePath);
  BuildArgs.push_back(interfacePath);
  subInvocation.setModuleName(moduleName);
  BuildArgs.push_back("-module-name");
  BuildArgs.push_back(moduleName);

  // Calculate output path of the module.
  llvm::SmallString<256> buffer;
  StringRef CacheHash;
  auto hashedOutput = computeCachedOutputPath(moduleName, interfacePath, buffer,
                                              CacheHash);
  // If no specific output path is given, use the hashed output path.
  if (outputPath.empty()) {
    outputPath = hashedOutput;
  }

  // Configure the outputs in front-end options. There must be an equal number of
  // inputs and outputs.
  std::vector<std::string> outputFiles{"/<unused>"};
  std::vector<SupplementaryOutputPaths> ModuleOutputPaths;
  ModuleOutputPaths.emplace_back();
  if (subInvocation.getFrontendOptions().RequestedAction ==
          FrontendOptions::ActionType::EmitModuleOnly) {
    ModuleOutputPaths.back().ModuleOutputPath = outputPath.str();
  }
  assert(subInvocation.getFrontendOptions().InputsAndOutputs.isWholeModule());
  subInvocation.getFrontendOptions().InputsAndOutputs
    .setMainAndSupplementaryOutputs(outputFiles, ModuleOutputPaths);

  SmallVector<const char *, 64> SubArgs;
  std::string CompilerVersion;
  // Extract compiler arguments from the interface file and use them to configure
  // the compiler invocation.
  if (extractSwiftInterfaceVersionAndArgs(subInvocation,
                                          SubArgs,
                                          CompilerVersion,
                                          interfacePath,
                                          diagLoc)) {
    return std::make_error_code(std::errc::not_supported);
  }
  // Insert arguments collected from the interface file.
  BuildArgs.insert(BuildArgs.end(), SubArgs.begin(), SubArgs.end());
  if (subInvocation.parseArgs(SubArgs, *Diags)) {
    return std::make_error_code(std::errc::not_supported);
  }
  CompilerInstance subInstance;
  SubCompilerInstanceInfo info;
  info.Instance = &subInstance;
  info.CompilerVersion = CompilerVersion;

  subInstance.getSourceMgr().setFileSystem(SM.getFileSystem());

  ForwardingDiagnosticConsumer FDC(*Diags);
  subInstance.addDiagnosticConsumer(&FDC);
  if (subInstance.setup(subInvocation)) {
    return std::make_error_code(std::errc::not_supported);
  }
  info.BuildArguments = BuildArgs;
  info.Hash = CacheHash;
  auto target =  *(std::find(BuildArgs.rbegin(), BuildArgs.rend(), "-target") - 1);
  auto langVersion = *(std::find(BuildArgs.rbegin(), BuildArgs.rend(),
                                 "-swift-version") - 1);
  std::array<StringRef, 6> ExtraPCMArgs = {
    // PCMs should use the target triple the interface will be using to build
    "-Xcc", "-target", "-Xcc", target,
    // PCMs should use the effective Swift language version for apinotes.
    "-Xcc", ArgSaver.save((llvm::Twine("-fapinotes-swift-version=") + langVersion).str())
  };
  info.ExtraPCMArgs = ExtraPCMArgs;
  // Run the action under the sub compiler instance.
  return action(info);
}

struct ExplicitSwiftModuleLoader::Implementation {
  ASTContext &Ctx;
  llvm::BumpPtrAllocator Allocator;
  llvm::StringMap<ExplicitModuleInfo> ExplicitModuleMap;
  Implementation(ASTContext &Ctx) : Ctx(Ctx) {}

  void parseSwiftExplicitModuleMap(StringRef fileName) {
    ExplicitModuleMapParser parser(Allocator);
    auto result =
        parser.parseSwiftExplicitModuleMap(fileName, ExplicitModuleMap);
    if (result == std::errc::invalid_argument)
      Ctx.Diags.diagnose(SourceLoc(), diag::explicit_swift_module_map_corrupted,
                         fileName);
    else if (result == std::errc::no_such_file_or_directory)
      Ctx.Diags.diagnose(SourceLoc(), diag::explicit_swift_module_map_missing,
                         fileName);
  }
};

ExplicitSwiftModuleLoader::ExplicitSwiftModuleLoader(
      ASTContext &ctx,
      DependencyTracker *tracker,
      ModuleLoadingMode loadMode,
      bool IgnoreSwiftSourceInfoFile):
        SerializedModuleLoaderBase(ctx, tracker, loadMode,
                                   IgnoreSwiftSourceInfoFile),
        Impl(*new Implementation(ctx)) {}

ExplicitSwiftModuleLoader::~ExplicitSwiftModuleLoader() { delete &Impl; }

bool ExplicitSwiftModuleLoader::findModule(ImportPath::Element ModuleID,
           SmallVectorImpl<char> *ModuleInterfacePath,
           std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
           std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
           std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
           bool &IsFramework, bool &IsSystemModule) {
  StringRef moduleName = ModuleID.Item.str();
  auto it = Impl.ExplicitModuleMap.find(moduleName);
  // If no explicit module path is given matches the name, return with an
  // error code.
  if (it == Impl.ExplicitModuleMap.end()) {
    return false;
  }
  auto &moduleInfo = it->getValue();
  if (moduleInfo.moduleBuffer) {
    // We found an explicit module matches the given name, give the buffer
    // back to the caller side.
    *ModuleBuffer = std::move(moduleInfo.moduleBuffer);
    return true;
  }

  // Set IsFramework bit according to the moduleInfo
  IsFramework = moduleInfo.isFramework;

  auto &fs = *Ctx.SourceMgr.getFileSystem();
  // Open .swiftmodule file
  auto moduleBuf = fs.getBufferForFile(moduleInfo.modulePath);
  if (!moduleBuf) {
    // We cannot read the module content, diagnose.
    Ctx.Diags.diagnose(SourceLoc(), diag::error_opening_explicit_module_file,
                       moduleInfo.modulePath);
    return false;
  }

  assert(moduleBuf);
  const bool isForwardingModule = !serialization::isSerializedAST(moduleBuf
    .get()->getBuffer());
  // If the module is a forwarding module, read the actual content from the path
  // encoded in the forwarding module as the actual module content.
  if (isForwardingModule) {
    auto forwardingModule = ForwardingModule::load(*moduleBuf.get());
    if (forwardingModule) {
      moduleBuf = fs.getBufferForFile(forwardingModule->underlyingModulePath);
      if (!moduleBuf) {
        // We cannot read the module content, diagnose.
        Ctx.Diags.diagnose(SourceLoc(), diag::error_opening_explicit_module_file,
                           moduleInfo.modulePath);
        return false;
      }
    } else {
      // We cannot read the module content, diagnose.
      Ctx.Diags.diagnose(SourceLoc(), diag::error_opening_explicit_module_file,
                         moduleInfo.modulePath);
      return false;
    }
  }
  assert(moduleBuf);
  // Move the opened module buffer to the caller.
  *ModuleBuffer = std::move(moduleBuf.get());

  // Open .swiftdoc file
  if (!moduleInfo.moduleDocPath.empty()) {
    auto moduleDocBuf = fs.getBufferForFile(moduleInfo.moduleDocPath);
    if (moduleBuf)
      *ModuleDocBuffer = std::move(moduleDocBuf.get());
  }
  // Open .swiftsourceinfo file
  if (!moduleInfo.moduleSourceInfoPath.empty()) {
    auto moduleSourceInfoBuf = fs.getBufferForFile(moduleInfo.moduleSourceInfoPath);
    if (moduleSourceInfoBuf)
      *ModuleSourceInfoBuffer = std::move(moduleSourceInfoBuf.get());
  }
  return true;
}

std::error_code ExplicitSwiftModuleLoader::findModuleFilesInDirectory(
  ImportPath::Element ModuleID,
  const SerializedModuleBaseName &BaseName,
  SmallVectorImpl<char> *ModuleInterfacePath,
  std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
  std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
  std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
  bool IsFramework) {
  llvm_unreachable("Not supported in the Explicit Swift Module Loader.");
  return std::make_error_code(std::errc::not_supported);
}

bool ExplicitSwiftModuleLoader::canImportModule(ImportPath::Module path,
                                                llvm::VersionTuple version,
                                                bool underlyingVersion) {
  // FIXME: Swift submodules?
  ImportPath::Element mID = path.front();
  StringRef moduleName = mID.Item.str();
  auto it = Impl.ExplicitModuleMap.find(moduleName);
  // If no provided explicit module matches the name, then it cannot be imported.
  if (it == Impl.ExplicitModuleMap.end()) {
    return false;
  }
  return true;
}

void ExplicitSwiftModuleLoader::collectVisibleTopLevelModuleNames(
      SmallVectorImpl<Identifier> &names) const {
  for (auto &entry: Impl.ExplicitModuleMap) {
    names.push_back(Ctx.getIdentifier(entry.getKey()));
  }
}

std::unique_ptr<ExplicitSwiftModuleLoader>
ExplicitSwiftModuleLoader::create(ASTContext &ctx,
    DependencyTracker *tracker, ModuleLoadingMode loadMode,
    ArrayRef<std::string> ExplicitModulePaths,
    StringRef ExplicitSwiftModuleMap,
    bool IgnoreSwiftSourceInfoFile) {
  auto result = std::unique_ptr<ExplicitSwiftModuleLoader>(
    new ExplicitSwiftModuleLoader(ctx, tracker, loadMode,
                                  IgnoreSwiftSourceInfoFile));
  auto &Impl = result->Impl;
  // If the explicit module map is given, try parse it.
  if (!ExplicitSwiftModuleMap.empty()) {
    // Parse a JSON file to collect explicitly built modules.
    Impl.parseSwiftExplicitModuleMap(ExplicitSwiftModuleMap);
  }
  // Collect .swiftmodule paths from -swift-module-path
  // FIXME: remove these.
  for (auto path: ExplicitModulePaths) {
    std::string name;
    // Load the explicit module into a buffer and get its name.
    std::unique_ptr<llvm::MemoryBuffer> buffer = getModuleName(ctx, path, name);
    if (buffer) {
      // Register this module for future loading.
      auto &entry = Impl.ExplicitModuleMap[name];
      entry.modulePath = path;
      entry.moduleBuffer = std::move(buffer);
    } else {
      // We cannot read the module content, diagnose.
      ctx.Diags.diagnose(SourceLoc(),
                         diag::error_opening_explicit_module_file,
                         path);
    }
  }

  return result;
}
