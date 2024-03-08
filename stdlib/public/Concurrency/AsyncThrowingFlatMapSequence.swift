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
  /// Creates an asynchronous sequence that concatenates the results of calling
  /// the given error-throwing transformation with each element of this
  /// sequence.
  ///
  /// Use this method to receive a single-level asynchronous sequence when your
  /// transformation produces an asynchronous sequence for each element.
  ///
  /// In this example, an asynchronous sequence called `Counter` produces `Int`
  /// values from `1` to `5`. The transforming closure takes the received `Int`
  /// and returns a new `Counter` that counts that high. For example, when the
  /// transform receives `3` from the base sequence, it creates a new `Counter`
  /// that produces the values `1`, `2`, and `3`. The `flatMap(_:)` method
  /// "flattens" the resulting sequence-of-sequences into a single
  /// `AsyncSequence`. However, when the closure receives `4`, it throws an
  /// error, terminating the sequence.
  ///
  ///     do {
  ///         let stream = Counter(howHigh: 5)
  ///             .flatMap { (value) -> Counter in
  ///                 if value == 4 {
  ///                     throw MyError()
  ///                 }
  ///                 return Counter(howHigh: value)
  ///             }
  ///         for try await number in stream {
  ///             print(number, terminator: " ")
  ///         }
  ///     } catch {
  ///         print(error)
  ///     }
  ///     // Prints "1 1 2 1 2 3 MyError() "
  ///
  /// - Parameter transform: An error-throwing mapping closure. `transform`
  ///   accepts an element of this sequence as its parameter and returns an
  ///   `AsyncSequence`. If `transform` throws an error, the sequence ends.
  /// - Returns: A single, flattened asynchronous sequence that contains all
  ///   elements in all the asynchronous sequences produced by `transform`. The
  ///   sequence ends either when the last sequence created from the last
  ///   element from base sequence ends, or when `transform` throws an error.
  @preconcurrency
  @inlinable
  public __consuming func flatMap<SegmentOfResult: AsyncSequence>(
    _ transform: @Sendable @escaping (Element) async throws -> SegmentOfResult
  ) -> AsyncThrowingFlatMapSequence<Self, SegmentOfResult> {
    return AsyncThrowingFlatMapSequence(self, transform: transform)
  }
}

/// An asynchronous sequence that concatenates the results of calling a given
/// error-throwing transformation with each element of this sequence.
@available(SwiftStdlib 5.1, *)
public struct AsyncThrowingFlatMapSequence<Base: AsyncSequence, SegmentOfResult: AsyncSequence> {
  @usableFromInline
  let base: Base

  @usableFromInline
  let transform: (Base.Element) async throws -> SegmentOfResult

  @usableFromInline
  init(
    _ base: Base,
    transform: @escaping (Base.Element) async throws -> SegmentOfResult
  ) {
    self.base = base
    self.transform = transform
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncThrowingFlatMapSequence: AsyncSequence {
  /// The type of element produced by this asynchronous sequence.
  ///
  /// The flat map sequence produces the type of element in the asynchronous
  /// sequence produced by the `transform` closure.
  public typealias Element = SegmentOfResult.Element
  /// The type of error produced by this asynchronous sequence.
  ///
  /// The flat map sequence produces errors from either the base
  /// sequence or the `transform` closure.
  public typealias Failure = any Error
  /// The type of iterator that produces elements of the sequence.
  public typealias AsyncIterator = Iterator

  /// The iterator that produces elements of the flat map sequence.
  public struct Iterator: AsyncIteratorProtocol {
    @usableFromInline
    var baseIterator: Base.AsyncIterator

    @usableFromInline
    let transform: (Base.Element) async throws -> SegmentOfResult

    @usableFromInline
    var currentIterator: SegmentOfResult.AsyncIterator?

    @usableFromInline
    var finished = false

    @usableFromInline
    init(
      _ baseIterator: Base.AsyncIterator,
      transform: @escaping (Base.Element) async throws -> SegmentOfResult
    ) {
      self.baseIterator = baseIterator
      self.transform = transform
    }

    /// Produces the next element in the flat map sequence.
    ///
    /// This iterator calls `next()` on its base iterator; if this call returns
    /// `nil`, `next()` returns `nil`. Otherwise, `next()` calls the
    /// transforming closure on the received element, takes the resulting
    /// asynchronous sequence, and creates an asynchronous iterator from it.
    /// `next()` then consumes values from this iterator until it terminates.
    /// At this point, `next()` is ready to receive the next value from the base
    /// sequence. If `transform` throws an error, the sequence terminates.
    @inlinable
    public mutating func next() async throws -> SegmentOfResult.Element? {
      while !finished {
        if var iterator = currentIterator {
          do {
            guard let element = try await iterator.next() else {
              currentIterator = nil
              continue
            }
            // restore the iterator since we just mutated it with next
            currentIterator = iterator
            return element
          } catch {
            finished = true
            throw error
          }
        } else {
          guard let item = try await baseIterator.next() else {
            return nil
          }
          let segment: SegmentOfResult
          do {
            segment = try await transform(item)
            var iterator = segment.makeAsyncIterator()
            guard let element = try await iterator.next() else {
              currentIterator = nil
              continue
            }
            currentIterator = iterator
            return element
          } catch {
            finished = true
            currentIterator = nil
            throw error
          }
        }
      }
      return nil
    }

    /// Produces the next element in the flat map sequence.
    ///
    /// This iterator calls `next(isolation:)` on its base iterator; if this
    /// call returns `nil`, `next(isolation:)` returns `nil`. Otherwise,
    /// `next(isolation:)` calls the transforming closure on the received
    /// element, takes the resulting asynchronous sequence, and creates an
    /// asynchronous iterator from it.  `next(isolation:)` then consumes values
    /// from this iterator until it terminates.  At this point,
    /// `next(isolation:)` is ready to receive the next value from the base
    /// sequence. If `transform` throws an error, the sequence terminates.
    @available(SwiftStdlib 6.0, *)
    @inlinable
    public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> SegmentOfResult.Element? {
      while !finished {
        if var iterator = currentIterator {
          do {
            guard let element = try await iterator.next(isolation: actor) else {
              currentIterator = nil
              continue
            }
            // restore the iterator since we just mutated it with next
            currentIterator = iterator
            return element
          } catch {
            finished = true
            throw error
          }
        } else {
          guard let item = try await baseIterator.next(isolation: actor) else {
            return nil
          }
          let segment: SegmentOfResult
          do {
            segment = try await transform(item)
            var iterator = segment.makeAsyncIterator()
            guard let element = try await iterator.next(isolation: actor) else {
              currentIterator = nil
              continue
            }
            currentIterator = iterator
            return element
          } catch {
            finished = true
            currentIterator = nil
            throw error
          }
        }
      }
      return nil
    }
  }

  @inlinable
  public __consuming func makeAsyncIterator() -> Iterator {
    return Iterator(base.makeAsyncIterator(), transform: transform)
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncThrowingFlatMapSequence: @unchecked Sendable 
  where Base: Sendable, 
        Base.Element: Sendable, 
        SegmentOfResult: Sendable, 
        SegmentOfResult.Element: Sendable { }

@available(*, unavailable)
extension AsyncThrowingFlatMapSequence.Iterator: Sendable { }
