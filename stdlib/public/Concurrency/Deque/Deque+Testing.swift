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

// This file contains exported but non-public entry points to support clear box
// testing.

extension _Deque {
  /// The maximum number of elements this deque is currently able to store
  /// without reallocating its storage buffer.
  ///
  /// This information isn't exposed as public, as the returned value isn't
  /// guaranteed to be stable, and may even differ between equal deques,
  /// violating value semantics.
  ///
  /// This property isn't intended to be used outside of `Deque`'s own test
  /// target.
  // @_spi(Testing)
  var _capacity: Int {
    _storage.capacity
  }

  /// The number of the storage slot in this deque that holds the first element.
  /// (Or would hold it after an insertion in case the deque is currently
  /// empty.)
  ///
  /// This property isn't intended to be used outside of `Deque`'s own test
  /// target.
  // @_spi(Testing)
  var _startSlot: Int {
    _storage.startSlot.position
  }

  /// Constructs a deque instance of the specified contents and layout. Exposed
  /// as to allow exhaustive input/output tests for `Deque`'s members.
  /// This isn't intended to be used outside of `Deque`'s own test target.
  // @_spi(Testing)
  init<S: Sequence>(
    _capacity capacity: Int,
    startSlot: Int,
    contents: S
  ) where S.Element == Element {
    let contents = ContiguousArray(contents)
    precondition(capacity >= 0)
    precondition(startSlot >= 0 && (startSlot < capacity || (capacity == 0 && startSlot == 0)))
    precondition(contents.count <= capacity)
    let startSlot = _Slot(at: startSlot)
    let buffer = _DequeBuffer<Element>.create(minimumCapacity: capacity) { _ in
      _DequeBufferHeader(capacity: capacity, count: contents.count, startSlot: startSlot)
    }
    let storage = _Deque<Element>._Storage(unsafeDowncast(buffer, to: _DequeBuffer.self))
    if contents.count > 0 {
      contents.withUnsafeBufferPointer { source in
        storage.update { target in
          let segments = target.mutableSegments()
          let c = segments.first.count
          segments.first._initialize(from: source.prefix(c)._rebased())
          if let second = segments.second {
            second._initialize(from: source.dropFirst(c)._rebased())
          }
        }
      }
    }
    self.init(_storage: storage)
    assert(self._capacity == capacity)
    assert(self._startSlot == startSlot.position)
    assert(self.count == contents.count)
  }
}
