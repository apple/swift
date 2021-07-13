//===----------------------------------------------------------------------===//
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

/// Creates a sequence of pairs built out of two underlying sequences.
///
/// In the `Zip2Sequence` instance returned by this function, the elements of
/// the *i*th pair are the *i*th elements of each underlying sequence. The
/// following example uses the `zip(_:_:)` function to iterate over an array
/// of strings and a countable range at the same time:
///
///     let words = ["one", "two", "three", "four"]
///     let numbers = 1...4
///
///     for (word, number) in zip(words, numbers) {
///         print("\(word): \(number)")
///     }
///     // Prints "one: 1"
///     // Prints "two: 2
///     // Prints "three: 3"
///     // Prints "four: 4"
///
/// If the two sequences passed to `zip(_:_:)` are different lengths, the
/// resulting sequence is the same length as the shorter sequence. In this
/// example, the resulting array is the same length as `words`:
///
///     let naturalNumbers = 1...Int.max
///     let zipped = Array(zip(words, naturalNumbers))
///     // zipped == [("one", 1), ("two", 2), ("three", 3), ("four", 4)]
///
/// - Parameters:
///   - sequence1: The first sequence or collection to zip.
///   - sequence2: The second sequence or collection to zip.
/// - Returns: A sequence of tuple pairs, where the elements of each pair are
///   corresponding elements of `sequence1` and `sequence2`.
@inlinable // generic-performance
public func zip<Sequence1, Sequence2>(
  _ sequence1: Sequence1, _ sequence2: Sequence2
) -> Zip2Sequence<Sequence1, Sequence2> {
  return Zip2Sequence(sequence1, sequence2)
}

/// A sequence of pairs built out of two underlying sequences.
///
/// In a `Zip2Sequence` instance, the elements of the *i*th pair are the *i*th
/// elements of each underlying sequence. To create a `Zip2Sequence` instance,
/// use the `zip(_:_:)` function.
///
/// The following example uses the `zip(_:_:)` function to iterate over an
/// array of strings and a countable range at the same time:
///
///     let words = ["one", "two", "three", "four"]
///     let numbers = 1...4
///
///     for (word, number) in zip(words, numbers) {
///         print("\(word): \(number)")
///     }
///     // Prints "one: 1"
///     // Prints "two: 2
///     // Prints "three: 3"
///     // Prints "four: 4"
@frozen // generic-performance
public struct Zip2Sequence<Sequence1: Sequence, Sequence2: Sequence> {
  @usableFromInline // generic-performance
  internal let _sequence1: Sequence1
  @usableFromInline // generic-performance
  internal let _sequence2: Sequence2

  /// Creates an instance that makes pairs of elements from `sequence1` and
  /// `sequence2`.
  @inlinable // generic-performance
  internal init(_ sequence1: Sequence1, _ sequence2: Sequence2) {
    (_sequence1, _sequence2) = (sequence1, sequence2)
  }
}

extension Zip2Sequence {
  /// An iterator for `Zip2Sequence`.
  @frozen // generic-performance
  public struct Iterator {
    @usableFromInline // generic-performance
    internal var _baseStream1: Sequence1.Iterator
    @usableFromInline // generic-performance
    internal var _baseStream2: Sequence2.Iterator
    @usableFromInline // generic-performance
    internal var _reachedEnd: Bool = false

    /// Creates an instance around a pair of underlying iterators.
    @inlinable // generic-performance
    internal init(
    _ iterator1: Sequence1.Iterator, 
    _ iterator2: Sequence2.Iterator
    ) {
      (_baseStream1, _baseStream2) = (iterator1, iterator2)
    }
  }
}

extension Zip2Sequence.Iterator: IteratorProtocol {
  /// The type of element returned by `next()`.
  public typealias Element = (Sequence1.Element, Sequence2.Element)

  /// Advances to the next element and returns it, or `nil` if no next element
  /// exists.
  ///
  /// Once `nil` has been returned, all subsequent calls return `nil`.
  @inlinable // generic-performance
  public mutating func next() -> Element? {
    // The next() function needs to track if it has reached the end.  If we
    // didn't, and the first sequence is longer than the second, then when we
    // have already exhausted the second sequence, on every subsequent call to
    // next() we would consume and discard one additional element from the
    // first sequence, even though next() had already returned nil.

    if _reachedEnd {
      return nil
    }

    guard let element1 = _baseStream1.next(),
          let element2 = _baseStream2.next() else {
      _reachedEnd = true
      return nil
    }

    return (element1, element2)
  }
}

extension Zip2Sequence: Sequence {
  public typealias Element = (Sequence1.Element, Sequence2.Element)

  /// Returns an iterator over the elements of this sequence.
  @inlinable // generic-performance
  public __consuming func makeIterator() -> Iterator {
    return Iterator(
      _sequence1.makeIterator(),
      _sequence2.makeIterator())
  }

  @inlinable // generic-performance
  public var underestimatedCount: Int {
    return Swift.min(
      _sequence1.underestimatedCount,
      _sequence2.underestimatedCount
    )
  }
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension Zip2Sequence: Collection
  where Sequence1: Collection, Sequence2: Collection
{
  @frozen
  public struct _Index {
    /// The position in the first underlying collection.
    public let base1: Sequence1.Index
    
    /// The position in the second underlying collection.
    public let base2: Sequence2.Index
    
    @inlinable
    init(base1: Sequence1.Index, base2: Sequence2.Index) {
      self.base1 = base1
      self.base2 = base2
    }
  }
  
  // Ensures that `Self.Index == SubSequence.Index`.
  public typealias Index = SubSequence._Index
  public typealias SubSequence =
    Zip2Sequence<Sequence1.SubSequence, Sequence2.SubSequence>
  
  @inlinable
  public var startIndex: Index {
    return isEmpty
      ? endIndex
      : Index(base1: _sequence1.startIndex, base2: _sequence2.startIndex)
  }
  
  @inlinable
  public var endIndex: Index {
    return Index(base1: _sequence1.endIndex, base2: _sequence2.endIndex)
  }
  
  /// Constructs an `Index` from its parts, returning `endIndex` if necessary.
  @inlinable
  internal func _pack(
    _ base1: Sequence1.Index,
    _ base2: Sequence2.Index
  ) -> Index {
    return base1 == _sequence1.endIndex || base2 == _sequence2.endIndex
      ? endIndex
      : Index(base1: base1, base2: base2)
  }
  
  /// Destructs an `Index` into its parts.
  ///
  /// - Complexity: O(*n*) if `index == endIndex` and the collection does not
  ///   conform to `RandomAccessCollection`, O(1) otherwise.
  @inlinable
  internal func _unpack(_ index: Index) -> (Sequence1.Index, Sequence2.Index) {
    if index == endIndex {
      let count = self.count
      return (
        _sequence1.index(_sequence1.startIndex, offsetBy: count),
        _sequence2.index(_sequence2.startIndex, offsetBy: count))
    } else {
      return (index.base1, index.base2)
    }
  }
  
  @inlinable
  public func index(after i: Index) -> Index {
    return _pack(
      _sequence1.index(after: i.base1),
      _sequence2.index(after: i.base2))
  }
  
  @inlinable
  public subscript(position: Index) -> Element {
    return (_sequence1[position.base1], _sequence2[position.base2])
  }
  
  @inlinable
  public subscript(bounds: Range<Index>) -> SubSequence {
    SubSequence(
      _sequence1[bounds.lowerBound.base1..<bounds.upperBound.base1],
      _sequence2[bounds.lowerBound.base2..<bounds.upperBound.base2])
  }
  
  @inlinable
  public func index(_ i: Index, offsetBy distance: Int) -> Index {
    let (base1, base2) = distance >= 0 ? (i.base1, i.base2) : _unpack(i)
    return _pack(
      _sequence1.index(base1, offsetBy: distance),
      _sequence2.index(base2, offsetBy: distance))
  }
  
  @inlinable
  public var isEmpty: Bool {
    return _sequence1.isEmpty || _sequence2.isEmpty
  }
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension Zip2Sequence: BidirectionalCollection
  where Sequence1: BidirectionalCollection,
        Sequence2: BidirectionalCollection
{
  @inlinable
  public func index(before i: Index) -> Index {
    let (base1, base2) = _unpack(i)
    return Index(
      base1: _sequence1.index(before: base1),
      base2: _sequence2.index(before: base2))
  }
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension Zip2Sequence._Index: Comparable {
  @inlinable
  public static func == (lhs: Self, rhs: Self) -> Bool {
    return lhs.base1 == rhs.base1
  }

  @inlinable
  public static func < (lhs: Self, rhs: Self) -> Bool {
    return lhs.base1 < rhs.base1
  }
}

/// Creates a sequence of pairs built out of two underlying sequences.
///
/// In the `Zip2Sequence` instance returned by this function, the elements of
/// the *i*th pair are the *i*th elements of each underlying sequence. The
/// following example uses the `zip(_:_:)` function to iterate over an array
/// of strings and a countable range at the same time:
///
///     let words = ["one", "two", "three", "four"]
///     let numbers = 1...4
///
///     for (word, number) in zip(words, numbers) {
///         print("\(word): \(number)")
///     }
///     // Prints "one: 1"
///     // Prints "two: 2
///     // Prints "three: 3"
///     // Prints "four: 4"
///
/// If the two sequences passed to `zip(_:_:)` are different lengths, the
/// resulting sequence is the same length as the shorter sequence. In this
/// example, the resulting array is the same length as `words`:
///
///     let naturalNumbers = 1...Int.max
///     let zipped = Array(zip(words, naturalNumbers))
///     // zipped == [("one", 1), ("two", 2), ("three", 3), ("four", 4)]
///
/// - Parameters:
///   - sequence1: The first sequence or collection to zip.
///   - sequence2: The second sequence or collection to zip.
/// - Returns: A sequence of tuple pairs, where the elements of each pair are
///   corresponding elements of `sequence1` and `sequence2`.
@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
@inlinable
public func zip<Base1: RandomAccessCollection, Base2: RandomAccessCollection>(
  _ base1: Base1, _ base2: Base2
) -> Zip2RandomAccessCollection<Base1, Base2> {
  return Zip2RandomAccessCollection(base1, base2)
}

/// A sequence of pairs built out of two underlying sequences.
///
/// In a `Zip2Sequence` instance, the elements of the *i*th pair are the *i*th
/// elements of each underlying sequence. To create a `Zip2Sequence` instance,
/// use the `zip(_:_:)` function.
///
/// The following example uses the `zip(_:_:)` function to iterate over an
/// array of strings and a countable range at the same time:
///
///     let words = ["one", "two", "three", "four"]
///     let numbers = 1...4
///
///     for (word, number) in zip(words, numbers) {
///         print("\(word): \(number)")
///     }
///     // Prints "one: 1"
///     // Prints "two: 2
///     // Prints "three: 3"
///     // Prints "four: 4"
@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
@frozen
public struct Zip2RandomAccessCollection<Base1, Base2>
  where Base1: RandomAccessCollection, Base2: RandomAccessCollection
{
  @usableFromInline
  internal let _base1: Base1
  @usableFromInline
  internal let _base2: Base2

  /// Creates an instance that makes pairs of elements from `base1` and `base2`.
  @inlinable // generic-performance
  internal init(_ base1: Base1, _ base2: Base2) {
    (_base1, _base2) = (base1, base2)
  }
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension Zip2RandomAccessCollection: RandomAccessCollection {
  @frozen
  public struct _Index {
    /// The position in the first underlying collection.
    public let base1: Base1.Index
    
    /// The position in the second underlying collection.
    public let base2: Base2.Index
    
    @inlinable
    init(base1: Base1.Index, base2: Base2.Index) {
      self.base1 = base1
      self.base2 = base2
    }
  }
  
  // Ensures that `Self.Index == SubSequence.Index`.
  public typealias Index = SubSequence._Index
  public typealias SubSequence =
    Zip2RandomAccessCollection<Base1.SubSequence, Base2.SubSequence>
  
  @inlinable
  public var startIndex: Index {
    return isEmpty
      ? endIndex
      : Index(base1: _base1.startIndex, base2: _base2.startIndex)
  }
  
  @inlinable
  public var endIndex: Index {
    return Index(base1: _base1.endIndex, base2: _base2.endIndex)
  }
  
  /// Constructs an `Index` from its parts, returning `endIndex` if necessary.
  @inlinable
  internal func _pack(_ base1: Base1.Index, _ base2: Base2.Index) -> Index {
    return base1 == _base1.endIndex || base2 == _base2.endIndex
      ? endIndex
      : Index(base1: base1, base2: base2)
  }
  
  /// Destructs an `Index` into its parts.
  ///
  /// - Complexity: O(1)
  @inlinable
  internal func _unpack(_ index: Index) -> (Base1.Index, Base2.Index) {
    if index == endIndex {
      let count = self.count
      return (
        _base1.index(_base1.startIndex, offsetBy: count),
        _base2.index(_base2.startIndex, offsetBy: count))
    } else {
      return (index.base1, index.base2)
    }
  }
  
  @inlinable
  public func index(after i: Index) -> Index {
    return _pack(
      _base1.index(after: i.base1),
      _base2.index(after: i.base2))
  }
  
  @inlinable
  public func index(before i: Index) -> Index {
    return _pack(
      _base1.index(before: i.base1),
      _base2.index(before: i.base2))
  }
  
  @inlinable
  public subscript(position: Index) -> (Base1.Element, Base2.Element) {
    return (_base1[position.base1], _base2[position.base2])
  }
  
  @inlinable
  public subscript(bounds: Range<Index>) -> SubSequence {
    SubSequence(
      _base1[bounds.lowerBound.base1..<bounds.upperBound.base1],
      _base2[bounds.lowerBound.base2..<bounds.upperBound.base2])
  }
  
  @inlinable
  public func index(_ i: Index, offsetBy distance: Int) -> Index {
    let (base1, base2) = distance >= 0 ? (i.base1, i.base2) : _unpack(i)
    return _pack(
      _base1.index(base1, offsetBy: distance),
      _base2.index(base2, offsetBy: distance))
  }
  
  @inlinable
  public func index(
    _ i: Index,
    offsetBy distance: Int,
    limitedBy limit: Index
  ) -> Index? {
    let (base1, base2) = distance >= 0 ? (i.base1, i.base2) : _unpack(i)
    guard 
      let newBase1 = _base1.index(
        base1,
        offsetBy: distance,
        limitedBy: limit.base1),
      let newBase2 = _base2.index(
        base2,
        offsetBy: distance,
        limitedBy: limit.base2)
    else { return nil }
    return _pack(newBase1, newBase2)
  }
  
  @inlinable
  public func distance(from start: Index, to end: Index) -> Int {
    guard start <= end else { return -distance(from: end, to: start) }
    return Swift.min(
      _base1.distance(from: start.base1, to: end.base1),
      _base2.distance(from: start.base2, to: end.base2)
    )
  }
  
  @inlinable
  public var count: Int {
    return Swift.min(_base1.count, _base2.count)
  }
  
  @inlinable
  public var isEmpty: Bool {
    return _base1.isEmpty || _base2.isEmpty
  }
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension Zip2RandomAccessCollection._Index: Comparable {
  @inlinable
  public static func == (lhs: Self, rhs: Self) -> Bool {
    return lhs.base1 == rhs.base1
  }

  @inlinable
  public static func < (lhs: Self, rhs: Self) -> Bool {
    return lhs.base1 < rhs.base1
  }
}
