//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// A value that represents either a success or a failure, including an
/// associated value in each case.
@frozen
public enum Result<Success, Failure: Error> {
  /// A success, storing a `Success` value.
  case success(Success)
  
  /// A failure, storing a `Failure` value.
  case failure(Failure)
  
  /// Returns a new result, mapping any success value using the given
  /// transformation.
  ///
  /// Use this method when you need to transform the value of a `Result`
  /// instance when it represents a success. The following example transforms
  /// the integer success value of a result into a string:
  ///
  ///     func getNextInteger() -> Result<Int, Error> { /* ... */ }
  ///
  ///     let integerResult = getNextInteger()
  ///     // integerResult == .success(5)
  ///     let stringResult = integerResult.map({ String($0) })
  ///     // stringResult == .success("5")
  ///
  /// - Parameter transform: A closure that takes the success value of this
  ///   instance.
  /// - Returns: A `Result` instance with the result of evaluating `transform`
  ///   as the new success value if this instance represents a success.
  @inlinable
  public func map<NewSuccess>(
    _ transform: (Success) -> NewSuccess
  ) -> Result<NewSuccess, Failure> {
    switch self {
    case let .success(success):
      return .success(transform(success))
    case let .failure(failure):
      return .failure(failure)
    }
  }
  
  /// Returns a new result, mapping any failure value using the given
  /// transformation.
  ///
  /// Use this method when you need to transform the value of a `Result`
  /// instance when it represents a failure. The following example transforms
  /// the error value of a result by wrapping it in a custom `Error` type:
  ///
  ///     struct DatedError: Error {
  ///         var error: Error
  ///         var date: Date
  ///
  ///         init(_ error: Error) {
  ///             self.error = error
  ///             self.date = Date()
  ///         }
  ///     }
  ///
  ///     let result: Result<Int, Error> = // ...
  ///     // result == .failure(<error value>)
  ///     let resultWithDatedError = result.mapError({ e in DatedError(e) })
  ///     // result == .failure(DatedError(error: <error value>, date: <date>))
  ///
  /// - Parameter transform: A closure that takes the failure value of the
  ///   instance.
  /// - Returns: A `Result` instance with the result of evaluating `transform`
  ///   as the new failure value if this instance represents a failure.
  @inlinable
  public func mapError<NewFailure>(
    _ transform: (Failure) -> NewFailure
  ) -> Result<Success, NewFailure> {
    switch self {
    case let .success(success):
      return .success(success)
    case let .failure(failure):
      return .failure(transform(failure))
    }
  }
  
  /// Returns a new result, mapping any success value using the given
  /// transformation and unwrapping the produced result.
  ///
  /// Use this method to avoid a nested result when your transformation
  /// produces another `Result` type.
  ///
  /// In this example, note the difference in the result of using `map` and
  /// `flatMap` with a transformation that returns an result type.
  ///
  ///     func getNextInteger() -> Result<Int, Error> {
  ///         .success(4)
  ///     }
  ///     func getNextAfterInteger(_ n: Int) -> Result<Int, Error> {
  ///         .success(n + 1)
  ///     }
  ///
  ///     let result = getNextInteger().map({ getNextAfterInteger($0) })
  ///     // result == .success(.success(5))
  ///
  ///     let result = getNextInteger().flatMap({ getNextAfterInteger($0) })
  ///     // result == .success(5)
  ///
  /// - Parameter transform: A closure that takes the success value of the
  ///   instance.
  /// - Returns: A `Result` instance, either from the closure or the previous
  ///   `.failure`.
  @inlinable
  public func flatMap<NewSuccess>(
    _ transform: (Success) -> Result<NewSuccess, Failure>
  ) -> Result<NewSuccess, Failure> {
    switch self {
    case let .success(success):
      return transform(success)
    case let .failure(failure):
      return .failure(failure)
    }
  }
  
  /// Returns a new result, mapping any failure value using the given
  /// transformation and unwrapping the produced result.
  ///
  /// - Parameter transform: A closure that takes the failure value of the
  ///   instance.
  /// - Returns: A `Result` instance, either from the closure or the previous 
  ///   `.success`.
  @inlinable
  public func flatMapError<NewFailure>(
    _ transform: (Failure) -> Result<Success, NewFailure>
  ) -> Result<Success, NewFailure> {
    switch self {
    case let .success(success):
      return .success(success)
    case let .failure(failure):
      return transform(failure)
    }
  }
  
  /// Returns the success value as a throwing expression.
  ///
  /// Use this method to retrieve the value of this result if it represents a
  /// success, or to catch the value if it represents a failure.
  ///
  ///     let integerResult: Result<Int, Error> = .success(5)
  ///     do {
  ///         let value = try integerResult.get()
  ///         print("The value is \(value).")
  ///     } catch {
  ///         print("Error retrieving the value: \(error)")
  ///     }
  ///     // Prints "The value is 5."
  ///
  /// - Returns: The success value, if the instance represents a success.
  /// - Throws: The failure value, if the instance represents a failure.
  @inlinable
  public func get() throws -> Success {
    switch self {
    case let .success(success):
      return success
    case let .failure(failure):
      throw failure
    }
  }

  @inlinable
  @_alwaysEmitIntoClient
  public var value: Success {
    get throws {
      switch self {
      case let .success(success):
        return success
      case let .failure(failure):
        throw failure
      }
    }
  }
}

extension Result where Failure == Swift.Error {
  /// Creates a new result by evaluating a throwing closure, capturing the
  /// returned value as a success, or any thrown error as a failure.
  ///
  /// - Parameter body: A throwing closure to evaluate.
  @_transparent
  public init(catching body: () throws -> Success) {
    do {
      self = .success(try body())
    } catch {
      self = .failure(error)
    }
  }
}

extension Result: Equatable where Success: Equatable, Failure: Equatable { }

extension Result: Hashable where Success: Hashable, Failure: Hashable { }

extension Result: Sendable where Success: Sendable, Failure: Sendable { }
