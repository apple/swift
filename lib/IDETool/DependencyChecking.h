//===--- DependencyChecking.h ---------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/LLVM.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/VirtualFileSystem.h"

namespace swift {
class CompilerInstance;

namespace ide {
/// Cache hash code of the dependencies into \p Map . If \p excludeBufferID is
/// specified, other source files are considered "dependencies", otherwise all
/// source files are considered "current"
void cacheDependencyHashIfNeeded(CompilerInstance &CI,
                                 llvm::Optional<unsigned> excludeBufferID,
                                 llvm::StringMap<llvm::hash_code> &Map);

/// Check if any dependent files are modified since \p timestamp. If
/// \p excludeBufferID is specified, other source files are considered
/// "dependencies", otherwise all source files are considered "current".
/// \p Map should be the map populated by \c cacheDependencyHashIfNeeded at the
/// previous dependency checking.
bool areAnyDependentFilesInvalidated(
    CompilerInstance &CI, llvm::vfs::FileSystem &FS,
    llvm::Optional<unsigned> excludeBufferID, llvm::sys::TimePoint<> timestamp,
    const llvm::StringMap<llvm::hash_code> &Map);

} // namespace ide
} // namespace swift
