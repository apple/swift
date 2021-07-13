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

/// Returns the lesser of two comparable values.
///
/// - Parameters:
///   - x: A value to compare.
///   - y: Another value to compare.
/// - Returns: The lesser of `x` and `y`. If `x` is equal to `y`, returns `x`.
@inlinable // protocol-only
public func min<T: Comparable>(_ x: T, _ y: T) -> T {
  // In case `x == y` we pick `x`.
  // This preserves any pre-existing order in case `T` has identity,
  // which is important for e.g. the stability of sorting algorithms.
  // `(min(x, y), max(x, y))` should return `(x, y)` in case `x == y`.
  return y < x ? y : x
}

/// Returns the least argument passed.
///
/// - Parameters:
///   - x: A value to compare.
///   - y: Another value to compare.
///   - z: A third value to compare.
///   - rest: Zero or more additional values.
/// - Returns: The least of all the arguments. If there are multiple equal
///   least arguments, the result is the first one.
@inlinable // protocol-only
public func min<T: Comparable>(_ x: T, _ y: T, _ z: T, _ rest: T...) -> T {
  var minValue = min(min(x, y), z)
  // In case `value == minValue`, we pick `minValue`. See min(_:_:).
  for value in rest where value < minValue {
    minValue = value
  }
  return minValue
}

/// Returns the greater of two comparable values.
///
/// - Parameters:
///   - x: A value to compare.
///   - y: Another value to compare.
/// - Returns: The greater of `x` and `y`. If `x` is equal to `y`, returns `y`.
@inlinable // protocol-only
public func max<T: Comparable>(_ x: T, _ y: T) -> T {
  // In case `x == y`, we pick `y`. See min(_:_:).
  return y >= x ? y : x
}

/// Returns the greatest argument passed.
///
/// - Parameters:
///   - x: A value to compare.
///   - y: Another value to compare.
///   - z: A third value to compare.
///   - rest: Zero or more additional values.
/// - Returns: The greatest of all the arguments. If there are multiple equal
///   greatest arguments, the result is the last one.
@inlinable // protocol-only
public func max<T: Comparable>(_ x: T, _ y: T, _ z: T, _ rest: T...) -> T {
  var maxValue = max(max(x, y), z)
  // In case `value == maxValue`, we pick `value`. See min(_:_:).
  for value in rest where value >= maxValue {
    maxValue = value
  }
  return maxValue
}

/// An enumeration of the elements of a sequence or collection.
///
/// `EnumeratedSequence` is a sequence of pairs (*n*, *x*), where *n*s are
/// consecutive `Int` values starting at zero, and *x*s are the elements of a
/// base sequence.
///
/// To create an instance of `EnumeratedSequence`, call `enumerated()` on a
/// sequence or collection. The following example enumerates the elements of
/// an array.
///
///     var s = ["foo", "bar"].enumerated()
///     for (n, x) in s {
///         print("\(n): \(x)")
///     }
///     // Prints "0: foo"
///     // Prints "1: bar"
@frozen
public struct EnumeratedSequence<Base: Sequence> {
  @usableFromInline
  internal var _base: Base

  /// Construct from a `Base` sequence.
  @inlinable
  internal init(_base: Base) {
    self._base = _base
  }
}

extension EnumeratedSequence {
  /// The iterator for `EnumeratedSequence`.
  ///
  /// An instance of this iterator wraps a base iterator and yields
  /// successive `Int` values, starting at zero, along with the elements of the
  /// underlying base iterator. The following example enumerates the elements of
  /// an array:
  ///
  ///     var iterator = ["foo", "bar"].enumerated().makeIterator()
  ///     iterator.next() // (0, "foo")
  ///     iterator.next() // (1, "bar")
  ///     iterator.next() // nil
  ///
  /// To create an instance, call
  /// `enumerated().makeIterator()` on a sequence or collection.
  @frozen
  public struct Iterator {
    @usableFromInline
    internal var _base: Base.Iterator
    @usableFromInline
    internal var _count: Int

    /// Construct from a `Base` iterator.
    @inlinable
    internal init(_base: Base.Iterator) {
      self._base = _base
      self._count = 0
    }
  }
}

extension EnumeratedSequence.Iterator: IteratorProtocol, Sequence {
  /// The type of element returned by `next()`.
  public typealias Element = (offset: Int, element: Base.Element)

  /// Advances to the next element and returns it, or `nil` if no next element
  /// exists.
  ///
  /// Once `nil` has been returned, all subsequent calls return `nil`.
  @inlinable
  public mutating func next() -> Element? {
    guard let b = _base.next() else { return nil }
    let result = (offset: _count, element: b)
    _count += 1 
    return result
  }
}

extension EnumeratedSequence: Sequence {
  /// Returns an iterator over the elements of this sequence.
  @inlinable
  public __consuming func makeIterator() -> Iterator {
    return Iterator(_base: _base.makeIterator())
  }
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension EnumeratedSequence: Collection where Base: Collection {
  @frozen
  public struct Index {
    /// The position in the underlying collection.
    public let base: Base.Index
    
    /// The offset corresponding to this index when `base` is not the end index,
    /// `0` otherwise.
    @usableFromInline
    internal let offset: Int
    
    @inlinable
    internal init(base: Base.Index, offset: Int) {
      self.base = base
      self.offset = offset
    }
  }
  
  @inlinable
  public var startIndex: Index {
    return Index(base: _base.startIndex, offset: 0)
  }
  
  @inlinable
  public var endIndex: Index {
    return Index(base: _base.endIndex, offset: 0)
  }
  
  /// Returns the offset corresponding to `index`.
  ///
  /// - Complexity: O(*n*) if `index == endIndex` and `Base` does not conform to
  ///   `RandomAccessCollection`, O(1) otherwise.
  @inlinable
  internal func _offset(of index: Index) -> Int {
    return index.base == _base.endIndex ? _base.count : index.offset
  }
  
  @inlinable
  public func index(after index: Index) -> Index {
    precondition(index.base != _base.endIndex, "Cannot advance past endIndex")
    return Index(base: _base.index(after: index.base), offset: index.offset + 1)
  }
  
  @inlinable
  public subscript(position: Index) -> Element {
    (position.offset, _base[position.base])
  }
  
  @inlinable
  public func index(_ i: Index, offsetBy distance: Int) -> Index {
    let index = _base.index(i.base, offsetBy: distance)
    let offset = distance >= 0 ? i.offset : _offset(of: i)
    return Index(base: index, offset: offset + distance)
  }
  
  @inlinable
  public func index(
    _ i: Index,
    offsetBy distance: Int,
    limitedBy limit: Index
  ) -> Index? {
    guard let index = _base.index(
      i.base,
      offsetBy: distance,
      limitedBy: limit.base
    ) else { return nil }
    let offset = distance >= 0 ? i.offset : _offset(of: i)
    return Index(base: index, offset: offset + distance)
  }
  
  @inlinable
  public func distance(from start: Index, to end: Index) -> Int {
    if start.base == _base.endIndex || end.base == _base.endIndex {
      return _base.distance(from: start.base, to: end.base)
    } else {
      return end.offset - start.offset
    }
  }
  
  @inlinable
  public var count: Int {
    return _base.count
  }
  
  @inlinable
  public var isEmpty: Bool {
    return _base.isEmpty
  }
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension EnumeratedSequence: BidirectionalCollection
  where Base: BidirectionalCollection
{
  @inlinable
  public func index(before index: Index) -> Index {
    return Index(
      base: _base.index(before: index.base),
      offset: _offset(of: index) - 1)
  }
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension EnumeratedSequence: RandomAccessCollection
  where Base: RandomAccessCollection {}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension EnumeratedSequence: LazySequenceProtocol
  where Base: LazySequenceProtocol {}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension EnumeratedSequence: LazyCollectionProtocol
  where Base: LazyCollectionProtocol {}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension EnumeratedSequence.Index: Comparable {
  @inlinable
  public static func == (lhs: Self, rhs: Self) -> Bool {
    return lhs.base == rhs.base
  }
  
  @inlinable
  public static func < (lhs: Self, rhs: Self) -> Bool {
    return lhs.base < rhs.base
  }
}
