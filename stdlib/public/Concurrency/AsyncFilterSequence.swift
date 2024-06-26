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
  /// Creates an asynchronous sequence that contains, in order, the elements of
  /// the base sequence that satisfy the given predicate.
  ///
  /// In this example, an asynchronous sequence called `Counter` produces `Int`
  /// values from `1` to `10`. The `filter(_:)` method returns `true` for even
  /// values and `false` for odd values, thereby filtering out the odd values:
  ///
  ///     let stream = Counter(howHigh: 10)
  ///         .filter { $0 % 2 == 0 }
  ///     for await number in stream {
  ///         print(number, terminator: " ")
  ///     }
  ///     // Prints "2 4 6 8 10 "
  ///
  /// - Parameter isIncluded: A closure that takes an element of the
  ///   asynchronous sequence as its argument and returns a Boolean value
  ///   that indicates whether to include the element in the filtered sequence.
  /// - Returns: An asynchronous sequence that contains, in order, the elements
  ///   of the base sequence that satisfy the given predicate.
  @preconcurrency 
  @inlinable
  public __consuming func filter(
    _ isIncluded: @Sendable @escaping (Element) async -> Bool
  ) -> AsyncFilterSequence<Self> {
    return AsyncFilterSequence(self, isIncluded: isIncluded)
  }
}

/// An asynchronous sequence that contains, in order, the elements of
/// the base sequence that satisfy a given predicate.
@available(SwiftStdlib 5.1, *)
public struct AsyncFilterSequence<Base: AsyncSequence> {
  @usableFromInline
  let base: Base

  @usableFromInline
  let isIncluded: (Element) async -> Bool

  @usableFromInline
  init(
    _ base: Base, 
    isIncluded: @escaping (Base.Element) async -> Bool
  ) {
    self.base = base
    self.isIncluded = isIncluded
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncFilterSequence: AsyncSequence {
  /// The type of element produced by this asynchronous sequence.
  ///
  /// The filter sequence produces whatever type of element its base
  /// sequence produces.
  public typealias Element = Base.Element
  /// The type of the error that can be produced by the sequence.
  ///
  /// The filter sequence produces whatever type of error its
  /// base sequence does.
  @available(SwiftStdlib 6.0, *)
  public typealias Failure = Base.Failure
  /// The type of iterator that produces elements of the sequence.
  public typealias AsyncIterator = Iterator

  /// The iterator that produces elements of the filter sequence.
  public struct Iterator: AsyncIteratorProtocol {
    // FIXME: Remove when $AssociatedTypeImplements is no longer needed
    @available(SwiftStdlib 6.0, *)
    public typealias Failure = Base.Failure

    @usableFromInline
    var baseIterator: Base.AsyncIterator

    @usableFromInline
    let isIncluded: (Base.Element) async -> Bool

    @usableFromInline
    init(
      _ baseIterator: Base.AsyncIterator,
      isIncluded: @escaping (Base.Element) async -> Bool
    ) {
      self.baseIterator = baseIterator
      self.isIncluded = isIncluded
    }

    /// Produces the next element in the filter sequence.
    ///
    /// This iterator calls `next()` on its base iterator; if this call returns
    /// `nil`, `next()` returns nil. Otherwise, `next()` evaluates the
    /// result with the `predicate` closure. If the closure returns `true`,
    /// `next()` returns the received element; otherwise it awaits the next
    /// element from the base iterator.
    @inlinable
    public mutating func next() async rethrows -> Base.Element? {
      while true {
        guard let element = try await baseIterator.next() else {
          return nil
        }
        if await isIncluded(element) {
          return element
        }
      }
    }

    /// Produces the next element in the filter sequence.
    ///
    /// This iterator calls `next(isolation:)` on its base iterator; if this
    /// call returns `nil`, `next(isolation:)` returns nil. Otherwise,
    /// `next(isolation:)` evaluates the result with the `predicate` closure. If
    /// the closure returns `true`, `next(isolation:)` returns the received
    /// element; otherwise it awaits the next element from the base iterator.
    @available(SwiftStdlib 6.0, *)
    @inlinable
    public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> Base.Element? {
      while true {
        guard let element = try await baseIterator.next(isolation: actor) else {
          return nil
        }
        if await isIncluded(element) {
          return element
        }
      }
    }
  }

  @inlinable
  public __consuming func makeAsyncIterator() -> Iterator {
    return Iterator(base.makeAsyncIterator(), isIncluded: isIncluded)
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncFilterSequence: @unchecked Sendable 
  where Base: Sendable, 
        Base.Element: Sendable { }

@available(*, unavailable)
extension AsyncFilterSequence.Iterator: Sendable { }

