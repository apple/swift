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
  /// Returns an asynchronous sequence, containing the initial, consecutive
  /// elements of the base sequence that satisfy the given predicate.
  ///
  /// Use `prefix(while:)` to produce values while elements from the base
  /// sequence meet a condition you specify. The modified sequence ends when
  /// the predicate closure returns `false`.
  ///
  /// In this example, an asynchronous sequence called `Counter` produces `Int`
  /// values from `1` to `10`. The `prefix(while:)` method causes the modified
  /// sequence to pass along values so long as they aren’t divisible by `2` and
  /// `3`. Upon reaching `6`, the sequence ends:
  ///
  ///     let stream = Counter(howHigh: 10)
  ///         .prefix { $0 % 2 != 0 || $0 % 3 != 0 }
  ///     for try await number in stream {
  ///         print(number, terminator: " ")
  ///     }
  ///     // Prints "1 2 3 4 5 "
  ///     
  /// - Parameter predicate: A closure that takes an element as a parameter and
  ///   returns a Boolean value indicating whether the element should be
  ///   included in the modified sequence.
  /// - Returns: An asynchronous sequence of the initial, consecutive
  ///   elements that satisfy `predicate`.
  @preconcurrency
  @inlinable
  public __consuming func prefix(
    while predicate: @Sendable @escaping (Element) async -> Bool
  ) rethrows -> AsyncPrefixWhileSequence<Self> {
    return AsyncPrefixWhileSequence(self, predicate: predicate)
  }
}

/// An asynchronous sequence, containing the initial, consecutive
/// elements of the base sequence that satisfy a given predicate.
@available(SwiftStdlib 5.1, *)
public struct AsyncPrefixWhileSequence<Base: AsyncSequence> {
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
extension AsyncPrefixWhileSequence: AsyncSequence {
  /// The type of element produced by this asynchronous sequence.
  ///
  /// The prefix-while sequence produces whatever type of element its base
  /// iterator produces.
  public typealias Element = Base.Element
  /// The type of the error that can be produced by the sequence.
  ///
  /// The prefix-while sequence produces whatever type of error its
  /// base sequence does.
  @available(SwiftStdlib 6.0, *)
  public typealias Failure = Base.Failure
  /// The type of iterator that produces elements of the sequence.
  public typealias AsyncIterator = Iterator

  /// The iterator that produces elements of the prefix-while sequence.
  public struct Iterator: AsyncIteratorProtocol {
    // FIXME: Remove when $AssociatedTypeImplements is no longer needed
    @available(SwiftStdlib 6.0, *)
    public typealias Failure = Base.Failure

    @usableFromInline
    var predicateHasFailed = false

    @usableFromInline
    var baseIterator: Base.AsyncIterator

    @usableFromInline
    let predicate: (Base.Element) async -> Bool

    @usableFromInline
    init(
      _ baseIterator: Base.AsyncIterator, 
      predicate: @escaping (Base.Element) async -> Bool
    ) {
      self.baseIterator = baseIterator
      self.predicate = predicate
    }

    /// Produces the next element in the prefix-while sequence.
    ///
    /// If the predicate hasn't yet failed, this method gets the next element
    /// from the base sequence and calls the predicate with it. If this call
    /// succeeds, this method passes along the element. Otherwise, it returns
    /// `nil`, ending the sequence.
    @inlinable
    public mutating func next() async rethrows -> Base.Element? {
      if !predicateHasFailed, let nextElement = try await baseIterator.next() {
        if await predicate(nextElement) {
          return nextElement
        } else {
          predicateHasFailed = true
        }
      }
      return nil
    }

    /// Produces the next element in the prefix-while sequence.
    ///
    /// If the predicate hasn't yet failed, this method gets the next element
    /// from the base sequence and calls the predicate with it. If this call
    /// succeeds, this method passes along the element. Otherwise, it returns
    /// `nil`, ending the sequence.
    @available(SwiftStdlib 6.0, *)
    @inlinable
    public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> Base.Element? {
      if !predicateHasFailed, let nextElement = try await baseIterator.next(isolation: actor) {
        if await predicate(nextElement) {
          return nextElement
        } else {
          predicateHasFailed = true
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
extension AsyncPrefixWhileSequence: @unchecked Sendable 
  where Base: Sendable, 
        Base.Element: Sendable { }

@available(*, unavailable)
extension AsyncPrefixWhileSequence.Iterator: Sendable { }
