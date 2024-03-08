//===--- CollectionOfOne.swift - A Collection with one element ------------===//
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

/// A collection containing a single element.
///
/// You can use a `CollectionOfOne` instance when you need to efficiently
/// represent a single value as a collection. For example, you can add a
/// single element to an array by using a `CollectionOfOne` instance with the
/// concatenation operator (`+`):
///
///     let a = [1, 2, 3, 4]
///     let toAdd = 100
///     let b = a + CollectionOfOne(toAdd)
///     // b == [1, 2, 3, 4, 100]
@frozen // trivial-implementation
public struct CollectionOfOne<Element> {
  @usableFromInline // trivial-implementation
  internal var _element: Element

  /// Creates an instance containing just the given element.
  ///
  /// - Parameter element: The element to store in the collection.
  @inlinable // trivial-implementation
  public init(_ element: Element) {
    self._element = element
  }
}

extension CollectionOfOne {
  /// An iterator that produces one or zero instances of an element.
  ///
  /// `IteratorOverOne` is the iterator for the `CollectionOfOne` type.
  @frozen // trivial-implementation
  public struct Iterator {
    @usableFromInline // trivial-implementation
    internal var _elements: Element?

    /// Construct an instance that generates `_element!`, or an empty
    /// sequence if `_element == nil`.
    @inlinable // trivial-implementation
    public // @testable
    init(_elements: Element?) {
      self._elements = _elements
    }
  }
}

extension CollectionOfOne.Iterator: IteratorProtocol {
  /// Advances to the next element and returns it, or `nil` if no next element
  /// exists.
  ///
  /// Once `nil` has been returned, all subsequent calls return `nil`.
  ///
  /// - Returns: The next element in the underlying sequence, if a next element
  ///   exists; otherwise, `nil`.
  @inlinable // trivial-implementation
  public mutating func next() -> Element? {
    let result = _elements
    _elements = nil
    return result
  }
}

extension CollectionOfOne: Sequence {
  @inlinable // trivial-implementation
  public func withContiguousStorageIfAvailable<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    return try withUnsafePointer(to: _element) {
      let buf = UnsafeBufferPointer(start: $0, count: 1)
      return try body(buf)
    }
  }
}

extension CollectionOfOne: RandomAccessCollection, MutableCollection {

  public typealias Index = Int
  public typealias Indices = Range<Int>
  public typealias SubSequence = Slice<CollectionOfOne<Element>>

  /// The position of the first element.
  ///
  /// In a `CollectionOfOne` instance, `startIndex` is always `0`.
  @inlinable // trivial-implementation
  public var startIndex: Index {
    return 0
  }

  /// The "past the end" position---that is, the position one greater than the
  /// last valid subscript argument.
  ///
  /// In a `CollectionOfOne` instance, `endIndex` is always `1`.
  @inlinable // trivial-implementation
  public var endIndex: Index {
    return 1
  }
  
  /// Returns the position immediately after the given index.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be `0`.
  /// - Returns: The index value immediately after `i`.
  @inlinable // trivial-implementation
  public func index(after i: Index) -> Index {
    _precondition(i == startIndex)
    return 1
  }

  /// Returns the position immediately before the given index.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be `1`.
  /// - Returns: The index value immediately before `i`.
  @inlinable // trivial-implementation
  public func index(before i: Index) -> Index {
    _precondition(i == endIndex)
    return 0
  }

  /// Returns an iterator over the elements of this collection.
  ///
  /// - Complexity: O(1)
  @inlinable // trivial-implementation
  public __consuming func makeIterator() -> Iterator {
    return Iterator(_elements: _element)
  }

  /// Accesses the element at the specified position.
  ///
  /// - Parameter position: The position of the element to access. The only
  ///   valid position in a `CollectionOfOne` instance is `0`.
  @inlinable // trivial-implementation
  public subscript(position: Int) -> Element {
    _read {
      _precondition(position == 0, "Index out of range")
      yield _element
    }
    _modify {
      _precondition(position == 0, "Index out of range")
      yield &_element
    }
  }

  @inlinable // trivial-implementation
  public subscript(bounds: Range<Int>) -> SubSequence {
    get {
      _failEarlyRangeCheck(bounds, bounds: 0..<1)
      return Slice(base: self, bounds: bounds)
    }
    set {
      _failEarlyRangeCheck(bounds, bounds: 0..<1)
      let n = newValue.count
      _precondition(bounds.count == n, "CollectionOfOne can't be resized")
      if n == 1 { self = newValue.base }
    }
  }

  /// The number of elements in the collection, which is always one.
  @inlinable // trivial-implementation
  public var count: Int {
    return 1
  }
}

@_unavailableInEmbedded
extension CollectionOfOne: CustomDebugStringConvertible {
  /// A textual representation of the collection, suitable for debugging.
  public var debugDescription: String {
    return "CollectionOfOne(\(String(reflecting: _element)))"
  }
}

#if SWIFT_ENABLE_REFLECTION
extension CollectionOfOne: CustomReflectable {
  public var customMirror: Mirror {
    return Mirror(self, children: ["element": _element])
  }
}
#endif

extension CollectionOfOne: Sendable where Element: Sendable { }
extension CollectionOfOne.Iterator: Sendable where Element: Sendable { }
