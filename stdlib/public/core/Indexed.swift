//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// A collection wrapper that iterates over the indices and elements of a
/// collection together.
@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
@frozen
public struct IndexedCollection<Base: Collection> {
  @usableFromInline
  internal let _base: Base
  
  @inlinable
  internal init(_base: Base) {
    self._base = _base
  }
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension Indexed: Collection {
extension IndexedCollection: Collection {
  public typealias Element = (index: Base.Index, element: Base.Element)
  public typealias Index = Base.Index
  public typealias Indices = Base.Indices
  public typealias SubSequence = IndexedCollection<Base.SubSequence>

  @inlinable
  public var startIndex: Index {
    _base.startIndex
  }
  
  @inlinable
  public var endIndex: Index {
    _base.endIndex
  }
  
  @inlinable
  public subscript(position: Index) -> Element {
    (index: position, element: _base[position])
  }
  
  @inlinable
  public func index(after i: Index) -> Index {
    _base.index(after: i)
  }
  
  @inlinable
  public func index(_ i: Index, offsetBy distance: Int) -> Index {
    _base.index(i, offsetBy: distance)
  }
  
  @inlinable
  public func index(
    _ i: Index,
    offsetBy distance: Int,
    limitedBy limit: Index
  ) -> Index? {
    _base.index(i, offsetBy: distance, limitedBy: limit)
  }
  
  @inlinable
  public func distance(from start: Index, to end: Index) -> Int {
    _base.distance(from: start, to: end)
  }
  
  @inlinable
  public var indices: Indices {
    _base.indices
  }
  
  @inlinable
  public subscript(bounds: Range<Index>) -> SubSequence {
    SubSequence(_base: _base[bounds])
  }
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension Indexed: BidirectionalCollection where Base: BidirectionalCollection {
extension IndexedCollection: BidirectionalCollection where Base: BidirectionalCollection {
  @inlinable
  public func index(before i: Index) -> Index {
    _base.index(before: i)
  }
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension Indexed: RandomAccessCollection where Base: RandomAccessCollection {}
extension IndexedCollection: RandomAccessCollection where Base: RandomAccessCollection {}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension Indexed: LazySequenceProtocol where Base: LazySequenceProtocol {}
extension IndexedCollection: LazySequenceProtocol where Base: LazySequenceProtocol {}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension Indexed: LazyCollectionProtocol where Base: LazyCollectionProtocol {}
extension IndexedCollection: LazyCollectionProtocol where Base: LazyCollectionProtocol {}

extension Collection {
  /// Returns a collection of pairs *(i, x)*, where *i* represents an index of
  /// the collection, and *x* represents an element.
  ///
  /// This example iterates over the indices and elements of a set, building an
  /// array consisting of indices of names with five or fewer letters.
  ///
  ///     let names: Set = ["Sofia", "Camilla", "Martina", "Mateo", "Nicolás"]
  ///     var shorterIndices: [Set<String>.Index] = []
  ///     for (i, name) in names.indexed() {
  ///         if name.count <= 5 {
  ///             shorterIndices.append(i)
  ///         }
  ///     }
  ///
  /// - Returns: A collection of paired indices and elements of this collection.
  ///
  /// - Complexity: O(1)
  @available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
  @inlinable
  public func indexed() -> IndexedCollection<Self> {
    IndexedCollection(_base: self)
  }
}
