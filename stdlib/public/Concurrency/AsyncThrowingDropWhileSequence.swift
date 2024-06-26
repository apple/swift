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
  /// Omits elements from the base sequence until a given error-throwing closure
  /// returns false, after which it passes through all remaining elements.
  ///
  /// Use `drop(while:)` to omit elements from an asynchronous sequence until
  /// the element received meets a condition you specify. If the closure you
  /// provide throws an error, the sequence produces no elements and throws
  /// the error instead.
  ///
  /// In this example, an asynchronous sequence called `Counter` produces `Int`
  /// values from `1` to `10`. The predicate passed to the `drop(while:)`
  /// method throws an error if it encounters an even number, and otherwise
  /// returns `true` while it receives elements less than `5`. Because the
  /// predicate throws when it receives `2` from the base sequence, this example
  /// throws without ever printing anything.
  ///
  ///     do {
  ///         let stream = Counter(howHigh: 10)
  ///             .drop {
  ///                 if $0 % 2 == 0 {
  ///                     throw EvenError()
  ///                 }
  ///                 return $0 < 5
  ///             }
  ///         for try await number in stream {
  ///             print(number)
  ///         }
  ///     } catch {
  ///         print(error)
  ///     }
  ///     // Prints "EvenError()"
  ///
  /// After the predicate returns `false`, the sequence never executes it again,
  /// and from then on the sequence passes through elements from its underlying
  /// sequence. A predicate that throws an error also never executes again.
  ///
  /// - Parameter predicate: An error-throwing closure that takes an element as
  ///   a parameter and returns a Boolean value indicating whether to drop the
  ///   element from the modified sequence.
  /// - Returns: An asynchronous sequence that skips over values until the
  ///   provided closure returns `false` or throws an error.
  @preconcurrency
  @inlinable
  public __consuming func drop(
    while predicate: @Sendable @escaping (Element) async throws -> Bool
  ) -> AsyncThrowingDropWhileSequence<Self> {
    AsyncThrowingDropWhileSequence(self, predicate: predicate)
  }
}

/// An asynchronous sequence which omits elements from the base sequence until a
/// given error-throwing closure returns false, after which it passes through
/// all remaining elements.
@available(SwiftStdlib 5.1, *)
public struct AsyncThrowingDropWhileSequence<Base: AsyncSequence> {
  @usableFromInline
  let base: Base

  @usableFromInline
  let predicate: (Base.Element) async throws -> Bool

  @usableFromInline
  init(
    _ base: Base, 
    predicate: @escaping (Base.Element) async throws -> Bool
  ) {
    self.base = base
    self.predicate = predicate
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncThrowingDropWhileSequence: AsyncSequence {
  /// The type of element produced by this asynchronous sequence.
  ///
  /// The drop-while sequence produces whatever type of element its base
  /// sequence produces.
  public typealias Element = Base.Element
  /// The type of element produced by this asynchronous sequence.
  ///
  /// The drop-while sequence produces errors from either the base
  /// sequence or the filtering closure.
  public typealias Failure = any Error
  /// The type of iterator that produces elements of the sequence.
  public typealias AsyncIterator = Iterator

  /// The iterator that produces elements of the drop-while sequence.
  public struct Iterator: AsyncIteratorProtocol {
    @usableFromInline
    var baseIterator: Base.AsyncIterator

    @usableFromInline
    let predicate: (Base.Element) async throws -> Bool

    @usableFromInline
    var finished = false

    @usableFromInline
    var doneDropping = false

    @usableFromInline
    init(
      _ baseIterator: Base.AsyncIterator, 
      predicate: @escaping (Base.Element) async throws -> Bool
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
    /// If calling the closure throws an error, the sequence ends and `next()`
    /// rethrows the error.
    @inlinable
    public mutating func next() async throws -> Base.Element? {
      while !finished && !doneDropping {
        guard let element = try await baseIterator.next() else {
          return nil
        }
        do {
          if try await predicate(element) == false {
            doneDropping = true
            return element
          }
        } catch {
          finished = true
          throw error
        }
      }
      guard !finished else { 
        return nil
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
    /// closure again.  If calling the closure throws an error, the sequence
    /// ends and `next(isolation:)` rethrows the error.
    @available(SwiftStdlib 6.0, *)
    @inlinable
    public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> Base.Element? {
      while !finished && !doneDropping {
        guard let element = try await baseIterator.next(isolation: actor) else {
          return nil
        }
        do {
          if try await predicate(element) == false {
            doneDropping = true
            return element
          }
        } catch {
          finished = true
          throw error
        }
      }
      guard !finished else { 
        return nil
      }
      return try await baseIterator.next(isolation: actor)
    }
  }

  @inlinable
  public __consuming func makeAsyncIterator() -> Iterator {
    return Iterator(base.makeAsyncIterator(), predicate: predicate)
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncThrowingDropWhileSequence: @unchecked Sendable 
  where Base: Sendable, 
        Base.Element: Sendable { }

@available(*, unavailable)
extension AsyncThrowingDropWhileSequence.Iterator: Sendable { }
