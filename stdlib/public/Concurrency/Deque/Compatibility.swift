//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
//
//===----------------------------------------------------------------------===//

/// This file is copied from swift-collections and should not be modified here.
/// Rather all changes should be made to swift-collections and copied back.

import Swift

extension Array {
  /// Returns true if `Array.withContiguousStorageIfAvailable` is broken
  /// in the stdlib we're currently running on.
  ///
  /// See https://bugs.swift.org/browse/SR-14663.
  internal static func _isWCSIABroken() -> Bool {
    #if _runtime(_ObjC)
    guard _isBridgedVerbatimToObjectiveC(Element.self) else {
      // SR-14663 only triggers on array values that are verbatim bridged
      // from Objective-C, so it cannot ever trigger for element types
      // that aren't verbatim bridged.
      return false
    }

    // SR-14663 was introduced in Swift 5.1. Check if we have a broken stdlib.

    // The bug is caused by a bogus precondition inside a non-inlinable stdlib
    // method, so to determine if we're affected, we need to check the currently
    // running OS version.
    #if os(macOS) || os(iOS) || os(watchOS) || os(tvOS)
    guard #available(macOS 10.15, iOS 13, watchOS 6, tvOS 13, *) else {
      // The OS is too old to be affected by this bug.
      return false
    }
    #endif
    // FIXME: When a stdlib is released that contains a fix, add a check for it.
    return true

    #else
    // Platforms that don't have an Objective-C runtime don't have verbatim
    // bridged array values, so the bug doesn't apply to them.
    return false
    #endif
  }
}

extension Sequence {
  // An adjusted version of the standard `withContiguousStorageIfAvailable`
  // method that works around https://bugs.swift.org/browse/SR-14663.
  internal func _withContiguousStorageIfAvailable_SR14663<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    if Self.self == Array<Element>.self && Array<Element>._isWCSIABroken() {
      return nil
    }

    return try self.withContiguousStorageIfAvailable(body)
  }
}
