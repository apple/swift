//===--- Bincompat.cpp - Binary compatibility checks. -----------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Checks for enabling binary compatibility workarounds.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Config.h"
#include "swift/Runtime/Bincompat.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/EnvironmentVariables.h"
#include "swift/Threading/Once.h"
#include "swift/shims/RuntimeShims.h"
#include <stdint.h>

// If this is an Apple OS, use the Apple binary compatibility rules
#if __has_include(<mach-o/dyld_priv.h>) && defined(SWIFT_RUNTIME_OS_VERSIONING)
  #include <mach-o/dyld_priv.h>
  #ifndef BINARY_COMPATIBILITY_APPLE
    #define BINARY_COMPATIBILITY_APPLE 1
  #endif
#else
  #undef BINARY_COMPATIBILITY_APPLE
#endif

namespace swift {

namespace runtime {

namespace bincompat {

#if BINARY_COMPATIBILITY_APPLE
enum sdk_test {
  oldOS, // Can't tell the app SDK used because this is too old an OS
  oldApp,
  newApp
};
static enum sdk_test isAppAtLeast(dyld_build_version_t version) {
  if (__builtin_available(macOS 11.3, iOS 14.5, tvOS 14.5, watchOS 7.4, *)) {
    // Query the SDK version used to build the currently-running executable
    if (dyld_program_sdk_at_least(version)) {
      return newApp;
    } else {
      return oldApp;
    }
  }
  // Older Apple OS lack the ability to test the SDK version of the running app
  return oldOS;
}

static enum sdk_test isAppAtLeastSpring2021() {
    const dyld_build_version_t spring_2021_os_versions = {0xffffffff, 0x007e50301};
    return isAppAtLeast(spring_2021_os_versions);
}

static enum sdk_test isAppAtLeastFall2023() {
    const dyld_build_version_t fall_2023_os_versions = {0xffffffff, 0x007e70901};
    return isAppAtLeast(fall_2023_os_versions);
}
#endif

static _SwiftStdlibVersion binCompatVersionOverride = { 0 };

static _SwiftStdlibVersion const knownVersions[] = {
  { /* 5.6.0 */0x050600 },
  { /* 5.7.0 */0x050700 },
  { 0 },
};

static bool isKnownBinCompatVersion(_SwiftStdlibVersion version) {
  for (int i = 0; knownVersions[i]._value != 0; ++i) {
    if (knownVersions[i]._value == version._value) {
      return true;
    }
  }
  return false;
}

static void checkBinCompatEnvironmentVariable(void *context) {
  _SwiftStdlibVersion version =
    { runtime::environment::SWIFT_BINARY_COMPATIBILITY_VERSION() };

  if (version._value > 0 && !isKnownBinCompatVersion(version)) {
    swift::warning(RuntimeErrorFlagNone,
                   "Warning: ignoring unknown SWIFT_BINARY_COMPATIBILITY_VERSION %x.\n",
                   version._value);
    return;
  }

  binCompatVersionOverride = version;
}

extern "C" __swift_bool _swift_stdlib_isExecutableLinkedOnOrAfter(
  _SwiftStdlibVersion version
) {
  static once_t getenvToken;
  swift::once(getenvToken, checkBinCompatEnvironmentVariable, nullptr);

  if (binCompatVersionOverride._value > 0) {
    return version._value <= binCompatVersionOverride._value;
  }

#if BINARY_COMPATIBILITY_APPLE
  // Return true for all known versions for now -- we can't map them to OS
  // versions at this time.
  return isKnownBinCompatVersion(version);

#else // !BINARY_COMPATIBILITY_APPLE
  return isKnownBinCompatVersion(version);
#endif
}

// Should we mimic the old override behavior when scanning protocol conformance records?

// Old apps expect protocol conformances to override each other in a particular
// order.  Starting with Swift 5.4, that order has changed as a result of
// significant performance improvements to protocol conformance scanning.  If
// this returns `true`, the protocol conformance scan will do extra work to
// mimic the old override behavior.
bool useLegacyProtocolConformanceReverseIteration() {
#if BINARY_COMPATIBILITY_APPLE
  switch (isAppAtLeastSpring2021()) {
  case oldOS: return false; // New (non-legacy) behavior on old OSes
  case oldApp: return true; // Legacy behavior for pre-Spring 2021 apps on new OS
  case newApp: return false; // New behavior for new apps
  }
#else
  return false; // Never use the legacy behavior on non-Apple OSes
#endif
}

// Should the dynamic cast operation crash when it sees
// a non-nullable Obj-C pointer with a null value?

// Obj-C does not strictly enforce non-nullability in all cases, so it is
// possible for Obj-C code to pass null pointers into Swift code even when
// declared non-nullable.  Such null pointers can lead to undefined behavior
// later on.  Starting in Swift 5.4, these unexpected null pointers are fatal
// runtime errors, but this is selectively disabled for old apps.
bool useLegacyPermissiveObjCNullSemanticsInCasting() {
#if BINARY_COMPATIBILITY_APPLE
  switch (isAppAtLeastSpring2021()) {
  case oldOS: return true; // Permissive (legacy) behavior on old OS
  case oldApp: return true; // Permissive (legacy) behavior for old apps
  case newApp: return false; // Strict behavior for new apps
  }
#else
  return false;  // Always use the strict behavior on non-Apple OSes
#endif
}

// Should casting a nil optional to another optional
// use the legacy semantics?

// For consistency, starting with Swift 5.4, casting Optional<Int> to
// Optional<Optional<Int>> always wraps the source in another layer
// of Optional.
// Earlier versions of the Swift runtime did not do this if the source
// optional was nil.  In that case, the outer target optional would be
// set to nil.
bool useLegacyOptionalNilInjectionInCasting() {
#if BINARY_COMPATIBILITY_APPLE
  switch (isAppAtLeastSpring2021()) {
  case oldOS: return true; // Legacy behavior on old OS
  case oldApp: return true; // Legacy behavior for old apps
  case newApp: return false; // Consistent behavior for new apps
  }
#else
  return false;  // Always use the 5.4 behavior on non-Apple OSes
#endif
}

// Should casting be strict about protocol conformance when
// boxing Swift values to pass to Obj-C?

// Earlier versions of the Swift runtime would allow you to
// cast a swift value to e.g., `NSCopying` or `NSObjectProtocol`
// even if that value did not actually conform.  This was
// due to the fact that the `__SwiftValue` box type itself
// conformed to these protocols.

// But this was not really sound, as it implies for example that
// `x is NSCopying` is always `true` regardless of whether
// `x` actually has the `copyWithZone()` method required
// by that protocol.
bool useLegacyObjCBoxingInCasting() {
#if BINARY_COMPATIBILITY_APPLE
  switch (isAppAtLeastFall2023()) {
  case oldOS: return true; // Legacy behavior on old OS
  case oldApp: return true; // Legacy behavior for old apps
  case newApp: return false; // New behavior for new apps
  }
#else
  return false; // Always use the new behavior on non-Apple OSes
#endif
}

// Should casting be strict about protocol conformance when
// unboxing values that were boxed for Obj-C use?

// Similar to `useLegacyObjCBoxingInCasting()`, but
// this applies to the case where you have already boxed
// some Swift non-reference-type into a `__SwiftValue`
// and are now casting to a protocol.

// For example, this cast
// `x as! AnyObject as? NSCopying`
// always succeeded with the legacy semantics.

bool useLegacySwiftValueUnboxingInCasting() {
#if BINARY_COMPATIBILITY_APPLE
  switch (isAppAtLeastFall2023()) {
  case oldOS: return true; // Legacy behavior on old OS
  case oldApp: return true; // Legacy behavior for old apps
  case newApp: return false; // New behavior for new apps
  }
#else
  return false; // Always use the new behavior on non-Apple OSes
#endif
}

} // namespace bincompat

} // namespace runtime

} // namespace swift
