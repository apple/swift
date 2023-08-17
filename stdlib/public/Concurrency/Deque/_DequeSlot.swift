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

internal struct _DequeSlot {
  internal var position: Int

  init(at position: Int) {
    assert(position >= 0)
    self.position = position
  }
}

extension _DequeSlot {
  internal static var zero: Self { Self(at: 0) }

  internal func advanced(by delta: Int) -> Self {
    Self(at: position &+ delta)
  }

  internal func orIfZero(_ value: Int) -> Self {
    guard position > 0 else { return Self(at: value) }
    return self
  }
}

extension _DequeSlot: CustomStringConvertible {
  internal var description: String {
    "@\(position)"
  }
}

extension _DequeSlot: Equatable {
  static func ==(left: Self, right: Self) -> Bool {
    left.position == right.position
  }
}

extension _DequeSlot: Comparable {
  static func <(left: Self, right: Self) -> Bool {
    left.position < right.position
  }
}

extension Range where Bound == _DequeSlot {
  internal var _count: Int { upperBound.position - lowerBound.position }
}
