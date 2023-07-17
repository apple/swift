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

/// A C++ type that represents a set of values, which might be repeating.
///
/// C++ standard library types such as `std::set`, `std::unordered_set` and
/// `std::multiset` conform to this protocol.
///
/// - SeeAlso: `CxxUniqueSet`
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

  /// Returns a Boolean value that indicates whether the given element exists
  /// in the set.
  @inlinable
  public func contains(_ element: Element) -> Bool {
    return count(element) > 0
  }
}

/// A C++ type that represents a set of unique values.
///
/// C++ standard library types such as `std::set` and `std::unordered_set`
/// conform to this protocol.
public protocol CxxUniqueSet<Element>: CxxSet {
  override associatedtype Element
  override associatedtype Size: BinaryInteger
  associatedtype RawMutableIterator: UnsafeCxxInputIterator
    where RawMutableIterator.Pointee == Element
  override associatedtype InsertionResult
    where InsertionResult: CxxPair<RawMutableIterator, Bool>
}

extension CxxUniqueSet {
  /// Inserts the given element in the set if it is not already present.
  ///
  /// If an element equal to `newMember` is already contained in the set, this
  /// method has no effect.
  ///
  /// - Parameter newMember: An element to insert into the set.
  /// - Returns: `(true, newMember)` if `newMember` was not contained in the
  ///   set. If an element equal to `newMember` was already contained in the
  ///   set, the method returns `(false, oldMember)`, where `oldMember` is the
  ///   element that was equal to `newMember`.
  @inlinable
  @discardableResult
  public mutating func insert(
    _ newMember: Element
  ) -> (inserted: Bool, memberAfterInsert: Element) {
    let insertionResult = self.__insertUnsafe(newMember)
    let rawIterator: RawMutableIterator = insertionResult.first
    let inserted: Bool = insertionResult.second
    return (inserted, rawIterator.pointee)
  }
}
