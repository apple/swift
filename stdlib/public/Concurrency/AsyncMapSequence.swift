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

import Swift

@available(SwiftStdlib 5.1, *)
extension AsyncSequence {
  /// Creates an asynchronous sequence that maps the given closure over the
  /// asynchronous sequence’s elements.
  ///
  /// Use the `map(_:)` method to transform every element received from a base
  /// asynchronous sequence. Typically, you use this to transform from one type
  /// of element to another.
  ///
  /// In this example, an asynchronous sequence called `Counter` produces `Int`
  /// values from `1` to `5`. The closure provided to the `map(_:)` method
  /// takes each `Int` and looks up a corresponding `String` from a
  /// `romanNumeralDict` dictionary. This means the outer `for await in` loop
  /// iterates over `String` instances instead of the underlying `Int` values
  /// that `Counter` produces:
  ///
  ///     let romanNumeralDict: [Int: String] =
  ///         [1: "I", 2: "II", 3: "III", 5: "V"]
  ///
  ///     let stream = Counter(howHigh: 5)
  ///         .map { romanNumeralDict[$0] ?? "(unknown)" }
  ///     for await numeral in stream {
  ///         print(numeral, terminator: " ")
  ///     }
  ///     // Prints "I II III (unknown) V "
  ///
  /// - Parameter transform: A mapping closure. `transform` accepts an element
  ///   of this sequence as its parameter and returns a transformed value of the
  ///   same or of a different type.
  /// - Returns: An asynchronous sequence that contains, in order, the elements
  ///   produced by the `transform` closure.
  @preconcurrency 
  @inlinable
  public __consuming func map<Transformed>(
    _ transform: @Sendable @escaping (Element) async -> Transformed
  ) -> AsyncMapSequence<Self, Transformed> {
    return AsyncMapSequence(self, transform: transform)
  }
}

/// An asynchronous sequence that maps the given closure over the asynchronous
/// sequence’s elements.
@available(SwiftStdlib 5.1, *)
public struct AsyncMapSequence<Base: AsyncSequence, Transformed> {
  @usableFromInline
  let base: Base

  @usableFromInline
  let transform: (Base.Element) async -> Transformed

  @usableFromInline
  init(
    _ base: Base, 
    transform: @escaping (Base.Element) async -> Transformed
  ) {
    self.base = base
    self.transform = transform
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncMapSequence: AsyncSequence {
  /// The type of element produced by this asynchronous sequence.
  ///
  /// The map sequence produces whatever type of element its transforming
  /// closure produces.
  public typealias Element = Transformed
  /// The type of the error that can be produced by the sequence.
  ///
  /// The map sequence produces whatever type of error its
  /// base sequence does.
  @available(SwiftStdlib 6.0, *)
  public typealias Failure = Base.Failure
  /// The type of iterator that produces elements of the sequence.
  public typealias AsyncIterator = Iterator

  /// The iterator that produces elements of the map sequence.
  public struct Iterator: AsyncIteratorProtocol {
    // FIXME: Remove when $AssociatedTypeImplements is no longer needed
    @available(SwiftStdlib 6.0, *)
    public typealias Failure = Base.Failure

    @usableFromInline
    var baseIterator: Base.AsyncIterator

    @usableFromInline
    let transform: (Base.Element) async -> Transformed

    @usableFromInline
    init(
      _ baseIterator: Base.AsyncIterator, 
      transform: @escaping (Base.Element) async -> Transformed
    ) {
      self.baseIterator = baseIterator
      self.transform = transform
    }

    /// Produces the next element in the map sequence.
    ///
    /// This iterator calls `next()` on its base iterator; if this call returns
    /// `nil`, `next()` returns `nil`. Otherwise, `next()` returns the result of
    /// calling the transforming closure on the received element.
    @inlinable
    public mutating func next() async rethrows -> Transformed? {
      guard let element = try await baseIterator.next() else {
        return nil
      }
      return await transform(element)
    }

    /// Produces the next element in the map sequence.
    ///
    /// This iterator calls `next(isolation:)` on its base iterator; if this
    /// call returns `nil`, `next(isolation:)` returns `nil`. Otherwise,
    /// `next(isolation:)` returns the result of calling the transforming
    /// closure on the received element.
    @available(SwiftStdlib 6.0, *)
    @inlinable
    public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> Transformed? {
      guard let element = try await baseIterator.next(isolation: actor) else {
        return nil
      }
      return await transform(element)
    }
  }

  @inlinable
  public __consuming func makeAsyncIterator() -> Iterator {
    return Iterator(base.makeAsyncIterator(), transform: transform)
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncMapSequence: @unchecked Sendable 
  where Base: Sendable, 
        Base.Element: Sendable, 
        Transformed: Sendable { }

@available(*, unavailable)
extension AsyncMapSequence.Iterator: Sendable { }
