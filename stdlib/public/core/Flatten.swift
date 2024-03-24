//===--- Flatten.swift ----------------------------------------*- swift -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// A sequence consisting of all the elements contained in each segment
/// contained in some `Base` sequence.
///
/// The elements of this view are a concatenation of the elements of
/// each sequence in the base.
///
/// The `joined` method is always lazy, but does not implicitly
/// confer laziness on algorithms applied to its result.  In other
/// words, for ordinary sequences `s`:
///
/// * `s.joined()` does not create new storage
/// * `s.joined().map(f)` maps eagerly and returns a new array
/// * `s.lazy.joined().map(f)` maps lazily and returns a `LazyMapSequence`
@frozen // lazy-performance
public struct FlattenSequence<Base: Sequence> where Base.Element: Sequence {

  @usableFromInline // lazy-performance
  internal var _base: Base

  /// Creates a concatenation of the elements of the elements of `base`.
  ///
  /// - Complexity: O(1)
  @inlinable // lazy-performance
  internal init(_base: Base) {
    self._base = _base
  }
}

extension FlattenSequence: Sendable where Base: Sendable {}

extension FlattenSequence {
  @frozen // lazy-performance
  public struct Iterator {
    @usableFromInline // lazy-performance
    internal var _base: Base.Iterator
    @usableFromInline // lazy-performance
    internal var _inner: Base.Element.Iterator?

    /// Construct around a `base` iterator.
    @inlinable // lazy-performance
    internal init(_base: Base.Iterator) {
      self._base = _base
    }
  }
}

extension FlattenSequence.Iterator: Sendable
  where Base.Iterator: Sendable, Base.Element.Iterator: Sendable {}

extension FlattenSequence.Iterator: IteratorProtocol {
  public typealias Element = Base.Element.Element
  
  /// Advances to the next element and returns it, or `nil` if no next element
  /// exists.
  ///
  /// Once `nil` has been returned, all subsequent calls return `nil`.
  ///
  /// - Precondition: `next()` has not been applied to a copy of `self`
  ///   since the copy was made.
  @inlinable // lazy-performance
  public mutating func next() -> Element? {
    repeat {
      if _fastPath(_inner != nil) {
        let ret = _inner!.next()
        if _fastPath(ret != nil) {
          return ret
        }
      }
      let s = _base.next()
      if _slowPath(s == nil) {
        return nil
      }
      _inner = s!.makeIterator()
    }
    while true
  }
}

extension FlattenSequence.Iterator: Sequence { }

extension FlattenSequence: Sequence {
  /// Returns an iterator over the elements of this sequence.
  ///
  /// - Complexity: O(1).
  @inlinable // lazy-performance
  public __consuming func makeIterator() -> Iterator {
    return Iterator(_base: _base.makeIterator())
  }
}

extension Sequence where Element: Sequence {
  /// Returns the elements of this sequence of sequences, concatenated.
  ///
  /// In this example, an array of three ranges is flattened so that the
  /// elements of each range can be iterated in turn.
  ///
  ///     let ranges = [0..<3, 8..<10, 15..<17]
  ///
  ///     // A for-in loop over 'ranges' accesses each range:
  ///     for range in ranges {
  ///       print(range)
  ///     }
  ///     // Prints "0..<3"
  ///     // Prints "8..<10"
  ///     // Prints "15..<17"
  ///
  ///     // Use 'joined()' to access each element of each range:
  ///     for index in ranges.joined() {
  ///         print(index, terminator: " ")
  ///     }
  ///     // Prints: "0 1 2 8 9 15 16"
  ///
  /// - Returns: A flattened view of the elements of this
  ///   sequence of sequences.
  @inlinable // lazy-performance
  public __consuming func joined() -> FlattenSequence<Self> {
    return FlattenSequence(_base: self)
  }
}

extension LazySequenceProtocol where Element: Sequence {
  /// Returns a lazy sequence that concatenates the elements of this sequence of
  /// sequences.
  @inlinable // lazy-performance
  public __consuming func joined() -> LazySequence<FlattenSequence<Elements>> {
    return FlattenSequence(_base: elements).lazy
  }
}

public typealias FlattenCollection<T: Collection> = FlattenSequence<T> where T.Element: Collection

extension FlattenSequence where Base: Collection, Base.Element: Collection {
  /// A position in a FlattenCollection
  @frozen // lazy-performance
  public struct Index {
    /// The position in the outer collection of collections.
    @usableFromInline // lazy-performance
    internal let _outer: Base.Index

    /// The position in the inner collection at `base[_outer]`, or `nil` if
    /// `_outer == base.endIndex`.
    ///
    /// When `_inner != nil`, `_inner!` is a valid subscript of `base[_outer]`;
    /// when `_inner == nil`, `_outer == base.endIndex` and this index is
    /// `endIndex` of the `FlattenCollection`.
    @usableFromInline // lazy-performance
    internal let _inner: Base.Element.Index?

    @inlinable // lazy-performance
    internal init(_ _outer: Base.Index, _ inner: Base.Element.Index?) {
      self._outer = _outer
      self._inner = inner
    }
  }
}

extension FlattenSequence.Index: Sendable
  where Base.Index: Sendable, Base.Element.Index: Sendable {}

extension FlattenSequence.Index: Equatable where Base: Collection, Base.Element: Collection {
  @inlinable // lazy-performance
  public static func == (
    lhs: FlattenCollection<Base>.Index,
    rhs: FlattenCollection<Base>.Index
  ) -> Bool {
    return lhs._outer == rhs._outer && lhs._inner == rhs._inner
  }
}

extension FlattenSequence.Index: Comparable where Base: Collection, Base.Element: Collection {
  @inlinable // lazy-performance
  public static func < (
    lhs: FlattenCollection<Base>.Index,
    rhs: FlattenCollection<Base>.Index
  ) -> Bool {
    // FIXME: swift-3-indexing-model: tests.
    if lhs._outer != rhs._outer {
      return lhs._outer < rhs._outer
    }

    if let lhsInner = lhs._inner, let rhsInner = rhs._inner {
      return lhsInner < rhsInner
    }

    // When combined, the two conditions above guarantee that both
    // `_outer` indices are `_base.endIndex` and both `_inner` indices
    // are `nil`, since `_inner` is `nil` iff `_outer == base.endIndex`.
    _precondition(lhs._inner == nil && rhs._inner == nil)

    return false
  }
}

extension FlattenSequence.Index: Hashable
  where Base: Collection, Base.Element: Collection, Base.Index: Hashable, Base.Element.Index: Hashable {
  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  @inlinable
  public func hash(into hasher: inout Hasher) {
    hasher.combine(_outer)
    hasher.combine(_inner)
  }
}

extension FlattenCollection: Collection {
  /// The position of the first element in a non-empty collection.
  ///
  /// In an empty collection, `startIndex == endIndex`.
  @inlinable // lazy-performance
  public var startIndex: Index {
    let end = _base.endIndex
    var outer = _base.startIndex
    while outer != end {
      let innerCollection = _base[outer]
      if !innerCollection.isEmpty {
        return Index(outer, innerCollection.startIndex)
      }
      _base.formIndex(after: &outer)
    }

    return endIndex
  }

  /// The collection's "past the end" position.
  ///
  /// `endIndex` is not a valid argument to `subscript`, and is always
  /// reachable from `startIndex` by zero or more applications of
  /// `index(after:)`.
  @inlinable // lazy-performance
  public var endIndex: Index {
    return Index(_base.endIndex, nil)
  }

  @inlinable // lazy-performance
  internal func _index(after i: Index) -> Index {
    let innerCollection = _base[i._outer]
    let nextInner = innerCollection.index(after: i._inner!)
    if _fastPath(nextInner != innerCollection.endIndex) {
      return Index(i._outer, nextInner)
    }

    var nextOuter = _base.index(after: i._outer)
    while nextOuter != _base.endIndex {
      let nextInnerCollection = _base[nextOuter]
      if !nextInnerCollection.isEmpty {
        return Index(nextOuter, nextInnerCollection.startIndex)
      }
      _base.formIndex(after: &nextOuter)
    }

    return endIndex
  }

  @inlinable // lazy-performance
  internal func _index(before i: Index) -> Index {
    var prevOuter = i._outer
    if prevOuter == _base.endIndex {
      prevOuter = _base.index(prevOuter, offsetBy: -1)
    }
    var prevInnerCollection = _base[prevOuter]
    var prevInner = i._inner ?? prevInnerCollection.endIndex

    while prevInner == prevInnerCollection.startIndex {
      prevOuter = _base.index(prevOuter, offsetBy: -1)
      prevInnerCollection = _base[prevOuter]
      prevInner = prevInnerCollection.endIndex
    }

    return Index(prevOuter, prevInnerCollection.index(prevInner, offsetBy: -1))
  }

  // TODO: swift-3-indexing-model - add docs
  @inlinable // lazy-performance
  public func index(after i: Index) -> Index {
    return _index(after: i)
  }

  @inlinable // lazy-performance
  public func formIndex(after i: inout Index) {
    i = index(after: i)
  }

  @inlinable // lazy-performance
  public func distance(from start: Index, to end: Index) -> Int {
    let distanceIsNegative = start > end

    // The following check ensures that distance(from:to:) is invoked on
    // the _base at least once, to trigger a _precondition in forward only
    // collections.
    if distanceIsNegative {
      _ = _base.distance(from: _base.endIndex, to: _base.startIndex)
    }

    // This path handles indices belonging to the same collection.
    if start._outer == end._outer {
      guard let i = start._inner, let j = end._inner else { return 0 }
      return _base[start._outer].distance(from: i, to: j)
    }

    // The following path combines the distances of three regions.
    let lowerBound: Index
    let upperBound: Index

    let step: Int
    var distance: Int

    // Note that lowerBound is a valid index because start != end.
    if distanceIsNegative {
      step = -1
      lowerBound = end
      upperBound = start
      let lowest = _base[lowerBound._outer]
      distance = lowest.distance(from: lowest.endIndex, to: lowerBound._inner!)
    } else {
      step = 01
      lowerBound = start
      upperBound = end
      let lowest = _base[lowerBound._outer]
      distance = lowest.distance(from: lowerBound._inner!, to: lowest.endIndex)
    }

    // We can use each collection's count in the middle region since the
    // fast path ensures that the other regions cover a nonzero distance,
    // which means that an extra Int.min distance should trap regardless.
    var outer = _base.index(after: lowerBound._outer)
    while outer < upperBound._outer {
      // 0 ... Int.max can always be negated.
      distance += _base[outer].count &* step
      _base.formIndex(after: &outer)
    }

    // This unwraps if start != endIndex and end != endIndex. We can use the
    // positive distance for the same reason that we can use the collection's
    // count in the middle region.
    if let inner = upperBound._inner {
      // 0 ... Int.max can always be negated.
      let highest = _base[upperBound._outer]
      distance += highest.distance(from: highest.startIndex, to: inner) &* step
    }

    return distance
  }

  @inline(__always)
  @inlinable // lazy-performance
  internal func _advanceIndex(_ i: inout Index, step: Int) {
    _internalInvariant(-1...1 ~= step, "step should be within the -1...1 range")
    i = step < 0 ? _index(before: i) : _index(after: i)
  }

  @inline(__always)
  @inlinable // lazy-performance
  internal func _ensureBidirectional(step: Int) {
    // FIXME: This seems to be the best way of checking whether _base is
    // forward only without adding an extra protocol requirement.
    // index(_:offsetBy:limitedBy:) is chosen because it is supposed to return
    // nil when the resulting index lands outside the collection boundaries,
    // and therefore likely does not trap in these cases.
    if step < 0 {
      _ = _base.index(
        _base.endIndex, offsetBy: step, limitedBy: _base.startIndex)
    }
  }

  @inlinable // lazy-performance
  public func index(_ i: Index, offsetBy n: Int) -> Index {
    var i = i
    let step = n.signum()
    _ensureBidirectional(step: step)
    for _ in 0 ..< abs(n) {
      _advanceIndex(&i, step: step)
    }
    return i
  }

  @inlinable // lazy-performance
  public func formIndex(_ i: inout Index, offsetBy n: Int) {
    i = index(i, offsetBy: n)
  }

  @inlinable // lazy-performance
  public func index(
    _ i: Index, offsetBy n: Int, limitedBy limit: Index
  ) -> Index? {
    var i = i
    let step = n.signum()
    // The following line makes sure that index(_:offsetBy:limitedBy:) is
    // invoked on the _base at least once, to trigger a _precondition in
    // forward only collections.
    _ensureBidirectional(step: step)
    for _ in 0 ..< abs(n) {
      if i == limit {
        return nil
      }
      _advanceIndex(&i, step: step)
    }
    return i
  }

  @inlinable // lazy-performance
  public func formIndex(
    _ i: inout Index, offsetBy n: Int, limitedBy limit: Index
  ) -> Bool {
    if let advancedIndex = index(i, offsetBy: n, limitedBy: limit) {
      i = advancedIndex
      return true
    }
    i = limit
    return false
  }

  /// Accesses the element at `position`.
  ///
  /// - Precondition: `position` is a valid position in `self` and
  ///   `position != endIndex`.
  @inlinable // lazy-performance
  public subscript(position: Index) -> Base.Element.Element {
    return _base[position._outer][position._inner!]
  }

  @inlinable // lazy-performance
  public subscript(bounds: Range<Index>) -> Slice<FlattenCollection<Base>> {
    return Slice(base: self, bounds: bounds)
  }
}

extension FlattenCollection: BidirectionalCollection
  where Base: BidirectionalCollection, Base.Element: BidirectionalCollection {

  // FIXME(performance): swift-3-indexing-model: add custom advance/distance
  // methods that skip over inner collections when random-access

  // TODO: swift-3-indexing-model - add docs
  @inlinable // lazy-performance
  public func index(before i: Index) -> Index {
    return _index(before: i)
  }

  @inlinable // lazy-performance
  public func formIndex(before i: inout Index) {
    i = index(before: i)
  }
}
