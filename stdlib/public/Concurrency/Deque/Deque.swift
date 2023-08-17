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

/// A collection implementing a double-ended queue. `Deque` (pronounced "deck")
/// implements an ordered random-access collection that supports efficient
/// insertions and removals from both ends.
///
///     var colors: Deque = ["red", "yellow", "blue"]
///
/// Deques implement the same indexing semantics as arrays: they use integer
/// indices, and the first element of a nonempty deque is always at index zero.
/// Like arrays, deques conform to `RangeReplaceableCollection`,
/// `MutableCollection` and `RandomAccessCollection`, providing a familiar
/// interface for manipulating their contents:
///
///     print(colors[1]) // "yellow"
///     print(colors[3]) // Runtime error: Index out of range
///
///     colors.insert("green", at: 1)
///     // ["red", "green", "yellow", "blue"]
///
///     colors.remove(at: 2) // "yellow"
///     // ["red", "green", "blue"]
///
/// Like all variable-size collections on the standard library, `Deque`
/// implements value semantics: each deque has an independent value that
/// includes the values of its elements. Modifying one deque does not affect any
/// others:
///
///     var copy = deque
///     copy[1] = "violet"
///     print(copy)  // ["red", "violet", "blue"]
///     print(deque) // ["red", "green", "blue"]
///
/// This is implemented with the copy-on-write optimization. Multiple copies of
/// a deque share the same underlying storage until you modify one of the
/// copies. When that happens, the deque being modified replaces its storage
/// with a uniquely owned copy of itself, which is then modified in place.
///
/// `Deque` stores its elements in a circular buffer, which allows efficient
/// insertions and removals at both ends of the collection; however, this comes
/// at the cost of potentially discontiguous storage. In contrast, `Array` is
/// (usually) backed by a contiguous buffer, where new data can be efficiently
/// appended to the end, but inserting at the front is relatively slow, as
/// existing elements need to be shifted to make room.
///
/// This difference in implementation means that while the interface of a deque
/// is very similar to an array, the operations have different performance
/// characteristics. Mutations near the front are expected to be significantly
/// faster in deques, but arrays may measure slightly faster for general
/// random-access lookups.
///
/// Deques provide a handful of additional operations that make it easier to
/// insert and remove elements at the front. This includes queue operations such
/// as `popFirst` and `prepend`, including the ability to directly prepend a
/// sequence of elements:
///
///     colors.append("green")
///     colors.prepend("orange")
///     // colors: ["orange", "red", "blue", "yellow", "green"]
///
///     colors.popLast() // "green"
///     colors.popFirst() // "orange"
///     // colors: ["red", "blue", "yellow"]
///
///     colors.prepend(contentsOf: ["purple", "teal"])
///     // colors: ["purple", "teal", "red", "blue", "yellow"]
///
/// Unlike arrays, deques do not currently provide direct unsafe access to their
/// underlying storage. They also lack a `capacity` property -- the size of the
/// storage buffer at any given point is an unstable implementation detail that
/// should not affect application logic. (However, deques do provide a
/// `reserveCapacity` method.)
struct _Deque<Element> {
  internal typealias _Slot = _DequeSlot

  internal var _storage: _Storage

  internal init(_storage: _Storage) {
    self._storage = _storage
  }

  /// Creates and empty deque with preallocated space for at least the specified
  /// number of elements.
  ///
  /// - Parameter minimumCapacity: The minimum number of elements that the
  ///   newly created deque should be able to store without reallocating its
  ///   storage buffer.
  init(minimumCapacity: Int) {
    self._storage = _Storage(minimumCapacity: minimumCapacity)
  }
}
