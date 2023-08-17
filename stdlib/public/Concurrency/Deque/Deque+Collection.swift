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

extension _Deque: Sequence {
  // Implementation note: we could also use the default `IndexingIterator` here.
  // This custom implementation performs direct storage access to eliminate any
  // and all index validation overhead. It also optimizes away repeated
  // conversions from indices to storage slots.

  /// An iterator over the members of a deque.
  struct Iterator: IteratorProtocol {
    internal var _storage: _Deque._Storage

    internal var _nextSlot: _Slot

    internal var _endSlot: _Slot

    internal init(_storage: _Deque._Storage, start: _Slot, end: _Slot) {
      self._storage = _storage
      self._nextSlot = start
      self._endSlot = end
    }

    internal init(_base: _Deque) {
      self = _base._storage.read { handle in
        let start = handle.startSlot
        let end = Swift.min(start.advanced(by: handle.count), handle.limSlot)
        return Self(_storage: _base._storage, start: start, end: end)
      }
    }

    internal init(_base: _Deque, from index: Int) {
      self = _base._storage.read { handle in
        assert(index >= 0 && index <= handle.count)
        let start = handle.slot(forOffset: index)
        if index == handle.count {
          return Self(_storage: _base._storage, start: start, end: start)
        }
        var end = handle.endSlot
        if start >= end { end = handle.limSlot }
        return Self(_storage: _base._storage, start: start, end: end)
      }
    }

    internal mutating func _swapSegment() -> Bool {
      assert(_nextSlot == _endSlot)
      return _storage.read { handle in
        let end = handle.endSlot
        if end == .zero || end == _nextSlot {
          return false
        }
        _endSlot = end
        _nextSlot = .zero
        return true
      }
    }

    /// Advances to the next element and returns it, or `nil` if no next element
    /// exists.
    ///
    /// Once `nil` has been returned, all subsequent calls return `nil`.
    mutating func next() -> Element? {
      if _nextSlot == _endSlot {
        guard _swapSegment() else { return nil }
      }
      assert(_nextSlot < _endSlot)
      let slot = _nextSlot
      _nextSlot = _nextSlot.advanced(by: 1)
      return _storage.read { handle in
        return handle.ptr(at: slot).pointee
      }
    }
  }

  /// Returns an iterator over the elements of the deque.
  ///
  /// - Complexity: O(1)
  func makeIterator() -> Iterator {
    Iterator(_base: self)
  }

  __consuming func _copyToContiguousArray() -> ContiguousArray<Element> {
    ContiguousArray(unsafeUninitializedCapacity: _storage.count) { target, count in
      _storage.read { source in
        let segments = source.segments()
        let c = segments.first.count
        target[..<c]._rebased()._initialize(from: segments.first)
        count += segments.first.count
        if let second = segments.second {
          target[c ..< c + second.count]._rebased()._initialize(from: second)
          count += second.count
        }
        assert(count == source.count)
      }
    }
  }

  __consuming func _copyContents(
    initializing target: UnsafeMutableBufferPointer<Element>
  ) -> (Iterator, UnsafeMutableBufferPointer<Element>.Index) {
    _storage.read { source in
      let segments = source.segments()
      let c1 = Swift.min(segments.first.count, target.count)
      target[..<c1]._rebased()._initialize(from: segments.first.prefix(c1)._rebased())
      guard target.count > c1, let second = segments.second else {
        return (Iterator(_base: self, from: c1), c1)
      }
      let c2 = Swift.min(second.count, target.count - c1)
      target[c1 ..< c1 + c2]._rebased()._initialize(from: second.prefix(c2)._rebased())
      return (Iterator(_base: self, from: c1 + c2), c1 + c2)
    }
  }

  /// Call `body(b)`, where `b` is an unsafe buffer pointer to the deque's
  /// contiguous storage, if available. If the deque's contents aren't stored
  /// contiguously, `body` is not called and `nil` is returned. The supplied
  /// buffer pointer is only valid for the duration of the call.
  ///
  /// Often, the optimizer can eliminate bounds- and uniqueness-checks within an
  /// algorithm, but when that fails, invoking the same algorithm on the unsafe
  /// buffer supplied to `body` lets you trade safety for speed.
  ///
  /// - Parameters:
  ///   - body: The function to invoke.
  ///
  /// - Returns: The value returned by `body`, or `nil` if `body` wasn't called.
  ///
  /// - Complexity: O(1) when this instance has a unique reference to its
  ///    underlying storage; O(`count`) otherwise.
  func withContiguousStorageIfAvailable<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    return try _storage.read { handle in
      let endSlot = handle.startSlot.advanced(by: handle.count)
      guard endSlot.position <= handle.capacity else { return nil }
      return try body(handle.buffer(for: handle.startSlot ..< endSlot))
    }
  }
}

extension _Deque: RandomAccessCollection {
  typealias Index = Int
  typealias SubSequence = Slice<Self>
  typealias Indices = Range<Int>

  /// The number of elements in the deque.
  ///
  /// - Complexity: O(1)
  var count: Int { _storage.count }

  /// The position of the first element in a nonempty deque.
  ///
  /// For an instance of `Deque`, `startIndex` is always zero. If the deque is
  /// empty, `startIndex` is equal to `endIndex`.
  ///
  /// - Complexity: O(1)
  var startIndex: Int { 0 }

  /// The deque’s “past the end” position—that is, the position one greater than
  /// the last valid subscript argument.
  ///
  /// For an instance of `Deque`, `endIndex` is always equal to its `count`. If
  /// the deque is empty, `endIndex` is equal to `startIndex`.
  ///
  /// - Complexity: O(1)
  var endIndex: Int { count }

  /// The indices that are valid for subscripting this deque, in ascending order.
  ///
  /// - Complexity: O(1)
  var indices: Range<Int> { 0 ..< count }

  /// Returns the position immediately after the given index.
  ///
  /// - Parameter `i`: A valid index of the deque. `i` must be less than
  ///    `endIndex`.
  ///
  /// - Returns: The next valid index immediately after `i`.
  ///
  /// - Complexity: O(1)
  func index(after i: Int) -> Int {
    // Note: Like `Array`, index manipulation methods on deques don't trap on
    // invalid indices. (Indices are still validated on element access.)
    return i + 1
  }

  /// Replaces the given index with its successor.
  ///
  /// - Parameter `i`: A valid index of the deque. `i` must be less than
  ///    `endIndex`.
  ///
  /// - Complexity: O(1)
  func formIndex(after i: inout Int) {
    // Note: Like `Array`, index manipulation methods on deques
    // don't trap on invalid indices.
    // (Indices are still validated on element access.)
    i += 1
  }

  /// Returns the position immediately before the given index.
  ///
  /// - Parameter `i`: A valid index of the deque. `i` must be greater than
  ///    `startIndex`.
  ///
  /// - Returns: The preceding valid index immediately before `i`.
  ///
  /// - Complexity: O(1)
  func index(before i: Int) -> Int {
    // Note: Like `Array`, index manipulation methods on deques don't trap on
    // invalid indices. (Indices are still validated on element access.)
    return i - 1
  }

  /// Replaces the given index with its predecessor.
  ///
  /// - Parameter `i`: A valid index of the deque. `i` must be greater than `startIndex`.
  ///
  /// - Complexity: O(1)
  func formIndex(before i: inout Int) {
    // Note: Like `Array`, index manipulation methods on deques don't trap on
    // invalid indices. (Indices are still validated on element access.)
    i -= 1
  }

  /// Returns an index that is the specified distance from the given index.
  ///
  /// The value passed as `distance` must not offset `i` beyond the bounds of
  /// the collection.
  ///
  /// - Parameters:
  ///   - i: A valid index of the deque.
  ///   - `distance`: The distance by which to offset `i`.
  ///
  /// - Returns: An index offset by `distance` from the index `i`. If `distance`
  ///    is positive, this is the same value as the result of `distance` calls
  ///    to `index(after:)`. If `distance` is negative, this is the same value
  ///    as the result of `abs(distance)` calls to `index(before:)`.
  ///
  /// - Complexity: O(1)
  func index(_ i: Int, offsetBy distance: Int) -> Int {
    // Note: Like `Array`, index manipulation methods on deques don't trap on
    // invalid indices. (Indices are still validated on element access.)
    return i + distance
  }

  /// Returns an index that is the specified distance from the given index,
  /// unless that distance is beyond a given limiting index.
  ///
  /// - Parameters:
  ///   - i: A valid index of the array.
  ///   - distance: The distance to offset `i`.
  ///   - limit: A valid index of the deque to use as a limit.
  ///      If `distance > 0`, then `limit` has no effect it is less than `i`.
  ///      Likewise, if `distance < 0`, then `limit` has no effect if it is
  ///      greater than `i`.
  ///
  /// - Returns: An index offset by `distance` from the index `i`, unless that
  ///    index would be beyond `limit` in the direction of movement. In that
  ///    case, the method returns `nil`.
  ///
  /// - Complexity: O(1)
  func index(
    _ i: Int,
    offsetBy distance: Int,
    limitedBy limit: Int
  ) -> Int? {
    // Note: Like `Array`, index manipulation methods on deques
    // don't trap on invalid indices.
    // (Indices are still validated on element access.)
    let l = limit - i
    if distance > 0 ? l >= 0 && l < distance : l <= 0 && distance < l {
      return nil
    }
    return i + distance
  }


  /// Returns the distance between two indices.
  ///
  /// - Parameters:
  ///   - start: A valid index of the collection.
  ///   - end: Another valid index of the collection.
  ///
  /// - Returns: The distance between `start` and `end`. If `end` is equal to
  ///    `start`, the result is zero. Otherwise the result is positive if `end`
  ///    is greater than `start`.
  ///
  /// - Complexity: O(1)
  func distance(from start: Int, to end: Int) -> Int {
    // Note: Like `Array`, index manipulation method on deques
    // don't trap on invalid indices.
    // (Indices are still validated on element access.)
    return end - start
  }

  /// Accesses the element at the specified position.
  ///
  /// - Parameters:
  ///   - index: The position of the element to access. `index` must be greater
  ///      than or equal to `startIndex` and less than `endIndex`.
  ///
  /// - Complexity: Reading an element from a deque is O(1). Writing is O(1)
  ///    unless the deque’s storage is shared with another deque, in which case
  ///    writing is O(`count`).
  subscript(index: Int) -> Element {
    get {
      precondition(index >= 0 && index < count, "Index out of bounds")
      return _storage.read { $0.ptr(at: $0.slot(forOffset: index)).pointee }
    }
    set {
      precondition(index >= 0 && index < count, "Index out of bounds")
      _storage.ensureUnique()
      _storage.update { handle in
        let slot = handle.slot(forOffset: index)
        handle.ptr(at: slot).pointee = newValue
      }
    }
    _modify {
      precondition(index >= 0 && index < count, "Index out of bounds")
      _storage.ensureUnique()
      // We technically aren't supposed to escape storage pointers out of a
      // managed buffer, so we escape a `(slot, value)` pair instead, leaving
      // the corresponding slot temporarily uninitialized.
      var (slot, value) = _storage.update { handle -> (_Slot, Element) in
        let slot = handle.slot(forOffset: index)
        return (slot, handle.ptr(at: slot).move())
      }
      defer {
        _storage.update { handle in
          handle.ptr(at: slot).initialize(to: value)
        }
      }
      yield &value
    }
  }

  /// Accesses a contiguous subrange of the deque's elements.
  ///
  /// - Parameters:
  ///   - bounds: A range of the deque's indices. The bounds of the range must
  ///      be valid indices of the deque (including the `endIndex`).
  ///
  /// The accessed slice uses the same indices for the same elements as the
  /// original collection.
  subscript(bounds: Range<Int>) -> Slice<Self> {
    get {
      precondition(bounds.lowerBound >= 0 && bounds.upperBound <= count,
                   "Invalid bounds")
      return Slice(base: self, bounds: bounds)
    }
    set(source) {
      precondition(bounds.lowerBound >= 0 && bounds.upperBound <= count,
                   "Invalid bounds")
      self.replaceSubrange(bounds, with: source)
    }
  }
}

extension _Deque: MutableCollection {
  /// Exchanges the values at the specified indices of the collection.
  ///
  /// Both parameters must be valid indices of the collection and not equal to
  /// `endIndex`. Passing the same index as both `i` and `j` has no effect.
  ///
  /// - Parameters:
  ///   - i: The index of the first value to swap.
  ///   - j: The index of the second value to swap.
  ///
  /// - Complexity: O(1) when this instance has a unique reference to its
  ///    underlying storage; O(`count`) otherwise.
  mutating func swapAt(_ i: Int, _ j: Int) {
    precondition(i >= 0 && i < count, "Index out of bounds")
    precondition(j >= 0 && j < count, "Index out of bounds")
    _storage.ensureUnique()
    _storage.update { handle in
      let slot1 = handle.slot(forOffset: i)
      let slot2 = handle.slot(forOffset: j)
      handle.mutableBuffer.swapAt(slot1.position, slot2.position)
    }
  }

  // FIXME: Implement `partition(by:)` by making storage contiguous,
  // and partitioning that.

  /// Call `body(b)`, where `b` is an unsafe buffer pointer to the deque's
  /// mutable contiguous storage. If the deque's contents aren't stored
  /// contiguously, `body` is not called and `nil` is returned. The supplied
  /// buffer pointer is only valid for the duration of the call.
  ///
  /// Often, the optimizer can eliminate bounds- and uniqueness-checks within an
  /// algorithm, but when that fails, invoking the same algorithm on the unsafe
  /// buffer supplied to `body` lets you trade safety for speed.
  ///
  /// - Parameters:
  ///   - body: The function to invoke.
  ///
  /// - Returns: The value returned by `body`, or `nil` if `body` wasn't called.
  ///
  /// - Complexity: O(1) when this instance has a unique reference to its
  ///    underlying storage; O(`count`) otherwise. (Not counting the call to
  ///    `body`.)
  mutating func withContiguousMutableStorageIfAvailable<R>(
    _ body: (inout UnsafeMutableBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    _storage.ensureUnique()
    return try _storage.update { handle in
      let endSlot = handle.startSlot.advanced(by: handle.count)
      guard endSlot.position <= handle.capacity else {
        // FIXME: Rotate storage such that it becomes contiguous.
        return nil
      }
      let original = handle.mutableBuffer(for: handle.startSlot ..< endSlot)
      var extract = original
      defer {
        precondition(extract.baseAddress == original.baseAddress && extract.count == original.count,
                     "Closure must not replace the provided buffer")
      }
      return try body(&extract)
    }
  }

  mutating func _withUnsafeMutableBufferPointerIfSupported<R>(
    _ body: (inout UnsafeMutableBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    return try withContiguousMutableStorageIfAvailable(body)
  }
}

extension _Deque: RangeReplaceableCollection {
  /// Creates a new, empty deque.
  ///
  /// This is equivalent to initializing with an empty array literal.
  /// For example:
  ///
  ///     let deque1 = Deque<Int>()
  ///     print(deque1.isEmpty) // true
  ///
  ///     let deque2: Deque<Int> = []
  ///     print(deque2.isEmpty) // true
  ///
  /// - Complexity: O(1)
  init() {
    _storage = _Storage()
  }

  /// Reserves enough space to store the specified number of elements.
  ///
  /// If you are adding a known number of elements to a deque, use this method
  /// to avoid multiple reallocations. It ensures that the deque has unique
  /// storage, with space allocated for at least the requested number of
  /// elements.
  ///
  /// - Parameters:
  ///   - minimumCapacity: The requested number of elements to store.
  ///
  /// - Complexity: O(`count`)
  mutating func reserveCapacity(_ minimumCapacity: Int) {
    _storage.ensureUnique(minimumCapacity: minimumCapacity, linearGrowth: true)
  }

  /// Replaces a range of elements with the elements in the specified
  /// collection.
  ///
  /// This method has the effect of removing the specified range of elements
  /// from the deque and inserting the new elements at the same location. The
  /// number of new elements need not match the number of elements being
  /// removed.
  ///
  /// - Parameters:
  ///   - subrange: The subrange of the deque to replace. The bounds of the
  ///      subrange must be valid indices of the deque (including the
  ///      `endIndex`).
  ///   - newElements: The new elements to add to the deque.
  ///
  /// - Complexity: O(`self.count + newElements.count`). If the operation needs
  ///    to change the size of the deque, it minimizes the number of existing
  ///    items that need to be moved by shifting elements either before or after
  ///    `subrange`.
  mutating func replaceSubrange<C: Collection>(
    _ subrange: Range<Int>,
    with newElements: __owned C
  ) where C.Element == Element {
    precondition(subrange.lowerBound >= 0 && subrange.upperBound <= count, "Index range out of bounds")
    let removalCount = subrange.count
    let insertionCount = newElements.count
    let deltaCount = insertionCount - removalCount
    _storage.ensureUnique(minimumCapacity: count + deltaCount)

    let replacementCount = Swift.min(removalCount, insertionCount)
    let targetCut = subrange.lowerBound + replacementCount
    let sourceCut = newElements.index(newElements.startIndex, offsetBy: replacementCount)

    _storage.update { target in
      target.uncheckedReplaceInPlace(
        inOffsets: subrange.lowerBound ..< targetCut,
        with: newElements[..<sourceCut])
      if deltaCount < 0 {
        let r = targetCut ..< subrange.upperBound
        assert(replacementCount + r.count == removalCount)
        target.uncheckedRemove(offsets: r)
      } else if deltaCount > 0 {
        target.uncheckedInsert(
          contentsOf: newElements[sourceCut...],
          count: deltaCount,
          atOffset: targetCut)
      }
    }
  }

  /// Creates a new deque containing the specified number of a single, repeated
  /// value.
  ///
  /// - Parameters:
  ///   - repeatedValue: The element to repeat.
  ///   - count: The number of times to repeat the element. `count` must be zero
  ///      or greater.
  ///
  /// - Complexity: O(`count`)
  init(repeating repeatedValue: Element, count: Int) {
    precondition(count >= 0)
    self.init(minimumCapacity: count)
    _storage.update { handle in
      assert(handle.startSlot == .zero)
      if count > 0 {
        handle.ptr(at: .zero).initialize(repeating: repeatedValue, count: count)
      }
      handle.count = count
    }
  }

  /// Creates a deque containing the elements of a sequence.
  ///
  /// - Parameters:
  ///   - elements: The sequence of elements to turn into a deque.
  ///
  /// - Complexity: O(*n*), where *n* is the number of elements in the sequence.
  init<S: Sequence>(_ elements: S) where S.Element == Element {
    self.init()
    self.append(contentsOf: elements)
  }

  /// Creates a deque containing the elements of a collection.
  ///
  /// - Parameters:
  ///   - elements: The collection of elements to turn into a deque.
  ///
  /// - Complexity: O(`elements.count`)
  init<C: Collection>(_ elements: C) where C.Element == Element {
    let c = elements.count
    guard c > 0 else { _storage = _Storage(); return }
    self._storage = _Storage(minimumCapacity: c)
    _storage.update { handle in
      assert(handle.startSlot == .zero)
      let target = handle.mutableBuffer(for: .zero ..< _Slot(at: c))
      let done: Void? = elements._withContiguousStorageIfAvailable_SR14663 { source in
        target._initialize(from: source)
      }
      if done == nil {
        target._initialize(from: elements)
      }
      handle.count = c
    }
  }

  /// Adds a new element at the end of the deque.
  ///
  /// Use this method to append a single element to the end of a deque.
  ///
  ///     var numbers: Deque = [1, 2, 3, 4, 5]
  ///     numbers.append(100)
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 4, 5, 100]"
  ///
  /// Because deques increase their allocated capacity using an exponential
  /// strategy, appending a single element to a deque is an O(1) operation when
  /// averaged over many calls to the `append(_:)` method. When a deque has
  /// additional capacity and is not sharing its storage with another instance,
  /// appending an element is O(1). When a deque needs to reallocate storage
  /// before prepending or its storage is shared with another copy, appending is
  /// O(`count`).
  ///
  /// - Parameters:
  ///   - newElement: The element to append to the deque.
  ///
  /// - Complexity: Amortized O(1)
  ///
  /// - SeeAlso: `prepend(_:)`
  mutating func append(_ newElement: Element) {
    _storage.ensureUnique(minimumCapacity: count + 1)
    _storage.update {
      $0.uncheckedAppend(newElement)
    }
  }

  /// Adds the elements of a sequence to the end of the deque.
  ///
  /// Use this method to append the elements of a sequence to the front of this
  /// deque. This example appends the elements of a `Range<Int>` instance to a
  /// deque of integers.
  ///
  ///     var numbers: Deque = [1, 2, 3, 4, 5]
  ///     numbers.append(contentsOf: 10...15)
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15]"
  ///
  /// - Parameter newElements: The elements to append to the deque.
  ///
  /// - Complexity: Amortized O(`newElements.count`).
  mutating func append<S: Sequence>(contentsOf newElements: S) where S.Element == Element {
    let done: Void? = newElements._withContiguousStorageIfAvailable_SR14663 { source in
      _storage.ensureUnique(minimumCapacity: count + source.count)
      _storage.update { $0.uncheckedAppend(contentsOf: source) }
    }
    if done != nil {
      return
    }

    let underestimatedCount = newElements.underestimatedCount
    reserveCapacity(count + underestimatedCount)
    var it: S.Iterator = _storage.update { target in
      let gaps = target.availableSegments()
      let (it, copied) = gaps.initialize(fromSequencePrefix: newElements)
      target.count += copied
      return it
    }
    while let next = it.next() {
      _storage.ensureUnique(minimumCapacity: count + 1)
      _storage.update { target in
        target.uncheckedAppend(next)
        let gaps = target.availableSegments()
        target.count += gaps.initialize(fromPrefixOf: &it)
      }
    }
  }

  /// Adds the elements of a collection to the end of the deque.
  ///
  /// Use this method to append the elements of a collection to the front of
  /// this deque. This example appends the elements of a `Range<Int>` instance
  /// to a deque of integers.
  ///
  ///     var numbers: Deque = [1, 2, 3, 4, 5]
  ///     numbers.append(contentsOf: 10...15)
  ///     print(numbers)
  ///     // Prints "[1, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15]"
  ///
  /// - Parameter newElements: The elements to append to the deque.
  ///
  /// - Complexity: Amortized O(`newElements.count`).
  mutating func append<C: Collection>(contentsOf newElements: C) where C.Element == Element {
    let done: Void? = newElements._withContiguousStorageIfAvailable_SR14663 { source in
      _storage.ensureUnique(minimumCapacity: count + source.count)
      _storage.update { $0.uncheckedAppend(contentsOf: source) }
    }
    guard done == nil else { return }

    let c = newElements.count
    guard c > 0 else { return }
    reserveCapacity(count + c)
    _storage.update { target in
      let gaps = target.availableSegments().prefix(c)
      gaps.initialize(from: newElements)
      target.count += c
    }
  }

  /// Inserts a new element at the specified position.
  ///
  /// The new element is inserted before the element currently at the specified
  /// index. If you pass the deque’s `endIndex` as the `index` parameter, the
  /// new element is appended to the deque.
  ///
  /// - Parameters:
  ///   - newElement: The new element to insert into the deque.
  ///   - index: The position at which to insert the new element. `index` must
  ///      be a valid index of the deque (including `endIndex`).
  ///
  /// - Complexity: O(`count`). The operation shifts existing elements either
  ///    towards the beginning or the end of the deque to minimize the number of
  ///    elements that need to be moved. When inserting at the start or the end,
  ///    this reduces the complexity to amortized O(1).
  mutating func insert(_ newElement: Element, at index: Int) {
    precondition(index >= 0 && index <= count,
                 "Can't insert element at invalid index")
    _storage.ensureUnique(minimumCapacity: count + 1)
    _storage.update { target in
      if index == 0 {
        target.uncheckedPrepend(newElement)
        return
      }
      if index == count {
        target.uncheckedAppend(newElement)
        return
      }
      let gap = target.openGap(ofSize: 1, atOffset: index)
      assert(gap.first.count == 1)
      gap.first.baseAddress!.initialize(to: newElement)
    }
  }

  /// Inserts the elements of a collection into the deque at the specified
  /// position.
  ///
  /// The new elements are inserted before the element currently at the
  /// specified index. If you pass the deque's `endIndex` property as the
  /// `index` parameter, the new elements are appended to the deque.
  ///
  /// - Parameters:
  ///   - newElements: The new elements to insert into the deque.
  ///   - index: The position at which to insert the new elements. `index` must
  ///      be a valid index of the deque (including `endIndex`).
  ///
  /// - Complexity: O(`count + newElements.count`). The operation shifts
  ///    existing elements either towards the beginning or the end of the deque
  ///    to minimize the number of elements that need to be moved. When
  ///    inserting at the start or the end, this reduces the complexity to
  ///    amortized O(1).
  mutating func insert<C: Collection>(
    contentsOf newElements: __owned C, at index: Int
  ) where C.Element == Element {
    precondition(index >= 0 && index <= count,
                 "Can't insert elements at an invalid index")
    let newCount = newElements.count
    _storage.ensureUnique(minimumCapacity: count + newCount)
    _storage.update { target in
      target.uncheckedInsert(contentsOf: newElements, count: newCount, atOffset: index)
    }
  }

  /// Removes and returns the element at the specified position.
  ///
  /// To close the resulting gap, all elements following the specified position
  /// are (logically) moved up by one index position. (Internally, the deque may
  /// actually decide to shift previous elements forward instead to minimize the
  /// number of elements that need to be moved.)
  ///
  /// - Parameters:
  ///   - index: The position of the element to remove. `index` must be a valid
  ///      index of the array.
  ///
  /// - Returns: The element originally at the specified index.
  ///
  /// - Complexity: O(`count`). Removing elements from the start or end of the
  ///    deque costs O(1) if the deque's storage isn't shared.
  @discardableResult
  mutating func remove(at index: Int) -> Element {
    precondition(index >= 0 && index < self.count, "Index out of bounds")
    // FIXME: Implement storage shrinking
    _storage.ensureUnique()
    return _storage.update { target in
      // FIXME: Add direct implementation & see if it makes a difference
      let result = self[index]
      target.uncheckedRemove(offsets: index ..< index + 1)
      return result
    }
  }

  /// Removes the elements in the specified subrange from the deque.

  /// All elements following the specified range are (logically) moved up to
  /// close the resulting gap. (Internally, the deque may actually decide to
  /// shift previous elements forward instead to minimize the number of elements
  /// that need to be moved.)
  ///
  /// - Parameters:
  ///   - bounds: The range of the collection to be removed. The bounds of the
  ///      range must be valid indices of the collection.
  ///
  /// - Complexity: O(`count`). Removing elements from the start or end of the
  ///    deque costs O(`bounds.count`) if the deque's storage isn't shared.
  mutating func removeSubrange(_ bounds: Range<Int>) {
    precondition(bounds.lowerBound >= 0 && bounds.upperBound <= self.count,
                 "Index range out of bounds")
    _storage.ensureUnique()
    _storage.update { $0.uncheckedRemove(offsets: bounds) }
  }

  mutating func _customRemoveLast() -> Element? {
    precondition(!isEmpty, "Cannot remove last element of an empty Deque")
    _storage.ensureUnique()
    return _storage.update { $0.uncheckedRemoveLast() }
  }

  mutating func _customRemoveLast(_ n: Int) -> Bool {
    precondition(n >= 0, "Can't remove a negative number of elements")
    precondition(n <= count, "Can't remove more elements than there are in the Collection")
    _storage.ensureUnique()
    _storage.update { $0.uncheckedRemoveLast(n) }
    return true
  }

  /// Removes and returns the first element of the deque.
  ///
  /// The collection must not be empty.
  ///
  /// - Returns: The removed element.
  ///
  /// - Complexity: O(1) if the underlying storage isn't shared; otherwise
  ///    O(`count`).
  @discardableResult
  mutating func removeFirst() -> Element {
    precondition(!isEmpty, "Cannot remove first element of an empty Deque")
    _storage.ensureUnique()
    return _storage.update { $0.uncheckedRemoveFirst() }
  }

  /// Removes the specified number of elements from the beginning of the deque.
  ///
  /// - Parameter n: The number of elements to remove from the deque. `n` must
  ///    be greater than or equal to zero and must not exceed the number of
  ///    elements in the deque.
  ///
  /// - Complexity: O(`n`) if the underlying storage isn't shared; otherwise
  ///    O(`count`).
  mutating func removeFirst(_ n: Int) {
    precondition(n >= 0, "Can't remove a negative number of elements")
    precondition(n <= count, "Can't remove more elements than there are in the Collection")
    _storage.ensureUnique()
    return _storage.update { $0.uncheckedRemoveFirst(n) }
  }

  /// Removes all elements from the deque.
  ///
  /// - Parameter keepCapacity: Pass true to keep the existing storage capacity
  ///    of the deque after removing its elements. The default value is false.
  ///
  /// - Complexity: O(`count`)
  mutating func removeAll(keepingCapacity keepCapacity: Bool = false) {
    if keepCapacity {
      _storage.ensureUnique()
      _storage.update { $0.uncheckedRemoveAll() }
    } else {
      self = _Deque()
    }
  }
}
