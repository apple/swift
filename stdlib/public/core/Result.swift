//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2018 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// A value that represents either a success or a failure, including an
/// associated value in each case.
@frozen
public enum Result<Success: ~Copyable, Failure: Error> {
  /// A success, storing a `Success` value.
  case success(Success)

  /// A failure, storing a `Failure` value.
  case failure(Failure)
}

extension Result: Copyable where Success: Copyable {}

extension Result: Sendable where Success: Sendable & ~Copyable {}

extension Result: Equatable where Success: Equatable, Failure: Equatable {}

extension Result: Hashable where Success: Hashable, Failure: Hashable {}

extension Result {
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
  ///     let stringResult = integerResult.map { String($0) }
  ///     // stringResult == .success("5")
  ///
  /// - Parameter transform: A closure that takes the success value of this
  ///   instance.
  /// - Returns: A `Result` instance with the result of evaluating `transform`
  ///   as the new success value if this instance represents a success.
  @_alwaysEmitIntoClient
  @_disfavoredOverload // FIXME: Workaround for source compat issue with
                       // functions that used to shadow the original map
                       // (rdar://125016028)
  public func map<NewSuccess: ~Copyable>(
    _ transform: (Success) -> NewSuccess
  ) -> Result<NewSuccess, Failure> {
    switch self {
    case let .success(success):
      return .success(transform(success))
    case let .failure(failure):
      return .failure(failure)
    }
  }

  @_spi(SwiftStdlibLegacyABI) @available(swift, obsoleted: 1)
  @_silgen_name("$ss6ResultO3mapyAByqd__q_Gqd__xXElF")
  @usableFromInline
  internal func __abi_map<NewSuccess>(
    _ transform: (Success) -> NewSuccess
  ) -> Result<NewSuccess, Failure> {
    switch self {
    case let .success(success):
      return .success(transform(success))
    case let .failure(failure):
      return .failure(failure)
    }
  }
}

@_disallowFeatureSuppression(NoncopyableGenerics)
extension Result where Success: ~Copyable {
  // FIXME(NCG): Make this public.
  @_alwaysEmitIntoClient
  public consuming func _consumingMap<NewSuccess: ~Copyable>(
    _ transform: (consuming Success) -> NewSuccess
  ) -> Result<NewSuccess, Failure> {
    switch consume self {
    case let .success(success):
      return .success(transform(consume success))
    case let .failure(failure):
      return .failure(consume failure)
    }
  }

  // FIXME(NCG): Make this public.
  @_alwaysEmitIntoClient
  public borrowing func _borrowingMap<NewSuccess: ~Copyable>(
    _ transform: (borrowing Success) -> NewSuccess
  ) -> Result<NewSuccess, Failure> {
    switch self {
    case .success(let success):
      return .success(transform(success))
    case let .failure(failure):
      return .failure(failure)
    }
  }
}

extension Result where Success: ~Copyable {
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
  ///     let resultWithDatedError = result.mapError { DatedError($0) }
  ///     // result == .failure(DatedError(error: <error value>, date: <date>))
  ///
  /// - Parameter transform: A closure that takes the failure value of the
  ///   instance.
  /// - Returns: A `Result` instance with the result of evaluating `transform`
  ///   as the new failure value if this instance represents a failure.
  @_alwaysEmitIntoClient
  public consuming func mapError<NewFailure>(
    _ transform: (Failure) -> NewFailure
  ) -> Result<Success, NewFailure> {
    switch consume self {
    case let .success(success):
      return .success(consume success)
    case let .failure(failure):
      return .failure(transform(failure))
    }
  }
}

@_disallowFeatureSuppression(NoncopyableGenerics)
extension Result {
  @_spi(SwiftStdlibLegacyABI) @available(swift, obsoleted: 1)
  @usableFromInline
  internal func mapError<NewFailure>(
    _ transform: (Failure) -> NewFailure
  ) -> Result<Success, NewFailure> {
    switch self {
    case let .success(success):
      return .success(success)
    case let .failure(failure):
      return .failure(transform(failure))
    }
  }
}

extension Result {
  /// Returns a new result, mapping any success value using the given
  /// transformation and unwrapping the produced result.
  ///
  /// Use this method to avoid a nested result when your transformation
  /// produces another `Result` type.
  ///
  /// In this example, note the difference in the result of using `map` and
  /// `flatMap` with a transformation that returns a result type.
  ///
  ///     func getNextInteger() -> Result<Int, Error> {
  ///         .success(4)
  ///     }
  ///     func getNextAfterInteger(_ n: Int) -> Result<Int, Error> {
  ///         .success(n + 1)
  ///     }
  ///
  ///     let result = getNextInteger().map { getNextAfterInteger($0) }
  ///     // result == .success(.success(5))
  ///
  ///     let result = getNextInteger().flatMap { getNextAfterInteger($0) }
  ///     // result == .success(5)
  ///
  /// - Parameter transform: A closure that takes the success value of the
  ///   instance.
  /// - Returns: A `Result` instance, either from the closure or the previous
  ///   `.failure`.
  @_alwaysEmitIntoClient
  @_disfavoredOverload // FIXME: Workaround for source compat issue with
                       // functions that used to shadow the original flatMap
                       // (rdar://125016028)
  public func flatMap<NewSuccess: ~Copyable>(
    _ transform: (Success) -> Result<NewSuccess, Failure>
  ) -> Result<NewSuccess, Failure> {
    switch self {
    case let .success(success):
      return transform(success)
    case let .failure(failure):
      return .failure(failure)
    }
  }

  @_spi(SwiftStdlibLegacyABI) @available(swift, obsoleted: 1)
  @_silgen_name("$ss6ResultO7flatMapyAByqd__q_GADxXElF")
  @usableFromInline
  internal func __abi_flatMap<NewSuccess>(
    _ transform: (Success) -> Result<NewSuccess, Failure>
  ) -> Result<NewSuccess, Failure> {
    switch self {
    case let .success(success):
      return transform(success)
    case let .failure(failure):
      return .failure(failure)
    }
  }
}

@_disallowFeatureSuppression(NoncopyableGenerics)
extension Result where Success: ~Copyable {
  // FIXME(NCG): Make this public.
  @_alwaysEmitIntoClient
  public consuming func _consumingFlatMap<NewSuccess: ~Copyable>(
    _ transform: (consuming Success) -> Result<NewSuccess, Failure>
  ) -> Result<NewSuccess, Failure> {
    switch consume self {
    case let .success(success):
      return transform(consume success)
    case let .failure(failure):
      return .failure(failure)
    }
  }

  // FIXME(NCG): Make this public.
  @_alwaysEmitIntoClient
  public borrowing func _borrowingFlatMap<NewSuccess: ~Copyable>(
    _ transform: (borrowing Success) -> Result<NewSuccess, Failure>
  ) -> Result<NewSuccess, Failure> {
    switch self {
    case .success(let success):
      return transform(success)
    case let .failure(failure):
      return .failure(failure)
    }
  }
}

extension Result where Success: ~Copyable {
  /// Returns a new result, mapping any failure value using the given
  /// transformation and unwrapping the produced result.
  ///
  /// - Parameter transform: A closure that takes the failure value of the
  ///   instance.
  /// - Returns: A `Result` instance, either from the closure or the previous
  ///   `.success`.
  @_alwaysEmitIntoClient
  public consuming func flatMapError<NewFailure>(
    _ transform: (Failure) -> Result<Success, NewFailure>
  ) -> Result<Success, NewFailure> {
    switch consume self {
    case let .success(success):
      return .success(success)
    case let .failure(failure):
      return transform(failure)
    }
  }
}

extension Result {
  @_spi(SwiftStdlibLegacyABI) @available(swift, obsoleted: 1)
  @_silgen_name("$ss6ResultO12flatMapErroryAByxqd__GADq_XEs0D0Rd__lF")
  @usableFromInline
  internal func __abi_flatMapError<NewFailure>(
    _ transform: (Failure) -> Result<Success, NewFailure>
  ) -> Result<Success, NewFailure> {
    switch self {
    case let .success(success):
      return .success(success)
    case let .failure(failure):
      return transform(failure)
    }
  }
}

extension Result where Success: ~Copyable {
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
  @_alwaysEmitIntoClient
  public consuming func get() throws(Failure) -> Success {
    switch consume self {
    case let .success(success):
      return success
    case let .failure(failure):
      throw failure
    }
  }
}

extension Result where Success: ~Copyable {
  /// Creates a new result by evaluating a throwing closure, capturing the
  /// returned value as a success, or any thrown error as a failure.
  ///
  /// - Parameter body: A potentially throwing closure to evaluate.
  @_alwaysEmitIntoClient
  public init(catching body: () throws(Failure) -> Success) {
    do {
      self = .success(try body())
    } catch {
      self = .failure(error)
    }
  }
}

extension Result {
  /// ABI: Historical get() throws
  @_spi(SwiftStdlibLegacyABI) @available(swift, obsoleted: 1)
  @_silgen_name("$ss6ResultO3getxyKF")
  @usableFromInline
  func __abi_get() throws -> Success {
    switch self {
    case let .success(success):
      return success
    case let .failure(failure):
      throw failure
    }
  }

}

extension Result where Failure == Swift.Error {
  /// ABI: Historical init(catching:)
  @_spi(SwiftStdlibLegacyABI) @available(swift, obsoleted: 1)
  @_silgen_name("$ss6ResultOss5Error_pRs_rlE8catchingAByxsAC_pGxyKXE_tcfC")
  @usableFromInline
  init(__abi_catching body: () throws(Failure) -> Success) {
    do {
      self = .success(try body())
    } catch {
      self = .failure(error)
    }
  }
}
