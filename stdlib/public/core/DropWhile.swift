//===--- DropWhile.swift - Lazy views for drop(while:) --------*- swift -*-===//
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

/// A sequence whose elements consist of the elements that follow the initial
/// consecutive elements of some base sequence that satisfy a given predicate.
@frozen // lazy-performance
public struct LazyDropWhileSequence<Base: Sequence> {
  public typealias Element = Base.Element

  @usableFromInline // lazy-performance
  internal var _base: Base
  @usableFromInline // lazy-performance
  internal let _predicate: (Element) -> Bool

  /// Create an instance with elements `transform(x)` for each element
  /// `x` of base.
  @inlinable // lazy-performance
  internal init(_base: Base, predicate: @escaping (Element) -> Bool) {
    self._base = _base
    self._predicate = predicate
  }

}

extension LazyDropWhileSequence {
  /// An iterator over the elements traversed by a base iterator that follow the
  /// initial consecutive elements that satisfy a given predicate.
  ///
  /// This is the associated iterator for the `LazyDropWhileSequence`,
  /// `LazyDropWhileCollection`, and `LazyDropWhileBidirectionalCollection`
  /// types.
  @frozen // lazy-performance
  public struct Iterator {
    public typealias Element = Base.Element

    @usableFromInline // lazy-performance
    internal var _predicateHasFailed = false
    @usableFromInline // lazy-performance
    internal var _base: Base.Iterator
    @usableFromInline // lazy-performance
    internal let _predicate: (Element) -> Bool

    @inlinable // lazy-performance
    internal init(_base: Base.Iterator, predicate: @escaping (Element) -> Bool) {
      self._base = _base
      self._predicate = predicate
    }
  }
}

extension LazyDropWhileSequence.Iterator: IteratorProtocol {
  @inlinable // lazy-performance
  public mutating func next() -> Element? {
    // Once the predicate has failed for the first time, the base iterator
    // can be used for the rest of the elements.
    if _predicateHasFailed {
      return _base.next()
    }

    // Retrieve and discard elements from the base iterator until one fails
    // the predicate.
    while let nextElement = _base.next() {
      if !_predicate(nextElement) {
        _predicateHasFailed = true
        return nextElement
      }
    }
    return nil
  }  
}

extension LazyDropWhileSequence: Sequence {
  /// Returns an iterator over the elements of this sequence.
  ///
  /// - Complexity: O(1).
  @inlinable // lazy-performance
  @_eagerMove
  public __consuming func makeIterator() -> Iterator {
    return Iterator(_base: _base.makeIterator(), predicate: _predicate)
  }
}

extension LazyDropWhileSequence: LazySequenceProtocol {
  public typealias Elements = LazyDropWhileSequence
}

extension LazySequenceProtocol {
  /// Returns a lazy sequence that skips any initial elements that satisfy
  /// `predicate`.
  ///
  /// - Parameter predicate: A closure that takes an element of the sequence as
  ///   its argument and returns `true` if the element should be skipped or
  ///   `false` otherwise. Once `predicate` returns `false` it will not be
  ///   called again.
  @inlinable // lazy-performance
  @_eagerMove
  public __consuming func drop(
    while predicate: @escaping (Elements.Element) -> Bool
  ) -> LazyDropWhileSequence<Self.Elements> {
    return LazyDropWhileSequence(_base: self.elements, predicate: predicate)
  }
}

/// A lazy wrapper that includes the elements of an underlying
/// collection after any initial consecutive elements that satisfy a
/// predicate.
///
/// - Note: The performance of accessing `startIndex`, `first`, or any methods
///   that depend on `startIndex` depends on how many elements satisfy the
///   predicate at the start of the collection, and may not offer the usual
///   performance given by the `Collection` protocol. Be aware, therefore,
///   that general operations on lazy collections may not have the
///   documented complexity.
public typealias LazyDropWhileCollection<T: Collection> = LazyDropWhileSequence<T>

extension LazyDropWhileCollection: Collection {
  public typealias SubSequence = Slice<LazyDropWhileCollection<Base>>
  public typealias Index = Base.Index

  @inlinable // lazy-performance
  public var startIndex: Index {
    var index = _base.startIndex
    while index != _base.endIndex && _predicate(_base[index]) {
      _base.formIndex(after: &index)
    }
    return index
  }

  @inlinable // lazy-performance
  public var endIndex: Index {
    return _base.endIndex
  }

  @inlinable // lazy-performance
  public func index(after i: Index) -> Index {
    _precondition(i < _base.endIndex, "Can't advance past endIndex")
    return _base.index(after: i)
  }

  @inlinable // lazy-performance
  public subscript(position: Index) -> Element {
    return _base[position]
  }
}

extension LazyDropWhileCollection: BidirectionalCollection 
where Base: BidirectionalCollection {
  @inlinable // lazy-performance
  public func index(before i: Index) -> Index {
    _precondition(i > startIndex, "Can't move before startIndex")
    return _base.index(before: i)
  }
}

extension LazyDropWhileCollection: LazyCollectionProtocol { }
