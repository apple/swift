//===--- SerializedModuleLoader.cpp - Import Swift modules ----------------===//
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

#include "swift/Serialization/SerializedModuleLoader.h"
#include "ModuleFile.h"
#include "ModuleFileSharedCore.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/ModuleDependencies.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Version.h"
#include "swift/Option/Options.h"

#include "llvm/Option/OptTable.h"
#include "llvm/Option/ArgList.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/CommandLine.h"
#include <system_error>

using namespace swift;
using swift::version::Version;

namespace {

/// Apply \c body for each target-specific module file base name to search from
/// most to least desirable.
void forEachTargetModuleBasename(const ASTContext &Ctx,
                                 llvm::function_ref<void(StringRef)> body) {
  auto normalizedTarget = getTargetSpecificModuleTriple(Ctx.LangOpts.Target);
  body(normalizedTarget.str());

  // We used the un-normalized architecture as a target-specific
  // module name. Fall back to that behavior.
  body(Ctx.LangOpts.Target.getArchName());

  // FIXME: We used to use "major architecture" names for these files---the
  // names checked in "#if arch(...)". Fall back to that name in the one case
  // where it's different from what Swift 4.2 supported:
  // - 32-bit ARM platforms (formerly "arm")
  // We should be able to drop this once there's an Xcode that supports the
  // new names.
  if (Ctx.LangOpts.Target.getArch() == llvm::Triple::ArchType::arm) {
    body("arm");
  }
}

enum class SearchPathKind {
  Import,
  Framework,
  RuntimeLibrary,
};

/// Apply \p body for each module search path in \p Ctx until \p body returns
/// non-None value. Returns the return value from \p body, or \c None.
Optional<bool> forEachModuleSearchPath(
    const ASTContext &Ctx,
    llvm::function_ref<Optional<bool>(StringRef, SearchPathKind, bool isSystem)>
        callback) {
  for (const auto &path : Ctx.SearchPathOpts.ImportSearchPaths)
    if (auto result =
            callback(path, SearchPathKind::Import, /*isSystem=*/false))
      return result;

  for (const auto &path : Ctx.SearchPathOpts.FrameworkSearchPaths)
    if (auto result =
            callback(path.Path, SearchPathKind::Framework, path.IsSystem))
      return result;

  // Apple platforms have extra implicit framework search paths:
  // $SDKROOT/System/Library/Frameworks/ and $SDKROOT/Library/Frameworks/.
  if (Ctx.LangOpts.Target.isOSDarwin()) {
    SmallString<128> scratch;
    scratch = Ctx.SearchPathOpts.SDKPath;
    llvm::sys::path::append(scratch, "System", "Library", "Frameworks");
    if (auto result =
            callback(scratch, SearchPathKind::Framework, /*isSystem=*/true))
      return result;

    scratch = Ctx.SearchPathOpts.SDKPath;
    llvm::sys::path::append(scratch, "Library", "Frameworks");
    if (auto result =
            callback(scratch, SearchPathKind::Framework, /*isSystem=*/true))
      return result;
  }

  for (auto importPath : Ctx.SearchPathOpts.RuntimeLibraryImportPaths) {
    if (auto result = callback(importPath, SearchPathKind::RuntimeLibrary,
                               /*isSystem=*/true))
      return result;
  }

  return None;
}
} // end unnamed namespace

// Defined out-of-line so that we can see ~ModuleFile.
SerializedModuleLoaderBase::SerializedModuleLoaderBase(
    ASTContext &ctx, DependencyTracker *tracker, ModuleLoadingMode loadMode,
    bool IgnoreSwiftSourceInfoFile)
    : ModuleLoader(tracker), Ctx(ctx), LoadMode(loadMode),
      IgnoreSwiftSourceInfoFile(IgnoreSwiftSourceInfoFile) {}

SerializedModuleLoaderBase::~SerializedModuleLoaderBase() = default;
ImplicitSerializedModuleLoader::~ImplicitSerializedModuleLoader() = default;
MemoryBufferSerializedModuleLoader::~MemoryBufferSerializedModuleLoader() =
    default;

void SerializedModuleLoaderBase::collectVisibleTopLevelModuleNamesImpl(
    SmallVectorImpl<Identifier> &names, StringRef extension) const {
  llvm::SmallString<16> moduleSuffix;
  moduleSuffix += '.';
  moduleSuffix += file_types::getExtension(file_types::TY_SwiftModuleFile);

  llvm::SmallString<16> suffix;
  suffix += '.';
  suffix += extension;

  SmallVector<SmallString<64>, 2> targetFiles;
  forEachTargetModuleBasename(Ctx, [&](StringRef targetName) {
    targetFiles.emplace_back(targetName);
    targetFiles.back() += suffix;
  });

  auto &fs = *Ctx.SourceMgr.getFileSystem();

  // Apply \p body for each directory entry in \p dirPath.
  auto forEachDirectoryEntryPath =
      [&](StringRef dirPath, llvm::function_ref<void(StringRef)> body) {
        std::error_code errorCode;
        llvm::vfs::directory_iterator DI = fs.dir_begin(dirPath, errorCode);
        llvm::vfs::directory_iterator End;
        for (; !errorCode && DI != End; DI.increment(errorCode))
          body(DI->path());
      };

  // Check whether target specific module file exists or not in given directory.
  // $PATH/{arch}.{extension}
  auto checkTargetFiles = [&](StringRef path) -> bool {
    llvm::SmallString<256> scratch;
    for (auto targetFile : targetFiles) {
      scratch.clear();
      llvm::sys::path::append(scratch, path, targetFile);
      // If {arch}.{extension} exists, consider it's visible. Technically, we
      // should check the file type, permission, format, etc., but it's too
      // heavy to do that for each files.
      if (fs.exists(scratch))
        return true;
    }
    return false;
  };

  forEachModuleSearchPath(Ctx, [&](StringRef searchPath, SearchPathKind Kind,
                                   bool isSystem) {
    switch (Kind) {
    case SearchPathKind::Import: {
      // Look for:
      // $PATH/{name}.swiftmodule/{arch}.{extension} or
      // $PATH/{name}.{extension}
      forEachDirectoryEntryPath(searchPath, [&](StringRef path) {
        auto pathExt = llvm::sys::path::extension(path);
        if (pathExt != moduleSuffix && pathExt != suffix)
          return;

        auto stat = fs.status(path);
        if (!stat)
          return;
        if (pathExt == moduleSuffix && stat->isDirectory()) {
          if (!checkTargetFiles(path))
            return;
        } else if (pathExt != suffix || stat->isDirectory()) {
          return;
        }
        // Extract module name.
        auto name = llvm::sys::path::filename(path).drop_back(pathExt.size());
        names.push_back(Ctx.getIdentifier(name));
      });
      return None;
    }
    case SearchPathKind::RuntimeLibrary: {
      // Look for:
      // (Darwin OS) $PATH/{name}.swiftmodule/{arch}.{extension}
      // (Other OS)  $PATH/{name}.{extension}
      bool requireTargetSpecificModule = Ctx.LangOpts.Target.isOSDarwin();
      forEachDirectoryEntryPath(searchPath, [&](StringRef path) {
        auto pathExt = llvm::sys::path::extension(path);

        if (pathExt != moduleSuffix)
          if (requireTargetSpecificModule || pathExt != suffix)
            return;

        if (!checkTargetFiles(path)) {
          if (requireTargetSpecificModule)
            return;

          auto stat = fs.status(path);
          if (!stat || stat->isDirectory())
            return;
        }

        // Extract module name.
        auto name = llvm::sys::path::filename(path).drop_back(pathExt.size());
        names.push_back(Ctx.getIdentifier(name));
      });
      return None;
    }
    case SearchPathKind::Framework: {
      // Look for:
      // $PATH/{name}.framework/Modules/{name}.swiftmodule/{arch}.{extension}
      forEachDirectoryEntryPath(searchPath, [&](StringRef path) {
        if (llvm::sys::path::extension(path) != ".framework")
          return;

        // Extract Framework name.
        auto name = llvm::sys::path::filename(path).drop_back(
            StringLiteral(".framework").size());

        SmallString<256> moduleDir;
        llvm::sys::path::append(moduleDir, path, "Modules",
                                name + moduleSuffix);
        if (!checkTargetFiles(moduleDir))
          return;

        names.push_back(Ctx.getIdentifier(name));
      });
      return None;
    }
    }
    llvm_unreachable("covered switch");
  });
}

void ImplicitSerializedModuleLoader::collectVisibleTopLevelModuleNames(
    SmallVectorImpl<Identifier> &names) const {
  collectVisibleTopLevelModuleNamesImpl(
      names, file_types::getExtension(file_types::TY_SwiftModuleFile));
}

std::error_code SerializedModuleLoaderBase::openModuleDocFileIfPresent(
  ImportPath::Element ModuleID,
  const SerializedModuleBaseName &BaseName,
  std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer) {

  if (!ModuleDocBuffer)
    return std::error_code();

  llvm::vfs::FileSystem &FS = *Ctx.SourceMgr.getFileSystem();

  // Try to open the module documentation file.  If it does not exist, ignore
  // the error.  However, pass though all other errors.
  SmallString<256>
  ModuleDocPath{BaseName.getName(file_types::TY_SwiftModuleDocFile)};

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> ModuleDocOrErr =
    FS.getBufferForFile(ModuleDocPath);
  if (ModuleDocOrErr) {
    *ModuleDocBuffer = std::move(*ModuleDocOrErr);
  } else if (ModuleDocOrErr.getError() !=
               std::errc::no_such_file_or_directory) {
    return ModuleDocOrErr.getError();
  }

  return std::error_code();
}

std::unique_ptr<llvm::MemoryBuffer>
SerializedModuleLoaderBase::getModuleName(ASTContext &Ctx, StringRef modulePath,
                                          std::string &Name) {
  return ModuleFile::getModuleName(Ctx, modulePath, Name);
}

std::error_code
SerializedModuleLoaderBase::openModuleSourceInfoFileIfPresent(
    ImportPath::Element ModuleID,
    const SerializedModuleBaseName &BaseName,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer) {
  if (IgnoreSwiftSourceInfoFile || !ModuleSourceInfoBuffer)
    return std::error_code();

  llvm::vfs::FileSystem &FS = *Ctx.SourceMgr.getFileSystem();

  llvm::SmallString<128>
  PathWithoutProjectDir{BaseName.getName(file_types::TY_SwiftSourceInfoFile)};

  llvm::SmallString<128> PathWithProjectDir = PathWithoutProjectDir;

  // Insert "Project" before the filename in PathWithProjectDir.
  StringRef FileName = llvm::sys::path::filename(PathWithoutProjectDir);
  llvm::sys::path::remove_filename(PathWithProjectDir);
  llvm::sys::path::append(PathWithProjectDir, "Project");
  llvm::sys::path::append(PathWithProjectDir, FileName);

  // Try to open the module source info file from the "Project" directory.
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
  ModuleSourceInfoOrErr = FS.getBufferForFile(PathWithProjectDir);

  // If it does not exist, try to open the module source info file adjacent to
  // the .swiftmodule file.
  if (ModuleSourceInfoOrErr.getError() == std::errc::no_such_file_or_directory)
    ModuleSourceInfoOrErr = FS.getBufferForFile(PathWithoutProjectDir);

  // If we ended up with a different file system error, return it.
  if (ModuleSourceInfoOrErr)
    *ModuleSourceInfoBuffer = std::move(*ModuleSourceInfoOrErr);
  else if (ModuleSourceInfoOrErr.getError() !=
              std::errc::no_such_file_or_directory)
    return ModuleSourceInfoOrErr.getError();

  return std::error_code();
}

std::error_code SerializedModuleLoaderBase::openModuleFile(
    ImportPath::Element ModuleID, const SerializedModuleBaseName &BaseName,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer) {
  llvm::vfs::FileSystem &FS = *Ctx.SourceMgr.getFileSystem();

  // Try to open the module file first.  If we fail, don't even look for the
  // module documentation file.
  SmallString<256> ModulePath{BaseName.getName(file_types::TY_SwiftModuleFile)};

  // If there's no buffer to load into, simply check for the existence of
  // the module file.
  if (!ModuleBuffer) {
    llvm::ErrorOr<llvm::vfs::Status> statResult = FS.status(ModulePath);
    if (!statResult)
      return statResult.getError();

    if (!statResult->exists())
      return std::make_error_code(std::errc::no_such_file_or_directory);

    // FIXME: llvm::vfs::FileSystem doesn't give us information on whether or
    // not we can /read/ the file without actually trying to do so.
    return std::error_code();
  }

  // Actually load the file and error out if necessary.
  //
  // Use the default arguments except for IsVolatile that is set by the
  // frontend option -enable-volatile-modules. If set, we avoid the use of
  // mmap to workaround issues on NFS when the swiftmodule file loaded changes
  // on disk while it's in use.
  //
  // In practice, a swiftmodule file can chane when a client uses a
  // swiftmodule file from a framework while the framework is recompiled and
  // installed over existing files. Or when many processes rebuild the same
  // module interface.
  //
  // We have seen these scenarios leading to deserialization errors that on
  // the surface look like memory corruption.
  //
  // rdar://63755989
  bool enableVolatileModules = Ctx.LangOpts.EnableVolatileModules;
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> ModuleOrErr =
      FS.getBufferForFile(ModulePath,
                          /*FileSize=*/-1,
                          /*RequiresNullTerminator=*/true,
                          /*IsVolatile=*/enableVolatileModules);
  if (!ModuleOrErr)
    return ModuleOrErr.getError();

  *ModuleBuffer = std::move(ModuleOrErr.get());
  return std::error_code();
}

llvm::ErrorOr<ModuleDependencies> SerializedModuleLoaderBase::scanModuleFile(
    Twine modulePath) {
  // Open the module file
  auto &fs = *Ctx.SourceMgr.getFileSystem();
  auto moduleBuf = fs.getBufferForFile(modulePath);
  if (!moduleBuf)
    return moduleBuf.getError();

  // Load the module file without validation.
  std::shared_ptr<const ModuleFileSharedCore> loadedModuleFile;
  bool isFramework = false;
  serialization::ValidationInfo loadInfo =
      ModuleFileSharedCore::load(modulePath.str(),
                       std::move(moduleBuf.get()),
                       nullptr,
                       nullptr,
                       isFramework, loadedModuleFile);

  const std::string moduleDocPath;
  const std::string sourceInfoPath;
  // Map the set of dependencies over to the "module dependencies".
  auto dependencies = ModuleDependencies::forSwiftBinaryModule(modulePath.str(),
                                                               moduleDocPath,
                                                               sourceInfoPath,
                                                               isFramework);
  llvm::StringSet<> addedModuleNames;
  for (const auto &dependency : loadedModuleFile->getDependencies()) {
    // FIXME: Record header dependency?
    if (dependency.isHeader())
      continue;

    // Find the top-level module name.
    auto modulePathStr = dependency.getPrettyPrintedPath();
    StringRef moduleName = modulePathStr;
    auto dotPos = moduleName.find('.');
    if (dotPos != std::string::npos)
      moduleName = moduleName.slice(0, dotPos);

    dependencies.addModuleDependency(moduleName, &addedModuleNames);
  }

  return std::move(dependencies);
}

std::error_code ImplicitSerializedModuleLoader::findModuleFilesInDirectory(
    ImportPath::Element ModuleID,
    const SerializedModuleBaseName &BaseName,
    SmallVectorImpl<char> *ModuleInterfacePath,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
    bool IsFramework) {
  assert(((ModuleBuffer && ModuleDocBuffer) ||
          (!ModuleBuffer && !ModuleDocBuffer)) &&
         "Module and Module Doc buffer must both be initialized or NULL");

  if (LoadMode == ModuleLoadingMode::OnlyInterface)
    return std::make_error_code(std::errc::not_supported);

  auto ModuleErr = openModuleFile(ModuleID, BaseName, ModuleBuffer);
  if (ModuleErr)
    return ModuleErr;

  // If there are no buffers to load into, all we care about is whether the
  // module file existed.
  if (ModuleBuffer || ModuleDocBuffer || ModuleSourceInfoBuffer) {
    auto ModuleSourceInfoError = openModuleSourceInfoFileIfPresent(
        ModuleID, BaseName, ModuleSourceInfoBuffer
    );
    if (ModuleSourceInfoError)
      return ModuleSourceInfoError;

    auto ModuleDocErr = openModuleDocFileIfPresent(
        ModuleID, BaseName, ModuleDocBuffer
    );
    if (ModuleDocErr)
      return ModuleDocErr;
  }

  return std::error_code();
}

bool ImplicitSerializedModuleLoader::maybeDiagnoseTargetMismatch(
    SourceLoc sourceLocation, StringRef moduleName,
    const SerializedModuleBaseName &absoluteBaseName) {
  llvm::vfs::FileSystem &fs = *Ctx.SourceMgr.getFileSystem();

  // Get the last component of the base name, which is the target-specific one.
  auto target = llvm::sys::path::filename(absoluteBaseName.baseName);

  // Strip off the last component to get the .swiftmodule folder.
  auto dir = absoluteBaseName.baseName;
  llvm::sys::path::remove_filename(dir);

  std::error_code errorCode;
  std::string foundArchs;
  for (llvm::vfs::directory_iterator directoryIterator =
           fs.dir_begin(dir, errorCode), endIterator;
       directoryIterator != endIterator;
       directoryIterator.increment(errorCode)) {
    if (errorCode)
      return false;
    StringRef filePath = directoryIterator->path();
    StringRef extension = llvm::sys::path::extension(filePath);
    if (file_types::lookupTypeForExtension(extension) ==
          file_types::TY_SwiftModuleFile) {
      if (!foundArchs.empty())
        foundArchs += ", ";
      foundArchs += llvm::sys::path::stem(filePath).str();
    }
  }

  if (foundArchs.empty()) {
    // Maybe this swiftmodule directory only contains swiftinterfaces, or
    // maybe something else is going on. Regardless, we shouldn't emit a
    // possibly incorrect diagnostic.
    return false;
  }

  Ctx.Diags.diagnose(sourceLocation, diag::sema_no_import_target, moduleName,
                     target, foundArchs, dir);
  return true;
}

SerializedModuleBaseName::SerializedModuleBaseName(
    StringRef parentDir, const SerializedModuleBaseName &name)
    : baseName(parentDir) {
  llvm::sys::path::append(baseName, name.baseName);
}

std::string SerializedModuleBaseName::getName(file_types::ID fileTy) const {
  auto result = baseName;
  result += '.';
  result += file_types::getExtension(fileTy);

  return std::string(result.str());
}

bool
SerializedModuleLoaderBase::findModule(ImportPath::Element moduleID,
           SmallVectorImpl<char> *moduleInterfacePath,
           std::unique_ptr<llvm::MemoryBuffer> *moduleBuffer,
           std::unique_ptr<llvm::MemoryBuffer> *moduleDocBuffer,
           std::unique_ptr<llvm::MemoryBuffer> *moduleSourceInfoBuffer,
           bool &isFramework, bool &isSystemModule) {
  SmallString<32> moduleName(moduleID.Item.str());
  SerializedModuleBaseName genericBaseName(moduleName);

  auto genericModuleFileName =
      genericBaseName.getName(file_types::TY_SwiftModuleFile);

  SmallVector<SerializedModuleBaseName, 4> targetSpecificBaseNames;
  forEachTargetModuleBasename(Ctx, [&](StringRef targetName) {
    // Construct a base name like ModuleName.swiftmodule/arch-vendor-os
    SmallString<64> targetBaseName{genericModuleFileName};
    llvm::sys::path::append(targetBaseName, targetName);

    targetSpecificBaseNames.emplace_back(targetBaseName.str());
  });

  auto &fs = *Ctx.SourceMgr.getFileSystem();

  llvm::SmallString<256> currPath;

  /// Returns true if a target-specific module file was found, false if an error
  /// was diagnosed, or None if neither one happened and the search should
  /// continue.
  auto findTargetSpecificModuleFiles = [&](bool IsFramework) -> Optional<bool> {
    Optional<SerializedModuleBaseName> firstAbsoluteBaseName;

    for (const auto &targetSpecificBaseName : targetSpecificBaseNames) {
      SerializedModuleBaseName
      absoluteBaseName{currPath, targetSpecificBaseName};

      if (!firstAbsoluteBaseName.hasValue())
        firstAbsoluteBaseName.emplace(absoluteBaseName);

      auto result = findModuleFilesInDirectory(moduleID,
                        absoluteBaseName,
                        moduleInterfacePath,
                        moduleBuffer, moduleDocBuffer,
                        moduleSourceInfoBuffer,
                        IsFramework);
      if (!result) {
        return true;
      } else if (result == std::errc::not_supported) {
        return false;
      } else if (result != std::errc::no_such_file_or_directory) {
        return None;
      }
    }

    // We can only get here if all targetFileNamePairs failed with
    // 'std::errc::no_such_file_or_directory'.
    if (firstAbsoluteBaseName
        && maybeDiagnoseTargetMismatch(moduleID.Loc, moduleName,
                                       *firstAbsoluteBaseName)) {
      return false;
    } else {
      return None;
    }
  };

  auto result = forEachModuleSearchPath(
      Ctx,
      [&](StringRef path, SearchPathKind Kind,
          bool isSystem) -> Optional<bool> {
        currPath = path;
        isSystemModule = isSystem;

        switch (Kind) {
        case SearchPathKind::Import:
        case SearchPathKind::RuntimeLibrary: {
          isFramework = false;

          // On Apple platforms, we can assume that the runtime libraries use
          // target-specifi module files wihtin a `.swiftmodule` directory.
          // This was not always true on non-Apple platforms, and in order to
          // ease the transition, check both layouts.
          bool checkTargetSpecificModule = true;
          if (Kind != SearchPathKind::RuntimeLibrary ||
              !Ctx.LangOpts.Target.isOSDarwin()) {
            auto modulePath = currPath;
            llvm::sys::path::append(modulePath, genericModuleFileName);
            llvm::ErrorOr<llvm::vfs::Status> statResult = fs.status(modulePath);

            // Even if stat fails, we can't just return the error; the path
            // we're looking for might not be "Foo.swiftmodule".
            checkTargetSpecificModule = statResult && statResult->isDirectory();
          }

          if (checkTargetSpecificModule)
            // A .swiftmodule directory contains architecture-specific files.
            return findTargetSpecificModuleFiles(isFramework);

          SerializedModuleBaseName absoluteBaseName{currPath, genericBaseName};

          auto result = findModuleFilesInDirectory(
              moduleID, absoluteBaseName, moduleInterfacePath,
              moduleBuffer, moduleDocBuffer, moduleSourceInfoBuffer, isFramework);
          if (!result)
            return true;
          else if (result == std::errc::not_supported)
            return false;
          else
            return None;
        }
        case SearchPathKind::Framework: {
          isFramework = true;
          llvm::sys::path::append(currPath, moduleName + ".framework");

          // Check if the framework directory exists.
          if (!fs.exists(currPath))
            return None;

          // Frameworks always use architecture-specific files within a
          // .swiftmodule directory.
          llvm::sys::path::append(currPath, "Modules");
          return findTargetSpecificModuleFiles(isFramework);
        }
        }
        llvm_unreachable("covered switch");
      });
  return result.getValueOr(false);
}

static std::pair<StringRef, clang::VersionTuple>
getOSAndVersionForDiagnostics(const llvm::Triple &triple) {
  StringRef osName;
  unsigned major, minor, micro;
  if (triple.isMacOSX()) {
    // macOS triples represent their versions differently, so we have to use the
    // special accessor.
    triple.getMacOSXVersion(major, minor, micro);
    osName = swift::prettyPlatformString(PlatformKind::macOS);
  } else {
    triple.getOSVersion(major, minor, micro);
    if (triple.isWatchOS()) {
      osName = swift::prettyPlatformString(PlatformKind::watchOS);
    } else if (triple.isTvOS()) {
      assert(triple.isiOS() &&
             "LLVM treats tvOS as a kind of iOS, so tvOS is checked first");
      osName = swift::prettyPlatformString(PlatformKind::tvOS);
    } else if (triple.isiOS()) {
      osName = swift::prettyPlatformString(PlatformKind::iOS);
    } else {
      assert(!triple.isOSDarwin() && "unknown Apple OS");
      // Fallback to the LLVM triple name. This isn't great (it won't be
      // capitalized or anything), but it's better than nothing.
      osName = triple.getOSName();
    }
  }

  assert(!osName.empty());
  clang::VersionTuple version;
  if (micro != 0)
    version = clang::VersionTuple(major, minor, micro);
  else
    version = clang::VersionTuple(major, minor);
  return {osName, version};
}

LoadedFile *SerializedModuleLoaderBase::loadAST(
    ModuleDecl &M, Optional<SourceLoc> diagLoc,
    StringRef moduleInterfacePath,
    std::unique_ptr<llvm::MemoryBuffer> moduleInputBuffer,
    std::unique_ptr<llvm::MemoryBuffer> moduleDocInputBuffer,
    std::unique_ptr<llvm::MemoryBuffer> moduleSourceInfoInputBuffer,
    bool isFramework) {
  assert(moduleInputBuffer);

  // The buffers are moved into the shared core, so grab their IDs now in case
  // they're needed for diagnostics later.
  StringRef moduleBufferID = moduleInputBuffer->getBufferIdentifier();
  StringRef moduleDocBufferID;
  if (moduleDocInputBuffer)
    moduleDocBufferID = moduleDocInputBuffer->getBufferIdentifier();
  StringRef moduleSourceInfoID;
  if (moduleSourceInfoInputBuffer)
    moduleSourceInfoID = moduleSourceInfoInputBuffer->getBufferIdentifier();

  if (moduleInputBuffer->getBufferSize() % 4 != 0) {
    if (diagLoc)
      Ctx.Diags.diagnose(*diagLoc, diag::serialization_malformed_module,
                         moduleBufferID);
    return nullptr;
  }

  std::unique_ptr<ModuleFile> loadedModuleFile;
  std::shared_ptr<const ModuleFileSharedCore> loadedModuleFileCore;
  serialization::ValidationInfo loadInfo =
      ModuleFileSharedCore::load(moduleInterfacePath,
                       std::move(moduleInputBuffer),
                       std::move(moduleDocInputBuffer),
                       std::move(moduleSourceInfoInputBuffer),
                       isFramework, loadedModuleFileCore);
  SerializedASTFile *fileUnit = nullptr;

  if (loadInfo.status == serialization::Status::Valid) {
    loadedModuleFile =
        std::make_unique<ModuleFile>(std::move(loadedModuleFileCore));
    M.setResilienceStrategy(loadedModuleFile->getResilienceStrategy());

    // We've loaded the file. Now try to bring it into the AST.
    fileUnit = new (Ctx) SerializedASTFile(M, *loadedModuleFile);
    M.setStaticLibrary(loadedModuleFile->isStaticLibrary());
    if (loadedModuleFile->isTestable())
      M.setTestingEnabled();
    if (loadedModuleFile->arePrivateImportsEnabled())
      M.setPrivateImportsEnabled();
    if (loadedModuleFile->isImplicitDynamicEnabled())
      M.setImplicitDynamicEnabled();
    if (loadedModuleFile->hasIncrementalInfo())
      M.setHasIncrementalInfo();
    if (!loadedModuleFile->getModuleABIName().empty())
      M.setABIName(Ctx.getIdentifier(loadedModuleFile->getModuleABIName()));
    M.setUserModuleVersion(loadedModuleFile->getUserModuleVersion());
    auto diagLocOrInvalid = diagLoc.getValueOr(SourceLoc());
    loadInfo.status = loadedModuleFile->associateWithFileContext(
        fileUnit, diagLocOrInvalid, Ctx.LangOpts.AllowModuleWithCompilerErrors);

    // FIXME: This seems wrong. Overlay for system Clang module doesn't
    // necessarily mean it's "system" module. User can make their own overlay
    // in non-system directory.
    // Remove this block after we fix the test suite.
    if (auto shadowed = loadedModuleFile->getUnderlyingModule())
      if (shadowed->isSystemModule())
        M.setIsSystemModule(true);

    if (loadInfo.status == serialization::Status::Valid ||
        (Ctx.LangOpts.AllowModuleWithCompilerErrors &&
         (loadInfo.status == serialization::Status::TargetTooNew ||
          loadInfo.status == serialization::Status::TargetIncompatible))) {
      if (loadedModuleFile->hasSourceInfoFile() &&
          !loadedModuleFile->hasSourceInfo())
        Ctx.Diags.diagnose(diagLocOrInvalid,
                           diag::serialization_malformed_sourceinfo,
                           moduleSourceInfoID);

      Ctx.bumpGeneration();
      LoadedModuleFiles.emplace_back(std::move(loadedModuleFile),
                                     Ctx.getCurrentGeneration());
      findOverlayFiles(diagLoc.getValueOr(SourceLoc()), &M, fileUnit);
    } else {
      fileUnit = nullptr;
    }
  }

  if (loadInfo.status != serialization::Status::Valid) {
    if (diagLoc)
      serialization::diagnoseSerializedASTLoadFailure(
          Ctx, *diagLoc, loadInfo, moduleBufferID, moduleDocBufferID,
          loadedModuleFile.get(), M.getName());

    // Even though the module failed to load, it's possible its contents
    // include a source buffer that need to survive because it's already been
    // used for diagnostics.
    // Note this is only necessary in case a bridging header failed to load
    // during the `associateWithFileContext()` call.
    if (loadedModuleFile &&
        loadedModuleFile->mayHaveDiagnosticsPointingAtBuffer())
      OrphanedModuleFiles.push_back(std::move(loadedModuleFile));
  }

  return fileUnit;
}

void swift::serialization::diagnoseSerializedASTLoadFailure(
    ASTContext &Ctx, SourceLoc diagLoc,
    const serialization::ValidationInfo &loadInfo,
    StringRef moduleBufferID, StringRef moduleDocBufferID,
    ModuleFile *loadedModuleFile, Identifier ModuleName) {
  auto diagnoseDifferentLanguageVersion = [&](StringRef shortVersion) -> bool {
    if (shortVersion.empty())
      return false;

    SmallString<32> versionBuf;
    llvm::raw_svector_ostream versionString(versionBuf);
    versionString << Version::getCurrentLanguageVersion();
    if (versionString.str() == shortVersion)
      return false;

    Ctx.Diags.diagnose(
        diagLoc, diag::serialization_module_language_version_mismatch,
        loadInfo.shortVersion, versionString.str(), moduleBufferID);
    return true;
  };

  switch (loadInfo.status) {
  case serialization::Status::Valid:
    llvm_unreachable("At this point we know loading has failed");

  case serialization::Status::FormatTooNew:
    if (diagnoseDifferentLanguageVersion(loadInfo.shortVersion))
      break;
    Ctx.Diags.diagnose(diagLoc, diag::serialization_module_too_new,
                       moduleBufferID);
    break;
  case serialization::Status::FormatTooOld:
    if (diagnoseDifferentLanguageVersion(loadInfo.shortVersion))
      break;
    Ctx.Diags.diagnose(diagLoc, diag::serialization_module_too_old, ModuleName,
                       moduleBufferID);
    break;
  case serialization::Status::Malformed:
    Ctx.Diags.diagnose(diagLoc, diag::serialization_malformed_module,
                       moduleBufferID);
    break;

  case serialization::Status::MalformedDocumentation:
    assert(!moduleDocBufferID.empty());
    Ctx.Diags.diagnose(diagLoc, diag::serialization_malformed_module,
                       moduleDocBufferID);
    break;

  case serialization::Status::MissingDependency: {
    // Figure out /which/ dependencies are missing.
    // FIXME: Dependencies should be de-duplicated at serialization time,
    // not now.
    llvm::StringSet<> duplicates;
    llvm::SmallVector<ModuleFile::Dependency, 4> missing;
    std::copy_if(
        loadedModuleFile->getDependencies().begin(),
        loadedModuleFile->getDependencies().end(), std::back_inserter(missing),
        [&duplicates, &Ctx](const ModuleFile::Dependency &dependency) -> bool {
          if (dependency.isLoaded() || dependency.isHeader() ||
              (dependency.isImplementationOnly() &&
               Ctx.LangOpts.DebuggerSupport)) {
            return false;
          }
          return duplicates.insert(dependency.Core.RawPath).second;
        });

    // FIXME: only show module part of RawAccessPath
    assert(!missing.empty() && "unknown missing dependency?");
    if (missing.size() == 1) {
      Ctx.Diags.diagnose(diagLoc, diag::serialization_missing_single_dependency,
                         missing.front().Core.getPrettyPrintedPath());
    } else {
      llvm::SmallString<64> missingNames;
      missingNames += '\'';
      interleave(missing,
                 [&](const ModuleFile::Dependency &next) {
                   missingNames += next.Core.getPrettyPrintedPath();
                 },
                 [&] { missingNames += "', '"; });
      missingNames += '\'';

      Ctx.Diags.diagnose(diagLoc, diag::serialization_missing_dependencies,
                         missingNames);
    }

    if (Ctx.SearchPathOpts.SDKPath.empty() &&
        llvm::Triple(llvm::sys::getProcessTriple()).isMacOSX()) {
      Ctx.Diags.diagnose(SourceLoc(), diag::sema_no_import_no_sdk);
      Ctx.Diags.diagnose(SourceLoc(), diag::sema_no_import_no_sdk_xcrun);
    }
    break;
  }

  case serialization::Status::CircularDependency: {
    auto circularDependencyIter = llvm::find_if(
        loadedModuleFile->getDependencies(),
        [](const ModuleFile::Dependency &next) {
          return next.isLoaded() &&
                 !(next.Import.hasValue() &&
                   next.Import->importedModule->hasResolvedImports());
        });
    assert(circularDependencyIter !=
               loadedModuleFile->getDependencies().end() &&
           "circular dependency reported, but no module with unresolved "
           "imports found");

    // FIXME: We should include the path of the circularity as well, but that's
    // hard because we're discovering this /while/ resolving imports, which
    // means the problematic modules haven't been recorded yet.
    Ctx.Diags.diagnose(diagLoc, diag::serialization_circular_dependency,
                       circularDependencyIter->Core.getPrettyPrintedPath(),
                       ModuleName);
    break;
  }

  case serialization::Status::MissingUnderlyingModule: {
    Ctx.Diags.diagnose(diagLoc, diag::serialization_missing_underlying_module,
                       ModuleName);
    if (Ctx.SearchPathOpts.SDKPath.empty() &&
        llvm::Triple(llvm::sys::getProcessTriple()).isMacOSX()) {
      Ctx.Diags.diagnose(SourceLoc(), diag::sema_no_import_no_sdk);
      Ctx.Diags.diagnose(SourceLoc(), diag::sema_no_import_no_sdk_xcrun);
    }
    break;
  }

  case serialization::Status::FailedToLoadBridgingHeader:
    // We already emitted a diagnostic about the bridging header. Just emit
    // a generic message here.
    Ctx.Diags.diagnose(diagLoc, diag::serialization_load_failed,
                       ModuleName.str());
    break;

  case serialization::Status::NameMismatch: {
    // FIXME: This doesn't handle a non-debugger REPL, which should also treat
    // this as a non-fatal error.
    auto diagKind = diag::serialization_name_mismatch;
    if (Ctx.LangOpts.DebuggerSupport)
      diagKind = diag::serialization_name_mismatch_repl;
    Ctx.Diags.diagnose(diagLoc, diagKind, loadInfo.name, ModuleName.str());
    break;
  }

  case serialization::Status::TargetIncompatible: {
    // FIXME: This doesn't handle a non-debugger REPL, which should also treat
    // this as a non-fatal error.
    auto diagKind = diag::serialization_target_incompatible;
    if (Ctx.LangOpts.DebuggerSupport ||
        Ctx.LangOpts.AllowModuleWithCompilerErrors)
      diagKind = diag::serialization_target_incompatible_repl;
    Ctx.Diags.diagnose(diagLoc, diagKind, ModuleName, loadInfo.targetTriple,
                       moduleBufferID);
    break;
  }

  case serialization::Status::TargetTooNew: {
    llvm::Triple moduleTarget(llvm::Triple::normalize(loadInfo.targetTriple));

    std::pair<StringRef, clang::VersionTuple> moduleOSInfo =
        getOSAndVersionForDiagnostics(moduleTarget);
    std::pair<StringRef, clang::VersionTuple> compilationOSInfo =
        getOSAndVersionForDiagnostics(Ctx.LangOpts.Target);

    // FIXME: This doesn't handle a non-debugger REPL, which should also treat
    // this as a non-fatal error.
    auto diagKind = diag::serialization_target_too_new;
    if (Ctx.LangOpts.DebuggerSupport ||
        Ctx.LangOpts.AllowModuleWithCompilerErrors)
      diagKind = diag::serialization_target_too_new_repl;
    Ctx.Diags.diagnose(diagLoc, diagKind, compilationOSInfo.first,
                       compilationOSInfo.second, ModuleName,
                       moduleOSInfo.second, moduleBufferID);
    break;
  }

  case serialization::Status::SDKMismatch:
    auto currentSDK = swift::getSDKName(Ctx.SearchPathOpts.SDKPath);
    auto moduleSDK = loadInfo.builderSDK;
    Ctx.Diags.diagnose(diagLoc, diag::serialization_sdk_mismatch,
                       ModuleName, moduleSDK, currentSDK, moduleBufferID);
    break;
  }
}

bool swift::extractCompilerFlagsFromInterface(StringRef buffer,
                                              llvm::StringSaver &ArgSaver,
                                              SmallVectorImpl<const char *> &SubArgs) {
  SmallVector<StringRef, 1> FlagMatches;
  auto FlagRe = llvm::Regex("^// swift-module-flags:(.*)$", llvm::Regex::Newline);
  if (!FlagRe.match(buffer, &FlagMatches))
    return true;
  assert(FlagMatches.size() == 2);
  llvm::cl::TokenizeGNUCommandLine(FlagMatches[1], ArgSaver, SubArgs);
  SmallVector<StringRef, 1> IgnFlagMatches;
  // Cherry-pick supported options from the ignorable list.
  auto IgnFlagRe = llvm::Regex("^// swift-module-flags-ignorable:(.*)$",
                               llvm::Regex::Newline);
  // It's OK the interface doesn't have the ignorable list, we just ignore them
  // all.
  if (!IgnFlagRe.match(buffer, &IgnFlagMatches))
    return false;
  SmallVector<const char *, 8> IgnSubArgs;
  llvm::cl::TokenizeGNUCommandLine(IgnFlagMatches[1], ArgSaver, IgnSubArgs);
  std::unique_ptr<llvm::opt::OptTable> table = swift::createSwiftOptTable();
  unsigned missingArgIdx = 0;
  unsigned missingArgCount = 0;
  auto parsedIgns = table->ParseArgs(IgnSubArgs, missingArgIdx, missingArgCount);
  for (auto parse: parsedIgns) {
    // Check if the option is a frontend option. This will filter out unknown
    // options and input-like options.
    if (!parse->getOption().hasFlag(options::FrontendOption))
      continue;
    // Push the supported option and its value to the list.
    // We shouldn't need to worry about cases like -tbd-install_name=Foo because
    // the parsing function should have droped alias options already.
    SubArgs.push_back(ArgSaver.save(parse->getSpelling()).data());
    for (auto value: parse->getValues())
      SubArgs.push_back(value);
  }

  return false;
}

llvm::VersionTuple
swift::extractUserModuleVersionFromInterface(StringRef moduleInterfacePath) {
  llvm::VersionTuple result;
  // Read the inteface file and extract its compiler arguments line
  if (auto file = llvm::MemoryBuffer::getFile(moduleInterfacePath)) {
    llvm::BumpPtrAllocator alloc;
    llvm::StringSaver argSaver(alloc);
    SmallVector<const char*, 8> args;
    (void)extractCompilerFlagsFromInterface((*file)->getBuffer(), argSaver, args);
    for (unsigned I = 0, N = args.size(); I + 1 < N; I++) {
      // Check the version number specified via -user-module-version.
      StringRef current(args[I]), next(args[I + 1]);
      if (current == "-user-module-version") {
        result.tryParse(next);
        break;
      }
    }
  }
  return result;
}

bool SerializedModuleLoaderBase::canImportModule(
    ImportPath::Element mID, llvm::VersionTuple version, bool underlyingVersion) {
  // If underlying version is specified, this should be handled by Clang importer.
  if (!version.empty() && underlyingVersion)
    return false;
  // Look on disk.
  SmallVectorImpl<char> *unusedModuleInterfacePath = nullptr;
  std::unique_ptr<llvm::MemoryBuffer> *unusedModuleBuffer = nullptr;
  std::unique_ptr<llvm::MemoryBuffer> *unusedModuleDocBuffer = nullptr;
  std::unique_ptr<llvm::MemoryBuffer> *unusedModuleSourceInfoBuffer = nullptr;
  bool isFramework = false;
  bool isSystemModule = false;

  llvm::SmallString<256> moduleInterfacePath;
  std::unique_ptr<llvm::MemoryBuffer> moduleInputBuffer;
  std::unique_ptr<llvm::MemoryBuffer> moduleDocBuffer;
  if (!version.empty()) {
    unusedModuleInterfacePath = &moduleInterfacePath;
    unusedModuleBuffer = &moduleInputBuffer;
    unusedModuleDocBuffer = &moduleDocBuffer;
  }

  auto found = findModule(mID, unusedModuleInterfacePath, unusedModuleBuffer,
                          unusedModuleDocBuffer, unusedModuleSourceInfoBuffer,
                          isFramework, isSystemModule);
  // If we cannot find the module, don't continue.
  if (!found)
    return false;
  // If no version number is specified, don't continue.
  if (version.empty())
    return true;
  assert(found);
  assert(!version.empty());
  assert(!underlyingVersion);
  llvm::VersionTuple currentVersion;
  if (!moduleInterfacePath.empty()) {
    currentVersion = extractUserModuleVersionFromInterface(moduleInterfacePath);
  }
  // If failing to extract the user version from the interface file, try the binary
  // format, if present.
  if (currentVersion.empty() && unusedModuleBuffer) {
    auto metaData =
      serialization::validateSerializedAST((*unusedModuleBuffer)->getBuffer());
    currentVersion = metaData.userModuleVersion;
  }

  if (currentVersion.empty()) {
    Ctx.Diags.diagnose(mID.Loc, diag::cannot_find_project_version, "Swift",
                       mID.Item.str());
    return true;
  }

  return currentVersion >= version;
}

bool MemoryBufferSerializedModuleLoader::canImportModule(
    ImportPath::Element mID, llvm::VersionTuple version, bool underlyingVersion) {
  // If underlying version is specified, this should be handled by Clang importer.
  if (!version.empty() && underlyingVersion)
    return false;
  auto mIt = MemoryBuffers.find(mID.Item.str());
  if (mIt == MemoryBuffers.end())
    return false;
  if (version.empty())
    return true;
  if (mIt->second.userVersion.empty()) {
    Ctx.Diags.diagnose(mID.Loc, diag::cannot_find_project_version, "Swift",
                       mID.Item.str());
    return true;
  }
  assert(!version.empty());
  assert(!(mIt->second.userVersion.empty()));
  return mIt->second.userVersion >= version;
}

ModuleDecl *
SerializedModuleLoaderBase::loadModule(SourceLoc importLoc,
                                       ImportPath::Module path) {
  // FIXME: Swift submodules?
  if (path.size() > 1)
    return nullptr;

  auto moduleID = path[0];
  bool isFramework = false;
  bool isSystemModule = false;

  llvm::SmallString<256> moduleInterfacePath;
  std::unique_ptr<llvm::MemoryBuffer> moduleInputBuffer;
  std::unique_ptr<llvm::MemoryBuffer> moduleDocInputBuffer;
  std::unique_ptr<llvm::MemoryBuffer> moduleSourceInfoInputBuffer;

  // Look on disk.
  if (!findModule(moduleID, &moduleInterfacePath, &moduleInputBuffer,
                  &moduleDocInputBuffer, &moduleSourceInfoInputBuffer,
                  isFramework, isSystemModule)) {
    return nullptr;
  }

  assert(moduleInputBuffer);

  auto M = ModuleDecl::create(moduleID.Item, Ctx);
  M->setIsSystemModule(isSystemModule);
  Ctx.addLoadedModule(M);
  SWIFT_DEFER { M->setHasResolvedImports(); };

  auto *file =
      loadAST(*M, moduleID.Loc, moduleInterfacePath,
              std::move(moduleInputBuffer), std::move(moduleDocInputBuffer),
              std::move(moduleSourceInfoInputBuffer), isFramework);
  if (file) {
    M->addFile(*file);
  } else {
    M->setFailedToLoad();
  }

  if (dependencyTracker && file) {
    auto DepPath = file->getFilename();
    // Don't record cached artifacts as dependencies.
    if (!isCached(DepPath)) {
      if (M->hasIncrementalInfo()) {
        dependencyTracker->addIncrementalDependency(DepPath,
                                                    M->getFingerprint());
      } else {
        dependencyTracker->addDependency(DepPath, /*isSystem=*/false);
      }
    }
  }
  return M;
}

ModuleDecl *
MemoryBufferSerializedModuleLoader::loadModule(SourceLoc importLoc,
                                               ImportPath::Module path) {
  // FIXME: Swift submodules?
  if (path.size() > 1)
    return nullptr;

  auto moduleID = path[0];

  // See if we find it in the registered memory buffers.

  // FIXME: Right now this works only with access paths of length 1.
  // Once submodules are designed, this needs to support suffix
  // matching and a search path.
  auto bufIter = MemoryBuffers.find(moduleID.Item.str());
  if (bufIter == MemoryBuffers.end())
    return nullptr;

  bool isFramework = false;
  std::unique_ptr<llvm::MemoryBuffer> moduleInputBuffer;
  moduleInputBuffer = std::move(bufIter->second.buffer);
  MemoryBuffers.erase(bufIter);
  assert(moduleInputBuffer);

  auto *M = ModuleDecl::create(moduleID.Item, Ctx);
  SWIFT_DEFER { M->setHasResolvedImports(); };

  auto *file = loadAST(*M, moduleID.Loc, /*moduleInterfacePath*/ "",
                       std::move(moduleInputBuffer), {}, {}, isFramework);
  if (!file)
    return nullptr;

  M->addFile(*file);
  Ctx.addLoadedModule(M);
  return M;
}

void SerializedModuleLoaderBase::loadExtensions(NominalTypeDecl *nominal,
                                                unsigned previousGeneration) {
  for (auto &modulePair : LoadedModuleFiles) {
    if (modulePair.second <= previousGeneration)
      continue;
    modulePair.first->loadExtensions(nominal);
  }
}

void SerializedModuleLoaderBase::loadObjCMethods(
       ClassDecl *classDecl,
       ObjCSelector selector,
       bool isInstanceMethod,
       unsigned previousGeneration,
       llvm::TinyPtrVector<AbstractFunctionDecl *> &methods) {
  for (auto &modulePair : LoadedModuleFiles) {
    if (modulePair.second <= previousGeneration)
      continue;
    modulePair.first->loadObjCMethods(classDecl, selector, isInstanceMethod,
                                      methods);
  }
}

void SerializedModuleLoaderBase::loadDerivativeFunctionConfigurations(
    AbstractFunctionDecl *originalAFD, unsigned int previousGeneration,
    llvm::SetVector<AutoDiffConfig> &results) {
  for (auto &modulePair : LoadedModuleFiles) {
    if (modulePair.second <= previousGeneration)
      continue;
    modulePair.first->loadDerivativeFunctionConfigurations(originalAFD,
                                                           results);
  }
}

std::error_code MemoryBufferSerializedModuleLoader::findModuleFilesInDirectory(
    ImportPath::Element ModuleID,
    const SerializedModuleBaseName &BaseName,
    SmallVectorImpl<char> *ModuleInterfacePath,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
    bool IsFramework) {
  // This is a soft error instead of an llvm_unreachable because this API is
  // primarily used by LLDB which makes it more likely that unwitting changes to
  // the Swift compiler accidentally break the contract.
  assert(false && "not supported");
  return std::make_error_code(std::errc::not_supported);
}

bool MemoryBufferSerializedModuleLoader::maybeDiagnoseTargetMismatch(
    SourceLoc sourceLocation, StringRef moduleName,
    const SerializedModuleBaseName &absoluteBaseName) {
  return false;
}

void SerializedModuleLoaderBase::verifyAllModules() {
#ifndef NDEBUG
  for (const LoadedModulePair &loaded : LoadedModuleFiles)
    loaded.first->verify();
#endif
}

//-----------------------------------------------------------------------------
// SerializedASTFile implementation
//-----------------------------------------------------------------------------

void SerializedASTFile::getImportedModules(
    SmallVectorImpl<ImportedModule> &imports,
    ModuleDecl::ImportFilter filter) const {
  File.getImportedModules(imports, filter);
}

void SerializedASTFile::collectLinkLibrariesFromImports(
    ModuleDecl::LinkLibraryCallback callback) const {
  llvm::SmallVector<ImportedModule, 8> Imports;
  File.getImportedModules(Imports, {ModuleDecl::ImportFilterKind::Exported,
                                    ModuleDecl::ImportFilterKind::Default});

  for (auto Import : Imports)
    Import.importedModule->collectLinkLibraries(callback);
}

void SerializedASTFile::collectLinkLibraries(
    ModuleDecl::LinkLibraryCallback callback) const {
  if (isSIB()) {
    collectLinkLibrariesFromImports(callback);
  } else {
    File.collectLinkLibraries(callback);
  }
}

bool SerializedASTFile::isSIB() const {
  return File.isSIB();
}

bool SerializedASTFile::hadLoadError() const {
  return File.hasError();
}

bool SerializedASTFile::isSystemModule() const {
  if (auto Mod = File.getUnderlyingModule()) {
    return Mod->isSystemModule();
  }
  return false;
}

void SerializedASTFile::lookupValue(DeclName name, NLKind lookupKind,
                                    SmallVectorImpl<ValueDecl*> &results) const{
  File.lookupValue(name, results);
}

StringRef
SerializedASTFile::getFilenameForPrivateDecl(const ValueDecl *decl) const {
  return File.FilenamesForPrivateValues.lookup(decl);
}

TypeDecl *SerializedASTFile::lookupLocalType(llvm::StringRef MangledName) const{
  return File.lookupLocalType(MangledName);
}

OpaqueTypeDecl *
SerializedASTFile::lookupOpaqueResultType(StringRef MangledName) {
  return File.lookupOpaqueResultType(MangledName);
}

TypeDecl *
SerializedASTFile::lookupNestedType(Identifier name,
                                    const NominalTypeDecl *parent) const {
  return File.lookupNestedType(name, parent);
}

void SerializedASTFile::lookupOperatorDirect(
    Identifier name, OperatorFixity fixity,
    TinyPtrVector<OperatorDecl *> &results) const {
  if (auto *op = File.lookupOperator(name, fixity))
    results.push_back(op);
}

void SerializedASTFile::lookupPrecedenceGroupDirect(
    Identifier name, TinyPtrVector<PrecedenceGroupDecl *> &results) const {
  if (auto *group = File.lookupPrecedenceGroup(name))
    results.push_back(group);
}

void SerializedASTFile::lookupVisibleDecls(ImportPath::Access accessPath,
                                           VisibleDeclConsumer &consumer,
                                           NLKind lookupKind) const {
  File.lookupVisibleDecls(accessPath, consumer, lookupKind);
}

void SerializedASTFile::lookupClassMembers(ImportPath::Access accessPath,
                                           VisibleDeclConsumer &consumer) const{
  File.lookupClassMembers(accessPath, consumer);
}

void
SerializedASTFile::lookupClassMember(ImportPath::Access accessPath,
                                     DeclName name,
                                     SmallVectorImpl<ValueDecl*> &decls) const {
  File.lookupClassMember(accessPath, name, decls);
}

void SerializedASTFile::lookupObjCMethods(
       ObjCSelector selector,
       SmallVectorImpl<AbstractFunctionDecl *> &results) const {
  File.lookupObjCMethods(selector, results);
}

Optional<Fingerprint>
SerializedASTFile::loadFingerprint(const IterableDeclContext *IDC) const {
  return File.loadFingerprint(IDC);
}

void SerializedASTFile::lookupImportedSPIGroups(
                        const ModuleDecl *importedModule,
                        llvm::SmallSetVector<Identifier, 4> &spiGroups) const {
  File.lookupImportedSPIGroups(importedModule, spiGroups);
}

Optional<CommentInfo>
SerializedASTFile::getCommentForDecl(const Decl *D) const {
  return File.getCommentForDecl(D);
}

Optional<ExternalSourceLocs::RawLocs>
SerializedASTFile::getExternalRawLocsForDecl(const Decl *D) const {
  return File.getExternalRawLocsForDecl(D);
}

Optional<StringRef>
SerializedASTFile::getGroupNameForDecl(const Decl *D) const {
  return File.getGroupNameForDecl(D);
}


Optional<StringRef>
SerializedASTFile::getSourceFileNameForDecl(const Decl *D) const {
  return File.getSourceFileNameForDecl(D);
}

Optional<unsigned>
SerializedASTFile::getSourceOrderForDecl(const Decl *D) const {
  return File.getSourceOrderForDecl(D);
}

void SerializedASTFile::collectAllGroups(
    SmallVectorImpl<StringRef> &Names) const {
  File.collectAllGroups(Names);
};

Optional<StringRef>
SerializedASTFile::getGroupNameByUSR(StringRef USR) const {
  return File.getGroupNameByUSR(USR);
}

void
SerializedASTFile::getTopLevelDecls(SmallVectorImpl<Decl*> &results) const {
  File.getTopLevelDecls(results);
}

void SerializedASTFile::getExportedPrespecializations(
    SmallVectorImpl<Decl *> &results) const {
  File.getExportedPrespecializations(results);
}
void SerializedASTFile::getTopLevelDeclsWhereAttributesMatch(
              SmallVectorImpl<Decl*> &results,
              llvm::function_ref<bool(DeclAttributes)> matchAttributes) const {
  File.getTopLevelDecls(results, matchAttributes);
}

void SerializedASTFile::getOperatorDecls(
    SmallVectorImpl<OperatorDecl *> &results) const {
  File.getOperatorDecls(results);
}

void SerializedASTFile::getPrecedenceGroups(
       SmallVectorImpl<PrecedenceGroupDecl*> &results) const {
  File.getPrecedenceGroups(results);
}

void
SerializedASTFile::getLocalTypeDecls(SmallVectorImpl<TypeDecl*> &results) const{
  File.getLocalTypeDecls(results);
}

void
SerializedASTFile::getOpaqueReturnTypeDecls(
                              SmallVectorImpl<OpaqueTypeDecl*> &results) const {
  File.getOpaqueReturnTypeDecls(results);
}

void
SerializedASTFile::getDisplayDecls(SmallVectorImpl<Decl*> &results) const {
  File.getDisplayDecls(results);
}

StringRef SerializedASTFile::getFilename() const {
  return File.getModuleFilename();
}

StringRef SerializedASTFile::getTargetTriple() const {
  return File.getTargetTriple();
}

ModuleDecl *SerializedASTFile::getUnderlyingModuleIfOverlay() const {
  return File.getUnderlyingModule();
}

const clang::Module *SerializedASTFile::getUnderlyingClangModule() const {
  if (auto *UnderlyingModule = File.getUnderlyingModule())
    return UnderlyingModule->findUnderlyingClangModule();
  return nullptr;
}

Identifier
SerializedASTFile::getDiscriminatorForPrivateValue(const ValueDecl *D) const {
  Identifier discriminator = File.getDiscriminatorForPrivateValue(D);
  assert(!discriminator.empty() && "no discriminator found for value");
  return discriminator;
}

void SerializedASTFile::collectBasicSourceFileInfo(
    llvm::function_ref<void(const BasicSourceFileInfo &)> callback) const {
  File.collectBasicSourceFileInfo(callback);
}
