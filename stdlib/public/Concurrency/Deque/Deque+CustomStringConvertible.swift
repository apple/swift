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

#if !SWIFT_STDLIB_STATIC_PRINT
import Swift

extension _Deque: CustomStringConvertible {
  /// A textual representation of this instance.
  var description: String {
    var result = "["
    var first = true
    for item in self {
      if first {
        first = false
      } else {
        result += ", "
      }
      print(item, terminator: "", to: &result)
    }
    result += "]"
    return result
  }
}
#endif