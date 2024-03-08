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
  @available(*, deprecated, renamed: "prefix(while:)")
  @preconcurrency
  @inlinable
  public __consuming func prefix(
    while predicate: @Sendable @escaping (Element) async throws -> Bool
  ) rethrows -> AsyncThrowingPrefixWhileSequence<Self> {
    return AsyncThrowingPrefixWhileSequence(self, predicate: predicate)
  }
  /// Returns an asynchronous sequence, containing the initial, consecutive
  /// elements of the base sequence that satisfy the given error-throwing
  /// predicate.
  ///
  /// Use `prefix(while:)` to produce values while elements from the base
  /// sequence meet a condition you specify. The modified sequence ends when
  /// the predicate closure returns `false` or throws an error.
  ///
  /// In this example, an asynchronous sequence called `Counter` produces `Int`
  /// values from `1` to `10`. The `prefix(_:)` method causes the modified
  /// sequence to pass through values less than `8`, but throws an
  /// error when it receives a value that's divisible by `5`:
  ///
  ///     do {
  ///         let stream = try Counter(howHigh: 10)
  ///             .prefix {
  ///                 if $0 % 5 == 0 {
  ///                     throw MyError()
  ///                 }
  ///                 return $0 < 8
  ///             }
  ///         for try await number in stream {
  ///             print(number, terminator: " ")
  ///         }
  ///     } catch {
  ///         print("Error: \(error)")
  ///     }
  ///     // Prints "1 2 3 4 Error: MyError() "
  ///
  /// - Parameter predicate: A error-throwing closure that takes an element of
  ///   the asynchronous sequence as its argument and returns a Boolean value
  ///   that indicates whether to include the element in the modified sequence.
  /// - Returns: An asynchronous sequence that contains, in order, the elements
  ///   of the base sequence that satisfy the given predicate. If the predicate
  ///   throws an error, the sequence contains only values produced prior to
  ///   the error.
  @preconcurrency
  @inlinable
  public __consuming func prefix(
    while predicate: @Sendable @escaping (Element) async throws -> Bool
  ) -> AsyncThrowingPrefixWhileSequence<Self> {
    return AsyncThrowingPrefixWhileSequence(self, predicate: predicate)
  }
}

/// An asynchronous sequence, containing the initial, consecutive
/// elements of the base sequence that satisfy the given error-throwing
/// predicate.
@available(SwiftStdlib 5.1, *)
public struct AsyncThrowingPrefixWhileSequence<Base: AsyncSequence> {
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
extension AsyncThrowingPrefixWhileSequence: AsyncSequence {
  /// The type of element produced by this asynchronous sequence.
  ///
  /// The prefix-while sequence produces whatever type of element its base
  /// iterator produces.
  public typealias Element = Base.Element
  /// The type of error produced by this asynchronous sequence.
  ///
  /// The prefix-while sequence produces errors from either the base
  /// sequence or the filtering closure.
  public typealias Failure = any Error
  /// The type of iterator that produces elements of the sequence.
  public typealias AsyncIterator = Iterator

  /// The iterator that produces elements of the prefix-while sequence.
  public struct Iterator: AsyncIteratorProtocol {
    @usableFromInline
    var predicateHasFailed = false

    @usableFromInline
    var baseIterator: Base.AsyncIterator

    @usableFromInline
    let predicate: (Base.Element) async throws -> Bool

    @usableFromInline
    init(
      _ baseIterator: Base.AsyncIterator, 
      predicate: @escaping (Base.Element) async throws -> Bool
    ) {
      self.baseIterator = baseIterator
      self.predicate = predicate
    }

    /// Produces the next element in the prefix-while sequence.
    ///
    /// If the predicate hasn't failed yet, this method gets the next element
    /// from the base sequence and calls the predicate with it. If this call
    /// succeeds, this method passes along the element. Otherwise, it returns
    /// `nil`, ending the sequence. If calling the predicate closure throws an
    /// error, the sequence ends and `next()` rethrows the error.
    @inlinable
    public mutating func next() async throws -> Base.Element? {
      if !predicateHasFailed, let nextElement = try await baseIterator.next() {
        do { 
          if try await predicate(nextElement) {
            return nextElement
          } else {
            predicateHasFailed = true
          }
        } catch {
          predicateHasFailed = true
          throw error
        }
      }
      return nil
    }

    /// Produces the next element in the prefix-while sequence.
    ///
    /// If the predicate hasn't failed yet, this method gets the next element
    /// from the base sequence and calls the predicate with it. If this call
    /// succeeds, this method passes along the element. Otherwise, it returns
    /// `nil`, ending the sequence. If calling the predicate closure throws an
    /// error, the sequence ends and `next(isolation:)` rethrows the error.
    @available(SwiftStdlib 6.0, *)
    @inlinable
    public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> Base.Element? {
      if !predicateHasFailed, let nextElement = try await baseIterator.next(isolation: actor) {
        do { 
          if try await predicate(nextElement) {
            return nextElement
          } else {
            predicateHasFailed = true
          }
        } catch {
          predicateHasFailed = true
          throw error
        }
      }
      return nil
    }
  }

  @inlinable
  public __consuming func makeAsyncIterator() -> Iterator {
    return Iterator(base.makeAsyncIterator(), predicate: predicate)
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncThrowingPrefixWhileSequence: @unchecked Sendable 
  where Base: Sendable, 
        Base.Element: Sendable { }

@available(SwiftStdlib 5.1, *)
extension AsyncThrowingPrefixWhileSequence.Iterator: @unchecked Sendable 
  where Base.AsyncIterator: Sendable, 
        Base.Element: Sendable { }
