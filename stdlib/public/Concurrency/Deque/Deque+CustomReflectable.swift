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

#if SWIFT_ENABLE_REFLECTION
import Swift

extension _Deque: CustomReflectable {
  /// The custom mirror for this instance.
  var customMirror: Mirror {
    Mirror(self, unlabeledChildren: self, displayStyle: .collection)
  }
}
#endif