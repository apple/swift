//===--- PlatformKind.h - Swift Language Platform Kinds ---------*- C++ -*-===//
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
// This file defines the platform kinds for API availability.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_PLATFORM_KIND_H
#define SWIFT_AST_PLATFORM_KIND_H

#include "swift/Basic/LLVM.h"
#include "swift/Config.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/VersionTuple.h"

namespace swift {

class LangOptions;

/// Available platforms for the availability attribute.
enum class PlatformKind: uint8_t {
  none,
#define AVAILABILITY_PLATFORM(X, PrettyName) X,
#include "swift/AST/PlatformKinds.def"
};

/// Returns the short string representing the platform, suitable for
/// use in availability specifications (e.g., "OSX").
StringRef platformString(PlatformKind platform);
  
/// Returns the platform kind corresponding to the passed-in short platform name
/// or None if such a platform kind does not exist.
llvm::Optional<PlatformKind> platformFromString(StringRef Name);

/// Returns a valid platform string that is closest to the candidate string
/// based on edit distance. Returns \c None if the closest valid platform
/// distance is not within a minimum threshold.
llvm::Optional<StringRef> closestCorrectedPlatformString(StringRef candidate);

/// Returns a human-readable version of the platform name as a string, suitable
/// for emission in diagnostics (e.g., "macOS").
StringRef prettyPlatformString(PlatformKind platform);

/// Returns whether the passed-in platform is active, given the language
/// options. A platform is active if either it is the target platform or its
/// AppExtension variant is the target platform. For example, OS X is
/// considered active when the target operating system is OS X and app extension
/// restrictions are enabled, but OSXApplicationExtension is not considered
/// active when the target platform is OS X and app extension restrictions are
/// disabled. PlatformKind::none is always considered active.
/// If ForTargetVariant is true then for zippered builds the target-variant
/// triple will be used rather than the target to determine whether the
/// platform is active.
bool isPlatformActive(PlatformKind Platform, const LangOptions &LangOpts,
                      bool ForTargetVariant = false);

/// Returns the target platform for the given language options.
PlatformKind targetPlatform(const LangOptions &LangOpts);

/// Returns true when availability attributes from the "parent" platform
/// should also apply to the "child" platform for declarations without
/// an explicit attribute for the child.
bool inheritsAvailabilityFromPlatform(PlatformKind Child, PlatformKind Parent);

llvm::VersionTuple canonicalizePlatformVersion(
    PlatformKind platform, const llvm::VersionTuple &version);

} // end namespace swift

#endif
