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
  /// Returns an asynchronous sequence, up to the specified maximum length,
  /// containing the initial elements of the base asynchronous sequence.
  ///
  /// Use `prefix(_:)` to reduce the number of elements produced by the
  /// asynchronous sequence.
  ///
  /// In this example, an asynchronous sequence called `Counter` produces `Int`
  /// values from `1` to `10`. The `prefix(_:)` method causes the modified
  /// sequence to pass through the first six values, then end.
  ///
  ///     for await number in Counter(howHigh: 10).prefix(6) {
  ///         print(number, terminator: " ")
  ///     }
  ///     // Prints "1 2 3 4 5 6 "
  ///
  /// If the count passed to `prefix(_:)` exceeds the number of elements in the
  /// base sequence, the result contains all of the elements in the sequence.
  ///
  /// - Parameter count: The maximum number of elements to return. The value of
  ///   `count` must be greater than or equal to zero.
  /// - Returns: An asynchronous sequence starting at the beginning of the
  ///   base sequence with at most `count` elements.
  @inlinable
  public __consuming func prefix(
    _ count: Int
  ) -> AsyncPrefixSequence<Self> {
    precondition(count >= 0,
      "Can't prefix a negative number of elements from an async sequence")
    return AsyncPrefixSequence(self, count: count)
  }
}

/// An asynchronous sequence, up to a specified maximum length,
/// containing the initial elements of a base asynchronous sequence.
@available(SwiftStdlib 5.1, *)
public struct AsyncPrefixSequence<Base: AsyncSequence> {
  @usableFromInline
  let base: Base

  @usableFromInline
  let count: Int

  @usableFromInline
  init(_ base: Base, count: Int) {
    self.base = base
    self.count = count
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncPrefixSequence: AsyncSequence {
  /// The type of element produced by this asynchronous sequence.
  ///
  /// The prefix sequence produces whatever type of element its base iterator
  /// produces.
  public typealias Element = Base.Element
  /// The type of the error that can be produced by the sequence.
  ///
  /// The prefix sequence produces whatever type of error its
  /// base sequence does.
  @available(SwiftStdlib 6.0, *)
  public typealias Failure = Base.Failure
  /// The type of iterator that produces elements of the sequence.
  public typealias AsyncIterator = Iterator

  /// The iterator that produces elements of the prefix sequence.
  public struct Iterator: AsyncIteratorProtocol {
    // FIXME: Remove when $AssociatedTypeImplements is no longer needed
    @available(SwiftStdlib 6.0, *)
    public typealias Failure = Base.Failure

    @usableFromInline
    var baseIterator: Base.AsyncIterator

    @usableFromInline
    var remaining: Int

    @usableFromInline
    init(_ baseIterator: Base.AsyncIterator, count: Int) {
      self.baseIterator = baseIterator
      self.remaining = count
    }

    /// Produces the next element in the prefix sequence.
    ///
    /// Until reaching the number of elements to include, this iterator calls
    /// `next()` on its base iterator and passes through the result. After
    /// reaching the maximum number of elements, subsequent calls to `next()`
    /// return `nil`.
    @inlinable
    public mutating func next() async rethrows -> Base.Element? {
      if remaining != 0 {
        remaining &-= 1
        return try await baseIterator.next()
      } else {
        return nil
      }
    }

    /// Produces the next element in the prefix sequence.
    ///
    /// Until reaching the number of elements to include, this iterator calls
    /// `next(isolation:)` on its base iterator and passes through the
    /// result. After reaching the maximum number of elements, subsequent calls
    /// to `next(isolation:)` return `nil`.
    @available(SwiftStdlib 6.0, *)
    @inlinable
    public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> Base.Element? {
      if remaining != 0 {
        remaining &-= 1
        return try await baseIterator.next(isolation: actor)
      } else {
        return nil
      }
    }
  }

  @inlinable
  public __consuming func makeAsyncIterator() -> Iterator {
    return Iterator(base.makeAsyncIterator(), count: count)
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncPrefixSequence: Sendable 
  where Base: Sendable, 
        Base.Element: Sendable { }

@available(*, unavailable)
extension AsyncPrefixSequence.Iterator: Sendable { }
