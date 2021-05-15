////===----------------------------------------------------------------------===//
////
//// This source file is part of the Swift.org open source project
////
//// Copyright (c) 2020 Apple Inc. and the Swift project authors
//// Licensed under Apache License v2.0 with Runtime Library Exception
////
//// See https://swift.org/LICENSE.txt for license information
//// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
////
////===----------------------------------------------------------------------===//

import Swift

extension Sequence {
  /// `async` version of `Sequence.map`.
  ///
  /// - SeeAlso: Sequence.map
  @inlinable
  public func map<T>(
    _ transform: (Element) async throws -> T
  ) async rethrows -> [T] {
    let initialCapacity = underestimatedCount
    var result = ContiguousArray<T>()
    result.reserveCapacity(initialCapacity)

    var iterator = self.makeIterator()

    // Add elements up to the initial capacity without checking for regrowth.
    for _ in 0..<initialCapacity {
      result.append(try await transform(iterator.next()!))
    }
    // Add remaining elements, if any.
    while let element = iterator.next() {
      result.append(try await transform(element))
    }
    return Array(result)
  }

  /// `async` version of `Sequence.filter`.
  ///
  /// - SeeAlso: Sequence.filter
  @inlinable
  public __consuming func filter(
    _ isIncluded: (Element) async throws -> Bool
  ) async rethrows -> [Element] {
    var result = ContiguousArray<Element>()

    var iterator = self.makeIterator()

    while let element = iterator.next() {
      if try await isIncluded(element) {
        result.append(element)
      }
    }

    return Array(result)
  }

  /// `async` version of `Sequence.forEach`.
  ///
  /// - SeeAlso: Sequence.forEach
  @_semantics("sequence.forEach")
  @inlinable
  public func forEach(
    _ body: (Element) async throws -> Void
  ) async rethrows {
    for element in self {
      try await body(element)
    }
  }

  /// `async` version of `Sequence.flatMap`.
  ///
  /// - SeeAlso: Sequence.flatMap
  @inlinable
  public func flatMap<SegmentOfResult: Sequence>(
    _ transform: (Element) async throws -> SegmentOfResult
  ) async rethrows -> [SegmentOfResult.Element] {
    var result: [SegmentOfResult.Element] = []
    for element in self {
      result.append(contentsOf: try await transform(element))
    }
    return result
  }

  /// `async` version of `Sequence.compactMap`.
  ///
  /// - SeeAlso: Sequence.compactMap
  @inlinable
  public func compactMap<ElementOfResult>(
    _ transform: (Element) async throws -> ElementOfResult?
  ) async rethrows -> [ElementOfResult] {
    var result: [ElementOfResult] = []
    for element in self {
      if let newElement = try await transform(element) {
        result.append(newElement)
      }
    }
    return result
  }

  /// `async` version of `Sequence.first`.
  ///
  /// - SeeAlso: Sequence.first
  @inlinable
  public func first(
    where predicate: (Element) async throws -> Bool
  ) async rethrows -> Element? {
    for element in self {
      if try await predicate(element) {
        return element
      }
    }
    return nil
  }

  /// `async` version of `Sequence.prefix`.
  ///
  /// - SeeAlso: Sequence.prefix
  @inlinable
  public __consuming func prefix(
    while predicate: (Element) async throws -> Bool
  ) async rethrows -> [Element] {
    var result = ContiguousArray<Element>()

    for element in self {
      guard try await predicate(element) else {
        break
      }
      result.append(element)
    }
    return Array(result)
  }
}
