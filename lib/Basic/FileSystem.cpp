//===--- FileSystem.cpp - Extra helpers for manipulating files ------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/FileSystem.h"

#include "swift/Basic/Assertions.h"
#include "clang/Basic/FileManager.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <optional>

using namespace swift;

namespace {
  class OpenFileRAII {
    static const int INVALID_FD = -1;
  public:
    int fd = INVALID_FD;

    ~OpenFileRAII() {
      if (fd != INVALID_FD)
        llvm::sys::Process::SafelyCloseFileDescriptor(fd);
    }
  };
} // end anonymous namespace

/// Does some simple checking to see if a temporary file can be written next to
/// \p outputPath and then renamed into place.
///
/// Helper for swift::atomicallyWritingToFile.
///
/// If the result is an error, the write won't succeed at all, and the calling
/// operation should bail out early.
static llvm::ErrorOr<bool>
canUseTemporaryForWrite(const llvm::StringRef outputPath) {
  namespace fs = llvm::sys::fs;

  if (outputPath == "-") {
    // Special case: "-" represents stdout, and LLVM's output stream APIs are
    // aware of this. It doesn't make sense to use a temporary in this case.
    return false;
  }

  fs::file_status status;
  (void)fs::status(outputPath, status);
  if (!fs::exists(status)) {
    // Assume we'll be able to write to both a temporary file and to the final
    // destination if the final destination doesn't exist yet.
    return true;
  }

  // Fail early if we can't write to the final destination.
  if (!fs::can_write(outputPath))
    return llvm::make_error_code(llvm::errc::operation_not_permitted);

  // Only use a temporary if the output is a regular file. This handles
  // things like '-o /dev/null'
  return fs::is_regular_file(status);
}

/// Attempts to open a temporary file next to \p outputPath, with the intent
/// that once the file has been written it will be renamed into place.
///
/// Helper for swift::atomicallyWritingToFile.
///
/// \param[out] openedStream On success, a stream opened for writing to the
/// temporary file that was just created.
/// \param outputPath The path to the final output file, which is used to decide
/// where to put the temporary.
///
/// \returns The path to the temporary file that was opened, or \c None if the
/// file couldn't be created.
static std::optional<std::string>
tryToOpenTemporaryFile(std::optional<llvm::raw_fd_ostream> &openedStream,
                       const llvm::StringRef outputPath) {
  namespace fs = llvm::sys::fs;

  // Create a temporary file path.
  // Insert a placeholder for a random suffix before the extension (if any).
  // Then because some tools glob for build artifacts (such as clang's own
  // GlobalModuleIndex.cpp), also append .tmp.
  llvm::SmallString<128> tempPath;
  const llvm::StringRef outputExtension =
      llvm::sys::path::extension(outputPath);
  tempPath = outputPath.drop_back(outputExtension.size());
  tempPath += "-%%%%%%%%";
  tempPath += outputExtension;
  tempPath += ".tmp";

  int fd;
  const unsigned perms = fs::all_read | fs::all_write;
  std::error_code EC = fs::createUniqueFile(tempPath, fd, tempPath,
                                            fs::OF_None, perms);

  if (EC) {
    // Ignore the specific error; the caller has to fall back to not using a
    // temporary anyway.
    return std::nullopt;
  }

  openedStream.emplace(fd, /*shouldClose=*/true);
  // Make sure the temporary file gets removed if we crash.
  llvm::sys::RemoveFileOnSignal(tempPath);
  return tempPath.str().str();
}

std::error_code swift::atomicallyWritingToFile(
    const llvm::StringRef outputPath,
    const llvm::function_ref<void(llvm::raw_pwrite_stream &)> action) {
  namespace fs = llvm::sys::fs;

  // FIXME: This is mostly a simplified version of
  // clang::CompilerInstance::createOutputFile. It would be great to share the
  // implementation.
  assert(!outputPath.empty());

  llvm::ErrorOr<bool> canUseTemporary = canUseTemporaryForWrite(outputPath);
  if (std::error_code error = canUseTemporary.getError())
    return error;

  std::optional<std::string> temporaryPath;
  {
    std::optional<llvm::raw_fd_ostream> OS;
    if (canUseTemporary.get()) {
      temporaryPath = tryToOpenTemporaryFile(OS, outputPath);

      if (!temporaryPath) {
        assert(!OS.has_value());
        // If we failed to create the temporary, fall back to writing to the
        // file directly. This handles the corner case where we cannot write to
        // the directory, but can write to the file.
      }
    }

    if (!OS.has_value()) {
      std::error_code error;
      OS.emplace(outputPath, error, fs::OF_None);
      if (error) {
        return error;
      }
    }

    action(OS.value());
    // In addition to scoping the use of 'OS', ending the scope here also
    // ensures that it's been flushed (by destroying it).
  }

  if (!temporaryPath.has_value()) {
    // If we didn't use a temporary, we're done!
    return std::error_code();
  }

  return swift::moveFileIfDifferent(temporaryPath.value(), outputPath);
}

llvm::ErrorOr<FileDifference>
swift::areFilesDifferent(const llvm::Twine &source,
                         const llvm::Twine &destination,
                         bool allowDestinationErrors) {
  namespace fs = llvm::sys::fs;

  if (fs::equivalent(source, destination)) {
    return FileDifference::IdenticalFile;
  }

  OpenFileRAII sourceFile;
  fs::file_status sourceStatus;
  if (std::error_code error = fs::openFileForRead(source, sourceFile.fd)) {
    // If we can't open the source file, fail.
    return error;
  }
  if (std::error_code error = fs::status(sourceFile.fd, sourceStatus)) {
    // If we can't stat the source file, fail.
    return error;
  }

  /// Converts an error from the destination file into either an error or
  /// DifferentContents return, depending on `allowDestinationErrors`.
  auto convertDestinationError = [=](std::error_code error) ->
      llvm::ErrorOr<FileDifference> {
    if (allowDestinationErrors){
      return FileDifference::DifferentContents;
    }
    return error;
  };

  OpenFileRAII destFile;
  fs::file_status destStatus;
  if (std::error_code error = fs::openFileForRead(destination, destFile.fd)) {
    // If we can't open the destination file, fail in the specified fashion.
    return convertDestinationError(error);
  }
  if (std::error_code error = fs::status(destFile.fd, destStatus)) {
    // If we can't open the destination file, fail in the specified fashion.
    return convertDestinationError(error);
  }

  uint64_t size = sourceStatus.getSize();
  if (size != destStatus.getSize()) {
    // If the files are different sizes, they must be different.
    return FileDifference::DifferentContents;
  }
  if (size == 0) {
    // If both files are zero size, they must be the same.
    return FileDifference::SameContents;
  }

  // The two files match in size, so we have to compare the bytes to determine
  // if they're the same.
  std::error_code sourceRegionErr;
  fs::mapped_file_region sourceRegion(fs::convertFDToNativeFile(sourceFile.fd),
                                      fs::mapped_file_region::readonly,
                                      size, 0, sourceRegionErr);
  if (sourceRegionErr) {
    return sourceRegionErr;
  }

  std::error_code destRegionErr;
  fs::mapped_file_region destRegion(fs::convertFDToNativeFile(destFile.fd),
                                    fs::mapped_file_region::readonly,
                                    size, 0, destRegionErr);

  if (destRegionErr) {
    return convertDestinationError(destRegionErr);
  }

  if (memcmp(sourceRegion.const_data(), destRegion.const_data(), size) != 0) {
    return FileDifference::DifferentContents;
  }

  return FileDifference::SameContents;
}

std::error_code swift::moveFileIfDifferent(const llvm::Twine &source,
                                           const llvm::Twine &destination) {
  namespace fs = llvm::sys::fs;

  auto result = areFilesDifferent(source, destination,
                                  /*allowDestinationErrors=*/true);

  if (!result)
    return result.getError();

  switch (*result) {
  case FileDifference::IdenticalFile:
    // Do nothing for a self-move.
    return std::error_code();

  case FileDifference::SameContents:
    // Files are identical; remove the source file.
    return fs::remove(source);

  case FileDifference::DifferentContents:
    // Files are different; overwrite the destination file.
    return fs::rename(source, destination);
  }
  llvm_unreachable("Unhandled FileDifference in switch");
}

llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
swift::vfs::getFileOrSTDIN(llvm::vfs::FileSystem &FS,
                           const llvm::Twine &Filename,
                           int64_t FileSize,
                           bool RequiresNullTerminator,
                           bool IsVolatile,
                           unsigned BADFRetry) {
  llvm::SmallString<256> NameBuf;
  llvm::StringRef NameRef = Filename.toStringRef(NameBuf);

  if (NameRef == "-")
    return llvm::MemoryBuffer::getSTDIN();
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> inputFileOrErr = nullptr;
  for (unsigned I = 0; I != BADFRetry + 1; ++ I) {
    inputFileOrErr = FS.getBufferForFile(Filename, FileSize,
                                         RequiresNullTerminator, IsVolatile);
    if (inputFileOrErr)
      return inputFileOrErr;
    if (inputFileOrErr.getError().value() != EBADF)
      return inputFileOrErr;
  }
  return inputFileOrErr;
}
