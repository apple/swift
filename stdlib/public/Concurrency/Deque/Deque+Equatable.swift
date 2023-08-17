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

extension _Deque: Equatable where Element: Equatable {
  /// Returns a Boolean value indicating whether two values are equal. Two
  /// deques are considered equal if they contain the same elements in the same
  /// order.
  ///
  /// - Complexity: O(`min(left.count, right.count)`)
  static func ==(left: Self, right: Self) -> Bool {
    return left.elementsEqual(right)
  }
}
