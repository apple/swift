//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// A C++ type that can be converted to a Swift collection.
public protocol CxxConvertibleToCollection<Element> {
  associatedtype Element
  associatedtype RawIterator: UnsafeCxxInputIterator
    where RawIterator.Pointee == Element

  /// Do not implement this function manually in Swift.
  func __beginUnsafe() -> RawIterator

  /// Do not implement this function manually in Swift.
  func __endUnsafe() -> RawIterator
}

extension CxxConvertibleToCollection {
  @inlinable
  public func forEach(_ body: (RawIterator.Pointee) throws -> Void) rethrows {
    var rawIterator = __beginUnsafe()
    let endIterator = __endUnsafe()
    while rawIterator != endIterator {
      try body(rawIterator.pointee)
      rawIterator = rawIterator.successor()
    }
  }
}

// Break the ambiguity between Sequence.forEach and CxxConvertibleToCollection.forEach.
extension CxxConvertibleToCollection where Self: Sequence {
  @inlinable
  public func forEach(_ body: (Element) throws -> Void) rethrows {
    for element in self {
      try body(element)
    }
  }
}

extension RangeReplaceableCollection {
  /// Creates a collection containing the elements of a C++ container.
  ///
  /// This initializes the collection by copying every element of the C++
  /// container.
  ///
  /// - Complexity: O(*n*), where *n* is the number of elements in the C++
  ///   container when each element is copied in O(1). Note that this might not
  ///   be true for certain C++ types, e.g. those with a custom copy
  ///   constructor that performs additional logic.
  @inlinable
  public init<C: CxxConvertibleToCollection>(_ elements: __shared C)
    where C.RawIterator.Pointee == Element {

    self.init()
    elements.forEach { self.append($0) }
  }
}

extension SetAlgebra {
  /// Creates a set containing the elements of a C++ container.
  ///
  /// This initializes the set by copying every element of the C++ container.
  ///
  /// - Complexity: O(*n*), where *n* is the number of elements in the C++
  ///   container when each element is copied in O(1). Note that this might not
  ///   be true for certain C++ types, e.g. those with a custom copy
  ///   constructor that performs additional logic.
  @inlinable
  public init<C: CxxConvertibleToCollection>(_ elements: __shared C)
    where C.RawIterator.Pointee == Element {

    self.init()
    elements.forEach { self.insert($0) }
  }
}
