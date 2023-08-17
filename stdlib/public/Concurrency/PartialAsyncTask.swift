//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Swift
@_implementationOnly import _SwiftConcurrencyShims

// TODO(swift): rename the file to ExecutorJob.swift eventually, we don't use PartialTask terminology anymore

@available(SwiftStdlib 5.1, *)
@_silgen_name("swift_job_run")
@usableFromInline
internal func _swiftJobRun(_ job: UnownedJob,
                           _ executor: UnownedSerialExecutor) -> ()

// ==== -----------------------------------------------------------------------
// MARK: UnownedJob

/// A unit of scheduleable work.
///
/// Unless you're implementing a scheduler,
/// you don't generally interact with jobs directly.
///
/// An `UnownedJob` must be eventually run *exactly once* using ``runSynchronously(on:)``.
/// Not doing so is effectively going to leak and "hang" the work that the job represents (e.g. a ``Task``).
@available(SwiftStdlib 5.1, *)
@frozen
public struct UnownedJob: Sendable {
  private var context: Builtin.Job

  @usableFromInline
  @available(SwiftStdlib 5.9, *)
  internal init(context: Builtin.Job) {
    self.context = context
  }

  #if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  /// Create an `UnownedJob` whose lifetime must be managed carefully until it is run exactly once.
  @available(SwiftStdlib 5.9, *)
  public init(_ job: __owned Job) { // must remain '__owned' in order to not break ABI
    self.context = job.context
  }
  #endif // !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY

  #if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  /// Create an `UnownedJob` whose lifetime must be managed carefully until it is run exactly once.
  @available(SwiftStdlib 5.9, *)
  public init(_ job: __owned ExecutorJob) { // must remain '__owned' in order to not break ABI
    self.context = job.context
  }
  #endif // !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY

  /// The priority of this job.
  @available(SwiftStdlib 5.9, *)
  public var priority: JobPriority {
    let raw = _swift_concurrency_jobPriority(self)
    return JobPriority(rawValue: raw)
  }

  @available(SwiftStdlib 5.9, *)
  internal var _context: Builtin.Job {
    context
  }

  /// Deprecated API to run a job on a specific executor.
  @_alwaysEmitIntoClient
  @inlinable
  @available(*, deprecated, renamed: "ExecutorJob.runSynchronously(on:)")
  public func _runSynchronously(on executor: UnownedSerialExecutor) {
    _swiftJobRun(self, executor)
  }

  /// Run this job on the passed in executor.
  ///
  /// This operation runs the job on the calling thread and *blocks* until the job completes.
  /// The intended use of this method is for an executor to determine when and where it
  /// wants to run the job and then call this method on it.
  ///
  /// The passed in executor reference is used to establish the executor context for the job,
  /// and should be the same executor as the one semantically calling the `runSynchronously` method.
  ///
  /// - Parameter executor: the executor this job will be semantically running on.
  @_alwaysEmitIntoClient
  @inlinable
  public func runSynchronously(on executor: UnownedSerialExecutor) {
    _swiftJobRun(self, executor)
  }

}

@available(SwiftStdlib 5.9, *)
extension UnownedJob: CustomStringConvertible {
  @available(SwiftStdlib 5.9, *)
  public var description: String {
    let id = _getJobTaskId(self)
    /// Tasks are always assigned an unique ID, however some jobs may not have it set,
    /// and it appearing as 0 for _different_ jobs may lead to misunderstanding it as
    /// being "the same 0 id job", we specifically print 0 (id not set) as nil.
    if (id > 0) {
      return "\(Self.self)(id: \(id))"
    } else {
      return "\(Self.self)(id: nil)"
    }
  }
}

#if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY

/// Deprecated equivalent of ``ExecutorJob``.
///
/// A unit of scheduleable work.
///
/// Unless you're implementing a scheduler,
/// you don't generally interact with jobs directly.
@available(SwiftStdlib 5.9, *)
@available(*, deprecated, renamed: "ExecutorJob")
@frozen
@_moveOnly
public struct Job: Sendable {
  internal var context: Builtin.Job

  @usableFromInline
  internal init(context: __owned Builtin.Job) {
    self.context = context
  }

  public init(_ job: UnownedJob) {
    self.context = job._context
  }

  public init(_ job: __owned ExecutorJob) {
    self.context = job.context
  }

  public var priority: JobPriority {
    let raw = _swift_concurrency_jobPriority(UnownedJob(context: self.context))
    return JobPriority(rawValue: raw)
  }

  // TODO: move only types cannot conform to protocols, so we can't conform to CustomStringConvertible;
  //       we can still offer a description to be called explicitly though.
  public var description: String {
    let id = _getJobTaskId(UnownedJob(context: self.context))
    /// Tasks are always assigned an unique ID, however some jobs may not have it set,
    /// and it appearing as 0 for _different_ jobs may lead to misunderstanding it as
    /// being "the same 0 id job", we specifically print 0 (id not set) as nil.
    if (id > 0) {
      return "Job(id: \(id))"
    } else {
      return "Job(id: nil)"
    }
  }
}

@available(SwiftStdlib 5.9, *)
extension Job {

  /// Run this job on the passed in executor.
  ///
  /// This operation runs the job on the calling thread and *blocks* until the job completes.
  /// The intended use of this method is for an executor to determine when and where it
  /// wants to run the job and then call this method on it.
  ///
  /// The passed in executor reference is used to establish the executor context for the job,
  /// and should be the same executor as the one semantically calling the `runSynchronously` method.
  ///
  /// This operation consumes the job, preventing it accidental use after it has ben run.
  ///
  /// Converting a `ExecutorJob` to an ``UnownedJob`` and invoking ``UnownedJob/runSynchronously(_:)` on it multiple times is undefined behavior,
  /// as a job can only ever be run once, and must not be accessed after it has been run.
  ///
  /// - Parameter executor: the executor this job will be semantically running on.
  @_alwaysEmitIntoClient
  @inlinable
  __consuming public func runSynchronously(on executor: UnownedSerialExecutor) {
    _swiftJobRun(UnownedJob(self), executor)
  }
}

/// A unit of scheduleable work.
///
/// Unless you're implementing a scheduler,
/// you don't generally interact with jobs directly.
@available(SwiftStdlib 5.9, *)
@frozen
@_moveOnly
public struct ExecutorJob: Sendable {
  internal var context: Builtin.Job

  @usableFromInline
  internal init(context: __owned Builtin.Job) {
    self.context = context
  }

  public init(_ job: UnownedJob) {
    self.context = job._context
  }

  public init(_ job: __owned Job) {
    self.context = job.context
  }

  public var priority: JobPriority {
    let raw = _swift_concurrency_jobPriority(UnownedJob(context: self.context))
    return JobPriority(rawValue: raw)
  }

  // TODO: move only types cannot conform to protocols, so we can't conform to CustomStringConvertible;
  //       we can still offer a description to be called explicitly though.
  public var description: String {
    let id = _getJobTaskId(UnownedJob(context: self.context))
    /// Tasks are always assigned an unique ID, however some jobs may not have it set,
    /// and it appearing as 0 for _different_ jobs may lead to misunderstanding it as
    /// being "the same 0 id job", we specifically print 0 (id not set) as nil.
    if (id > 0) {
      return "ExecutorJob(id: \(id))"
    } else {
      return "ExecutorJob(id: nil)"
    }
  }
}

@available(SwiftStdlib 5.9, *)
extension ExecutorJob {

  /// Run this job on the passed in executor.
  ///
  /// This operation runs the job on the calling thread and *blocks* until the job completes.
  /// The intended use of this method is for an executor to determine when and where it
  /// wants to run the job and then call this method on it.
  ///
  /// The passed in executor reference is used to establish the executor context for the job,
  /// and should be the same executor as the one semantically calling the `runSynchronously` method.
  ///
  /// This operation consumes the job, preventing it accidental use after it has ben run.
  ///
  /// Converting a `ExecutorJob` to an ``UnownedJob`` and invoking ``UnownedJob/runSynchronously(_:)` on it multiple times is undefined behavior,
  /// as a job can only ever be run once, and must not be accessed after it has been run.
  ///
  /// - Parameter executor: the executor this job will be semantically running on.
  @_alwaysEmitIntoClient
  @inlinable
  __consuming public func runSynchronously(on executor: UnownedSerialExecutor) {
    _swiftJobRun(UnownedJob(self), executor)
  }
}
#endif // !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY

// ==== -----------------------------------------------------------------------
// MARK: JobPriority

/// The priority of this job.
///
/// The executor determines how priority information affects the way tasks are scheduled.
/// The behavior varies depending on the executor currently being used.
/// Typically, executors attempt to run tasks with a higher priority
/// before tasks with a lower priority.
/// However, the semantics of how priority is treated are left up to each
/// platform and `Executor` implementation.
///
/// A ExecutorJob's priority is roughly equivalent to a `TaskPriority`,
/// however, since not all jobs are tasks, represented as separate type.
///
/// Conversions between the two priorities are available as initializers on the respective types.
@available(SwiftStdlib 5.9, *)
@frozen
public struct JobPriority: Sendable {
  public typealias RawValue = UInt8

  /// The raw priority value.
  public var rawValue: RawValue
}

@available(SwiftStdlib 5.9, *)
extension TaskPriority {
  /// Convert this ``UnownedJob/Priority`` to a ``TaskPriority``.
  ///
  /// Most values are directly interchangeable, but this initializer reserves the right to fail for certain values.
  @available(SwiftStdlib 5.9, *)
  public init?(_ p: JobPriority) {
    guard p.rawValue != 0 else {
      /// 0 is "undefined"
      return nil
    }
    self = TaskPriority(rawValue: p.rawValue)
  }
}

@available(SwiftStdlib 5.9, *)
extension JobPriority: Equatable {
  public static func == (lhs: JobPriority, rhs: JobPriority) -> Bool {
    lhs.rawValue == rhs.rawValue
  }

  public static func != (lhs: JobPriority, rhs: JobPriority) -> Bool {
    lhs.rawValue != rhs.rawValue
  }
}

@available(SwiftStdlib 5.9, *)
extension JobPriority: Comparable {
  public static func < (lhs: JobPriority, rhs: JobPriority) -> Bool {
    lhs.rawValue < rhs.rawValue
  }

  public static func <= (lhs: JobPriority, rhs: JobPriority) -> Bool {
    lhs.rawValue <= rhs.rawValue
  }

  public static func > (lhs: JobPriority, rhs: JobPriority) -> Bool {
    lhs.rawValue > rhs.rawValue
  }

  public static func >= (lhs: JobPriority, rhs: JobPriority) -> Bool {
    lhs.rawValue >= rhs.rawValue
  }
}

// ==== -----------------------------------------------------------------------
// MARK: UncheckedContinuation

/// A mechanism to interface
/// between synchronous and asynchronous code,
/// without correctness checking.
///
/// A *continuation* is an opaque representation of program state.
/// To create a continuation in asynchronous code,
/// call the `withUnsafeContinuation(_:)` or
/// `withUnsafeThrowingContinuation(_:)` function.
/// To resume the asynchronous task,
/// call the `resume(returning:)`,
/// `resume(throwing:)`,
/// `resume(with:)`,
/// or `resume()` method.
///
/// - Important: You must call a resume method exactly once
///   on every execution path throughout the program.
///   Resuming from a continuation more than once is undefined behavior.
///   Never resuming leaves the task in a suspended state indefinitely,
///   and leaks any associated resources.
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
@frozen
public struct UnsafeContinuation<T, E: Error>: Sendable {
  @usableFromInline internal var context: Builtin.RawUnsafeContinuation

  @_alwaysEmitIntoClient
  internal init(_ context: Builtin.RawUnsafeContinuation) {
    self.context = context
  }

  /// Resume the task that's awaiting the continuation
  /// by returning the given value.
  ///
  /// - Parameter value: The value to return from the continuation.
  ///
  /// A continuation must be resumed exactly once.
  /// If the continuation has already resumed,
  /// then calling this method results in undefined behavior.
  ///
  /// After calling this method,
  /// control immediately returns to the caller.
  /// The task continues executing
  /// when its executor schedules it.
  @_alwaysEmitIntoClient
  public func resume(returning value: __owned T) where E == Never {
    #if compiler(>=5.5) && $BuiltinContinuation
    Builtin.resumeNonThrowingContinuationReturning(context, value)
    #else
    fatalError("Swift compiler is incompatible with this SDK version")
    #endif
  }

  /// Resume the task that's awaiting the continuation
  /// by returning the given value.
  ///
  /// - Parameter value: The value to return from the continuation.
  ///
  /// A continuation must be resumed exactly once.
  /// If the continuation has already resumed,
  /// then calling this method results in undefined behavior.
  ///
  /// After calling this method,
  /// control immediately returns to the caller.
  /// The task continues executing
  /// when its executor schedules it.
  @_alwaysEmitIntoClient
  public func resume(returning value: __owned T) {
    #if compiler(>=5.5) && $BuiltinContinuation
    Builtin.resumeThrowingContinuationReturning(context, value)
    #else
    fatalError("Swift compiler is incompatible with this SDK version")
    #endif
  }

  /// Resume the task that's awaiting the continuation
  /// by throwing the given error.
  ///
  /// - Parameter error: The error to throw from the continuation.
  ///
  /// A continuation must be resumed exactly once.
  /// If the continuation has already resumed,
  /// then calling this method results in undefined behavior.
  ///
  /// After calling this method,
  /// control immediately returns to the caller.
  /// The task continues executing
  /// when its executor schedules it.
  @_alwaysEmitIntoClient
  public func resume(throwing error: __owned E) {
    #if compiler(>=5.5) && $BuiltinContinuation
    Builtin.resumeThrowingContinuationThrowing(context, error)
    #else
    fatalError("Swift compiler is incompatible with this SDK version")
    #endif
  }
}

@available(SwiftStdlib 5.1, *)
extension UnsafeContinuation {
  /// Resume the task that's awaiting the continuation
  /// by returning or throwing the given result value.
  ///
  /// - Parameter result: The result.
  ///   If it contains a `.success` value,
  ///   the continuation returns that value;
  ///   otherwise, it throws the `.error` value.
  ///
  /// A continuation must be resumed exactly once.
  /// If the continuation has already resumed,
  /// then calling this method results in undefined behavior.
  ///
  /// After calling this method,
  /// control immediately returns to the caller.
  /// The task continues executing
  /// when its executor schedules it.
  @_alwaysEmitIntoClient
  public func resume<Er: Error>(with result: Result<T, Er>) where E == Error {
    switch result {
      case .success(let val):
        self.resume(returning: val)
      case .failure(let err):
        self.resume(throwing: err)
    }
  }

  /// Resume the task that's awaiting the continuation
  /// by returning or throwing the given result value.
  ///
  /// - Parameter result: The result.
  ///   If it contains a `.success` value,
  ///   the continuation returns that value;
  ///   otherwise, it throws the `.error` value.
  ///
  /// A continuation must be resumed exactly once.
  /// If the continuation has already resumed,
  /// then calling this method results in undefined behavior.
  ///
  /// After calling this method,
  /// control immediately returns to the caller.
  /// The task continues executing
  /// when its executor schedules it.
  @_alwaysEmitIntoClient
  public func resume(with result: Result<T, E>) {
    switch result {
      case .success(let val):
        self.resume(returning: val)
      case .failure(let err):
        self.resume(throwing: err)
    }
  }

  /// Resume the task that's awaiting the continuation by returning.
  ///
  /// A continuation must be resumed exactly once.
  /// If the continuation has already resumed,
  /// then calling this method results in undefined behavior.
  ///
  /// After calling this method,
  /// control immediately returns to the caller.
  /// The task continues executing
  /// when its executor schedules it.
  @_alwaysEmitIntoClient
  public func resume() where T == Void {
    self.resume(returning: ())
  }
}

#if _runtime(_ObjC)

// Intrinsics used by SILGen to resume or fail continuations.
@available(SwiftStdlib 5.1, *)
@_alwaysEmitIntoClient
internal func _resumeUnsafeContinuation<T>(
  _ continuation: UnsafeContinuation<T, Never>,
  _ value: __owned T
) {
  continuation.resume(returning: value)
}

@available(SwiftStdlib 5.1, *)
@_alwaysEmitIntoClient
internal func _resumeUnsafeThrowingContinuation<T>(
  _ continuation: UnsafeContinuation<T, Error>,
  _ value: __owned T
) {
  continuation.resume(returning: value)
}

@available(SwiftStdlib 5.1, *)
@_alwaysEmitIntoClient
internal func _resumeUnsafeThrowingContinuationWithError<T>(
  _ continuation: UnsafeContinuation<T, Error>,
  _ error: __owned Error
) {
  continuation.resume(throwing: error)
}

#endif


/// Invokes the passed in closure with a unsafe continuation for the current task.
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
/// Unlike the "checked" continuation variant, the `UnsafeContinuation` does not
/// detect or diagnose any kind of misuse, so you need to be extra careful to avoid
/// calling `resume` twice or forgetting to call resume before letting go of the
/// continuation object.
///
/// - Parameter fn: A closure that takes an `UnsafeContinuation` parameter.
/// - Returns: The value continuation is resumed with.
///
/// - SeeAlso: `withUnsafeThrowingContinuation(function:_:)`
/// - SeeAlso: `withCheckedContinuation(function:_:)`
/// - SeeAlso: `withCheckedThrowingContinuation(function:_:)`
@available(SwiftStdlib 5.1, *)
@_unsafeInheritExecutor
@_alwaysEmitIntoClient
public func withUnsafeContinuation<T>(
  _ fn: (UnsafeContinuation<T, Never>) -> Void
) async -> T {
  return await Builtin.withUnsafeContinuation {
    fn(UnsafeContinuation<T, Never>($0))
  }
}

/// Invokes the passed in closure with a unsafe continuation for the current task.
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
/// Unlike the "checked" continuation variant, the `UnsafeContinuation` does not
/// detect or diagnose any kind of misuse, so you need to be extra careful to avoid
/// calling `resume` twice or forgetting to call resume before letting go of the
/// continuation object.
///
/// - Parameter fn: A closure that takes an `UnsafeContinuation` parameter.
/// - Returns: The value continuation is resumed with.
///
/// - SeeAlso: `withUnsafeContinuation(function:_:)`
/// - SeeAlso: `withCheckedContinuation(function:_:)`
/// - SeeAlso: `withCheckedThrowingContinuation(function:_:)`
@available(SwiftStdlib 5.1, *)
@_unsafeInheritExecutor
@_alwaysEmitIntoClient
public func withUnsafeThrowingContinuation<T>(
  _ fn: (UnsafeContinuation<T, Error>) -> Void
) async throws -> T {
  return try await Builtin.withUnsafeThrowingContinuation {
    fn(UnsafeContinuation<T, Error>($0))
  }
}

/// A hack to mark an SDK that supports swift_continuation_await.
@available(SwiftStdlib 5.1, *)
@_alwaysEmitIntoClient
public func _abiEnableAwaitContinuation() {
  fatalError("never use this function")
}

// ==== -----------------------------------------------------------------------
// MARK: Runtime functions

@available(SwiftStdlib 5.9, *)
@_silgen_name("swift_concurrency_jobPriority")
internal func _swift_concurrency_jobPriority(_ job: UnownedJob) -> UInt8
