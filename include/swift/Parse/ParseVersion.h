//===--- ParseVersion.h - Parser Swift Version Numbers ----------*- C++ -*-===//
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

#ifndef SWIFT_PARSE_PARSEVERSION_H
#define SWIFT_PARSE_PARSEVERSION_H

#include "swift/Basic/Version.h"

namespace swift {
class DiagnosticEngine;

namespace version {
/// Returns a version from the currently defined SWIFT_COMPILER_VERSION.
///
/// If SWIFT_COMPILER_VERSION is undefined, this will return the empty
/// compiler version.
Version getCurrentCompilerVersion();
} // namespace version

class VersionParser final {
public:
  /// Parse a version in the form used by the _compiler_version(string-literal)
  /// \#if condition.
  ///
  /// \note This is \em only used for the string literal version, so it includes
  /// backwards-compatibility logic to convert it to something that can be
  /// compared with a modern SWIFT_COMPILER_VERSION.
  static llvm::Optional<version::Version>
  parseCompilerVersionString(StringRef VersionString, SourceLoc Loc,
                             DiagnosticEngine *Diags);

  /// Parse a generic version string of the format [0-9]+(.[0-9]+)*
  ///
  /// Version components can be any unsigned 64-bit number.
  static llvm::Optional<version::Version>
  parseVersionString(StringRef VersionString, SourceLoc Loc,
                     DiagnosticEngine *Diags);
};
} // namespace swift

#endif // SWIFT_PARSE_PARSEVERSION_H
