//===--- Platform.h - Helpers related to target platforms -------*- C++ -*-===//
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

#ifndef SWIFT_BASIC_PLATFORM_H
#define SWIFT_BASIC_PLATFORM_H

#include "swift/Basic/LLVM.h"
#include "swift/Config.h"
#include "clang/Basic/DarwinSDKInfo.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {
  class Triple;
  class VersionTuple;
}

namespace swift {

  enum class DarwinPlatformKind : unsigned {
    MacOS,
    IPhoneOS,
    IPhoneOSSimulator,
    TvOS,
    TvOSSimulator,
    WatchOS,
    WatchOSSimulator
  };

  /// Returns true if the given triple represents iOS running in a simulator.
  bool tripleIsiOSSimulator(const llvm::Triple &triple);

  /// Returns true if the given triple represents AppleTV running in a simulator.
  bool tripleIsAppleTVSimulator(const llvm::Triple &triple);

  /// Returns true if the given triple represents watchOS running in a simulator.
  bool tripleIsWatchSimulator(const llvm::Triple &triple);

  /// Returns true if the given triple represents a macCatalyst environment.
  bool tripleIsMacCatalystEnvironment(const llvm::Triple &triple);

  /// Determine whether the triple infers the "simulator" environment.
  bool tripleInfersSimulatorEnvironment(const llvm::Triple &triple);

  /// Returns true if the given -target triple and -target-variant triple
  /// can be zippered.
  bool triplesAreValidForZippering(const llvm::Triple &target,
                                   const llvm::Triple &targetVariant);

  /// Returns the VersionTuple at which Swift first became available for the OS
  /// represented by `triple`.
  const llvm::Optional<llvm::VersionTuple>
  minimumAvailableOSVersionForTriple(const llvm::Triple &triple);

  /// Returns true if the given triple represents an OS that has all the
  /// "built-in" ABI-stable libraries (stdlib and _Concurrency)
  /// (eg. in /usr/lib/swift).
  bool tripleRequiresRPathForSwiftLibrariesInOS(const llvm::Triple &triple);

  /// Returns the platform name for a given target triple.
  ///
  /// For example, the iOS simulator has the name "iphonesimulator", while real
  /// iOS uses "iphoneos". OS X is "macosx". (These names are intended to be
  /// compatible with Xcode's SDKs.)
  ///
  /// If the triple does not correspond to a known platform, the empty string is
  /// returned.
  StringRef getPlatformNameForTriple(const llvm::Triple &triple);

  /// Returns the platform Kind for Darwin triples.
  DarwinPlatformKind getDarwinPlatformKind(const llvm::Triple &triple);

  /// Returns the architecture component of the path for a given target triple.
  ///
  /// Typically this is used for mapping the architecture component of the
  /// path.
  ///
  /// For example, on Linux "armv6l" and "armv7l" are mapped to "armv6" and
  /// "armv7", respectively, within LLVM. Therefore the component path for the
  /// architecture specific objects will be found in their "mapped" paths.
  ///
  /// This is a stop-gap until full Triple support (ala Clang) exists within swiftc.
  StringRef getMajorArchitectureName(const llvm::Triple &triple);

  /// Computes the normalized target triple used as the most preferred name for
  /// module loading.
  ///
  /// For platforms with fat binaries, this canonicalizes architecture,
  /// vendor, and OS names, strips OS versions, and makes inferred environments
  /// explicit. For other platforms, it returns the unmodified triple.
  ///
  /// The input triple should already be "normalized" in the sense that
  /// llvm::Triple::normalize() would not affect it.
  llvm::Triple getTargetSpecificModuleTriple(const llvm::Triple &triple);
  
  /// Computes the target triple without version information.
  llvm::Triple getUnversionedTriple(const llvm::Triple &triple);

  /// Get the Swift runtime version to deploy back to, given a deployment target expressed as an
  /// LLVM target triple.
  llvm::Optional<llvm::VersionTuple>
  getSwiftRuntimeCompatibilityVersionForTarget(const llvm::Triple &Triple);

  /// Retrieve the target SDK version for the given SDKInfo and target triple.
  llvm::VersionTuple getTargetSDKVersion(clang::DarwinSDKInfo &SDKInfo,
                                         const llvm::Triple &triple);

  /// Get SDK build version.
  std::string getSDKBuildVersion(StringRef SDKPath);
  std::string getSDKBuildVersionFromPlist(StringRef Path);

  /// Get SDK name.
  std::string getSDKName(StringRef SDKPath);
} // end namespace swift

#endif // SWIFT_BASIC_PLATFORM_H

