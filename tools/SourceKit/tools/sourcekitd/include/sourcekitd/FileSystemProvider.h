//===--- FileSystemProvider.h - ---------------------------------*- C++ -*-===//
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

#ifndef LLVM_SOURCEKITD_FILESYSTEMPROVIDER_H
#define LLVM_SOURCEKITD_FILESYSTEMPROVIDER_H

#include "SourceKit/Support/FileSystemProvider.h"
#include "clang/Basic/InMemoryOutputFileSystem.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringRef.h"

namespace SourceKit {

/// Registers a FileSystemProvider with the running sourcekitd. After this
/// function is called, requests can use this FileSystemProvider by setting
/// 'key.vfs.name' to Name.
///
/// The caller is responsible for keeping FileSystemProvider alive as long as
/// sourcekitd.
///
/// Is not threadsafe.
///
/// \param FileSystemProvider must be non-null
void setGlobalFileSystemProvider(
    llvm::StringRef Name, SourceKit::FileSystemProvider *FileSystemProvider);

/// Subsequent requests will write temporary output files to this filesystem
/// rather than to the real filesystem.
///
/// Is not threadsafe.
///
/// \param FS may be null, which makes subsequent requests start writing
/// temporary output files to the real filesystem again.
void setGlobalInMemoryOutputFileSystem(
    llvm::IntrusiveRefCntPtr<clang::InMemoryOutputFileSystem> FS);

} // namespace SourceKit

#endif
