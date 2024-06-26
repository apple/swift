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
  /// Creates an asynchronous sequence that maps an error-throwing closure over
  /// the base sequence’s elements, omitting results that don't return a value.
  ///
  /// Use the `compactMap(_:)` method to transform every element received from
  /// a base asynchronous sequence, while also discarding any `nil` results
  /// from the closure. Typically, you use this to transform from one type of
  /// element to another.
  ///
  /// In this example, an asynchronous sequence called `Counter` produces `Int`
  /// values from `1` to `5`. The closure provided to the `compactMap(_:)`
  /// method takes each `Int` and looks up a corresponding `String` from a
  /// `romanNumeralDict` dictionary. Since there is no key for `4`, the closure
  /// returns `nil` in this case, which `compactMap(_:)` omits from the
  /// transformed asynchronous sequence. When the value is `5`, the closure
  /// throws `MyError`, terminating the sequence.
  ///
  ///     let romanNumeralDict: [Int: String] =
  ///         [1: "I", 2: "II", 3: "III", 5: "V"]
  ///
  ///     do {
  ///         let stream = Counter(howHigh: 5)
  ///             .compactMap { (value) throws -> String? in
  ///                 if value == 5 {
  ///                     throw MyError()
  ///                 }
  ///                 return romanNumeralDict[value]
  ///             }
  ///         for try await numeral in stream {
  ///             print(numeral, terminator: " ")
  ///         }
  ///     } catch {
  ///         print("Error: \(error)")
  ///     }
  ///     // Prints "I II III Error: MyError() "
  ///
  /// - Parameter transform: An error-throwing mapping closure. `transform`
  ///   accepts an element of this sequence as its parameter and returns a
  ///   transformed value of the same or of a different type. If `transform`
  ///   throws an error, the sequence ends.
  /// - Returns: An asynchronous sequence that contains, in order, the
  ///   non-`nil` elements produced by the `transform` closure. The sequence
  ///   ends either when the base sequence ends or when `transform` throws an
  ///   error.
  @preconcurrency
  @inlinable
  public __consuming func compactMap<ElementOfResult>(
    _ transform: @Sendable @escaping (Element) async throws -> ElementOfResult?
  ) -> AsyncThrowingCompactMapSequence<Self, ElementOfResult> {
    return AsyncThrowingCompactMapSequence(self, transform: transform)
  }
}

/// An asynchronous sequence that maps an error-throwing closure over the base
/// sequence’s elements, omitting results that don't return a value.
@available(SwiftStdlib 5.1, *)
public struct AsyncThrowingCompactMapSequence<Base: AsyncSequence, ElementOfResult> {
  @usableFromInline
  let base: Base

  @usableFromInline
  let transform: (Base.Element) async throws -> ElementOfResult?

  @usableFromInline
  init(
    _ base: Base, 
    transform: @escaping (Base.Element) async throws -> ElementOfResult?
  ) {
    self.base = base
    self.transform = transform
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncThrowingCompactMapSequence: AsyncSequence {
  /// The type of element produced by this asynchronous sequence.
  ///
  /// The compact map sequence produces whatever type of element its
  /// transforming closure produces.
  public typealias Element = ElementOfResult
  /// The type of element produced by this asynchronous sequence.
  ///
  /// The compact map sequence produces errors from either the base
  /// sequence or the transforming closure.
  public typealias Failure = any Error
  /// The type of iterator that produces elements of the sequence.
  public typealias AsyncIterator = Iterator

  /// The iterator that produces elements of the compact map sequence.
  public struct Iterator: AsyncIteratorProtocol {
    public typealias Element = ElementOfResult

    @usableFromInline
    var baseIterator: Base.AsyncIterator

    @usableFromInline
    let transform: (Base.Element) async throws -> ElementOfResult?

    @usableFromInline
    var finished = false

    @usableFromInline
    init(
      _ baseIterator: Base.AsyncIterator, 
      transform: @escaping (Base.Element) async throws -> ElementOfResult?
    ) {
      self.baseIterator = baseIterator
      self.transform = transform
    }

    /// Produces the next element in the compact map sequence.
    ///
    /// This iterator calls `next()` on its base iterator; if this call returns
    /// `nil`, `next()` returns `nil`. Otherwise, `next()` calls the
    /// transforming closure on the received element, returning it if the
    /// transform returns a non-`nil` value. If the transform returns `nil`,
    /// this method continues to wait for further elements until it gets one
    /// that transforms to a non-`nil` value. If calling the closure throws an
    /// error, the sequence ends and `next()` rethrows the error.
    @inlinable
    public mutating func next() async throws -> ElementOfResult? {
      while !finished {
        guard let element = try await baseIterator.next() else {
          finished = true
          return nil
        }
        do {
          if let transformed = try await transform(element) {
            return transformed
          }
        } catch {
          finished = true
          throw error
        }
      }
      return nil
    }

    /// Produces the next element in the compact map sequence.
    ///
    /// This iterator calls `next()` on its base iterator; if this call
    /// returns `nil`, `next()` returns `nil`. Otherwise, `next()`
    /// calls the transforming closure on the received element, returning it if
    /// the transform returns a non-`nil` value. If the transform returns `nil`,
    /// this method continues to wait for further elements until it gets one
    /// that transforms to a non-`nil` value. If calling the closure throws an
    /// error, the sequence ends and `next()` rethrows the error.
    @available(SwiftStdlib 6.0, *)
    @inlinable
    public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> ElementOfResult? {
      while !finished {
        guard let element = try await baseIterator.next(isolation: actor) else {
          finished = true
          return nil
        }
        do {
          if let transformed = try await transform(element) {
            return transformed
          }
        } catch {
          finished = true
          throw error
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
extension AsyncThrowingCompactMapSequence: @unchecked Sendable 
  where Base: Sendable, 
        Base.Element: Sendable { }

@available(*, unavailable)
extension AsyncThrowingCompactMapSequence.Iterator: Sendable { }
