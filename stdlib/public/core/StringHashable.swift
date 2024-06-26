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

extension String: Hashable {
  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  public func hash(into hasher: inout Hasher) {
    if _fastPath(self._guts.isNFCFastUTF8) {
      self._guts.withFastUTF8 {
        hasher.combine(bytes: UnsafeRawBufferPointer($0))
      }
      hasher.combine(0xFF as UInt8) // terminator
    } else {
      _gutsSlice._normalizedHash(into: &hasher)
    }
  }
}

// FIXME: Add an explicit definition for `Substring.hash(into:)`.

extension StringProtocol {
  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  @_specialize(where Self == String)
  @_specialize(where Self == Substring)
  public func hash(into hasher: inout Hasher) {
    _gutsSlice._normalizedHash(into: &hasher)
  }
}

extension _StringGutsSlice {
  @_effects(releasenone) @inline(never) // slow-path
  internal func _normalizedHash(into hasher: inout Hasher) {
    if self.isNFCFastUTF8 {
      self.withFastUTF8 {
        hasher.combine(bytes: UnsafeRawBufferPointer($0))
      }
    } else {
      _withNFCCodeUnits {
        hasher.combine($0)
      }
    }
    hasher.combine(0xFF as UInt8) // terminator
  }
}

