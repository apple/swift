//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Swift

/// A type that asynchronously supplies the values of a sequence one at a
/// time.
///
/// The `AsyncIteratorProtocol` defines the type returned by the
/// `makeAsyncIterator()` method of the `AsyncSequence` protocol. In short,
/// the iterator is what produces the asynchronous sequence's values. The
/// protocol defines a single asynchronous method, `next()`, which either
/// produces the next element of the sequence, or returns `nil` to signal
/// the end of the sequence.
///
/// To implement your own `AsyncSequence`, implement a wrapped type that
/// conforms to `AsyncIteratorProtocol`. The following example shows a `Counter`
/// type that uses an inner iterator to monotonically generate `Int` values
/// until reaching a `howHigh` value. While this example isn't itself
/// asynchronous, it shows the shape of a custom sequence and iterator, and how
/// to use it as if it were asynchronous:
///
///     struct Counter: AsyncSequence {
///         typealias Element = Int
///         let howHigh: Int
///
///         struct AsyncIterator: AsyncIteratorProtocol {
///             let howHigh: Int
///             var current = 1
///
///             mutating func next() async -> Int? {
///                 // A genuinely asynchronous implementation uses the `Task`
///                 // API to check for cancellation here and return early.
///                 guard current <= howHigh else {
///                     return nil
///                 }
///
///                 let result = current
///                 current += 1
///                 return result
///             }
///         }
///
///         func makeAsyncIterator() -> AsyncIterator {
///             return AsyncIterator(howHigh: howHigh)
///         }
///     }
///
/// At the call site, this looks like:
///
///     for await number in Counter(howHigh: 10) {
///       print(number, terminator: " ")
///     }
///     // Prints "1 2 3 4 5 6 7 8 9 10 "
///
/// ### End of Iteration
///
/// The iterator returns `nil` to indicate the end of the sequence. After
/// returning `nil` (or throwing an error) from `next()`, the iterator enters
/// a terminal state, and all future calls to `next()` must return `nil`.
///
/// ### Cancellation
///
/// Types conforming to `AsyncIteratorProtocol` should use the cancellation
/// primitives provided by Swift's `Task` API. The iterator can choose how to
/// handle and respond to cancellation, including:
///
/// - Checking the `isCancelled` value of the current `Task` inside `next()`
///   and returning `nil` to terminate the sequence.
/// - Calling `checkCancellation()` on the `Task`, which throws a
///   `CancellationError`.
/// - Implementing `next()` with a
///   `withTaskCancellationHandler(handler:operation:)` invocation to
///   immediately react to cancellation.
///
/// If the iterator needs to clean up on cancellation, it can do so after
/// checking for cancellation as described above, or in `deinit` if it's
/// a reference type.
@available(SwiftStdlib 5.1, *)
public protocol AsyncIteratorProtocol<Element, Failure> {
  associatedtype Element

  /// The type of failure produced by iteration.
  @available(SwiftStdlib 6.0, *)
  associatedtype Failure: Error = any Error

  /// Asynchronously advances to the next element and returns it, or ends the
  /// sequence if there is no next element.
  ///
  /// - Returns: The next element, if it exists, or `nil` to signal the end of
  ///   the sequence.
  mutating func next() async throws -> Element?

  /// Asynchronously advances to the next element and returns it, or ends the
  /// sequence if there is no next element.
  ///
  /// - Returns: The next element, if it exists, or `nil` to signal the end of
  ///   the sequence.
  @available(SwiftStdlib 6.0, *)
  mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> Element?
}

@available(SwiftStdlib 5.1, *)
extension AsyncIteratorProtocol {
  /// Default implementation of `next(isolation:)` in terms of `next()`, which
  /// is required to maintain backward compatibility with existing async
  /// iterators.
  @available(SwiftStdlib 6.0, *)
  @inlinable
  public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> Element? {
    do {
      return try await next()
    } catch {
      throw error as! Failure
    }
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncIteratorProtocol {
  /// Default implementation of `next()` in terms of `next(isolation:)`, which
  /// is required to maintain backward compatibility with existing async
  /// iterators.
  @available(SwiftStdlib 6.0, *)
  @inlinable
  public mutating func next() async throws(Failure) -> Element? {
    return try await next(isolation: nil)
  }
}
