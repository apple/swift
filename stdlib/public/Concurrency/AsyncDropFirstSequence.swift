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
  /// Omits a specified number of elements from the base asynchronous sequence,
  /// then passes through all remaining elements.
  ///
  /// Use `dropFirst(_:)` when you want to drop the first *n* elements from the
  /// base sequence and pass through the remaining elements.
  ///
  /// In this example, an asynchronous sequence called `Counter` produces `Int`
  /// values from `1` to `10`. The `dropFirst(_:)` method causes the modified
  /// sequence to ignore the values `1` through `3`, and instead emit `4` through `10`:
  ///
  ///     for await number in Counter(howHigh: 10).dropFirst(3) {
  ///         print(number, terminator: " ")
  ///     }
  ///     // Prints "4 5 6 7 8 9 10 "
  ///
  /// If the number of elements to drop exceeds the number of elements in the
  /// sequence, the result is an empty sequence.
  ///
  /// - Parameter count: The number of elements to drop from the beginning of
  ///   the sequence. `count` must be greater than or equal to zero.
  /// - Returns: An asynchronous sequence that drops the first `count`
  ///   elements from the base sequence.
  @inlinable
  public __consuming func dropFirst(
    _ count: Int = 1
  ) -> AsyncDropFirstSequence<Self> {
    precondition(count >= 0, 
      "Can't drop a negative number of elements from an async sequence")
    return AsyncDropFirstSequence(self, dropping: count)
  }
}

/// An asynchronous sequence which omits a specified number of elements from the
/// base asynchronous sequence, then passes through all remaining elements.
@available(SwiftStdlib 5.1, *)
public struct AsyncDropFirstSequence<Base: AsyncSequence> {
  @usableFromInline
  let base: Base

  @usableFromInline
  let count: Int
  
  @usableFromInline 
  init(_ base: Base, dropping count: Int) {
    self.base = base
    self.count = count
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncDropFirstSequence: AsyncSequence {
  /// The type of element produced by this asynchronous sequence.
  ///
  /// The drop-first sequence produces whatever type of element its base
  /// iterator produces.
  public typealias Element = Base.Element
  /// The type of errors produced by this asynchronous sequence.
  ///
  /// The drop-first sequence produces whatever type of error its base
  /// sequence produces.
  @available(SwiftStdlib 6.0, *)
  public typealias Failure = Base.Failure
  /// The type of iterator that produces elements of the sequence.
  public typealias AsyncIterator = Iterator

  /// The iterator that produces elements of the drop-first sequence.
  public struct Iterator: AsyncIteratorProtocol {
    // FIXME: Remove when $AssociatedTypeImplements is no longer needed
    @available(SwiftStdlib 6.0, *)
    public typealias Failure = Base.Failure

    @usableFromInline
    var baseIterator: Base.AsyncIterator
    
    @usableFromInline
    var count: Int

    @usableFromInline
    init(_ baseIterator: Base.AsyncIterator, count: Int) {
      self.baseIterator = baseIterator
      self.count = count
    }

    /// Produces the next element in the drop-first sequence.
    ///
    /// Until reaching the number of elements to drop, this iterator calls
    /// `next()` on its base iterator and discards the result. If the base
    /// iterator returns `nil`, indicating the end of the sequence, this
    /// iterator returns `nil`. After reaching the number of elements to
    /// drop, this iterator passes along the result of calling `next()` on
    /// the base iterator.
    @inlinable
    public mutating func next() async rethrows -> Base.Element? {
      var remainingToDrop = count
      while remainingToDrop > 0 {
        guard try await baseIterator.next() != nil else {
          count = 0
          return nil
        }
        remainingToDrop -= 1
      }
      count = 0
      return try await baseIterator.next()
    }

    /// Produces the next element in the drop-first sequence.
    ///
    /// Until reaching the number of elements to drop, this iterator calls
    /// `next(isolation:)` on its base iterator and discards the result. If the
    /// base iterator returns `nil`, indicating the end of the sequence, this
    /// iterator returns `nil`. After reaching the number of elements to drop,
    /// this iterator passes along the result of calling `next(isolation:)` on
    /// the base iterator.
    @available(SwiftStdlib 6.0, *)
    @inlinable
    public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> Base.Element? {
      var remainingToDrop = count
      while remainingToDrop > 0 {
        guard try await baseIterator.next(isolation: actor) != nil else {
          count = 0
          return nil
        }
        remainingToDrop -= 1
      }
      count = 0
      return try await baseIterator.next(isolation: actor)
    }
  }

  @inlinable
  public __consuming func makeAsyncIterator() -> Iterator {
    return Iterator(base.makeAsyncIterator(), count: count)
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncDropFirstSequence {
  /// Omits a specified number of elements from the base asynchronous sequence,
  /// then passes through all remaining elements.
  ///
  /// When you call `dropFirst(_:)` on an asynchronous sequence that is already
  /// an `AsyncDropFirstSequence`, the returned sequence simply adds the new
  /// drop count to the current drop count.
  @inlinable
  public __consuming func dropFirst(
    _ count: Int = 1
  ) -> AsyncDropFirstSequence<Base> {
    // If this is already a AsyncDropFirstSequence, we can just sum the current 
    // drop count and additional drop count.
    precondition(count >= 0, 
      "Can't drop a negative number of elements from an async sequence")
    return AsyncDropFirstSequence(base, dropping: self.count + count)
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncDropFirstSequence: Sendable 
  where Base: Sendable, 
        Base.Element: Sendable { }

@available(*, unavailable)
extension AsyncDropFirstSequence.Iterator: Sendable { }
