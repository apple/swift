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

/// A type that that asychronously supplies the values of a sequence one at a
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
/// until a `howHigh` value is reached. While this example is not itself
/// asychronous, it shows the shape of a custom sequence and iterator, and can
/// be used as if it were asynchronous:
///
///     struct Counter : AsyncSequence {
///       typealias Element = Int
///       let howHigh: Int
///
///       struct AsyncIterator : AsyncIteratorProtocol {
///         let howHigh: Int
///         var current = 1
///         mutating func next() async -> Int? {
///           // A genuinely asychronous implementation could use the `Task`
///           // API to check for cancellation here and return early.
///           guard current <= howHigh else {
///             return nil
///           }
///
///           let result = current
///           current += 1
///           return result
///         }
///       }
///
///       func makeAsyncIterator() -> AsyncIterator {
///         return AsyncIterator(howHigh: howHigh)
///       }
///     }
///
/// At the call site, this looks like:
///
///     for await i in Counter(howHigh: 3) {
///       print(i, terminator: " ")
///     }
///     // Prints: 1 2 3 4 5 6 7 8 9 10
///
/// ### End of Iteration
///
/// The iterator returns `nil` to indicate the end of the sequence. After
/// returning `nil` (or throwing an error) from `next()`, all future calls to
/// `next()` must return `nil`.
///
/// ### Cancellation
///
/// Types conforming to `AsyncIteratorProtocol` should use the cancellation
/// primitives provided by Swift's Task API. The iterator can choose how to
/// respond to cancellation, such as by thowing `CancellationError` or
/// returning `nil` from `next()`.
///
/// If the iterator needs to clean up on cancellation, it can do so after
/// checking for cancellation (using the `Task` API), or in `deinit` (if it is
/// a class type).
@available(SwiftStdlib 5.5, *)
@rethrows
public protocol AsyncIteratorProtocol {
  associatedtype Element
  /// Asynchronously advances to the next element and returns it, or nil if no
  /// next element exists.
  /// - Returns: The next element, if it exists, or `nil` to signal the end of
  ///   the sequence.
  mutating func next() async throws -> Element?
}
