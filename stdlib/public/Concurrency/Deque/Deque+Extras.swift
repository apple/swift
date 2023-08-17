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

extension _Deque {
  /// Creates a deque with the specified capacity, then calls the given
  /// closure with a buffer covering the array's uninitialized memory.
  ///
  /// Inside the closure, set the `initializedCount` parameter to the number of
  /// elements that are initialized by the closure. The memory in the range
  /// `buffer[0..<initializedCount]` must be initialized at the end of the
  /// closure's execution, and the memory in the range
  /// `buffer[initializedCount...]` must be uninitialized. This postcondition
  /// must hold even if the `initializer` closure throws an error.
  ///
  /// - Note: While the resulting deque may have a capacity larger than the
  ///   requested amount, the buffer passed to the closure will cover exactly
  ///   the requested number of elements.
  ///
  /// - Parameters:
  ///   - unsafeUninitializedCapacity: The number of elements to allocate
  ///     space for in the new deque.
  ///   - initializer: A closure that initializes elements and sets the count
  ///     of the new deque.
  ///     - Parameters:
  ///       - buffer: A buffer covering uninitialized memory with room for the
  ///         specified number of elements.
  ///       - initializedCount: The count of initialized elements in the deque,
  ///         which begins as zero. Set `initializedCount` to the number of
  ///         elements you initialize.
  init(
    unsafeUninitializedCapacity capacity: Int,
    initializingWith initializer:
      (inout UnsafeMutableBufferPointer<Element>, inout Int) throws -> Void
  ) rethrows {
    self._storage = .init(minimumCapacity: capacity)
    try _storage.update { handle in
      handle.startSlot = .zero
      var count = 0
      var buffer = handle.mutableBuffer(for: .zero ..< _Slot(at: capacity))
      defer {
        precondition(count <= capacity,
          "Initialized count set to greater than specified capacity")
        let b = handle.mutableBuffer(for: .zero ..< _Slot(at: capacity))
        precondition(buffer.baseAddress == b.baseAddress && buffer.count == b.count,
          "Initializer relocated Deque storage")
        handle.count = count
      }
      try initializer(&buffer, &count)
    }
  }
}

extension _Deque {
  /// Removes and returns the first element of this deque, if it exists.
  ///
  /// - Returns: The first element of the original collection if the collection
  ///    isn't empty; otherwise, `nil`.
  ///
  /// - Complexity: O(1) when this instance has a unique reference to its
  ///    underlying storage; O(`count`) otherwise.
  mutating func popFirst() -> Element? {
    // FIXME: Add this to the stdlib on BidirectionalCollection
    // where Self == Self.SubSequence
    guard count > 0 else { return nil }
    _storage.ensureUnique()
    return _storage.update {
      $0.uncheckedRemoveFirst()
    }
  }

  // Note: `popLast` is implemented by the stdlib as a
  // `RangeReplaceableCollection` extension, defined in terms of
  // `_customRemoveLast`.


  /// Adds a new element at the front of the deque.
  ///
  /// Use this method to append a single element to the front of a deque.
  ///
  ///     var numbers: Deque = [1, 2, 3, 4, 5]
  ///     numbers.prepend(100)
  ///     print(numbers)
  ///     // Prints "[100, 1, 2, 3, 4, 5]"
  ///
  /// Because deques increase their allocated capacity using an exponential
  /// strategy, prepending a single element to a deque is an O(1) operation when
  /// averaged over many calls to the `prepend(_:)` method. When a deque has
  /// additional capacity and is not sharing its storage with another instance,
  /// prepending an element is O(1). When a deque needs to reallocate storage
  /// before prepending or its storage is shared with another copy, prepending
  /// is O(`count`).
  ///
  /// - Parameter newElement: The element to prepend to the deque.
  ///
  /// - Complexity: Amortized O(1).
  ///
  /// - SeeAlso: `append(_:)`
  mutating func prepend(_ newElement: Element) {
    _storage.ensureUnique(minimumCapacity: count + 1)
    return _storage.update {
      $0.uncheckedPrepend(newElement)
    }
  }

  /// Adds the elements of a collection to the front of the deque.
  ///
  /// Use this method to prepend the elements of a collection to the front of
  /// this deque. This example prepends the elements of a `Range<Int>` instance
  /// to a deque of integers.
  ///
  ///     var numbers: Deque = [1, 2, 3, 4, 5]
  ///     numbers.prepend(contentsOf: 10...15)
  ///     print(numbers)
  ///     // Prints "[10, 11, 12, 13, 14, 15, 1, 2, 3, 4, 5]"
  ///
  /// - Parameter newElements: The elements to prepend to the deque.
  ///
  /// - Complexity: Amortized O(`newElements.count`).
  ///
  /// - SeeAlso: `append(contentsOf:)`
  mutating func prepend<C: Collection>(contentsOf newElements: C) where C.Element == Element {
    let done: Void? = newElements._withContiguousStorageIfAvailable_SR14663 { source in
      _storage.ensureUnique(minimumCapacity: count + source.count)
      _storage.update { $0.uncheckedPrepend(contentsOf: source) }
    }
    guard done == nil else { return }

    let c = newElements.count
    guard c > 0 else { return }
    _storage.ensureUnique(minimumCapacity: count + c)
    _storage.update { target in
      let gaps = target.availableSegments().suffix(c)
      gaps.initialize(from: newElements)
      target.count += c
      target.startSlot = target.slot(target.startSlot, offsetBy: -c)
    }
  }

  /// Adds the elements of a sequence to the front of the deque.
  ///
  /// Use this method to prepend the elements of a sequence to the front of this
  /// deque. This example prepends the elements of a `Range<Int>` instance to a
  /// deque of integers.
  ///
  ///     var numbers: Deque = [1, 2, 3, 4, 5]
  ///     numbers.prepend(contentsOf: 10...15)
  ///     print(numbers)
  ///     // Prints "[10, 11, 12, 13, 14, 15, 1, 2, 3, 4, 5]"
  ///
  /// - Parameter newElements: The elements to prepend to the deque.
  ///
  /// - Complexity: Amortized O(`newElements.count`).
  ///
  /// - SeeAlso: `append(contentsOf:)`
  mutating func prepend<S: Sequence>(contentsOf newElements: S) where S.Element == Element {
    let done: Void? = newElements._withContiguousStorageIfAvailable_SR14663 { source in
      _storage.ensureUnique(minimumCapacity: count + source.count)
      _storage.update { $0.uncheckedPrepend(contentsOf: source) }
    }
    guard done == nil else { return }

    let originalCount = self.count
    self.append(contentsOf: newElements)
    let newCount = self.count
    let c = newCount - originalCount
    _storage.update { target in
      target.startSlot = target.slot(forOffset: originalCount)
      target.count = target.capacity
      target.closeGap(offsets: c ..< c + (target.capacity - newCount))
      assert(target.count == newCount)
    }
  }
}
