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
  /// Creates an asynchronous sequence that maps the given error-throwing
  /// closure over the asynchronous sequence’s elements.
  ///
  /// Use the `map(_:)` method to transform every element received from a base
  /// asynchronous sequence. Typically, you use this to transform from one type
  /// of element to another.
  ///
  /// In this example, an asynchronous sequence called `Counter` produces `Int`
  /// values from `1` to `5`. The closure provided to the `map(_:)` method
  /// takes each `Int` and looks up a corresponding `String` from a
  /// `romanNumeralDict` dictionary. This means the outer `for await in` loop
  /// iterates over `String` instances instead of the underlying `Int` values
  /// that `Counter` produces. Also, the dictionary doesn't provide a key for
  /// `4`, and the closure throws an error for any key it can't look up, so
  /// receiving this value from `Counter` ends the modified sequence with an
  /// error.
  ///
  ///     let romanNumeralDict: [Int: String] =
  ///         [1: "I", 2: "II", 3: "III", 5: "V"]
  ///
  ///     do {
  ///         let stream = Counter(howHigh: 5)
  ///             .map { (value) throws -> String in
  ///                 guard let roman = romanNumeralDict[value] else {
  ///                     throw MyError()
  ///                 }
  ///                 return roman
  ///             }
  ///         for try await numeral in stream {
  ///             print(numeral, terminator: " ")
  ///         }
  ///     } catch {
  ///         print("Error: \(error)")
  ///     }
  ///     // Prints "I II III Error: MyError() "
  ///
  /// - Parameter transform: A mapping closure. `transform` accepts an element
  ///   of this sequence as its parameter and returns a transformed value of the
  ///   same or of a different type. `transform` can also throw an error, which
  ///   ends the transformed sequence.
  /// - Returns: An asynchronous sequence that contains, in order, the elements
  ///   produced by the `transform` closure.
  @preconcurrency
  @inlinable
  public __consuming func map<Transformed>(
    _ transform: @Sendable @escaping (Element) async throws -> Transformed
  ) -> AsyncThrowingMapSequence<Self, Transformed> {
    return AsyncThrowingMapSequence(self, transform: transform)
  }
}

/// An asynchronous sequence that maps the given error-throwing closure over the
/// asynchronous sequence’s elements.
@available(SwiftStdlib 5.1, *)
public struct AsyncThrowingMapSequence<Base: AsyncSequence, Transformed> {
  @usableFromInline
  let base: Base

  @usableFromInline
  let transform: (Base.Element) async throws -> Transformed

  @usableFromInline
  init(
    _ base: Base, 
    transform: @escaping (Base.Element) async throws -> Transformed
  ) {
    self.base = base
    self.transform = transform
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncThrowingMapSequence: AsyncSequence {
  /// The type of element produced by this asynchronous sequence.
  ///
  /// The map sequence produces whatever type of element its the transforming
  /// closure produces.
  public typealias Element = Transformed
  /// The type of error produced by this asynchronous sequence.
  ///
  /// The map sequence produces errors from either the base
  /// sequence or the `transform` closure.
  public typealias Failure = any Error
  /// The type of iterator that produces elements of the sequence.
  public typealias AsyncIterator = Iterator

  /// The iterator that produces elements of the map sequence.
  public struct Iterator: AsyncIteratorProtocol {
    @usableFromInline
    var baseIterator: Base.AsyncIterator

    @usableFromInline
    let transform: (Base.Element) async throws -> Transformed

    @usableFromInline
    var finished = false

    @usableFromInline
    init(
      _ baseIterator: Base.AsyncIterator, 
      transform: @escaping (Base.Element) async throws -> Transformed
    ) {
      self.baseIterator = baseIterator
      self.transform = transform
    }

    /// Produces the next element in the map sequence.
    ///
    /// This iterator calls `next()` on its base iterator; if this call returns
    /// `nil`, `next()` returns nil. Otherwise, `next()` returns the result of
    /// calling the transforming closure on the received element. If calling
    /// the closure throws an error, the sequence ends and `next()` rethrows
    /// the error.
    @inlinable
    public mutating func next() async throws -> Transformed? {
      guard !finished, let element = try await baseIterator.next() else {
        return nil
      }
      do {
        return try await transform(element)
      } catch {
        finished = true
        throw error   
      }
    }

    /// Produces the next element in the map sequence.
    ///
    /// This iterator calls `next(isolation:)` on its base iterator; if this
    /// call returns `nil`, `next(isolation:)` returns nil. Otherwise,
    /// `next(isolation:)` returns the result of calling the transforming
    /// closure on the received element. If calling the closure throws an error,
    /// the sequence ends and `next(isolation:)` rethrows the error.
    @available(SwiftStdlib 6.0, *)
    @inlinable
    public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> Transformed? {
      guard !finished, let element = try await baseIterator.next(isolation: actor) else {
        return nil
      }
      do {
        return try await transform(element)
      } catch {
        finished = true
        throw error   
      }
    }
  }

  @inlinable
  public __consuming func makeAsyncIterator() -> Iterator {
    return Iterator(base.makeAsyncIterator(), transform: transform)
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncThrowingMapSequence: @unchecked Sendable 
  where Base: Sendable, 
        Base.Element: Sendable, 
        Transformed: Sendable { }

@available(*, unavailable)
extension AsyncThrowingMapSequence.Iterator: Sendable { }
