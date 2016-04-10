//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// A type that provides subscript access to its elements, with forward
/// index traversal.
///
/// - Important: In most cases, it's best to ignore this protocol and use
///   `Collection` instead, as it has a more complete interface.
public protocol IndexableBase {
  // FIXME(ABI)(compiler limitation): there is no reason for this protocol
  // to exist apart from missing compiler features that we emulate with it.
  //
  // This protocol is almost an implementation detail of the standard
  // library; it is used to deduce things like the `SubSequence` and
  // `Iterator` type from a minimal collection, but it is also used in
  // exposed places like as a constraint on `IndexingIterator`.

  /// A type that represents a position in the collection.
  ///
  /// Valid indices consist of the position of every element and a
  /// "past the end" position that's not valid for use as a subscript
  /// argument.
  ///
  /// - SeeAlso: endIndex
  associatedtype Index : Comparable

  /// The position of the first element in a non-empty collection.
  ///
  /// In an empty collection, `startIndex == endIndex`.
  ///
  /// - Complexity: O(1)
  var startIndex: Index { get }

  /// The collection's "past the end" position.
  ///
  /// `endIndex` is not a valid argument to `subscript`, and is always
  /// reachable from `startIndex` by zero or more applications of
  /// `successor(of:)`.
  ///
  /// - Complexity: O(1)
  var endIndex: Index { get }

  // The declaration of _Element and subscript here is a trick used to
  // break a cyclic conformance/deduction that Swift can't handle.  We
  // need something other than a Collection.Iterator.Element that can
  // be used as IndexingIterator<T>'s Element.  Here we arrange for
  // the Collection itself to have an Element type that's deducible from
  // its subscript.  Ideally we'd like to constrain this Element to be the same
  // as Collection.Iterator.Element (see below), but we have no way of
  // expressing it today.
  associatedtype _Element

  /// Accesses the element at the given `position`.
  ///
  /// - Complexity: O(1)
  ///
  /// - Requires: `position` is valid for subscripting `self`.  That
  ///   is, it is reachable from `startIndex` by zero or more
  ///   applications of `successor(of:)` and is not equal to `endIndex`.
  subscript(position: Index) -> _Element { get }

  // WORKAROUND: rdar://25214066
  /// A `Sequence` that can represent a contiguous subrange of `self`'s
  /// elements.
  associatedtype SubSequence

  /// Accesses the subsequence bounded by `bounds`.
  ///
  /// - Complexity: O(1)
  ///
  /// - Precondition: `(startIndex...endIndex).contains(bounds.lowerBound)` 
  ///   and `(startIndex...endIndex).contains(bounds.upperBound)`
  subscript(bounds: Range<Index>) -> SubSequence { get }
  
  /// Performs a range check in O(1), or a no-op when a range check is not
  /// implementable in O(1).
  ///
  /// The range check, if performed, is equivalent to:
  ///
  ///     precondition(bounds.contains(index))
  ///
  /// Use this function to perform a cheap range check for QoI purposes when
  /// memory safety is not a concern.  Do not rely on this range check for
  /// memory safety.
  ///
  /// The default implementation for forward and bidirectional indices is a
  /// no-op.  The default implementation for random access indices performs a
  /// range check.
  ///
  /// - Complexity: O(1).
  func _failEarlyRangeCheck(index: Index, bounds: Range<Index>)

  /// Performs a range check in O(1), or a no-op when a range check is not
  /// implementable in O(1).
  ///
  /// The range check, if performed, is equivalent to:
  ///
  ///     precondition(
  ///       bounds.contains(range.lowerBound) ||
  ///       range.lowerBound == bounds.upperBound)
  ///     precondition(
  ///       bounds.contains(range.upperBound) ||
  ///       range.upperBound == bounds.upperBound)
  ///
  /// Use this function to perform a cheap range check for QoI purposes when
  /// memory safety is not a concern.  Do not rely on this range check for
  /// memory safety.
  ///
  /// The default implementation for forward and bidirectional indices is a
  /// no-op.  The default implementation for random access indices performs a
  /// range check.
  ///
  /// - Complexity: O(1).
  func _failEarlyRangeCheck(range: Range<Index>, bounds: Range<Index>)

  /// Returns the position immediately after `i`.
  ///
  /// - Precondition: `(startIndex..<endIndex).contains(i)`
  @warn_unused_result
  func successor(of i: Index) -> Index

  /// Replaces `i` with its successor.
  func formSuccessor(i: inout Index)
}

public protocol Indexable : IndexableBase {
  /// A type that can represent the number of steps between pairs of
  /// `Index` values where one value is reachable from the other.
  ///
  /// Reachability is defined by the ability to produce one value from
  /// the other via zero or more applications of `successor(of: i)`.
  associatedtype IndexDistance : SignedInteger = Int

  /// Returns the result of advancing `i` by `n` positions.
  ///
  /// - Returns:
  ///   - If `n > 0`, the `n`th successor of `i`.
  ///   - If `n < 0`, the `n`th predecessor of `i`.
  ///   - Otherwise, `i` unmodified.
  ///
  /// - Precondition: `n >= 0` unless `Self` conforms to
  ///   `BidirectionalCollection`.
  /// - Precondition:
  ///   - If `n > 0`, `n <= self.distance(from: i, to: self.endIndex)`
  ///   - If `n < 0`, `n >= self.distance(from: i, to: self.startIndex)`
  ///
  /// - Complexity:
  ///   - O(1) if `Self` conforms to `RandomAccessCollection`.
  ///   - O(`abs(n)`) otherwise.
  @warn_unused_result
  func index(n: IndexDistance, stepsFrom i: Index) -> Index

  /// Returns the result of advancing `i` by `n` positions, or until it
  /// equals `limit`.
  ///
  /// - Returns:
  ///   - If `n > 0`, the `n`th successor of `i` or `limit`, whichever
  ///     is reached first.
  ///   - If `n < 0`, the `n`th predecessor of `i` or `limit`, whichever
  ///     is reached first.
  ///   - Otherwise, `i` unmodified.
  ///
  /// - Precondition: `n >= 0` unless `Self` conforms to
  ///   `BidirectionalCollection`.
  ///
  /// - Complexity:
  ///   - O(1) if `Self` conforms to `RandomAccessCollection`.
  ///   - O(`abs(n)`) otherwise.
  @warn_unused_result
  func index(
    n: IndexDistance, stepsFrom i: Index, limitedBy limit: Index
  ) -> Index

  /// Advances `i` by `n` positions.
  ///
  /// - Precondition: `n >= 0` unless `Self` conforms to
  ///   `BidirectionalCollection`.
  /// - Precondition:
  ///   - If `n > 0`, `n <= self.distance(from: i, to: self.endIndex)`
  ///   - If `n < 0`, `n >= self.distance(from: i, to: self.startIndex)`
  ///
  /// - Complexity:
  ///   - O(1) if `Self` conforms to `RandomAccessCollection`.
  ///   - O(`abs(n)`) otherwise.
  func formIndex(n: IndexDistance, stepsFrom i: inout Index)

  /// Advances `i` by `n` positions, or until it equals `limit`.
  ///
  /// - Precondition: `n >= 0` unless `Self` conforms to
  ///   `BidirectionalCollection`.
  ///
  /// - Complexity:
  ///   - O(1) if `Self` conforms to `RandomAccessCollection`.
  ///   - O(`abs(n)`) otherwise.
  func formIndex(
    n: IndexDistance, stepsFrom i: inout Index, limitedBy limit: Index
  )

  /// Returns the distance between `start` and `end`.
  ///
  /// - Precondition: `start <= end` unless `Self` conforms to
  ///   `BidirectionalCollection`.
  /// - Complexity:
  ///   - O(1) if `Self` conforms to `RandomAccessCollection`.
  ///   - O(`n`) otherwise, where `n` is the method's result.
  @warn_unused_result
  func distance(from start: Index, to end: Index) -> IndexDistance
}

/// The iterator used for collections that don't specify one.
public struct IndexingIterator<
  Elements : IndexableBase
  // FIXME(compiler limitation):
  // Elements : Collection
> : IteratorProtocol, Sequence {

  /// Create an *iterator* over the given collection.
  public /// @testable
  init(_elements: Elements) {
    self._elements = _elements
    self._position = _elements.startIndex
  }

  /// Advance to the next element and return it, or `nil` if no next
  /// element exists.
  ///
  /// - Precondition: No preceding call to `self.next()` has returned `nil`.
  public mutating func next() -> Elements._Element? {
    if _position == _elements.endIndex { return nil }
    let element = _elements[_position]
    _elements.formSuccessor(&_position)
    return element
  }

  internal let _elements: Elements
  internal var _position: Elements.Index
}

/// A multi-pass sequence with addressable positions.
///
/// Positions are represented by an associated `Index` type.  Whereas
/// an arbitrary sequence may be consumed as it is traversed, a
/// collection is multi-pass: any element may be revisited merely by
/// saving its index.
///
/// The sequence view of the elements is identical to the collection
/// view.  In other words, the following code binds the same series of
/// values to `x` as does `for x in self {}`:
///
///     for i in self.indices {
///         let x = self[i]
///     }
public protocol Collection : Indexable, Sequence {
  /// A type that can represent the number of steps between pairs of
  /// `Index` values where one value is reachable from the other.
  ///
  /// Reachability is defined by the ability to produce one value from
  /// the other via zero or more applications of `successor(of:)`.
  associatedtype IndexDistance : SignedInteger = Int

  /// A type that provides the sequence's iteration interface and
  /// encapsulates its iteration state.
  ///
  /// By default, a `Collection` satisfies `Sequence` by
  /// supplying a `IndexingIterator` as its associated `Iterator`
  /// type.
  associatedtype Iterator : IteratorProtocol = IndexingIterator<Self>

  // FIXME: Needed here so that the `Iterator` is properly deduced from
  // a custom `makeIterator()` function.  Otherwise we get an
  // `IndexingIterator`. <rdar://problem/21539115>
  /// Returns an iterator over the elements of `self`.
  func makeIterator() -> Iterator

  /// A `Sequence` that can represent a contiguous subrange of `self`'s
  /// elements.
  ///
  /// - Note: This associated type appears as a requirement in
  ///   `Sequence`, but is restated here with stricter
  ///   constraints: in a `Collection`, the `SubSequence` should
  ///   also be a `Collection`.
  associatedtype SubSequence : IndexableBase, Sequence = Slice<Self>
  // FIXME(compiler limitation):
  // associatedtype SubSequence : Collection
  //   where
  //   Iterator.Element == SubSequence.Iterator.Element,
  //   SubSequence.Index == Index,
  //   SubSequence.Indices == Indices,
  //   SubSequence.SubSequence == SubSequence
  //
  // (<rdar://problem/20715009> Implement recursive protocol
  // constraints)
  //
  // These constraints allow processing collections in generic code by
  // repeatedly slicing them in a loop.

  /// Returns the element at the given `position`.
  subscript(position: Index) -> Iterator.Element { get }

  /// Returns a collection representing a contiguous sub-range of
  /// `self`'s elements.
  ///
  /// - Complexity: O(1)
  subscript(bounds: Range<Index>) -> SubSequence { get }

  /// A collection type whose elements are the indices of `self` that
  /// are valid for subscripting, in ascending order.
  associatedtype Indices : IndexableBase, Sequence = DefaultIndices<Self>

  // FIXME(compiler limitation):
  // associatedtype Indices : Collection
  //   where
  //   Indices.Iterator.Element == Index,
  //   Indices.Index == Index,
  //   Indices.SubSequence == Indices
  //   = DefaultIndices<Self>

  /// The indices that are valid for subscripting `self`, in ascending order.
  ///
  /// - Note: `indices` can hold a strong reference to the collection itself,
  ///   causing the collection to be non-uniquely referenced.  If you need to
  ///   mutate the collection while iterating over its indices, use the
  ///   `successor(of:)` method starting with `startIndex` to produce indices
  ///   instead.
  ///   
  ///   ```
  ///   var c = [10, 20, 30, 40, 50]
  ///   var i = c.startIndex
  ///   while i != c.endIndex {
  ///       c[i] /= 5
  ///       i = c.successor(of: i)
  ///   }
  ///   // c == [2, 4, 6, 8, 10]
  ///   ```
  var indices: Indices { get }

  /// Returns `self[startIndex..<end]`
  ///
  /// - Precondition: `end >= self.startIndex && end <= self.endIndex`
  /// - Complexity: O(1)
  @warn_unused_result
  func prefix(upTo end: Index) -> SubSequence

  /// Returns `self[start..<endIndex]`
  ///
  /// - Precondition: `start >= self.startIndex && start <= self.endIndex`
  /// - Complexity: O(1)
  @warn_unused_result
  func suffix(from start: Index) -> SubSequence

  /// Returns `prefix(upTo: successor(of: position)`
  ///
  /// - Precondition: `position >= self.startIndex && position < self.endIndex`
  /// - Complexity: O(1)
  @warn_unused_result
  func prefix(through position: Index) -> SubSequence

  /// Returns `true` iff `self` is empty.
  var isEmpty: Bool { get }

  /// Returns the number of elements.
  ///
  /// - Complexity: O(1) if `Self` conforms to
  ///   `RandomAccessCollection`; O(N) otherwise.
  var count: IndexDistance { get }

  // The following requirement enables dispatching for indexOf when
  // the element type is Equatable.
  /// Returns `Optional(Optional(index))` if an element was found
  /// or `Optional(nil)` if an element was determined to be missing;
  /// otherwise, `nil`.
  ///
  /// - Complexity: O(N).
  @warn_unused_result
  func _customIndexOfEquatableElement(_ element: Iterator.Element) -> Index??

  /// The first element of `self`, or `nil` if `self` is empty.
  var first: Iterator.Element? { get }

  /// Returns the result of advancing `i` by `n` positions.
  ///
  /// - Returns:
  ///   - If `n > 0`, the `n`th successor of `i`.
  ///   - If `n < 0`, the `n`th predecessor of `i`.
  ///   - Otherwise, `i` unmodified.
  ///
  /// - Precondition: `n >= 0` unless `Self` conforms to
  ///   `BidirectionalCollection`.
  /// - Precondition:
  ///   - If `n > 0`, `n <= self.distance(from: i, to: self.endIndex)`
  ///   - If `n < 0`, `n >= self.distance(from: i, to: self.startIndex)`
  ///
  /// - Complexity:
  ///   - O(1) if `Self` conforms to `RandomAccessCollection`.
  ///   - O(`abs(n)`) otherwise.
  @warn_unused_result
  func index(n: IndexDistance, stepsFrom i: Index) -> Index

  // FIXME: swift-3-indexing-model: Should this mention preconditions on `n`?
  /// Returns the result of advancing `i` by `n` positions, or until it
  /// equals `limit`.
  ///
  /// - Returns:
  ///   - If `n > 0`, the `n`th successor of `i` or `limit`, whichever
  ///     is reached first.
  ///   - If `n < 0`, the `n`th predecessor of `i` or `limit`, whichever
  ///     is reached first.
  ///   - Otherwise, `i` unmodified.
  ///
  /// - Precondition: `n >= 0` unless `Self` conforms to
  ///   `BidirectionalCollection`.
  ///
  /// - Complexity:
  ///   - O(1) if `Self` conforms to `RandomAccessCollection`.
  ///   - O(`abs(n)`) otherwise.
  @warn_unused_result
  func index(
    n: IndexDistance, stepsFrom i: Index, limitedBy limit: Index
  ) -> Index

  /// Returns the distance between `start` and `end`.
  ///
  /// - Precondition: `start <= end` unless `Self` conforms to
  ///   `BidirectionalCollection`.
  /// - Complexity:
  ///   - O(1) if `Self` conforms to `RandomAccessCollection`.
  ///   - O(`n`) otherwise, where `n` is the method's result.
  @warn_unused_result
  func distance(from start: Index, to end: Index) -> IndexDistance
}

/// Default implementation for forward collections.
extension Indexable {
  @inline(__always)
  public func formSuccessor(i: inout Index) {
    // FIXME: swift-3-indexing-model: tests.
    i = successor(of: i)
  }

  public func _failEarlyRangeCheck(index: Index, bounds: Range<Index>) {
    // FIXME: swift-3-indexing-model: tests.
    _precondition(
      bounds.lowerBound <= index,
      "out of bounds: index < startIndex")
    _precondition(
      index < bounds.upperBound,
      "out of bounds: index >= endIndex")
  }

  public func _failEarlyRangeCheck(range: Range<Index>, bounds: Range<Index>) {
    // FIXME: swift-3-indexing-model: tests.
    _precondition(
      bounds.lowerBound <= range.lowerBound,
      "out of bounds: range begins before startIndex")
    _precondition(
      range.lowerBound <= bounds.upperBound,
      "out of bounds: range ends after endIndex")
    _precondition(
      bounds.lowerBound <= range.upperBound,
      "out of bounds: range ends before bounds.lowerBound")
    _precondition(
      range.upperBound <= bounds.upperBound,
      "out of bounds: range begins after bounds.upperBound")
  }

  @warn_unused_result
  public func index(n: IndexDistance, stepsFrom i: Index) -> Index {
    // FIXME: swift-3-indexing-model: tests.
    return self._advanceForward(i, by: n)
  }

  @warn_unused_result
  public func index(
    n: IndexDistance, stepsFrom i: Index, limitedBy limit: Index
  ) -> Index {
    // FIXME: swift-3-indexing-model: tests.
    return self._advanceForward(i, by: n, limitedBy: limit)
  }

  public func formIndex(n: IndexDistance, stepsFrom i: inout Index) {
    i = index(n, stepsFrom: i)
  }

  public func formIndex(
    n: IndexDistance, stepsFrom i: inout Index, limitedBy limit: Index
  ) {
    i = index(n, stepsFrom: i, limitedBy: limit)
  }
  
  @warn_unused_result
  public func distance(from start: Index, to end: Index) -> IndexDistance {
    // FIXME: swift-3-indexing-model: tests.
    _precondition(start <= end,
      "Only BidirectionalCollections can have end come before start")

    var start = start
    var count: IndexDistance = 0
    while start != end {
      count = count + 1
      formSuccessor(&start)
    }
    return count
  }

  /// Do not use this method directly; call advanced(by: n) instead.
  @inline(__always)
  @warn_unused_result
  internal func _advanceForward(i: Index, by n: IndexDistance) -> Index {
    _precondition(n >= 0,
      "Only BidirectionalCollections can be advanced by a negative amount")

    var i = i
    for _ in stride(from: 0, to: n, by: 1) {
      formSuccessor(&i)
    }
    return i
  }

  /// Do not use this method directly; call advanced(by: n, limit) instead.
  @inline(__always)
  @warn_unused_result
  internal
  func _advanceForward(i: Index, by n: IndexDistance, limitedBy limit: Index) -> Index {
    _precondition(n >= 0,
      "Only BidirectionalCollections can be advanced by a negative amount")

    var i = i
    for _ in stride(from: 0, to: n, by: 1) {
      if (limit == i) {
        break;
      }
      formSuccessor(&i)
    }
    return i
  }
}

/// Supply optimized defaults for `Collection` models that use some model
/// of `Strideable` as their `Index`.
extension Indexable where Index : Strideable {
  @warn_unused_result
  public func successor(of i: Index) -> Index {
    // FIXME: swift-3-indexing-model: tests.
    _failEarlyRangeCheck(i, bounds: startIndex..<endIndex)

    return i.advanced(by: 1)
  }

  /*
  @warn_unused_result
  public func index(n: IndexDistance, stepsFrom i: Index) -> Index {
    _precondition(n >= 0,
      "Only BidirectionalCollections can be advanced by a negative amount")
    // FIXME: swift-3-indexing-model: range check i

    // FIXME: swift-3-indexing-model - error: cannot invoke 'advanced' with an argument list of type '(by: Self.IndexDistance)'
    return i.advanced(by: n)
  }

  @warn_unused_result
  public func index(n: IndexDistance, stepsFrom i: Index, limitedBy limit: Index) -> Index {
    _precondition(n >= 0,
      "Only BidirectionalCollections can be advanced by a negative amount")
    // FIXME: swift-3-indexing-model: range check i

    // FIXME: swift-3-indexing-model - error: cannot invoke 'advanced' with an argument list of type '(by: Self.IndexDistance)'
    let i = i.advanced(by: n)
    if (i >= limit) {
      return limit
    }
    return i
  }

  @warn_unused_result
  public func distance(from start: Index, to end: Index) -> IndexDistance {
    _precondition(start <= end,
      "Only BidirectionalCollections can have end come before start")
    // FIXME: swift-3-indexing-model: range check supplied start and end?

    // FIXME: swift-3-indexing-model - error: cannot invoke 'distance' with an argument list of type '(to: Self.Index)'
    return start.distance(to: end)
  }
  */
}

/// Supply the default `makeIterator()` method for `Collection` models
/// that accept the default associated `Iterator`,
/// `IndexingIterator<Self>`.
extension Collection where Iterator == IndexingIterator<Self> {
  public func makeIterator() -> IndexingIterator<Self> {
    return IndexingIterator(_elements: self)
  }
}

/// Supply the default "slicing" `subscript` for `Collection` models
/// that accept the default associated `SubSequence`, `Slice<Self>`.
extension Collection where SubSequence == Slice<Self> {
  public subscript(bounds: Range<Index>) -> Slice<Self> {
    _failEarlyRangeCheck(bounds, bounds: startIndex..<endIndex)
    return Slice(base: self, bounds: bounds)
  }
}

// TODO: swift-3-indexing-model - review the following
extension Collection where SubSequence == Self {
  /// If `!self.isEmpty`, remove the first element and return it, otherwise
  /// return `nil`.
  ///
  /// - Complexity: O(1)
  @warn_unused_result
  public mutating func popFirst() -> Iterator.Element? {
    guard !isEmpty else { return nil }
    let element = first!
    self = self[successor(of: startIndex)..<endIndex]
    return element
  }
}

/// Default implementations of core requirements
extension Collection {
  /// Returns `true` iff `self` is empty.
  ///
  /// - Complexity: O(1)
  public var isEmpty: Bool {
    return startIndex == endIndex
  }

  /// Returns the first element of `self`, or `nil` if `self` is empty.
  ///
  /// - Complexity: O(1)
  public var first: Iterator.Element? {
    // NB: Accessing `startIndex` may not be O(1) for some lazy collections,
    // so instead of testing `isEmpty` and then returning the first element,
    // we'll just rely on the fact that the iterator always yields the
    // first element first.
    var i = makeIterator()
    return i.next()
  }
// TODO: swift-3-indexing-model - uncomment and replace above ready (or should we still use the iterator one?)
  /// Returns the first element of `self`, or `nil` if `self` is empty.
  ///
  /// - Complexity: O(1)
  //  public var first: Iterator.Element? {
  //    return isEmpty ? nil : self[startIndex]
  //  }

// TODO: swift-3-indexing-model - review the following
  /// Returns a value less than or equal to the number of elements in
  /// `self`, *nondestructively*.
  ///
  /// - Complexity: O(`count`).
  public var underestimatedCount: Int {
    return numericCast(count)
  }

  /// Returns the number of elements.
  ///
  /// - Complexity: O(1) if `Self` conforms to `RandomAccessCollection`;
  ///   O(N) otherwise.
  public var count: IndexDistance {
    return distance(from: startIndex, to: endIndex)
  }

// TODO: swift-3-indexing-model - rename the following to _customIndexOfEquatable(element)?
  /// Customization point for `Sequence.index(of:)`.
  ///
  /// Define this method if the collection can find an element in less than
  /// O(N) by exploiting collection-specific knowledge.
  ///
  /// - Returns: `nil` if a linear search should be attempted instead,
  ///   `Optional(nil)` if the element was not found, or
  ///   `Optional(Optional(index))` if an element was found.
  ///
  /// - Complexity: O(`count`).
  @warn_unused_result
  public // dispatching
  func _customIndexOfEquatableElement(_: Iterator.Element) -> Index?? {
    return nil
  }
}

//===----------------------------------------------------------------------===//
// Default implementations for Collection
//===----------------------------------------------------------------------===//

extension Collection {
// TODO: swift-3-indexing-model - review the following
  /// Returns an `Array` containing the results of mapping `transform`
  /// over `self`.
  ///
  /// - Complexity: O(N).
  @warn_unused_result
  public func map<T>(
    @noescape _ transform: (Iterator.Element) throws -> T
  ) rethrows -> [T] {
    let count: Int = numericCast(self.count)
    if count == 0 {
      return []
    }

    var result = ContiguousArray<T>()
    result.reserveCapacity(count)

    var i = self.startIndex

    for _ in 0..<count {
      result.append(try transform(self[i]))
      formSuccessor(&i)
    }

    _expectEnd(i, self)
    return Array(result)
  }

  /// Returns a subsequence containing all but the first `n` elements.
  ///
  /// - Precondition: `n >= 0`
  /// - Complexity: O(`n`)
  @warn_unused_result
  public func dropFirst(_ n: Int) -> SubSequence {
    _precondition(n >= 0, "Can't drop a negative number of elements from a collection")
    let start = index(numericCast(n), stepsFrom: startIndex, limitedBy: endIndex)
    return self[start..<endIndex]
  }

  /// Returns a subsequence containing all but the last `n` elements.
  ///
  /// - Precondition: `n >= 0`
  /// - Complexity: O(`self.count`)
  @warn_unused_result
  public func dropLast(_ n: Int) -> SubSequence {
    _precondition(
      n >= 0, "Can't drop a negative number of elements from a collection")
    let amount = Swift.max(0, numericCast(count) - n)
    let end = index(numericCast(amount), stepsFrom: startIndex, limitedBy: endIndex)
    return self[startIndex..<end]
  }

  /// Returns a subsequence, up to `maxLength` in length, containing the
  /// initial elements.
  ///
  /// If `maxLength` exceeds `self.count`, the result contains all
  /// the elements of `self`.
  ///
  /// - Precondition: `maxLength >= 0`
  /// - Complexity: O(`maxLength`)
  @warn_unused_result
  public func prefix(_ maxLength: Int) -> SubSequence {
    _precondition(
      maxLength >= 0,
      "Can't take a prefix of negative length from a collection")
    let end = index(numericCast(maxLength), stepsFrom: startIndex, limitedBy: endIndex)
    return self[startIndex..<end]
  }

  /// Returns a slice, up to `maxLength` in length, containing the
  /// final elements of `self`.
  ///
  /// If `maxLength` exceeds `s.count`, the result contains all
  /// the elements of `self`.
  ///
  /// - Precondition: `maxLength >= 0`
  /// - Complexity: O(`self.count`)
  @warn_unused_result
  public func suffix(_ maxLength: Int) -> SubSequence {
    _precondition(
      maxLength >= 0,
      "Can't take a suffix of negative length from a collection")
    let amount = Swift.max(0, numericCast(count) - maxLength)
    let start = index(numericCast(amount), stepsFrom: startIndex, limitedBy: endIndex)
    return self[start..<endIndex]
  }

  /// Returns `self[startIndex..<end]`
  ///
  /// - Precondition: `end >= self.startIndex && end <= self.endIndex`
  /// - Complexity: O(1)
  @warn_unused_result
  public func prefix(upTo end: Index) -> SubSequence {
    return self[startIndex..<end]
  }

  /// Returns `self[start..<endIndex]`
  ///
  /// - Precondition: `start >= self.startIndex && start <= self.endIndex`
  /// - Complexity: O(1)
  @warn_unused_result
  public func suffix(from start: Index) -> SubSequence {
    return self[start..<endIndex]
  }

  /// Returns `prefix(upTo: successor(of: position))`
  ///
  /// - Precondition: `position >= self.startIndex && position < self.endIndex`
  /// - Complexity: O(1)
  @warn_unused_result
  public func prefix(through position: Index) -> SubSequence {
    return prefix(upTo: successor(of: position))
  }

  // TODO: swift-3-indexing-model - review the following
  /// Returns the maximal `SubSequence`s of `self`, in order, that
  /// don't contain elements satisfying the predicate `isSeparator`.
  ///
  /// - Parameter maxSplits: The maximum number of `SubSequence`s to
  ///   return, minus 1.
  ///   If `maxSplits + 1` `SubSequence`s are returned, the last one is
  ///   a suffix of `self` containing *all* the elements of `self` following the
  ///   last split point.
  ///   The default value is `Int.max`.
  ///
  /// - Parameter omittingEmptySubsequences: If `false`, an empty `SubSequence`
  ///   is produced in the result for each pair of consecutive elements
  ///   satisfying `isSeparator`.
  ///   The default value is `true`.
  ///
  /// - Precondition: `maxSplits >= 0`
  @warn_unused_result
  public func split(
    maxSplits: Int = Int.max,
    omittingEmptySubsequences: Bool = true,
    @noescape isSeparator: (Iterator.Element) throws -> Bool
  ) rethrows -> [SubSequence] {
    _precondition(maxSplits >= 0, "Must take zero or more splits")

    var result: [SubSequence] = []
    var subSequenceStart: Index = startIndex

    func appendSubsequence(end: Index) -> Bool {
      if subSequenceStart == end && omittingEmptySubsequences {
        return false
      }
      result.append(self[subSequenceStart..<end])
      return true
    }

    if maxSplits == 0 || isEmpty {
      appendSubsequence(end: endIndex)
      return result
    }

    var subSequenceEnd = subSequenceStart
    let cachedEndIndex = endIndex
    while subSequenceEnd != cachedEndIndex {
      if try isSeparator(self[subSequenceEnd]) {
        let didAppend = appendSubsequence(end: subSequenceEnd)
        formSuccessor(&subSequenceEnd)
        subSequenceStart = subSequenceEnd
        if didAppend && result.count == maxSplits {
          break
        }
        continue
      }
      formSuccessor(&subSequenceEnd)
    }

    if subSequenceStart != cachedEndIndex || !omittingEmptySubsequences {
      result.append(self[subSequenceStart..<cachedEndIndex])
    }

    return result
  }
}

// TODO: swift-3-indexing-model - review the following
extension Collection where Iterator.Element : Equatable {
  /// Returns the maximal `SubSequence`s of `self`, in order, around a
  /// `separator` element.
  ///
  /// - Parameter maxSplits: The maximum number of `SubSequence`s to
  ///   return, minus 1.
  ///   If `maxSplits + 1` `SubSequence`s are returned, the last one is
  ///   a suffix of `self` containing *all* the elements of `self` following the
  ///   last split point.
  ///   The default value is `Int.max`.
  ///
  /// - Parameter omittingEmptySubsequences: If `false`, an empty `SubSequence`
  ///   is produced in the result for each pair of consecutive elements
  ///   equal to `separator`.
  ///   The default value is `true`.
  ///
  /// - Precondition: `maxSplits >= 0`
  @warn_unused_result
  public func split(
    separator: Iterator.Element,
    maxSplits: Int = Int.max,
    omittingEmptySubsequences: Bool = true
  ) -> [SubSequence] {
  return split(
    maxSplits: maxSplits,
    omittingEmptySubsequences: omittingEmptySubsequences,
    isSeparator: { $0 == separator })
  }
}

// TODO: swift-3-indexing-model - review the following
extension Collection where SubSequence == Self {
  /// Remove the element at `startIndex` and return it.
  ///
  /// - Complexity: O(1)
  /// - Precondition: `!self.isEmpty`.
  @discardableResult
  public mutating func removeFirst() -> Iterator.Element {
    _precondition(!isEmpty, "can't remove items from an empty collection")
    let element = first!
    self = self[successor(of: startIndex)..<endIndex]
    return element
  }

  /// Remove the first `n` elements.
  ///
  /// - Complexity:
  ///   - O(1) if `Self` conforms to `RandomAccessCollection`
  ///   - O(n) otherwise
  /// - Precondition: `n >= 0 && self.count >= n`.
  public mutating func removeFirst(_ n: Int) {
    if n == 0 { return }
    _precondition(n >= 0, "number of elements to remove should be non-negative")
    _precondition(count >= numericCast(n),
      "can't remove more items from a collection than it contains")
    self = self[index(numericCast(n), stepsFrom: startIndex)..<endIndex]
  }
}

// TODO: swift-3-indexing-model - review the following
extension Sequence
  where Self : _ArrayProtocol, Self.Element == Self.Iterator.Element {
  // A fast implementation for when you are backed by a contiguous array.
  public func _copyContents(
    initializing ptr: UnsafeMutablePointer<Iterator.Element>
  ) -> UnsafeMutablePointer<Iterator.Element> {
    let s = self._baseAddressIfContiguous
    if s != nil {
      let count = self.count
      ptr.initializeFrom(s, count: count)
      _fixLifetime(self._owner)
      return ptr + count
    } else {
      var p = ptr
      for x in self {
        p.initialize(with: x)
        p += 1
      }
      return p
    }
  }
}

extension Collection {
  public func _preprocessingPass<R>(
    @noescape _ preprocess: () throws -> R
  ) rethrows -> R? {
    return try preprocess()
  }
}

@available(*, unavailable, message: "Bit enum has been deprecated. Please use Int instead.")
public enum Bit {}

@available(*, unavailable, renamed: "IndexingIterator")
public struct IndexingGenerator<Elements : IndexableBase> {}

@available(*, unavailable, renamed: "Collection")
public typealias CollectionType = Collection

extension Collection {
  @available(*, unavailable, renamed: "Iterator")
  public typealias Generator = Iterator

  @available(*, unavailable, renamed: "makeIterator")
  public func generate() -> Iterator {
    fatalError("unavailable function can't be called")
  }

  @available(*, unavailable, message: "Removed in Swift 3. Please use underestimatedCount property.")
  public func underestimateCount() -> Int {
    fatalError("unavailable function can't be called")
  }

  @available(*, unavailable, message: "Please use split(maxSplits:omittingEmptySubsequences:isSeparator:) instead")
  public func split(
    _ maxSplit: Int = Int.max,
    allowEmptySlices: Bool = false,
    @noescape isSeparator: (Iterator.Element) throws -> Bool
  ) rethrows -> [SubSequence] {
    fatalError("unavailable function can't be called")
  }
}

extension Collection where Iterator.Element : Equatable {
  @available(*, unavailable, message: "Please use split(separator:maxSplits:omittingEmptySubsequences:) instead")
  public func split(
    _ separator: Iterator.Element,
    maxSplit: Int = Int.max,
    allowEmptySlices: Bool = false
  ) -> [SubSequence] {
    fatalError("unavailable function can't be called")
  }
}

@available(*, unavailable, message: "PermutationGenerator has been removed in Swift 3")
public struct PermutationGenerator<C : Collection, Indices : Sequence> {}
