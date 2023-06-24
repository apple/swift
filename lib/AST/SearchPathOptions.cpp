//===----------------------- SearchPathOptions.cpp ------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/SearchPathOptions.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/Errc.h"

using namespace swift;

void ModuleSearchPathLookup::addFilesInPathToLookupTable(
    llvm::vfs::FileSystem *FS, StringRef SearchPath, ModuleSearchPathKind Kind,
    bool IsSystem, unsigned SearchPathIndex) {
  std::error_code Error;
  auto entryAlreadyExists = [this](ModuleSearchPathKind Kind,
                                   unsigned SearchPathIndex) -> bool {
    return llvm::any_of(LookupTable, [&](const auto &LookupTableEntry) {
      return llvm::any_of(
          LookupTableEntry.second, [&](ModuleSearchPathPtr ExistingSearchPath) {
            return ExistingSearchPath->getKind() == Kind &&
                   ExistingSearchPath->getIndex() == SearchPathIndex;
          });
    });
  };
  assert(!entryAlreadyExists(Kind, SearchPathIndex) &&
         "Search path with this kind and index already exists");
  ModuleSearchPathPtr TableEntry =
      new ModuleSearchPath(SearchPath, Kind, IsSystem, SearchPathIndex);
  for (auto Dir = FS->dir_begin(SearchPath, Error);
       !Error && Dir != llvm::vfs::directory_iterator(); Dir.increment(Error)) {
    StringRef Filename = llvm::sys::path::filename(Dir->path());
    LookupTable[Filename].push_back(TableEntry);
  }
}

void ModuleSearchPathLookup::rebuildLookupTable(const SearchPathOptions *Opts,
                                                llvm::vfs::FileSystem *FS,
                                                bool IsOSDarwin) {
  clearLookupTable();

  for (auto Entry : llvm::enumerate(Opts->getImportSearchPaths())) {
    addFilesInPathToLookupTable(FS, Entry.value(),
                                ModuleSearchPathKind::Import,
                                /*isSystem=*/false, Entry.index());
  }

  for (auto Entry : llvm::enumerate(Opts->getFrameworkSearchPaths())) {
    addFilesInPathToLookupTable(FS, Entry.value().Path, ModuleSearchPathKind::Framework,
                                Entry.value().IsSystem, Entry.index());
  }

  // Apple platforms have extra implicit framework search paths:
  // $SDKROOT/System/Library/Frameworks/ and $SDKROOT/Library/Frameworks/.
  if (IsOSDarwin) {
    for (auto Entry : llvm::enumerate(Opts->getDarwinImplicitFrameworkSearchPaths())) {
      addFilesInPathToLookupTable(FS, Entry.value(),
                                  ModuleSearchPathKind::DarwinImplicitFramework,
                                  /*isSystem=*/true, Entry.index());
    }
  }

  for (auto Entry : llvm::enumerate(Opts->getRuntimeLibraryImportPaths())) {
    addFilesInPathToLookupTable(FS, Entry.value(),
                                ModuleSearchPathKind::RuntimeLibrary,
                                /*isSystem=*/true, Entry.index());
  }

  State.FileSystem = FS;
  State.IsOSDarwin = IsOSDarwin;
  State.Opts = Opts;
  State.IsPopulated = true;
}

SmallVector<const ModuleSearchPath *, 4>
ModuleSearchPathLookup::searchPathsContainingFile(
    const SearchPathOptions *Opts, llvm::ArrayRef<std::string> Filenames,
    llvm::vfs::FileSystem *FS, bool IsOSDarwin) {
  if (!State.IsPopulated || State.FileSystem != FS ||
      State.IsOSDarwin != IsOSDarwin || State.Opts != Opts) {
    rebuildLookupTable(Opts, FS, IsOSDarwin);
  }

  // Gather all search paths that include a file whose name is in Filenames.
  // To make sure that we don't include the same search paths twice, keep track
  // of which search paths have already been added to Result by their kind and
  // Index in ResultIds.
  // Note that if a search path is specified twice by including it twice in
  // compiler arguments or by specifying it as different kinds (e.g. once as
  // import and once as framework search path), these search paths are
  // considered different (because they have different indices/kinds and may
  // thus still be included twice.
  llvm::SmallVector<const ModuleSearchPath *, 4> Result;
  llvm::SmallSet<std::pair<ModuleSearchPathKind, unsigned>, 4> ResultIds;

  for (auto &Filename : Filenames) {
    for (auto &Entry : LookupTable[Filename]) {
      if (ResultIds.insert(std::make_pair(Entry->getKind(), Entry->getIndex()))
              .second) {
        Result.push_back(Entry.get());
      }
    }
  }

  // Make sure we maintain the same search paths order that we had used in
  // populateLookupTableIfNecessary after merging results from
  // different filenames.
  llvm::sort(Result, [](const ModuleSearchPath *Lhs,
                        const ModuleSearchPath *Rhs) { return *Lhs < *Rhs; });
  return Result;
}

/// Loads a VFS YAML file located at \p File using \p BaseFS and adds it to
/// \p OverlayFS. Returns an error if either loading the \p File failed or it
/// is invalid.
static llvm::Error loadAndValidateVFSOverlay(
    const std::string &File,
    const llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> &BaseFS,
    const llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> &OverlayFS) {
  auto Buffer = BaseFS->getBufferForFile(File);
  if (!Buffer)
    return llvm::createFileError(File, Buffer.getError());

  auto VFS = llvm::vfs::getVFSFromYAML(std::move(Buffer.get()), nullptr, File);
  if (!VFS)
    return llvm::createFileError(File, llvm::errc::invalid_argument);

  OverlayFS->pushOverlay(std::move(VFS));
  return llvm::Error::success();
}

llvm::Expected<llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>>
SearchPathOptions::makeOverlayFileSystem(
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> BaseFS) const {
  // TODO: This implementation is different to how Clang reads overlays in.
  // Expose a helper in Clang rather than doing this ourselves.

  auto OverlayFS =
      llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(BaseFS);

  llvm::Error AllErrors = llvm::Error::success();
  bool hasOverlays = false;
  for (const auto &File : VFSOverlayFiles) {
    hasOverlays = true;
    if (auto Err = loadAndValidateVFSOverlay(File, BaseFS, OverlayFS))
      AllErrors = llvm::joinErrors(std::move(AllErrors), std::move(Err));
  }

  if (AllErrors)
    return std::move(AllErrors);

  if (hasOverlays)
    return OverlayFS;
  return BaseFS;
}
