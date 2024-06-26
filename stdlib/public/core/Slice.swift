//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// A view into a subsequence of elements of another collection.
///
/// A slice stores a base collection and the start and end indices of the view.
/// It does not copy the elements from the collection into separate storage.
/// Thus, creating a slice has O(1) complexity.
///
/// Slices Share Indices
/// --------------------
///
/// Indices of a slice can be used interchangeably with indices of the base
/// collection. An element of a slice is located under the same index in the
/// slice and in the base collection, as long as neither the collection nor
/// the slice has been mutated since the slice was created.
///
/// For example, suppose you have an array holding the number of absences from
/// each class during a session.
///
///     var absences = [0, 2, 0, 4, 0, 3, 1, 0]
///
/// You're tasked with finding the day with the most absences in the second
/// half of the session. To find the index of the day in question, follow
/// these steps:
///
/// 1) Create a slice of the `absences` array that holds the second half of the
///    days.
/// 2) Use the `max(by:)` method to determine the index of the day with the
///    most absences.
/// 3) Print the result using the index found in step 2 on the original
///    `absences` array.
///
/// Here's an implementation of those steps:
///
///     let secondHalf = absences.suffix(absences.count / 2)
///     if let i = secondHalf.indices.max(by: { secondHalf[$0] < secondHalf[$1] }) {
///         print("Highest second-half absences: \(absences[i])")
///     }
///     // Prints "Highest second-half absences: 3"
///
/// Slices Inherit Semantics
/// ------------------------
///
/// A slice inherits the value or reference semantics of its base collection.
/// That is, if a `Slice` instance is wrapped around a mutable collection that
/// has value semantics, such as an array, mutating the original collection
/// would trigger a copy of that collection, and not affect the base
/// collection stored inside of the slice.
///
/// For example, if you update the last element of the `absences` array from
/// `0` to `2`, the `secondHalf` slice is unchanged.
///
///     absences[7] = 2
///     print(absences)
///     // Prints "[0, 2, 0, 4, 0, 3, 1, 2]"
///     print(secondHalf)
///     // Prints "[0, 3, 1, 0]"
///
/// Use slices only for transient computation. A slice may hold a reference to
/// the entire storage of a larger collection, not just to the portion it
/// presents, even after the base collection's lifetime ends. Long-term
/// storage of a slice may therefore prolong the lifetime of elements that are
/// no longer otherwise accessible, which can erroneously appear to be memory
/// leakage.
///
/// - Note: Using a `Slice` instance with a mutable collection requires that
///   the base collection's `subscript(_: Index)` setter does not invalidate
///   indices. If mutations need to invalidate indices in your custom
///   collection type, don't use `Slice` as its subsequence type. Instead,
///   define your own subsequence type that takes your index invalidation
///   requirements into account.
@frozen // generic-performance
public struct Slice<Base: Collection> {
  public var _startIndex: Base.Index
  public var _endIndex: Base.Index

  @usableFromInline // generic-performance
  internal var _base: Base

  /// Creates a view into the given collection that allows access to elements
  /// within the specified range.
  ///
  /// It is unusual to need to call this method directly. Instead, create a
  /// slice of a collection by using the collection's range-based subscript or
  /// by using methods that return a subsequence.
  ///
  ///     let singleDigits = 0...9
  ///     let subSequence = singleDigits.dropFirst(5)
  ///     print(Array(subSequence))
  ///     // Prints "[5, 6, 7, 8, 9]"
  ///
  /// In this example, the expression `singleDigits.dropFirst(5))` is
  /// equivalent to calling this initializer with `singleDigits` and a
  /// range covering the last five items of `singleDigits.indices`.
  ///
  /// - Parameters:
  ///   - base: The collection to create a view into.
  ///   - bounds: The range of indices to allow access to in the new slice.
  @inlinable // generic-performance
  public init(base: Base, bounds: Range<Base.Index>) {
    self._base = base
    self._startIndex = bounds.lowerBound
    self._endIndex = bounds.upperBound
  }

  /// The underlying collection of the slice.
  ///
  /// You can use a slice's `base` property to access its base collection. The
  /// following example declares `singleDigits`, a range of single digit
  /// integers, and then drops the first element to create a slice of that
  /// range, `singleNonZeroDigits`. The `base` property of the slice is equal
  /// to `singleDigits`.
  ///
  ///     let singleDigits = 0..<10
  ///     let singleNonZeroDigits = singleDigits.dropFirst()
  ///     // singleNonZeroDigits is a Slice<Range<Int>>
  ///
  ///     print(singleNonZeroDigits.count)
  ///     // Prints "9"
  ///     print(singleNonZeroDigits.base.count)
  ///     // Prints "10"
  ///     print(singleDigits == singleNonZeroDigits.base)
  ///     // Prints "true"
  @inlinable // generic-performance
  public var base: Base {
    return _base
  }

  @_alwaysEmitIntoClient @inline(__always)
  internal var _bounds: Range<Base.Index> {
    Range(_uncheckedBounds: (_startIndex, _endIndex))
  }
}

extension Slice: Collection {
  public typealias Index = Base.Index
  public typealias Indices = Base.Indices
  public typealias Element = Base.Element
  public typealias SubSequence = Slice<Base>
  public typealias Iterator = IndexingIterator<Slice<Base>>

  @inlinable // generic-performance
  public var startIndex: Index {
    return _startIndex
  }

  @inlinable // generic-performance
  public var endIndex: Index {
    return _endIndex
  }

  @inlinable // generic-performance
  public subscript(index: Index) -> Base.Element {
    get {
      _failEarlyRangeCheck(index, bounds: _bounds)
      return _base[index]
    }
  }

  @inlinable // generic-performance
  public subscript(bounds: Range<Index>) -> Slice<Base> {
    get {
      _failEarlyRangeCheck(bounds, bounds: _bounds)
      // Allow the underlying collection to have a say about what indices can be
      // slice boundaries. This is crucial for cases where the same index type
      // is used in multiple collection types of varying granularity (similar to
      // `String` views) -- in these cases, the bounds of the given range may
      // happen to fall in between valid indices in the base collection, which
      // `Slice` isn't prepared to handle. Letting the base collection do the
      // actual slicing allows it to properly validate indices, and to
      // optionally handle unaligned slices by rounding their boundaries down to
      // the nearest valid index.
      let slice = _base[bounds]
      return Slice(base: _base, bounds: slice.startIndex ..< slice.endIndex)
    }
  }

  public var indices: Indices {
    return _base.indices[_bounds]
  }

  @inlinable // generic-performance
  public func index(after i: Index) -> Index {
    // FIXME: swift-3-indexing-model: range check.
    return _base.index(after: i)
  }

  @inlinable // generic-performance
  public func formIndex(after i: inout Index) {
    // FIXME: swift-3-indexing-model: range check.
    _base.formIndex(after: &i)
  }

  @inlinable // generic-performance
  public func index(_ i: Index, offsetBy n: Int) -> Index {
    // FIXME: swift-3-indexing-model: range check.
    return _base.index(i, offsetBy: n)
  }

  @inlinable // generic-performance
  public func index(
    _ i: Index, offsetBy n: Int, limitedBy limit: Index
  ) -> Index? {
    // FIXME: swift-3-indexing-model: range check.
    return _base.index(i, offsetBy: n, limitedBy: limit)
  }

  @inlinable // generic-performance
  public func distance(from start: Index, to end: Index) -> Int {
    // FIXME: swift-3-indexing-model: range check.
    return _base.distance(from: start, to: end)
  }

  @inlinable // generic-performance
  public func _failEarlyRangeCheck(_ index: Index, bounds: Range<Index>) {
    _base._failEarlyRangeCheck(index, bounds: bounds)
  }

  @inlinable // generic-performance
  public func _failEarlyRangeCheck(_ range: Range<Index>, bounds: Range<Index>) {
    _base._failEarlyRangeCheck(range, bounds: bounds)
  }

  @_alwaysEmitIntoClient @inlinable
  public func withContiguousStorageIfAvailable<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    try _base.withContiguousStorageIfAvailable { buffer in
      let start = _base.distance(from: _base.startIndex, to: _startIndex)
      let count = _base.distance(from: _startIndex, to: _endIndex)
      let slice = UnsafeBufferPointer(rebasing: buffer[start ..< start + count])
      return try body(slice)
    }
  }
}

extension Slice {
  @_alwaysEmitIntoClient
  public __consuming func _copyContents(
      initializing buffer: UnsafeMutableBufferPointer<Element>
  ) -> (Iterator, UnsafeMutableBufferPointer<Element>.Index) {
    if let (_, copied) = self.withContiguousStorageIfAvailable({
      $0._copyContents(initializing: buffer)
    }) {
      let position = index(startIndex, offsetBy: copied)
      return (Iterator(_elements: self, _position: position), copied)
    }

    return _copySequenceContents(initializing: buffer)
  }
}

extension Slice: BidirectionalCollection where Base: BidirectionalCollection {
  @inlinable // generic-performance
  public func index(before i: Index) -> Index {
    // FIXME: swift-3-indexing-model: range check.
    return _base.index(before: i)
  }

  @inlinable // generic-performance
  public func formIndex(before i: inout Index) {
    // FIXME: swift-3-indexing-model: range check.
    _base.formIndex(before: &i)
  }
}


extension Slice: MutableCollection where Base: MutableCollection {
  @inlinable // generic-performance
  public subscript(index: Index) -> Base.Element {
    get {
      _failEarlyRangeCheck(index, bounds: _bounds)
      return _base[index]
    }
    set {
      _failEarlyRangeCheck(index, bounds: _bounds)
      _base[index] = newValue
      // MutableSlice requires that the underlying collection's subscript
      // setter does not invalidate indices, so our `startIndex` and `endIndex`
      // continue to be valid.
    }
  }

  @inlinable // generic-performance
  public subscript(bounds: Range<Index>) -> Slice<Base> {
    get {
      _failEarlyRangeCheck(bounds, bounds: _bounds)
      // Allow the underlying collection to have a say about what indices can be
      // slice boundaries. This is crucial for cases where the same index type
      // is used in multiple collection types of varying granularity (similar to
      // `String` views) -- in these cases, the bounds of the given range may
      // happen to fall in between valid indices in the base collection, which
      // `Slice` isn't prepared to handle. Letting the base collection do the
      // actual slicing allows it to properly validate indices, and to
      // optionally handle unaligned slices by rounding their boundaries down to
      // the nearest valid index.
      let slice = _base[bounds]
      return Slice(base: _base, bounds: slice.startIndex ..< slice.endIndex)
    }
    set {
      _writeBackMutableSlice(&self, bounds: bounds, slice: newValue)
    }
  }

  @_alwaysEmitIntoClient @inlinable
  public mutating func withContiguousMutableStorageIfAvailable<R>(
    _ body: (inout UnsafeMutableBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    // We're calling `withContiguousMutableStorageIfAvailable` twice here so
    // that we don't calculate index distances unless we know we'll use them.
    // The expectation here is that the base collection will make itself
    // contiguous on the first try and the second call will be relatively cheap.
    guard _base.withContiguousMutableStorageIfAvailable({ _ in }) != nil
    else {
      return nil
    }
    let start = _base.distance(from: _base.startIndex, to: _startIndex)
    let count = _base.distance(from: _startIndex, to: _endIndex)
    return try _base.withContiguousMutableStorageIfAvailable { buffer in
      var slice = UnsafeMutableBufferPointer(
        rebasing: buffer[start ..< start + count])
      let copy = slice
      defer {
        _precondition(
          slice.baseAddress == copy.baseAddress &&
          slice.count == copy.count,
          "Slice.withContiguousMutableStorageIfAvailable: replacing the buffer is not allowed")
      }
      return try body(&slice)
    }
  }
}


extension Slice: RandomAccessCollection where Base: RandomAccessCollection { }

extension Slice: RangeReplaceableCollection
  where Base: RangeReplaceableCollection {
  @inlinable // generic-performance
  public init() {
    self._base = Base()
    self._startIndex = _base.startIndex
    self._endIndex = _base.endIndex
  }

  @inlinable // generic-performance
  public init(repeating repeatedValue: Base.Element, count: Int) {
    self._base = Base(repeating: repeatedValue, count: count)
    self._startIndex = _base.startIndex
    self._endIndex = _base.endIndex
  }

  @inlinable // generic-performance
  public init<S>(_ elements: S) where S: Sequence, S.Element == Base.Element {
    self._base = Base(elements)
    self._startIndex = _base.startIndex
    self._endIndex = _base.endIndex
  }

  @inlinable // generic-performance
  public mutating func replaceSubrange<C>(
    _ subRange: Range<Index>, with newElements: C
  ) where C: Collection, C.Element == Base.Element {

    // FIXME: swift-3-indexing-model: range check.
    let sliceOffset =
      _base.distance(from: _base.startIndex, to: _startIndex)
    let newSliceCount =
      _base.distance(from: _startIndex, to: subRange.lowerBound)
      + _base.distance(from: subRange.upperBound, to: _endIndex)
      + newElements.count
    _base.replaceSubrange(subRange, with: newElements)
    _startIndex = _base.index(_base.startIndex, offsetBy: sliceOffset)
    _endIndex = _base.index(_startIndex, offsetBy: newSliceCount)
  }

  @inlinable // generic-performance
  public mutating func insert(_ newElement: Base.Element, at i: Index) {
    // FIXME: swift-3-indexing-model: range check.
    let sliceOffset = _base.distance(from: _base.startIndex, to: _startIndex)
    let newSliceCount = count + 1
    _base.insert(newElement, at: i)
    _startIndex = _base.index(_base.startIndex, offsetBy: sliceOffset)
    _endIndex = _base.index(_startIndex, offsetBy: newSliceCount)
  }

  @inlinable // generic-performance
  public mutating func insert<S>(contentsOf newElements: S, at i: Index)
  where S: Collection, S.Element == Base.Element {

    // FIXME: swift-3-indexing-model: range check.
    let sliceOffset = _base.distance(from: _base.startIndex, to: _startIndex)
    let newSliceCount = count + newElements.count
    _base.insert(contentsOf: newElements, at: i)
    _startIndex = _base.index(_base.startIndex, offsetBy: sliceOffset)
    _endIndex = _base.index(_startIndex, offsetBy: newSliceCount)
  }

  @inlinable // generic-performance
  public mutating func remove(at i: Index) -> Base.Element {
    // FIXME: swift-3-indexing-model: range check.
    let sliceOffset = _base.distance(from: _base.startIndex, to: _startIndex)
    let newSliceCount = count - 1
    let result = _base.remove(at: i)
    _startIndex = _base.index(_base.startIndex, offsetBy: sliceOffset)
    _endIndex = _base.index(_startIndex, offsetBy: newSliceCount)
    return result
  }

  @inlinable // generic-performance
  public mutating func removeSubrange(_ bounds: Range<Index>) {
    // FIXME: swift-3-indexing-model: range check.
    let sliceOffset = _base.distance(from: _base.startIndex, to: _startIndex)
    let newSliceCount =
      count - distance(from: bounds.lowerBound, to: bounds.upperBound)
    _base.removeSubrange(bounds)
    _startIndex = _base.index(_base.startIndex, offsetBy: sliceOffset)
    _endIndex = _base.index(_startIndex, offsetBy: newSliceCount)
  }
}

extension Slice
  where Base: RangeReplaceableCollection, Base: BidirectionalCollection {
  
  @inlinable // generic-performance
  public mutating func replaceSubrange<C>(
    _ subRange: Range<Index>, with newElements: C
  ) where C: Collection, C.Element == Base.Element {
    // FIXME: swift-3-indexing-model: range check.
    if subRange.lowerBound == _base.startIndex {
      let newSliceCount =
        _base.distance(from: _startIndex, to: subRange.lowerBound)
        + _base.distance(from: subRange.upperBound, to: _endIndex)
        + newElements.count
      _base.replaceSubrange(subRange, with: newElements)
      _startIndex = _base.startIndex
      _endIndex = _base.index(_startIndex, offsetBy: newSliceCount)
    } else {
      let shouldUpdateStartIndex = subRange.lowerBound == _startIndex
      let lastValidIndex = _base.index(before: subRange.lowerBound)
      let newEndIndexOffset =
        _base.distance(from: subRange.upperBound, to: _endIndex)
        + newElements.count + 1
      _base.replaceSubrange(subRange, with: newElements)
      if shouldUpdateStartIndex {
        _startIndex = _base.index(after: lastValidIndex)
      }
      _endIndex = _base.index(lastValidIndex, offsetBy: newEndIndexOffset)
    }
  }

  @inlinable // generic-performance
  public mutating func insert(_ newElement: Base.Element, at i: Index) {
    // FIXME: swift-3-indexing-model: range check.
    if i == _base.startIndex {
      let newSliceCount = count + 1
      _base.insert(newElement, at: i)
      _startIndex = _base.startIndex
      _endIndex = _base.index(_startIndex, offsetBy: newSliceCount)
    } else {
      let shouldUpdateStartIndex = i == _startIndex
      let lastValidIndex = _base.index(before: i)
      let newEndIndexOffset = _base.distance(from: i, to: _endIndex) + 2
      _base.insert(newElement, at: i)
      if shouldUpdateStartIndex {
        _startIndex = _base.index(after: lastValidIndex)
      }
      _endIndex = _base.index(lastValidIndex, offsetBy: newEndIndexOffset)
    }
  }

  @inlinable // generic-performance
  public mutating func insert<S>(contentsOf newElements: S, at i: Index)
  where S: Collection, S.Element == Base.Element {
    // FIXME: swift-3-indexing-model: range check.
    if i == _base.startIndex {
      let newSliceCount = count + newElements.count
      _base.insert(contentsOf: newElements, at: i)
      _startIndex = _base.startIndex
      _endIndex = _base.index(_startIndex, offsetBy: newSliceCount)
    } else {
      let shouldUpdateStartIndex = i == _startIndex
      let lastValidIndex = _base.index(before: i)
      let newEndIndexOffset =
        _base.distance(from: i, to: _endIndex)
        + newElements.count + 1
      _base.insert(contentsOf: newElements, at: i)
      if shouldUpdateStartIndex {
        _startIndex = _base.index(after: lastValidIndex)
      }
      _endIndex = _base.index(lastValidIndex, offsetBy: newEndIndexOffset)
    }
  }

  @inlinable // generic-performance
  public mutating func remove(at i: Index) -> Base.Element {
    // FIXME: swift-3-indexing-model: range check.
    if i == _base.startIndex {
      let newSliceCount = count - 1
      let result = _base.remove(at: i)
      _startIndex = _base.startIndex
      _endIndex = _base.index(_startIndex, offsetBy: newSliceCount)
      return result
    } else {
      let shouldUpdateStartIndex = i == _startIndex
      let lastValidIndex = _base.index(before: i)
      let newEndIndexOffset = _base.distance(from: i, to: _endIndex)
      let result = _base.remove(at: i)
      if shouldUpdateStartIndex {
        _startIndex = _base.index(after: lastValidIndex)
      }
      _endIndex = _base.index(lastValidIndex, offsetBy: newEndIndexOffset)
      return result
    }
  }

  @inlinable // generic-performance
  public mutating func removeSubrange(_ bounds: Range<Index>) {
    // FIXME: swift-3-indexing-model: range check.
    if bounds.lowerBound == _base.startIndex {
      let newSliceCount =
        count - _base.distance(from: bounds.lowerBound, to: bounds.upperBound)
      _base.removeSubrange(bounds)
      _startIndex = _base.startIndex
      _endIndex = _base.index(_startIndex, offsetBy: newSliceCount)
    } else {
      let shouldUpdateStartIndex = bounds.lowerBound == _startIndex
      let lastValidIndex = _base.index(before: bounds.lowerBound)
      let newEndIndexOffset =
          _base.distance(from: bounds.lowerBound, to: _endIndex)
        - _base.distance(from: bounds.lowerBound, to: bounds.upperBound)
        + 1
      _base.removeSubrange(bounds)
      if shouldUpdateStartIndex {
        _startIndex = _base.index(after: lastValidIndex)
      }
      _endIndex = _base.index(lastValidIndex, offsetBy: newEndIndexOffset)
    }
  }
}

extension Slice: Sendable
where Base: Sendable, Base.Index: Sendable { }
