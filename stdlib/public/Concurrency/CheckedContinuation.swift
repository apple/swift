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
@_silgen_name("swift_continuation_logFailedCheck")
internal func logFailedCheck(_ message: UnsafeRawPointer)

/// Implementation class that holds the `UnsafeContinuation` instance for
/// a `CheckedContinuation`.
@available(SwiftStdlib 5.1, *)
internal final class CheckedContinuationCanary: @unchecked Sendable {
  // The instance state is stored in tail-allocated raw memory, so that
  // we can atomically check the continuation state.

  private init() { fatalError("must use create") }

  private static func _create(continuation: UnsafeRawPointer, function: String)
      -> Self {
    let instance = Builtin.allocWithTailElems_1(self,
      1._builtinWordValue,
      (UnsafeRawPointer?, String).self)

    instance._continuationPtr.initialize(to: continuation)
    instance._functionPtr.initialize(to: function)
    return instance
  }

  private var _continuationPtr: UnsafeMutablePointer<UnsafeRawPointer?> {
    return UnsafeMutablePointer<UnsafeRawPointer?>(
      Builtin.projectTailElems(self, (UnsafeRawPointer?, String).self))
  }
  private var _functionPtr: UnsafeMutablePointer<String> {
    let tailPtr = UnsafeMutableRawPointer(
      Builtin.projectTailElems(self, (UnsafeRawPointer?, String).self))

    let functionPtr = tailPtr 
        + MemoryLayout<(UnsafeRawPointer?, String)>.offset(of: \(UnsafeRawPointer?, String).1)!

    return functionPtr.assumingMemoryBound(to: String.self)
  }

  internal static func create<T, E>(continuation: UnsafeContinuation<T, E>,
                                 function: String) -> Self {
    return _create(
        continuation: unsafeBitCast(continuation, to: UnsafeRawPointer.self),
        function: function)
  }

  internal var function: String {
    return _functionPtr.pointee
  }

  // Take the continuation away from the container, or return nil if it's
  // already been taken.
  internal func takeContinuation<T, E>() -> UnsafeContinuation<T, E>? {
    // Atomically exchange the current continuation value with a null pointer.
    let rawContinuationPtr = unsafeBitCast(_continuationPtr,
      to: Builtin.RawPointer.self)
    let rawOld = Builtin.atomicrmw_xchg_seqcst_Word(rawContinuationPtr,
      0._builtinWordValue)

    return unsafeBitCast(rawOld, to: UnsafeContinuation<T, E>?.self)
  }

  deinit {
    _functionPtr.deinitialize(count: 1)
    // Log if the continuation was never consumed before the instance was
    // destructed.
    if _continuationPtr.pointee != nil {
      logFailedCheck("SWIFT TASK CONTINUATION MISUSE: \(function) leaked its continuation!\n")
    }
  }
}

/// A mechanism to interface
/// between synchronous and asynchronous code,
/// logging correctness violations.
///
/// A *continuation* is an opaque representation of program state.
/// To create a continuation in asynchronous code,
/// call the `withUnsafeContinuation(function:_:)` or
/// `withUnsafeThrowingContinuation(function:_:)` function.
/// To resume the asynchronous task,
/// call the `resume(returning:)`,
/// `resume(throwing:)`,
/// `resume(with:)`,
/// or `resume()` method.
///
/// - Important: You must call a resume method exactly once
///   on every execution path throughout the program.
///
/// Resuming from a continuation more than once is undefined behavior.
/// Never resuming leaves the task in a suspended state indefinitely,
/// and leaks any associated resources.
/// `CheckedContinuation` logs a message
/// if either of these invariants is violated.
///
/// `CheckedContinuation` performs runtime checks
/// for missing or multiple resume operations.
/// `UnsafeContinuation` avoids enforcing these invariants at runtime
/// because it aims to be a low-overhead mechanism
/// for interfacing Swift tasks with
/// event loops, delegate methods, callbacks,
/// and other non-`async` scheduling mechanisms.
/// However, during development, the ability to verify that the
/// invariants are being upheld in testing is important.
/// Because both types have the same interface,
/// you can replace one with the other in most circumstances,
/// without making other changes.
@available(SwiftStdlib 5.1, *)
public struct CheckedContinuation<T, E: Error>: Sendable {
  private let canary: CheckedContinuationCanary
  
  /// Creates a checked continuation from an unsafe continuation.
  ///
  /// Instead of calling this initializer,
  /// most code calls the `withCheckedContinuation(function:_:)` or
  /// `withCheckedThrowingContinuation(function:_:)` function instead.
  /// You only need to initialize
  /// your own `CheckedContinuation<T, E>` if you already have an
  /// `UnsafeContinuation` you want to impose checking on.
  ///
  /// - Parameters:
  ///   - continuation: An instance of `UnsafeContinuation`
  ///     that hasn't yet been resumed.
  ///     After passing the unsafe continuation to this initializer,
  ///     don't use it outside of this object.
  ///   - function: A string identifying the declaration that is the notional
  ///     source for the continuation, used to identify the continuation in
  ///     runtime diagnostics related to misuse of this continuation.
  public init(continuation: UnsafeContinuation<T, E>, function: String = #function) {
    canary = CheckedContinuationCanary.create(
      continuation: continuation,
      function: function)
  }
  
  /// Resume the task awaiting the continuation by having it return normally
  /// from its suspension point.
  ///
  /// - Parameter value: The value to return from the continuation.
  ///
  /// A continuation must be resumed exactly once. If the continuation has
  /// already been resumed through this object, then the attempt to resume
  /// the continuation will trap.
  ///
  /// After `resume` enqueues the task, control immediately returns to
  /// the caller. The task continues executing when its executor is
  /// able to reschedule it.
  public func resume(returning value: __owned T) {
    if let c: UnsafeContinuation<T, E> = canary.takeContinuation() {
      c.resume(returning: value)
    } else {
      fatalError("SWIFT TASK CONTINUATION MISUSE: \(canary.function) tried to resume its continuation more than once, returning \(value)!\n")
    }
  }
  
  /// Resume the task awaiting the continuation by having it throw an error
  /// from its suspension point.
  ///
  /// - Parameter error: The error to throw from the continuation.
  ///
  /// A continuation must be resumed exactly once. If the continuation has
  /// already been resumed through this object, then the attempt to resume
  /// the continuation will trap.
  ///
  /// After `resume` enqueues the task, control immediately returns to
  /// the caller. The task continues executing when its executor is
  /// able to reschedule it.
  public func resume(throwing error: __owned E) {
    if let c: UnsafeContinuation<T, E> = canary.takeContinuation() {
      c.resume(throwing: error)
    } else {
      fatalError("SWIFT TASK CONTINUATION MISUSE: \(canary.function) tried to resume its continuation more than once, throwing \(error)!\n")
    }
  }
}

@available(SwiftStdlib 5.1, *)
extension CheckedContinuation {
  /// Resume the task awaiting the continuation by having it either
  /// return normally or throw an error based on the state of the given
  /// `Result` value.
  ///
  /// - Parameter result: A value to either return or throw from the
  ///   continuation.
  ///
  /// A continuation must be resumed exactly once. If the continuation has
  /// already been resumed through this object, then the attempt to resume
  /// the continuation will trap.
  ///
  /// After `resume` enqueues the task, control immediately returns to
  /// the caller. The task continues executing when its executor is
  /// able to reschedule it.
  @_alwaysEmitIntoClient
  public func resume<Er: Error>(with result: Result<T, Er>) where E == Error {
    switch result {
      case .success(let val):
        self.resume(returning: val)
      case .failure(let err):
        self.resume(throwing: err)
    }
  }

  /// Resume the task awaiting the continuation by having it either
  /// return normally or throw an error based on the state of the given
  /// `Result` value.
  ///
  /// - Parameter result: A value to either return or throw from the
  ///   continuation.
  ///
  /// A continuation must be resumed exactly once. If the continuation has
  /// already been resumed through this object, then the attempt to resume
  /// the continuation will trap.
  ///
  /// After `resume` enqueues the task, control immediately returns to
  /// the caller. The task continues executing when its executor is
  /// able to reschedule it.
  @_alwaysEmitIntoClient
  public func resume(with result: Result<T, E>) {
    switch result {
      case .success(let val):
        self.resume(returning: val)
      case .failure(let err):
        self.resume(throwing: err)
    }
  }

  /// Resume the task awaiting the continuation by having it return normally
  /// from its suspension point.
  ///
  /// A continuation must be resumed exactly once. If the continuation has
  /// already been resumed through this object, then the attempt to resume
  /// the continuation will trap.
  ///
  /// After `resume` enqueues the task, control immediately returns to
  /// the caller. The task continues executing when its executor is
  /// able to reschedule it.
  @_alwaysEmitIntoClient
  public func resume() where T == Void {
    self.resume(returning: ())
  }
}

/// Invokes the passed in closure with a checked continuation for the current task.
///
/// The body of the closure executes synchronously on the calling task, and once it returns
/// the calling task is suspended. It is possible to immediately resume the task, or escape the
/// continuation in order to complete it afterwards, which will then resume the suspended task.
///
/// You must invoke the continuation's `resume` method exactly once.
///
/// Missing to invoke it (eventually) will cause the calling task to remain suspended
/// indefinitely which will result in the task "hanging" as well as being leaked with
/// no possibility to destroy it.
///
/// The checked continuation offers detection of mis-use, and dropping the last reference
/// to it, without having resumed it will trigger a warning. Resuming a continuation twice
/// is also diagnosed and will cause a crash.
///
/// - Parameters:
///   - function: A string identifying the declaration that is the notional
///     source for the continuation, used to identify the continuation in
///     runtime diagnostics related to misuse of this continuation.
///   - body: A closure that takes a `CheckedContinuation` parameter.
/// - Returns: The value continuation is resumed with.
///
/// - SeeAlso: `withCheckedThrowingContinuation(function:_:)`
/// - SeeAlso: `withUnsafeContinuation(function:_:)`
/// - SeeAlso: `withUnsafeThrowingContinuation(function:_:)`
@available(SwiftStdlib 5.1, *)
@_unsafeInheritExecutor // ABI compatibility with Swift 5.1
@inlinable
public func withCheckedContinuation<T>(
    function: String = #function,
    _ body: (CheckedContinuation<T, Never>) -> Void
) async -> T {
  return await withUnsafeContinuation {
    body(CheckedContinuation(continuation: $0, function: function))
  }
}

/// Invokes the passed in closure with a checked continuation for the current task.
///
/// The body of the closure executes synchronously on the calling task, and once it returns
/// the calling task is suspended. It is possible to immediately resume the task, or escape the
/// continuation in order to complete it afterwards, which will then resume the suspended task.
///
/// If `resume(throwing:)` is called on the continuation, this function throws that error.
///
/// You must invoke the continuation's `resume` method exactly once.
///
/// Missing to invoke it (eventually) will cause the calling task to remain suspended
/// indefinitely which will result in the task "hanging" as well as being leaked with
/// no possibility to destroy it.
///
/// The checked continuation offers detection of mis-use, and dropping the last reference
/// to it, without having resumed it will trigger a warning. Resuming a continuation twice
/// is also diagnosed and will cause a crash.
///
/// - Parameters:
///   - function: A string identifying the declaration that is the notional
///     source for the continuation, used to identify the continuation in
///     runtime diagnostics related to misuse of this continuation.
///   - body: A closure that takes a `CheckedContinuation` parameter.
/// - Returns: The value continuation is resumed with.
///
/// - SeeAlso: `withCheckedContinuation(function:_:)`
/// - SeeAlso: `withUnsafeContinuation(function:_:)`
/// - SeeAlso: `withUnsafeThrowingContinuation(function:_:)`
@available(SwiftStdlib 5.1, *)
@_unsafeInheritExecutor // ABI compatibility with Swift 5.1
@inlinable
public func withCheckedThrowingContinuation<T>(
    function: String = #function,
    _ body: (CheckedContinuation<T, Error>) -> Void
) async throws -> T {
  return try await withUnsafeThrowingContinuation {
    body(CheckedContinuation(continuation: $0, function: function))
  }
}

