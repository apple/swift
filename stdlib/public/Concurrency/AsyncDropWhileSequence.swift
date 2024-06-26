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
  /// Omits elements from the base asynchronous sequence until a given closure
  /// returns false, after which it passes through all remaining elements.
  ///
  /// Use `drop(while:)` to omit elements from an asynchronous sequence until
  /// the element received meets a condition you specify.
  ///
  /// In this example, an asynchronous sequence called `Counter` produces `Int`
  /// values from `1` to `10`. The `drop(while:)` method causes the modified
  /// sequence to ignore received values until it encounters one that is
  /// divisible by `3`:
  ///
  ///     let stream = Counter(howHigh: 10)
  ///         .drop { $0 % 3 != 0 }
  ///     for await number in stream {
  ///         print(number, terminator: " ")
  ///     }
  ///     // Prints "3 4 5 6 7 8 9 10 "
  ///
  /// After the predicate returns `false`, the sequence never executes it again,
  /// and from then on the sequence passes through elements from its underlying
  /// sequence as-is.
  ///
  /// - Parameter predicate: A closure that takes an element as a parameter and
  ///   returns a Boolean value indicating whether to drop the element from the
  ///   modified sequence.
  /// - Returns: An asynchronous sequence that skips over values from the
  ///   base sequence until the provided closure returns `false`.
  @preconcurrency 
  @inlinable
  public __consuming func drop(
    while predicate: @Sendable @escaping (Element) async -> Bool
  ) -> AsyncDropWhileSequence<Self> {
    AsyncDropWhileSequence(self, predicate: predicate)
  }
}

/// An asynchronous sequence which omits elements from the base sequence until a
/// given closure returns false, after which it passes through all remaining
/// elements.
@available(SwiftStdlib 5.1, *)
public struct AsyncDropWhileSequence<Base: AsyncSequence> {
  @usableFromInline
  let base: Base

  @usableFromInline
  let predicate: (Base.Element) async -> Bool

  @usableFromInline
  init(
    _ base: Base, 
    predicate: @escaping (Base.Element) async -> Bool
  ) {
    self.base = base
    self.predicate = predicate
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncDropWhileSequence: AsyncSequence {
  
  /// The type of element produced by this asynchronous sequence.
  ///
  /// The drop-while sequence produces whatever type of element its base
  /// sequence produces.
  public typealias Element = Base.Element
  /// The type of errors produced by this asynchronous sequence.
  ///
  /// The drop-while sequence produces whatever type of error its base
  /// sequence produces.
  @available(SwiftStdlib 6.0, *)
  public typealias Failure = Base.Failure
  /// The type of iterator that produces elements of the sequence.
  public typealias AsyncIterator = Iterator

  /// The iterator that produces elements of the drop-while sequence.
  public struct Iterator: AsyncIteratorProtocol {
    // FIXME: Remove when $AssociatedTypeImplements is no longer needed
    @available(SwiftStdlib 6.0, *)
    public typealias Failure = Base.Failure

    @usableFromInline
    var baseIterator: Base.AsyncIterator

    @usableFromInline
    var predicate: ((Base.Element) async -> Bool)?

    @usableFromInline
    init(
      _ baseIterator: Base.AsyncIterator, 
      predicate: @escaping (Base.Element) async -> Bool
    ) {
      self.baseIterator = baseIterator
      self.predicate = predicate
    }

    /// Produces the next element in the drop-while sequence.
    ///
    /// This iterator calls `next()` on its base iterator and evaluates the
    /// result with the `predicate` closure. As long as the predicate returns
    /// `true`, this method returns `nil`. After the predicate returns `false`,
    /// for a value received from the base iterator, this method returns that
    /// value. After that, the iterator returns values received from its
    /// base iterator as-is, and never executes the predicate closure again.
    @inlinable
    public mutating func next() async rethrows -> Base.Element? {
      while let predicate = self.predicate {
        guard let element = try await baseIterator.next() else {
          return nil
        }
        if await predicate(element) == false {
          self.predicate = nil
          return element
        }
      }
      return try await baseIterator.next()
    }

    /// Produces the next element in the drop-while sequence.
    ///
    /// This iterator calls `next(isolation:)` on its base iterator and
    /// evaluates the result with the `predicate` closure. As long as the
    /// predicate returns `true`, this method returns `nil`. After the predicate
    /// returns `false`, for a value received from the base iterator, this
    /// method returns that value. After that, the iterator returns values
    /// received from its base iterator as-is, and never executes the predicate
    /// closure again.
    @available(SwiftStdlib 6.0, *)
    @inlinable
    public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> Base.Element? {
      while let predicate = self.predicate {
        guard let element = try await baseIterator.next(isolation: actor) else {
          return nil
        }
        if await predicate(element) == false {
          self.predicate = nil
          return element
        }
      }
      return try await baseIterator.next(isolation: actor)
    }
  }

  /// Creates an instance of the drop-while sequence iterator.
  @inlinable
  public __consuming func makeAsyncIterator() -> Iterator {
    return Iterator(base.makeAsyncIterator(), predicate: predicate)
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncDropWhileSequence: @unchecked Sendable 
  where Base: Sendable, 
        Base.Element: Sendable { }

@available(*, unavailable)
extension AsyncDropWhileSequence.Iterator: Sendable { }
