//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

public protocol CxxSet<Element> {
  associatedtype Element
  associatedtype Size: BinaryInteger

  // std::pair<iterator, bool> for std::set and std::unordered_set
  // iterator for std::multiset
  associatedtype InsertionResult 

  init()

  @discardableResult
  mutating func __insertUnsafe(_ element: Element) -> InsertionResult

  func count(_ element: Element) -> Size
}

extension CxxSet {
  /// Creates a C++ set containing the elements of a Swift Sequence.
  ///
  /// This initializes the set by copying every element of the sequence.
  ///
  /// - Complexity: O(*n*), where *n* is the number of elements in the Swift
  ///   sequence
  @inlinable
  public init<S: Sequence>(_ sequence: __shared S) where S.Element == Element {
    self.init()
    for item in sequence {
      self.__insertUnsafe(item)
    }
  }

  @inlinable
  public func contains(_ element: Element) -> Bool {
    return count(element) > 0
  }
}

public protocol CxxUniqueSet<Element>: CxxSet {
  override associatedtype Element
  override associatedtype Size: BinaryInteger
  associatedtype RawMutableIterator: UnsafeCxxInputIterator
    where RawMutableIterator.Pointee == Element
  override associatedtype InsertionResult
    where InsertionResult: CxxPair<RawMutableIterator, Bool>
}

extension CxxUniqueSet {
  @inlinable
  @discardableResult
  public mutating func insert(
    _ element: Element
  ) -> (inserted: Bool, memberAfterInsert: Element) {
    let insertionResult = self.__insertUnsafe(element)
    let rawIterator: RawMutableIterator = insertionResult.first
    let inserted: Bool = insertionResult.second
    return (inserted, rawIterator.pointee)
  }
}
