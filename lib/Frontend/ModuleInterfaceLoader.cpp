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
#include "swift/Frontend/CachingUtils.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/ModuleInterfaceSupport.h"
#include "swift/Parse/ParseVersion.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Serialization/Validation.h"
#include "swift/Strings.h"
#include "clang/Basic/Module.h"
#include "clang/Frontend/CompileJobCacheResult.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/CAS/ActionCache.h"
#include "llvm/CAS/ObjectStore.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualOutputBackend.h"
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

static bool serializedASTLooksValid(const llvm::MemoryBuffer &buf,
                                    bool requiresOSSAModules,
                                    StringRef requiredSDK,
                                    bool requiresRevisionMatch) {
  auto VI = serialization::validateSerializedAST(
      buf.getBuffer(), requiresOSSAModules, requiredSDK, requiresRevisionMatch);
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
  enum class ReasonIgnored {
    NotIgnored,
    PublicFramework,
    InterfacePreferred,
  };
  struct CandidateModule {
    std::string path;
    Optional<serialization::Status> serializationStatus;
    ModuleKind kind;
    ReasonIgnored reasonIgnored;
    SmallVector<std::string, 10> outOfDateDependencies;
    SmallVector<std::string, 10> missingDependencies;
  };
  SmallVector<CandidateModule, 3> candidateModules;

  CandidateModule &getOrInsertCandidateModule(StringRef path) {
    for (auto &mod : candidateModules) {
      if (mod.path == path) return mod;
    }
    candidateModules.push_back({path.str(),
                                None,
                                ModuleKind::Normal,
                                ReasonIgnored::NotIgnored,
                                {},
                                {}});
    return candidateModules.back();
  }

  /// Sets the kind of a module that failed to load.
  void setModuleKind(StringRef path, ModuleKind kind) {
    getOrInsertCandidateModule(path).kind = kind;
  }

  /// Sets the serialization status of the module at \c path. If this is
  /// anything other than \c Valid, a note will be added stating why the module
  /// was invalid.
  void setSerializationStatus(StringRef path, serialization::Status status) {
    getOrInsertCandidateModule(path).serializationStatus = status;
  }

  /// Registers an out-of-date dependency at \c depPath for the module
  /// at \c modulePath.
  void addOutOfDateDependency(StringRef modulePath, StringRef depPath) {
    getOrInsertCandidateModule(modulePath)
        .outOfDateDependencies.push_back(depPath.str());
  }

  /// Registers a missing dependency at \c depPath for the module
  /// at \c modulePath.
  void addMissingDependency(StringRef modulePath, StringRef depPath) {
    getOrInsertCandidateModule(modulePath)
        .missingDependencies.push_back(depPath.str());
  }

  /// Sets the reason that the module at \c path was ignored. If this is
  /// anything besides \c NotIgnored a note will be added stating why the module
  /// was ignored.
  void addIgnoredModule(StringRef modulePath, ReasonIgnored reasonIgnored) {
    getOrInsertCandidateModule(modulePath).reasonIgnored = reasonIgnored;
  }

  /// Determines if we saw the given module path and registered is as out of
  /// date.
  bool sawOutOfDateModule(StringRef modulePath) {
    for (auto &mod : candidateModules)
      if (mod.path == modulePath &&
          mod.reasonIgnored == ReasonIgnored::NotIgnored)
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
    auto SDKVer = getSDKBuildVersion(ctx.SearchPathOpts.getSDKPath());
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
    for (auto &mod : candidateModules) {
      // If a the compiled module was ignored, diagnose the reason.
      if (mod.reasonIgnored != ReasonIgnored::NotIgnored) {
        diags.diagnose(loc, diag::compiled_module_ignored_reason, mod.path,
                       (unsigned)mod.reasonIgnored);
      } else {
        diags.diagnose(loc, diag::out_of_date_module_here, (unsigned)mod.kind,
                       mod.path);
      }

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

/// Constructs the full path of the dependency \p dep by prepending the SDK
/// path if necessary.
StringRef getFullDependencyPath(const FileDependency &dep,
                                const ASTContext &ctx,
                                SmallVectorImpl<char> &scratch) {
  if (!dep.isSDKRelative())
    return dep.getPath();

  path::native(ctx.SearchPathOpts.getSDKPath(), scratch);
  llvm::sys::path::append(scratch, dep.getPath());
  return StringRef(scratch.data(), scratch.size());
}

class UpToDateModuleCheker {
  ASTContext &ctx;
  llvm::vfs::FileSystem &fs;
  RequireOSSAModules_t requiresOSSAModules;

public:
  UpToDateModuleCheker(ASTContext &ctx, RequireOSSAModules_t requiresOSSAModules)
     : ctx(ctx),
       fs(*ctx.SourceMgr.getFileSystem()),
       requiresOSSAModules(requiresOSSAModules) {}
  
  // Check if all the provided file dependencies are up-to-date compared to
  // what's currently on disk.
  bool dependenciesAreUpToDate(StringRef modulePath,
                               ModuleRebuildInfo &rebuildInfo,
                               ArrayRef<FileDependency> deps,
                               bool skipSystemDependencies) {
    SmallString<128> SDKRelativeBuffer;
    for (auto &in : deps) {
      if (skipSystemDependencies && in.isSDKRelative() &&
          in.isModificationTimeBased()) {
        continue;
      }
      StringRef fullPath = getFullDependencyPath(in, ctx, SDKRelativeBuffer);
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
      ModuleRebuildInfo &rebuildInfo,
      SmallVectorImpl<FileDependency> &allDeps) {

    // Clear the existing dependencies, because we're going to re-fill them
    // in validateSerializedAST.
    allDeps.clear();

    LLVM_DEBUG(llvm::dbgs() << "Validating deps of " << path << "\n");
    auto validationInfo = serialization::validateSerializedAST(
        buf.getBuffer(), requiresOSSAModules, ctx.LangOpts.SDKName,
        !ctx.LangOpts.DebuggerSupport,
        /*ExtendedValidationInfo=*/nullptr, &allDeps);

    if (validationInfo.status != serialization::Status::Valid) {
      rebuildInfo.setSerializationStatus(path, validationInfo.status);
      return false;
    }

    bool skipCheckingSystemDependencies =
        ctx.SearchPathOpts.DisableModulesValidateSystemDependencies;
    return dependenciesAreUpToDate(path, rebuildInfo, allDeps,
                                   skipCheckingSystemDependencies);
  }

  // Check that the output .swiftmodule file is at least as new as all the
  // dependencies it read when it was built last time.
  bool swiftModuleIsUpToDate(
    StringRef modulePath, ModuleRebuildInfo &rebuildInfo,
    SmallVectorImpl<FileDependency> &AllDeps,
    std::unique_ptr<llvm::MemoryBuffer> &moduleBuffer) {
    auto OutBuf = fs.getBufferForFile(modulePath);
    if (!OutBuf)
      return false;
    moduleBuffer = std::move(*OutBuf);
    return serializedASTBufferIsUpToDate(modulePath, *moduleBuffer, rebuildInfo, AllDeps);
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
  UpToDateModuleCheker upToDateChecker;
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
        upToDateChecker(ctx, requiresOSSAModules),
        modulePath(modulePath), interfacePath(interfacePath),
        moduleName(moduleName),
        prebuiltCacheDir(prebuiltCacheDir),
        backupInterfaceDir(backupInterfaceDir),
        cacheDir(cacheDir), diagnosticLoc(diagLoc),
        dependencyTracker(dependencyTracker), loadMode(loadMode), Opts(Opts),
        requiresOSSAModules(requiresOSSAModules) {}

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
    if (!modBuf || !serializedASTLooksValid(*modBuf.get(), requiresOSSAModules,
                                            ctx.LangOpts.SDKName,
                                            !ctx.LangOpts.DebuggerSupport))
      return false;

    // Next, check the dependencies in the forwarding file.
    for (auto &dep : fwd.dependencies) {
      deps.push_back(
        FileDependency::modTimeBased(
          dep.path, dep.isSDKRelative, dep.size, dep.lastModificationTime));
    }

    bool skipCheckingSystemDependencies =
        ctx.SearchPathOpts.DisableModulesValidateSystemDependencies;
    if (!upToDateChecker.dependenciesAreUpToDate(path, rebuildInfo, deps,
                                                 skipCheckingSystemDependencies))
      return false;

    moduleBuffer = std::move(*modBuf);
    return true;
  }

  bool canInterfaceHavePrebuiltModule() {
    StringRef sdkPath = ctx.SearchPathOpts.getSDKPath();
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
      assert(path::stem(inParentDirName) ==
             ctx.getRealModuleName(ctx.getIdentifier(moduleName)).str());
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
    StringRef sdkPath = ctx.SearchPathOpts.getSDKPath();

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
    using ReasonIgnored = ModuleRebuildInfo::ReasonIgnored;
    std::pair<std::string, std::string> result;
    // Should we attempt to load a swiftmodule adjacent to the swiftinterface?
    bool shouldLoadAdjacentModule = !ctx.IgnoreAdjacentModules;

    // Don't use the adjacent swiftmodule for frameworks from the public
    // Frameworks folder of the SDK.
    SmallString<128> publicFrameworksPath;
    llvm::sys::path::append(publicFrameworksPath,
                            ctx.SearchPathOpts.getSDKPath(),
                            "System", "Library", "Frameworks");
    if (!ctx.SearchPathOpts.getSDKPath().empty() &&
        modulePath.startswith(publicFrameworksPath)) {
      shouldLoadAdjacentModule = false;
      rebuildInfo.addIgnoredModule(modulePath, ReasonIgnored::PublicFramework);
    }

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
      rebuildInfo.addIgnoredModule(modulePath,
                                   ReasonIgnored::InterfacePreferred);
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
        if (upToDateChecker.serializedASTBufferIsUpToDate(adjacentMod, *adjacentModuleBuffer.get(),
                                                          rebuildInfo, deps)) {
          LLVM_DEBUG(llvm::dbgs() << "Found up-to-date module at "
                                  << adjacentMod
                                  << "; deferring to serialized module loader\n");
          UsableModulePath = adjacentMod;
          return std::make_error_code(std::errc::not_supported);
        } else if (isInResourceDir(adjacentMod) &&
                   loadMode == ModuleLoadingMode::PreferSerialized &&
                   rebuildInfo.getOrInsertCandidateModule(adjacentMod).serializationStatus !=
                     serialization::Status::SDKMismatch) {
          // Special-case here: If we're loading a .swiftmodule from the resource
          // dir adjacent to the compiler, defer to the serialized loader instead
          // of falling back. This is mainly to support development of Swift,
          // where one might change the module format version but forget to
          // recompile the standard library. If that happens, don't fall back
          // and silently recompile the standard library -- instead, error like
          // we used to.
          // Still accept modules built with a different SDK, allowing the use
          // of one toolchain against a different SDK.
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
      if (upToDateChecker.swiftModuleIsUpToDate(prebuiltMod, rebuildInfo,
                                                deps, moduleBuffer)) {
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
      } else if (upToDateChecker.serializedASTBufferIsUpToDate(cachedOutputPath, *buf,
                                                               rebuildInfo, deps)) {
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
      const DiscoveredModule &mod, llvm::vfs::OutputBackend &backend,
      StringRef outputPath, SmallVectorImpl<FileDependency> &deps) {
    assert(mod.isPrebuilt() &&
           "cannot write forwarding file for non-prebuilt module");
    ForwardingModule fwd(mod.path);

    SmallVector<FileDependency, 16> depsAdjustedToMTime;

    // FIXME: We need to avoid re-statting all these dependencies, otherwise
    //        we may record out-of-date information.
    SmallString<128> SDKRelativeBuffer;
    auto addDependency = [&](FileDependency dep) -> FileDependency {
      auto status = fs.status(getFullDependencyPath(dep, ctx, SDKRelativeBuffer));
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

    auto hadError = withOutputPath(diags, backend, outputPath,
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
        if (writeForwardingModuleAndUpdateDeps(module, ctx.getOutputBackend(),
                                               cachedOutputPath, allDeps))
          return std::make_error_code(std::errc::not_supported);

      // Report the module's dependencies to the dependencyTracker
      if (dependencyTracker) {
        SmallString<128> SDKRelativeBuffer;
        for (auto &dep: allDeps) {
          StringRef fullPath = getFullDependencyPath(dep, ctx, SDKRelativeBuffer);
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
    auto remarkRebuild = Opts.remarkOnRebuildFromInterface
                       ? llvm::function_ref<void()>(remarkRebuildAll)
                       : nullptr;

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
      Identifier realName = ctx.getRealModuleName(ctx.getIdentifier(moduleName));
      ImplicitModuleInterfaceBuilder builder(
        ctx.SourceMgr, diagsToUse,
        astDelegate, interfacePath, realName.str(), cacheDir,
        prebuiltCacheDir, backupInterfaceDir, StringRef(),
        Opts.disableInterfaceLock,
        ctx.IgnoreAdjacentModules, diagnosticLoc,
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
      ImplicitModuleInterfaceBuilder fallbackBuilder(
        ctx.SourceMgr, &ctx.Diags, astDelegate, backupPath, moduleName, cacheDir,
        prebuiltCacheDir, backupInterfaceDir, StringRef(),
        Opts.disableInterfaceLock,
        ctx.IgnoreAdjacentModules, diagnosticLoc,
        dependencyTracker);
      if (rebuildInfo.sawOutOfDateModule(modulePath))
        fallbackBuilder.addExtraDependency(modulePath);
      // Add the canonical interface path as a dependency of this module.
      // This ensures that after the user manually fixed the canonical interface
      // file and removed the fallback interface file, we can rebuild the cache.
      fallbackBuilder.addExtraDependency(interfacePath);
      // Use cachedOutputPath as the output file path. This output path was
      // calculated using the canonical interface file path to make sure we
      // can find it from the canonical interface file.
      auto failedAgain = fallbackBuilder.buildSwiftModule(cachedOutputPath,
                                                          /*shouldSerializeDeps*/true,
                                                          &moduleBuffer,
                                                          remarkRebuild);
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
    ImportPath::Element ModuleID, const SerializedModuleBaseName &BaseName,
    SmallVectorImpl<char> *ModuleInterfacePath,
    SmallVectorImpl<char> *ModuleInterfaceSourcePath,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
    bool skipBuildingInterface, bool IsFramework,
    bool isTestableImport) {

  // If running in OnlySerialized mode, ModuleInterfaceLoader
  // should not have been constructed at all.
  assert(LoadMode != ModuleLoadingMode::OnlySerialized);

  std::string ModPath{BaseName.getName(file_types::TY_SwiftModuleFile)};

  // First check to see if the .swiftinterface exists at all. Bail if not.
  auto &fs = *Ctx.SourceMgr.getFileSystem();
  std::string InPath = BaseName.findInterfacePath(fs).getValueOr("");
  if (InPath.empty()) {
    if (fs.exists(ModPath)) {
      LLVM_DEBUG(llvm::dbgs()
                 << "No .swiftinterface file found adjacent to module file "
                 << ModPath << "\n");
      return std::make_error_code(std::errc::not_supported);
    }
    return std::make_error_code(std::errc::no_such_file_or_directory);
  }

  if (ModuleInterfaceSourcePath)
    ModuleInterfaceSourcePath->assign(InPath.begin(), InPath.end());

  // If we've been told to skip building interfaces, we are done here and do
  // not need to have the module actually built. For example, if we are
  // currently answering a `canImport` query, it is enough to have found
  // the interface.
  if (skipBuildingInterface) {
    if (ModuleInterfacePath)
      ModuleInterfacePath->assign(InPath.begin(), InPath.end());
    return std::error_code();
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
      ModuleInterfacePath->assign(InPath.begin(), InPath.end());
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
ModuleInterfaceCheckerImpl::getCompiledModuleCandidatesForInterface(StringRef moduleName, StringRef interfacePath) {
  // Derive .swiftmodule path from the .swiftinterface path.
  auto interfaceExt = file_types::getExtension(file_types::TY_SwiftModuleInterfaceFile);
  auto newExt = file_types::getExtension(file_types::TY_SwiftModuleFile);
  llvm::SmallString<32> modulePath;

  // When looking up the module for a private interface, strip the '.private.' section of the base name
  if (interfacePath.endswith(".private." + interfaceExt.str())) {
    auto newBaseName = llvm::sys::path::stem(llvm::sys::path::stem(interfacePath));
    modulePath = llvm::sys::path::parent_path(interfacePath);
    llvm::sys::path::append(modulePath, newBaseName + "." + newExt.str());
  } else {
    modulePath = interfacePath;
    llvm::sys::path::replace_extension(modulePath, newExt);
  }

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
    ArrayRef<std::string> candidates, llvm::vfs::OutputBackend &backend,
    StringRef outputPath) {
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
    if (Impl.upToDateChecker.swiftModuleIsUpToDate(mod, Impl.rebuildInfo,
                                                   deps, moduleBuffer)) {
      // If so, emit a forwarding module to the candidate.
      ForwardingModule FM(mod);
      auto hadError = withOutputPath(Ctx.Diags, backend, outputPath,
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
    StringRef OutPath, StringRef ABIOutputPath,
    bool SerializeDependencyHashes,
    bool TrackSystemDependencies, ModuleInterfaceLoaderOptions LoaderOpts,
    RequireOSSAModules_t RequireOSSAModules,
    bool silenceInterfaceDiagnostics) {
  InterfaceSubContextDelegateImpl astDelegate(
      SourceMgr, &Diags, SearchPathOpts, LangOpts, ClangOpts, LoaderOpts,
      /*CreateCacheDirIfAbsent*/ true, CacheDir, PrebuiltCacheDir,
      BackupInterfaceDir,
      SerializeDependencyHashes, TrackSystemDependencies, RequireOSSAModules);
  ImplicitModuleInterfaceBuilder builder(SourceMgr, &Diags, astDelegate, InPath,
                                         ModuleName, CacheDir, PrebuiltCacheDir,
                                         BackupInterfaceDir, ABIOutputPath,
                                         LoaderOpts.disableInterfaceLock,
                                         silenceInterfaceDiagnostics);
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
  ImplicitModuleInterfaceBuilder backupBuilder(SourceMgr, &Diags, astDelegate, backInPath,
                                               ModuleName, CacheDir, PrebuiltCacheDir,
                                               BackupInterfaceDir, ABIOutputPath,
                                               LoaderOpts.disableInterfaceLock,
                                               silenceInterfaceDiagnostics);
  // Ensure we can rebuild module after user changed the original interface file.
  backupBuilder.addExtraDependency(InPath);
  // FIXME: We really only want to serialize 'important' dependencies here, if
  //        we want to ship the built swiftmodules to another machine.
  return backupBuilder.buildSwiftModule(OutPath, /*shouldSerializeDeps*/true,
                                        /*ModuleBuffer*/nullptr, nullptr,
                                        SearchPathOpts.CandidateCompiledModules);
}

static bool readSwiftInterfaceVersionAndArgs(SourceManager &SM,
                                             DiagnosticEngine &Diags,
                                             llvm::StringSaver &ArgSaver,
                                             SwiftInterfaceInfo &interfaceInfo,
                                             StringRef interfacePath,
                                             SourceLoc diagnosticLoc) {
  llvm::vfs::FileSystem &fs = *SM.getFileSystem();
  auto FileOrError = swift::vfs::getFileOrSTDIN(fs, interfacePath);
  if (!FileOrError) {
    // Don't use this->diagnose() because it'll just try to re-open
    // interfacePath.
    Diags.diagnose(diagnosticLoc, diag::error_open_input_file, interfacePath,
                   FileOrError.getError().message());
    return true;
  }
  auto SB = FileOrError.get()->getBuffer();
  auto VersRe = getSwiftInterfaceFormatVersionRegex();
  auto CompRe = getSwiftInterfaceCompilerVersionRegex();
  SmallVector<StringRef, 2> VersMatches, CompMatches;

  if (!VersRe.match(SB, &VersMatches)) {
    InterfaceSubContextDelegateImpl::diagnose(
        interfacePath, diagnosticLoc, SM, &Diags,
        diag::error_extracting_version_from_module_interface);
    return true;
  }

  if (extractCompilerFlagsFromInterface(interfacePath, SB, ArgSaver,
                                        interfaceInfo.Arguments)) {
    InterfaceSubContextDelegateImpl::diagnose(
        interfacePath, diagnosticLoc, SM, &Diags,
        diag::error_extracting_version_from_module_interface);
    return true;
  }

  assert(VersMatches.size() == 2);
  // FIXME We should diagnose this at a location that makes sense:
  auto Vers =
      VersionParser::parseVersionString(VersMatches[1], SourceLoc(), &Diags);
  if (!Vers.has_value()) {
    InterfaceSubContextDelegateImpl::diagnose(
        interfacePath, diagnosticLoc, SM, &Diags,
        diag::error_extracting_version_from_module_interface);
    return true;
  }

  if (CompRe.match(SB, &CompMatches)) {
    assert(CompMatches.size() == 2);
    interfaceInfo.CompilerVersion = ArgSaver.save(CompMatches[1]).str();

    // For now, successfully parsing the tools version out of the interface is
    // optional.
    auto ToolsVersRe = getSwiftInterfaceCompilerToolsVersionRegex();
    SmallVector<StringRef, 2> VendorToolsVersMatches;
    if (ToolsVersRe.match(interfaceInfo.CompilerVersion,
                          &VendorToolsVersMatches)) {
      interfaceInfo.CompilerToolsVersion = VersionParser::parseVersionString(
          VendorToolsVersMatches[1], SourceLoc(), nullptr);
    }
  } else {
    // Don't diagnose; handwritten module interfaces don't include this field.
    interfaceInfo.CompilerVersion = "(unspecified, file possibly handwritten)";
  }

  // For now: we support anything with the same "major version" and assume
  // minor versions might be interesting for debugging, or special-casing a
  // compatible field variant.
  if (Vers->asMajorVersion() != InterfaceFormatVersion.asMajorVersion()) {
    InterfaceSubContextDelegateImpl::diagnose(
        interfacePath, diagnosticLoc, SM, &Diags,
        diag::unsupported_version_of_module_interface, interfacePath, *Vers);
    return true;
  }
  return false;
}

bool ModuleInterfaceLoader::buildExplicitSwiftModuleFromSwiftInterface(
    CompilerInstance &Instance, const StringRef moduleCachePath,
    const StringRef backupInterfaceDir, const StringRef prebuiltCachePath,
    const StringRef ABIDescriptorPath, StringRef interfacePath,
    StringRef outputPath, bool ShouldSerializeDeps,
    ArrayRef<std::string> CompiledCandidates,
    DependencyTracker *tracker) {
  
  if (!Instance.getInvocation().getIRGenOptions().AlwaysCompile) {
    // First, check if the expected output already exists and possibly
    // up-to-date w.r.t. all of the dependencies it was built with. If so, early
    // exit.
    UpToDateModuleCheker checker(
        Instance.getASTContext(),
        RequireOSSAModules_t(Instance.getSILOptions()));
    ModuleRebuildInfo rebuildInfo;
    SmallVector<FileDependency, 3> allDeps;
    std::unique_ptr<llvm::MemoryBuffer> moduleBuffer;
    if (checker.swiftModuleIsUpToDate(outputPath, rebuildInfo, allDeps,
                                      moduleBuffer)) {
      if (Instance.getASTContext()
              .LangOpts.EnableSkipExplicitInterfaceModuleBuildRemarks) {
        Instance.getDiags().diagnose(
            SourceLoc(), diag::explicit_interface_build_skipped, outputPath);
      }
      return false;
    }
  }
  
  // Read out the compiler version.
  llvm::BumpPtrAllocator alloc;
  llvm::StringSaver ArgSaver(alloc);
  SwiftInterfaceInfo InterfaceInfo;
  readSwiftInterfaceVersionAndArgs(Instance.getSourceMgr(), Instance.getDiags(),
                                   ArgSaver, InterfaceInfo, interfacePath,
                                   SourceLoc());

  auto Builder = ExplicitModuleInterfaceBuilder(
      Instance, &Instance.getDiags(), Instance.getSourceMgr(),
      moduleCachePath, backupInterfaceDir, prebuiltCachePath,
      ABIDescriptorPath, {});
  auto error = Builder.buildSwiftModuleFromInterface(
      interfacePath, outputPath, ShouldSerializeDeps, /*ModuleBuffer*/nullptr,
      CompiledCandidates, InterfaceInfo.CompilerVersion);
  if (!error)
    return false;
  else
    return true;
}

void ModuleInterfaceLoader::collectVisibleTopLevelModuleNames(
    SmallVectorImpl<Identifier> &names) const {
  collectVisibleTopLevelModuleNamesImpl(
      names,
      file_types::getExtension(file_types::TY_SwiftModuleInterfaceFile));
}

void InterfaceSubContextDelegateImpl::inheritOptionsForBuildingInterface(
    const SearchPathOptions &SearchPathOpts, const LangOptions &LangOpts,
    const ClangImporterOptions &clangImporterOpts,
    bool suppressRemarks, RequireOSSAModules_t RequireOSSAModules) {
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

  if (LangOpts.ClangTarget.has_value()) {
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

  genericSubInvocation.setImportSearchPaths(
      SearchPathOpts.getImportSearchPaths());
  genericSubInvocation.setFrameworkSearchPaths(
      SearchPathOpts.getFrameworkSearchPaths());
  if (!SearchPathOpts.getSDKPath().empty()) {
    // Add -sdk arguments to the module building commands.
    // Module building commands need this because dependencies sometimes use
    // sdk-relative paths (prebuilt modules for example). Without -sdk, the command
    // will not be able to local these dependencies, leading to unnecessary
    // building from textual interfaces.
    GenericArgs.push_back("-sdk");
    GenericArgs.push_back(ArgSaver.save(SearchPathOpts.getSDKPath()));
    genericSubInvocation.setSDKPath(SearchPathOpts.getSDKPath().str());
  }

  genericSubInvocation.getFrontendOptions().InputMode
      = FrontendOptions::ParseInputMode::SwiftModuleInterface;
  if (!SearchPathOpts.RuntimeResourcePath.empty()) {
    genericSubInvocation.setRuntimeResourcePath(SearchPathOpts.RuntimeResourcePath);
  }

  // Inhibit warnings from the genericSubInvocation since we are assuming the user
  // is not in a position to address them.
  genericSubInvocation.getDiagnosticOptions().SuppressWarnings = true;
  GenericArgs.push_back("-suppress-warnings");
  // Inherit the parent invocation's setting on whether to suppress remarks
  if (suppressRemarks) {
    genericSubInvocation.getDiagnosticOptions().SuppressRemarks = true;
    GenericArgs.push_back("-suppress-remarks");
  }

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
  if (LangOpts.DisableAvailabilityChecking) {
    genericSubInvocation.getLangOptions().DisableAvailabilityChecking = true;
    GenericArgs.push_back("-disable-availability-checking");
  }

  // Pass-down the obfuscators so we can get the serialized search paths properly.
  genericSubInvocation.setSerializedPathObfuscator(
    SearchPathOpts.DeserializedPathRecoverer);
  SearchPathOpts.DeserializedPathRecoverer
    .forEachPair([&](StringRef lhs, StringRef rhs) {
      GenericArgs.push_back("-serialized-path-obfuscate");
      std::string pair = (llvm::Twine(lhs) + "=" + rhs).str();
      GenericArgs.push_back(ArgSaver.save(pair));
  });

  if (LangOpts.hasFeature(Feature::LayoutPrespecialization)) {
    genericSubInvocation.getLangOptions().Features.insert(
      Feature::LayoutPrespecialization);
  }

  // Validate Clang modules once per-build session flags must be consistent
  // across all module sub-invocations
  if (clangImporterOpts.ValidateModulesOnce) {
    genericSubInvocation.getClangImporterOptions().ValidateModulesOnce = true;
    genericSubInvocation.getClangImporterOptions().BuildSessionFilePath = clangImporterOpts.BuildSessionFilePath;
    GenericArgs.push_back("-validate-clang-modules-once");
    GenericArgs.push_back("-clang-build-session-file");
    GenericArgs.push_back(clangImporterOpts.BuildSessionFilePath);
  }

  if (!clangImporterOpts.CASPath.empty()) {
    genericSubInvocation.getClangImporterOptions().CASPath =
        clangImporterOpts.CASPath;
    GenericArgs.push_back("-enable-cas");
    GenericArgs.push_back("-cas-path");
    GenericArgs.push_back(clangImporterOpts.CASPath);
  }

  if (clangImporterOpts.UseClangIncludeTree) {
    genericSubInvocation.getClangImporterOptions().UseClangIncludeTree =
        clangImporterOpts.UseClangIncludeTree;
    GenericArgs.push_back("-clang-include-tree");
  }
}

bool InterfaceSubContextDelegateImpl::extractSwiftInterfaceVersionAndArgs(
    CompilerInvocation &subInvocation, SwiftInterfaceInfo &interfaceInfo,
    StringRef interfacePath, SourceLoc diagnosticLoc) {
  if (readSwiftInterfaceVersionAndArgs(SM, *Diags, ArgSaver, interfaceInfo,
                                       interfacePath, diagnosticLoc))
    return true;

  SmallString<32> ExpectedModuleName = subInvocation.getModuleName();
  if (subInvocation.parseArgs(interfaceInfo.Arguments, *Diags)) {
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
                                     clangImporterOpts,
                                     Diags->getSuppressRemarks(),
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
  // If building an application extension, make sure API use
  // is restricted accordingly in downstream dependnecies.
  if (langOpts.EnableAppExtensionRestrictions) {
    genericSubInvocation.getLangOptions().EnableAppExtensionRestrictions = true;
    GenericArgs.push_back("-application-extension");
  }

  // Save the parent invocation's Target Triple
  ParentInvocationTarget = langOpts.Target;

  // Pass down -explicit-swift-module-map-file
  // FIXME: we shouldn't need this. Remove it?
  StringRef explicitSwiftModuleMap = searchPathOpts.ExplicitSwiftModuleMap;
  genericSubInvocation.getSearchPathOptions().ExplicitSwiftModuleMap =
    explicitSwiftModuleMap.str();
  auto &subClangImporterOpts = genericSubInvocation.getClangImporterOptions();
  // Respect the detailed-record preprocessor setting of the parent context.
  // This, and the "raw" clang module format it implicitly enables, are
  // required by sourcekitd.
  subClangImporterOpts.DetailedPreprocessingRecord =
    clangImporterOpts.DetailedPreprocessingRecord;
  subClangImporterOpts.CASPath = clangImporterOpts.CASPath;

  // If the compiler has been asked to be strict with ensuring downstream dependencies
  // get the parent invocation's context, or this is an Explicit build, inherit the
  // extra Clang arguments also.
  if (LoaderOpts.strictImplicitModuleContext || LoaderOpts.disableImplicitSwiftModule) {
    // Inherit any clang-specific state of the compilation (macros, clang flags, etc.)
    subClangImporterOpts.ExtraArgs = clangImporterOpts.ExtraArgs;
    for (auto arg : subClangImporterOpts.ExtraArgs) {
      GenericArgs.push_back("-Xcc");
      GenericArgs.push_back(ArgSaver.save(arg));
    }
  }

  subClangImporterOpts.EnableClangSPI = clangImporterOpts.EnableClangSPI;
  if (!subClangImporterOpts.EnableClangSPI) {
    GenericArgs.push_back("-disable-clang-spi");
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
  // This flag only matters when we are verifying an textual interface.
  frontendOpts.DowngradeInterfaceVerificationError =
    LoaderOpts.downgradeInterfaceVerificationError;
  // Note that we don't assume cachePath is the same as the Clang
  // module cache path at this point.
  if (buildModuleCacheDirIfAbsent && !moduleCachePath.empty())
    (void)llvm::sys::fs::create_directories(moduleCachePath);

  // Inherit all block list configuration files
  frontendOpts.BlocklistConfigFilePaths = langOpts.BlocklistConfigFilePaths;
  for (auto &blocklist: langOpts.BlocklistConfigFilePaths) {
    GenericArgs.push_back("-blocklist-file");
    GenericArgs.push_back(blocklist);
  }
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

  return llvm::toString(llvm::APInt(64, H), 36, /*Signed=*/false);
}

std::error_code
InterfaceSubContextDelegateImpl::runInSubContext(StringRef moduleName,
                                                 StringRef interfacePath,
                                                 StringRef outputPath,
                                                 SourceLoc diagLoc,
    llvm::function_ref<std::error_code(ASTContext&, ModuleDecl*, ArrayRef<StringRef>,
                            ArrayRef<StringRef>, StringRef)> action) {
  return runInSubCompilerInstance(moduleName, interfacePath, outputPath,
                                  diagLoc, /*silenceErrors=*/false,
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
                                                          bool silenceErrors,
                  llvm::function_ref<std::error_code(SubCompilerInstanceInfo&)> action) {
  // We are about to mess up the compiler invocation by using the compiler
  // arguments in the textual interface file. So copy to use a new compiler
  // invocation.
  CompilerInvocation subInvocation = genericSubInvocation;

  // Save the target triple from the original context.
  llvm::Triple originalTargetTriple(subInvocation.getLangOptions().Target);

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
  SwiftInterfaceInfo interfaceInfo;
  // Extract compiler arguments from the interface file and use them to configure
  // the compiler invocation.
  if (extractSwiftInterfaceVersionAndArgs(subInvocation, interfaceInfo,
                                          interfacePath, diagLoc)) {
    return std::make_error_code(std::errc::not_supported);
  }

  // Prior to Swift 5.9, swiftinterfaces were always built (accidentally) with
  // `-target-min-inlining-version target` prepended to the argument list. To
  // preserve compatibility we must continue to prepend those flags to the
  // invocation when the interface was generated by an older compiler.
  if (auto toolsVersion = interfaceInfo.CompilerToolsVersion) {
    if (toolsVersion < version::Version{5, 9}) {
      SubArgs.push_back("-target-min-inlining-version");
      SubArgs.push_back("target");
    }
  }

  SubArgs.insert(SubArgs.end(), interfaceInfo.Arguments.begin(),
                 interfaceInfo.Arguments.end());

  // Insert arguments collected from the interface file.
  BuildArgs.insert(BuildArgs.end(), SubArgs.begin(), SubArgs.end());
  if (subInvocation.parseArgs(SubArgs, *Diags)) {
    return std::make_error_code(std::errc::not_supported);
  }

  // If the target triple parsed from the Swift interface file differs
  // only in subarchitecture from the original target triple, then
  // we have loaded a Swift interface from a different-but-compatible
  // architecture slice. Use the original subarchitecture.
  llvm::Triple parsedTargetTriple(subInvocation.getTargetTriple());
  if (parsedTargetTriple.getSubArch() != originalTargetTriple.getSubArch() &&
      parsedTargetTriple.getArch() == originalTargetTriple.getArch() &&
      parsedTargetTriple.getVendor() == originalTargetTriple.getVendor() &&
      parsedTargetTriple.getOS() == originalTargetTriple.getOS() &&
      parsedTargetTriple.getEnvironment()
      == originalTargetTriple.getEnvironment()) {
    parsedTargetTriple.setArchName(originalTargetTriple.getArchName());
    subInvocation.setTargetTriple(parsedTargetTriple.str());

    // Overload the target in the BuildArgs as well
    BuildArgs.push_back("-target");
    BuildArgs.push_back(parsedTargetTriple.str());
  }

  CompilerInstance subInstance;
  SubCompilerInstanceInfo info;
  info.Instance = &subInstance;
  info.CompilerVersion = interfaceInfo.CompilerVersion;

  subInstance.getSourceMgr().setFileSystem(SM.getFileSystem());

  ForwardingDiagnosticConsumer FDC(*Diags);
  if (!silenceErrors)
    subInstance.addDiagnosticConsumer(&FDC);
  std::string InstanceSetupError;
  if (subInstance.setup(subInvocation, InstanceSetupError)) {
    return std::make_error_code(std::errc::not_supported);
  }

  info.BuildArguments = BuildArgs;
  info.Hash = CacheHash;
  auto target = *(std::find(BuildArgs.rbegin(), BuildArgs.rend(), "-target") - 1);
  auto langVersion = *(std::find(BuildArgs.rbegin(), BuildArgs.rend(),
                                 "-swift-version") - 1);

  std::vector<StringRef> ExtraPCMArgs = {
    // PCMs should use the effective Swift language version for apinotes.
    "-Xcc",
    ArgSaver.save((llvm::Twine("-fapinotes-swift-version=") + langVersion).str())
  };
  if (!subInvocation.getLangOptions().ClangTarget.has_value()) {
    ExtraPCMArgs.insert(ExtraPCMArgs.begin(), {"-Xcc", "-target",
                                               "-Xcc", target});
  }

  info.ExtraPCMArgs = ExtraPCMArgs;
  // Run the action under the sub compiler instance.
  return action(info);
}

struct ExplicitSwiftModuleLoader::Implementation {
  ASTContext &Ctx;
  llvm::BumpPtrAllocator Allocator;
  llvm::StringMap<ExplicitSwiftModuleInputInfo> ExplicitModuleMap;
  Implementation(ASTContext &Ctx) : Ctx(Ctx) {}

  void parseSwiftExplicitModuleMap(StringRef fileName) {
    ExplicitModuleMapParser parser(Allocator);
    llvm::StringMap<ExplicitClangModuleInputInfo> ExplicitClangModuleMap;
    // Load the input file.
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> fileBufOrErr =
        llvm::MemoryBuffer::getFile(fileName);
    if (!fileBufOrErr) {
      Ctx.Diags.diagnose(SourceLoc(), diag::explicit_swift_module_map_missing,
                         fileName);
      return;
    }

    auto hasError = parser.parseSwiftExplicitModuleMap(
        (*fileBufOrErr)->getMemBufferRef(), ExplicitModuleMap,
        ExplicitClangModuleMap);

    if (hasError)
      Ctx.Diags.diagnose(SourceLoc(), diag::explicit_swift_module_map_corrupted,
                         fileName);

    // A single module map can define multiple modules; keep track of the ones
    // we've seen so that we don't generate duplicate flags.
    std::set<std::string> moduleMapsSeen;
    std::vector<std::string> &extraClangArgs = Ctx.ClangImporterOpts.ExtraArgs;
    for (auto &entry : ExplicitClangModuleMap) {
      const auto &moduleMapPath = entry.getValue().moduleMapPath;
      if (!moduleMapPath.empty() &&
          moduleMapsSeen.find(moduleMapPath) == moduleMapsSeen.end()) {
        moduleMapsSeen.insert(moduleMapPath);
        extraClangArgs.push_back(
            (Twine("-fmodule-map-file=") + moduleMapPath).str());
      }

      const auto &modulePath = entry.getValue().modulePath;
      if (!modulePath.empty()) {
        extraClangArgs.push_back(
            (Twine("-fmodule-file=") + entry.getKey() + "=" + modulePath)
                .str());
      }
    }
  }

  void addCommandLineExplicitInputs(
      const std::vector<std::pair<std::string, std::string>>
          &commandLineExplicitInputs) {
    for (const auto &moduleInput : commandLineExplicitInputs) {
      ExplicitSwiftModuleInputInfo entry(moduleInput.second, {}, {});
      ExplicitModuleMap.try_emplace(moduleInput.first, std::move(entry));
    }
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

bool ExplicitSwiftModuleLoader::findModule(
    ImportPath::Element ModuleID, SmallVectorImpl<char> *ModuleInterfacePath,
    SmallVectorImpl<char> *ModuleInterfaceSourcePath,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
    bool skipBuildingInterface, bool isTestableDependencyLookup,
    bool &IsFramework, bool &IsSystemModule) {
  // Find a module with an actual, physical name on disk, in case
  // -module-alias is used (otherwise same).
  //
  // For example, if '-module-alias Foo=Bar' is passed in to the frontend, and an
  // input file has 'import Foo', a module called Bar (real name) should be searched.
  StringRef moduleName = Ctx.getRealModuleName(ModuleID.Item).str();

  auto it = Impl.ExplicitModuleMap.find(moduleName);
  // If no explicit module path is given matches the name, return with an
  // error code.
  if (it == Impl.ExplicitModuleMap.end()) {
    return false;
  }
  auto &moduleInfo = it->getValue();

  // Set IsFramework bit according to the moduleInfo
  IsFramework = moduleInfo.isFramework;
  IsSystemModule = moduleInfo.isSystem;

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
  if (moduleInfo.moduleDocPath.has_value()) {
    auto moduleDocBuf = fs.getBufferForFile(moduleInfo.moduleDocPath.value());
    if (moduleBuf)
      *ModuleDocBuffer = std::move(moduleDocBuf.get());
  }
  // Open .swiftsourceinfo file
  if (moduleInfo.moduleSourceInfoPath.has_value()) {
    auto moduleSourceInfoBuf = fs.getBufferForFile(moduleInfo.moduleSourceInfoPath.value());
    if (moduleSourceInfoBuf)
      *ModuleSourceInfoBuffer = std::move(moduleSourceInfoBuf.get());
  }
  return true;
}

std::error_code ExplicitSwiftModuleLoader::findModuleFilesInDirectory(
    ImportPath::Element ModuleID, const SerializedModuleBaseName &BaseName,
    SmallVectorImpl<char> *ModuleInterfacePath,
    SmallVectorImpl<char> *ModuleInterfaceSourcePath,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
    bool skipBuildingInterface, bool IsFramework,
    bool IsTestableDependencyLookup) {
  llvm_unreachable("Not supported in the Explicit Swift Module Loader.");
  return std::make_error_code(std::errc::not_supported);
}

bool ExplicitSwiftModuleLoader::canImportModule(
    ImportPath::Module path, ModuleVersionInfo *versionInfo,
    bool isTestableDependencyLookup) {
  // FIXME: Swift submodules?
  if (path.hasSubmodule())
    return false;
  ImportPath::Element mID = path.front();
  // Look up the module with the real name (physical name on disk);
  // in case `-module-alias` is used, the name appearing in source files
  // and the real module name are different. For example, '-module-alias Foo=Bar'
  // maps Foo appearing in source files, e.g. 'import Foo', to the real module
  // name Bar (on-disk name), which should be searched for loading.
  StringRef moduleName = Ctx.getRealModuleName(mID.Item).str();
  auto it = Impl.ExplicitModuleMap.find(moduleName);
  // If no provided explicit module matches the name, then it cannot be imported.
  if (it == Impl.ExplicitModuleMap.end()) {
    return false;
  }

  // If the caller doesn't want version info we're done.
  if (!versionInfo)
    return true;

  // Open .swiftmodule file and read out the version
  auto &fs = *Ctx.SourceMgr.getFileSystem();
  auto moduleBuf = fs.getBufferForFile(it->second.modulePath);
  if (!moduleBuf) {
    Ctx.Diags.diagnose(SourceLoc(), diag::error_opening_explicit_module_file,
                       it->second.modulePath);
    return false;
  }
  auto metaData = serialization::validateSerializedAST(
      (*moduleBuf)->getBuffer(), Ctx.SILOpts.EnableOSSAModules,
      Ctx.LangOpts.SDKName, !Ctx.LangOpts.DebuggerSupport);
  versionInfo->setVersion(metaData.userModuleVersion,
                          ModuleVersionSourceKind::SwiftBinaryModule);
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
    StringRef ExplicitSwiftModuleMap,
    const std::vector<std::pair<std::string, std::string>> &ExplicitSwiftModuleInputs,
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
  // If some modules are provided with explicit
  // '-swift-module-file' options, add those as well.
  if (!ExplicitSwiftModuleInputs.empty()) {
    Impl.addCommandLineExplicitInputs(ExplicitSwiftModuleInputs);
  }

  return result;
}

struct ExplicitCASModuleLoader::Implementation {
  ASTContext &Ctx;
  llvm::BumpPtrAllocator Allocator;
  llvm::cas::ObjectStore &CAS;
  llvm::cas::ActionCache &Cache;

  llvm::StringMap<ExplicitSwiftModuleInputInfo> ExplicitModuleMap;

  Implementation(ASTContext &Ctx, llvm::cas::ObjectStore &CAS,
                 llvm::cas::ActionCache &Cache)
      : Ctx(Ctx), CAS(CAS), Cache(Cache) {}

  llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> loadBuffer(StringRef ID) {
    auto key = CAS.parseID(ID);
    if (!key)
      return key.takeError();

    auto ref = CAS.getReference(*key);
    if (!ref)
      return nullptr;

    auto loaded = CAS.getProxy(*ref);
    if (!loaded)
      return loaded.takeError();

    return loaded->getMemoryBuffer();
  }

  // Same as the regular explicit module map but must be loaded from
  // CAS, instead of a file that is not tracked by the dependency.
  void parseSwiftExplicitModuleMap(StringRef ID) {
    ExplicitModuleMapParser parser(Allocator);
    llvm::StringMap<ExplicitClangModuleInputInfo> ExplicitClangModuleMap;
    auto buf = loadBuffer(ID);
    if (!buf) {
      Ctx.Diags.diagnose(SourceLoc(), diag::error_cas,
                         toString(buf.takeError()));
      return;
    }
    if (!*buf) {
      Ctx.Diags.diagnose(SourceLoc(), diag::explicit_swift_module_map_missing,
                         ID);
      return;
    }
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> fileBufOrErr =
        llvm::MemoryBuffer::getFile(ID);

    auto hasError = parser.parseSwiftExplicitModuleMap(
        (*buf)->getMemBufferRef(), ExplicitModuleMap, ExplicitClangModuleMap);

    if (hasError)
      Ctx.Diags.diagnose(SourceLoc(), diag::explicit_swift_module_map_corrupted,
                         ID);

    std::set<std::string> moduleMapsSeen;
    std::vector<std::string> &extraClangArgs = Ctx.ClangImporterOpts.ExtraArgs;
    for (auto &entry : ExplicitClangModuleMap) {
      const auto &moduleMapPath = entry.getValue().moduleMapPath;
      if (!moduleMapPath.empty() &&
          !Ctx.ClangImporterOpts.UseClangIncludeTree &&
          moduleMapsSeen.find(moduleMapPath) == moduleMapsSeen.end()) {
        moduleMapsSeen.insert(moduleMapPath);
        extraClangArgs.push_back(
            (Twine("-fmodule-map-file=") + moduleMapPath).str());
      }

      const auto &modulePath = entry.getValue().modulePath;
      if (!modulePath.empty()) {
        extraClangArgs.push_back(
            (Twine("-fmodule-file=") + entry.getKey() + "=" + modulePath)
                .str());
      }
      auto cachePath = entry.getValue().moduleCacheKey;
      if (cachePath) {
        extraClangArgs.push_back("-Xclang");
        extraClangArgs.push_back("-fmodule-file-cache-key");
        extraClangArgs.push_back("-Xclang");
        extraClangArgs.push_back(modulePath);
        extraClangArgs.push_back("-Xclang");
        extraClangArgs.push_back(*cachePath);
      }
    }
  }

  void addCommandLineExplicitInputs(
      const std::vector<std::pair<std::string, std::string>>
          &commandLineExplicitInputs) {
    for (const auto &moduleInput : commandLineExplicitInputs) {
      ExplicitSwiftModuleInputInfo entry(moduleInput.second, {}, {});
      ExplicitModuleMap.try_emplace(moduleInput.first, std::move(entry));
    }
  }

  llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
  loadFileBuffer(StringRef ID, StringRef Name) {
    auto key = CAS.parseID(ID);
    if (!key)
      return key.takeError();

    auto moduleLookup = Cache.get(*key);
    if (!moduleLookup)
      return moduleLookup.takeError();
    if (!*moduleLookup)
      return nullptr;

    auto moduleRef = CAS.getReference(**moduleLookup);
    if (!moduleRef)
      return nullptr;

    clang::cas::CompileJobResultSchema schema(CAS);
    auto result = schema.load(*moduleRef);
    if (!result)
      return result.takeError();
    auto output = result->getOutput(
        clang::cas::CompileJobCacheResult::OutputKind::MainOutput);
    if (!output)
      return nullptr;

    auto buf = CAS.getProxy(output->Object);
    if (!buf)
      return buf.takeError();

    return buf->getMemoryBuffer(Name);
  }

  llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
  loadModuleFromPath(StringRef Path, DiagnosticEngine &Diags) {
    for (auto &Deps : ExplicitModuleMap) {
      if (Deps.second.modulePath == Path) {
        if (!Deps.second.moduleCacheKey)
          return nullptr;
        return loadCachedCompileResultFromCacheKey(
            CAS, Cache, Diags, *Deps.second.moduleCacheKey, Path);
      }
    }
    return nullptr;
  }
};

ExplicitCASModuleLoader::ExplicitCASModuleLoader(ASTContext &ctx,
                                                 llvm::cas::ObjectStore &CAS,
                                                 llvm::cas::ActionCache &cache,
                                                 DependencyTracker *tracker,
                                                 ModuleLoadingMode loadMode,
                                                 bool IgnoreSwiftSourceInfoFile)
    : SerializedModuleLoaderBase(ctx, tracker, loadMode,
                                 IgnoreSwiftSourceInfoFile),
      Impl(*new Implementation(ctx, CAS, cache)) {}

ExplicitCASModuleLoader::~ExplicitCASModuleLoader() { delete &Impl; }

bool ExplicitCASModuleLoader::findModule(
    ImportPath::Element ModuleID, SmallVectorImpl<char> *ModuleInterfacePath,
    SmallVectorImpl<char> *ModuleInterfaceSourcePath,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
    bool skipBuildingInterface, bool isTestableDependencyLookup,
    bool &IsFramework, bool &IsSystemModule) {
  // Find a module with an actual, physical name on disk, in case
  // -module-alias is used (otherwise same).
  //
  // For example, if '-module-alias Foo=Bar' is passed in to the frontend, and
  // an input file has 'import Foo', a module called Bar (real name) should be
  // searched.
  StringRef moduleName = Ctx.getRealModuleName(ModuleID.Item).str();

  auto it = Impl.ExplicitModuleMap.find(moduleName);
  // If no explicit module path is given matches the name, return with an
  // error code.
  if (it == Impl.ExplicitModuleMap.end()) {
    return false;
  }
  auto &moduleInfo = it->getValue();

  // Set IsFramework bit according to the moduleInfo
  IsFramework = moduleInfo.isFramework;
  IsSystemModule = moduleInfo.isSystem;

  // Fallback check for module cache key passed on command-line as module path.
  std::string moduleCASID = moduleInfo.moduleCacheKey
                                ? *moduleInfo.moduleCacheKey
                                : moduleInfo.modulePath;

  // FIXME: the loaded module buffer doesn't set an identifier so it
  // is not tracked in dependency tracker, which doesn't handle modules
  // that are not located on disk.
  auto moduleBuf = loadCachedCompileResultFromCacheKey(Impl.CAS, Impl.Cache,
                                                       Ctx.Diags, moduleCASID);
  if (!moduleBuf) {
    // We cannot read the module content, diagnose.
    Ctx.Diags.diagnose(SourceLoc(), diag::error_opening_explicit_module_file,
                       moduleInfo.modulePath);
    return false;
  }

  const bool isForwardingModule =
      !serialization::isSerializedAST(moduleBuf->getBuffer());
  // If the module is a forwarding module, read the actual content from the path
  // encoded in the forwarding module as the actual module content.
  if (isForwardingModule) {
    auto forwardingModule = ForwardingModule::load(*moduleBuf.get());
    if (forwardingModule) {
      // Look through ExplicitModuleMap for paths.
      // TODO: need to have dependency scanner reports forwarded module as
      // dependency for this compilation and ingested into CAS.
      auto moduleOrErr = Impl.loadModuleFromPath(
          forwardingModule->underlyingModulePath, Ctx.Diags);
      if (!moduleOrErr) {
        llvm::consumeError(moduleOrErr.takeError());
        Ctx.Diags.diagnose(SourceLoc(),
                           diag::error_opening_explicit_module_file,
                           moduleInfo.modulePath);
        return false;
      }
      moduleBuf = std::move(*moduleOrErr);
      if (!moduleBuf) {
        // We cannot read the module content, diagnose.
        Ctx.Diags.diagnose(SourceLoc(),
                           diag::error_opening_explicit_module_file,
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
  *ModuleBuffer = std::move(moduleBuf);

  // TODO: support .swiftdoc file and .swiftsourceinfo file
  return true;
}

std::error_code ExplicitCASModuleLoader::findModuleFilesInDirectory(
    ImportPath::Element ModuleID, const SerializedModuleBaseName &BaseName,
    SmallVectorImpl<char> *ModuleInterfacePath,
    SmallVectorImpl<char> *ModuleInterfaceSourcePath,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
    bool skipBuildingInterface, bool IsFramework,
    bool IsTestableDependencyLookup) {
  llvm_unreachable("Not supported in the Explicit Swift Module Loader.");
  return std::make_error_code(std::errc::not_supported);
}

bool ExplicitCASModuleLoader::canImportModule(
    ImportPath::Module path, ModuleVersionInfo *versionInfo,
    bool isTestableDependencyLookup) {
  // FIXME: Swift submodules?
  if (path.hasSubmodule())
    return false;
  ImportPath::Element mID = path.front();
  // Look up the module with the real name (physical name on disk);
  // in case `-module-alias` is used, the name appearing in source files
  // and the real module name are different. For example, '-module-alias
  // Foo=Bar' maps Foo appearing in source files, e.g. 'import Foo', to the real
  // module name Bar (on-disk name), which should be searched for loading.
  StringRef moduleName = Ctx.getRealModuleName(mID.Item).str();
  auto it = Impl.ExplicitModuleMap.find(moduleName);
  // If no provided explicit module matches the name, then it cannot be
  // imported.
  if (it == Impl.ExplicitModuleMap.end()) {
    return false;
  }

  // If the caller doesn't want version info we're done.
  if (!versionInfo)
    return true;

  // Open .swiftmodule file and read out the version
  std::string moduleCASID = it->second.moduleCacheKey
                                ? *it->second.moduleCacheKey
                                : it->second.modulePath;
  auto moduleBuf = Impl.loadFileBuffer(moduleCASID, it->second.modulePath);
  if (!moduleBuf) {
    Ctx.Diags.diagnose(SourceLoc(), diag::error_cas,
                       toString(moduleBuf.takeError()));
    return false;
  }
  if (!*moduleBuf) {
    Ctx.Diags.diagnose(SourceLoc(), diag::error_opening_explicit_module_file,
                       it->second.modulePath);
    return false;
  }
  auto metaData = serialization::validateSerializedAST(
      (*moduleBuf)->getBuffer(), Ctx.SILOpts.EnableOSSAModules,
      Ctx.LangOpts.SDKName, !Ctx.LangOpts.DebuggerSupport);
  versionInfo->setVersion(metaData.userModuleVersion,
                          ModuleVersionSourceKind::SwiftBinaryModule);
  return true;
}

void ExplicitCASModuleLoader::collectVisibleTopLevelModuleNames(
    SmallVectorImpl<Identifier> &names) const {
  for (auto &entry : Impl.ExplicitModuleMap) {
    names.push_back(Ctx.getIdentifier(entry.getKey()));
  }
}

std::unique_ptr<ExplicitCASModuleLoader> ExplicitCASModuleLoader::create(
    ASTContext &ctx, llvm::cas::ObjectStore &CAS, llvm::cas::ActionCache &cache,
    DependencyTracker *tracker, ModuleLoadingMode loadMode,
    StringRef ExplicitSwiftModuleMap,
    const std::vector<std::pair<std::string, std::string>>
        &ExplicitSwiftModuleInputs,
    bool IgnoreSwiftSourceInfoFile) {
  auto result =
      std::unique_ptr<ExplicitCASModuleLoader>(new ExplicitCASModuleLoader(
          ctx, CAS, cache, tracker, loadMode, IgnoreSwiftSourceInfoFile));
  auto &Impl = result->Impl;
  // If the explicit module map is given, try parse it.
  if (!ExplicitSwiftModuleMap.empty()) {
    // Parse a JSON file to collect explicitly built modules.
    Impl.parseSwiftExplicitModuleMap(ExplicitSwiftModuleMap);
  }
  // If some modules are provided with explicit
  // '-swift-module-file' options, add those as well.
  if (!ExplicitSwiftModuleInputs.empty()) {
    Impl.addCommandLineExplicitInputs(ExplicitSwiftModuleInputs);
  }

  return result;
}
