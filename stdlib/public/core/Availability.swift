//===----------------------------------------------------------------------===//
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

import SwiftShims

/// Returns 1 if the running OS version is greater than or equal to
/// major.minor.patchVersion and 0 otherwise.
///
/// This is a magic entry point known to the compiler. It is called in
/// generated code for API availability checking.
@_semantics("availability.osversion")
@_effects(readnone)
@_unavailableInEmbedded
public func _stdlib_isOSVersionAtLeast(
  _ major: Builtin.Word,
  _ minor: Builtin.Word,
  _ patch: Builtin.Word
) -> Builtin.Int1 {
#if (os(macOS) || os(iOS) || os(tvOS) || os(watchOS) || os(visionOS)) && SWIFT_RUNTIME_OS_VERSIONING
  if Int(major) == 9999 {
    return true._value
  }
  let runningVersion = _swift_stdlib_operatingSystemVersion()
  
  let result =
    (runningVersion.majorVersion,runningVersion.minorVersion,runningVersion.patchVersion)
    >= (Int(major),Int(minor),Int(patch))

  return result._value
#else
  // FIXME: As yet, there is no obvious versioning standard for platforms other
  // than Darwin-based OSes, so we just assume false for now. 
  // rdar://problem/18881232
  return false._value
#endif
}

#if os(macOS) && SWIFT_RUNTIME_OS_VERSIONING
// This is a magic entry point known to the compiler. It is called in
// generated code for API availability checking.
@_semantics("availability.osversion")
@_effects(readnone)
@_unavailableInEmbedded
public func _stdlib_isOSVersionAtLeastOrVariantVersionAtLeast(
  _ major: Builtin.Word,
  _ minor: Builtin.Word,
  _ patch: Builtin.Word,
  _ variantMajor: Builtin.Word,
  _ variantMinor: Builtin.Word,
  _ variantPatch: Builtin.Word
  ) -> Builtin.Int1 {
  return _stdlib_isOSVersionAtLeast(major, minor, patch)
}
#endif

public typealias _SwiftStdlibVersion = SwiftShims._SwiftStdlibVersion

/// Return true if the main executable was linked with an SDK version
/// corresponding to the given Swift Stdlib release, or later. Otherwise, return
/// false.
///
/// This is useful to maintain compatibility with older binaries after a
/// behavioral change in the stdlib.
///
/// This function must not be called from inlinable code.
@inline(__always)
@_unavailableInEmbedded
internal func _isExecutableLinkedOnOrAfter(
  _ stdlibVersion: _SwiftStdlibVersion
) -> Bool {
#if SWIFT_RUNTIME_OS_VERSIONING
  return _swift_stdlib_isExecutableLinkedOnOrAfter(stdlibVersion)
#else
  return true
#endif
}

extension _SwiftStdlibVersion {
  @_alwaysEmitIntoClient
  public static var v5_6_0: Self { Self(_value: 0x050600) }

  @_alwaysEmitIntoClient
  public static var v5_7_0: Self { Self(_value: 0x050700) }

  // Note: As of now, there is no bincompat level defined for the versions
  // below. If you need to use one of these in a call to
  // `_isExecutableLinkedOnOrAfter`, then you'll need to define the
  // corresponding version in the runtime.
  @_alwaysEmitIntoClient
  public static var v5_8_0: Self { Self(_value: 0x050800) }
  @_alwaysEmitIntoClient
  public static var v5_9_0: Self { Self(_value: 0x050900) }
  @_alwaysEmitIntoClient
  public static var v5_10_0: Self { Self(_value: 0x050A00) }
  @_alwaysEmitIntoClient
  public static var v6_0_0: Self { Self(_value: 0x060000) }
  @_alwaysEmitIntoClient
  public static var v6_1_0: Self { Self(_value: 0x060100) }

  @available(SwiftStdlib 5.7, *)
  public static var current: Self { .v6_1_0 }
}

@available(SwiftStdlib 5.7, *)
@_unavailableInEmbedded
extension _SwiftStdlibVersion: CustomStringConvertible {
  @available(SwiftStdlib 5.7, *)
  public var description: String {
    let major = _value >> 16
    let minor = (_value >> 8) & 0xFF
    let patch = _value & 0xFF
    return "\(major).\(minor).\(patch)"
  }
}


