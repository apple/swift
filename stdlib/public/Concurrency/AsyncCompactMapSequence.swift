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
  /// asynchronous sequence’s elements, omitting results that don't return a
  /// value.
  ///
  /// Use the `compactMap(_:)` method to transform every element received from
  /// a base asynchronous sequence, while also discarding any `nil` results
  /// from the closure. Typically, you use this to transform from one type of
  /// element to another.
  ///
  /// In this example, an asynchronous sequence called `Counter` produces `Int`
  /// values from `1` to `5`. The closure provided to the `compactMap(_:)`
  /// method takes each `Int` and looks up a corresponding `String` from a
  /// `romanNumeralDict` dictionary. Because there is no key for `4`, the closure
  /// returns `nil` in this case, which `compactMap(_:)` omits from the
  /// transformed asynchronous sequence.
  ///
  ///     let romanNumeralDict: [Int: String] =
  ///         [1: "I", 2: "II", 3: "III", 5: "V"]
  ///         
  ///     let stream = Counter(howHigh: 5)
  ///         .compactMap { romanNumeralDict[$0] }
  ///     for await numeral in stream {
  ///         print(numeral, terminator: " ")
  ///     }
  ///     // Prints "I II III V "
  ///
  /// - Parameter transform: A mapping closure. `transform` accepts an element
  ///   of this sequence as its parameter and returns a transformed value of the
  ///   same or of a different type.
  /// - Returns: An asynchronous sequence that contains, in order, the
  ///   non-`nil` elements produced by the `transform` closure.
  @preconcurrency
  @inlinable
  public __consuming func compactMap<ElementOfResult>(
    _ transform: @Sendable @escaping (Element) async -> ElementOfResult?
  ) -> AsyncCompactMapSequence<Self, ElementOfResult> {
    return AsyncCompactMapSequence(self, transform: transform)
  }
}

/// An asynchronous sequence that maps a given closure over the asynchronous
/// sequence’s elements, omitting results that don't return a value.
@available(SwiftStdlib 5.1, *)
public struct AsyncCompactMapSequence<Base: AsyncSequence, ElementOfResult> {
  @usableFromInline
  let base: Base

  @usableFromInline
  let transform: (Base.Element) async -> ElementOfResult?

  @usableFromInline
  init(
    _ base: Base, 
    transform: @escaping (Base.Element) async -> ElementOfResult?
  ) {
    self.base = base
    self.transform = transform
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncCompactMapSequence: AsyncSequence {
  /// The type of element produced by this asynchronous sequence.
  ///
  /// The compact map sequence produces whatever type of element its
  /// transforming closure produces.
  public typealias Element = ElementOfResult
  /// The type of the error that can be produced by the sequence.
  ///
  /// The compact map sequence produces whatever type of error its
  /// base sequence does.
  @available(SwiftStdlib 6.0, *)
  public typealias Failure = Base.Failure
  /// The type of iterator that produces elements of the sequence.
  public typealias AsyncIterator = Iterator

  /// The iterator that produces elements of the compact map sequence.
  public struct Iterator: AsyncIteratorProtocol {
    public typealias Element = ElementOfResult

    // FIXME: Remove when $AssociatedTypeImplements is no longer needed
    @available(SwiftStdlib 6.0, *)
    public typealias Failure = Base.Failure

    @usableFromInline
    var baseIterator: Base.AsyncIterator

    @usableFromInline
    let transform: (Base.Element) async -> ElementOfResult?

    @usableFromInline
    init(
      _ baseIterator: Base.AsyncIterator, 
      transform: @escaping (Base.Element) async -> ElementOfResult?
    ) {
      self.baseIterator = baseIterator
      self.transform = transform
    }

    /// Produces the next element in the compact map sequence.
    ///
    /// This iterator calls `next()` on its base iterator; if this call returns
    /// `nil`, `next()` returns `nil`. Otherwise, `next()` calls the
    /// transforming closure on the received element, returning it if the
    /// transform returns a non-`nil` value. If the transform returns `nil`,
    /// this method continues to wait for further elements until it gets one
    /// that transforms to a non-`nil` value.
    @inlinable
    public mutating func next() async rethrows -> ElementOfResult? {
      while true {
        guard let element = try await baseIterator.next() else {
          return nil
        }

        if let transformed = await transform(element) {
          return transformed
        }
      }
    }

    /// Produces the next element in the compact map sequence.
    ///
    /// This iterator calls `next()` on its base iterator; if this call returns
    /// `nil`, `next()` returns `nil`. Otherwise, `next()` calls the
    /// transforming closure on the received element, returning it if the
    /// transform returns a non-`nil` value. If the transform returns `nil`,
    /// this method continues to wait for further elements until it gets one
    /// that transforms to a non-`nil` value.
    @available(SwiftStdlib 6.0, *)
    @inlinable
    public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> ElementOfResult? {
      while true {
        guard let element = try await baseIterator.next(isolation: actor) else {
          return nil
        }

        if let transformed = await transform(element) {
          return transformed
        }
      }
    }
  }

  @inlinable
  public __consuming func makeAsyncIterator() -> Iterator {
    return Iterator(base.makeAsyncIterator(), transform: transform)
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncCompactMapSequence: @unchecked Sendable 
  where Base: Sendable, 
        Base.Element: Sendable, 
        ElementOfResult: Sendable { }

@available(*, unavailable)
extension AsyncCompactMapSequence.Iterator: Sendable { }
