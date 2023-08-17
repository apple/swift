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

/// A service that can execute jobs.
@available(SwiftStdlib 5.1, *)
public protocol Executor: AnyObject, Sendable {

  // Since lack move-only type support in the SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY configuration
  // Do not deprecate the UnownedJob enqueue in that configuration just yet - as we cannot introduce the replacements.
  #if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  @available(SwiftStdlib 5.1, *)
  @available(*, deprecated, message: "Implement 'enqueue(_: __owned ExecutorJob)' instead")
  #endif // !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  func enqueue(_ job: UnownedJob)

  // Cannot introduce these methods in SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  // since it lacks move-only type support.
  #if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  @available(SwiftStdlib 5.9, *)
  @available(*, deprecated, message: "Implement 'enqueue(_: __owned ExecutorJob)' instead")
  func enqueue(_ job: consuming Job)
  #endif // !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY

  #if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  @available(SwiftStdlib 5.9, *)
  func enqueue(_ job: consuming ExecutorJob)
  #endif // !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
}

/// A service that executes jobs.
@available(SwiftStdlib 5.1, *)
public protocol SerialExecutor: Executor {
  // This requirement is repeated here as a non-override so that we
  // get a redundant witness-table entry for it.  This allows us to
  // avoid drilling down to the base conformance just for the basic
  // work-scheduling operation.
  @_nonoverride
  #if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  @available(SwiftStdlib 5.1, *)
  @available(*, deprecated, message: "Implement 'enqueue(_: __owned ExecutorJob)' instead")
  #endif // !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  func enqueue(_ job: UnownedJob)

  #if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  // This requirement is repeated here as a non-override so that we
  // get a redundant witness-table entry for it.  This allows us to
  // avoid drilling down to the base conformance just for the basic
  // work-scheduling operation.
  @_nonoverride
  @available(SwiftStdlib 5.9, *)
  @available(*, deprecated, message: "Implement 'enqueue(_: __owned ExecutorJob)' instead")
  func enqueue(_ job: consuming Job)
  #endif // !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY

  #if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  // This requirement is repeated here as a non-override so that we
  // get a redundant witness-table entry for it.  This allows us to
  // avoid drilling down to the base conformance just for the basic
  // work-scheduling operation.
  @_nonoverride
  @available(SwiftStdlib 5.9, *)
  func enqueue(_ job: consuming ExecutorJob)
  #endif // !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY

  /// Convert this executor value to the optimized form of borrowed
  /// executor references.
  func asUnownedSerialExecutor() -> UnownedSerialExecutor

  /// If this executor has complex equality semantics, and the runtime needs to compare
  /// two executors, it will first attempt the usual pointer-based equality check,
  /// and if it fails it will compare the types of both executors, if they are the same,
  /// it will finally invoke this method, in an attempt to let the executor itself decide
  /// if this and the `other` executor represent the same serial, exclusive, isolation context.
  ///
  /// This method must be implemented with great care, as wrongly returning `true` would allow
  /// code from a different execution context (e.g. thread) to execute code which was intended
  /// to be isolated by another actor.
  ///
  /// This check is not used when performing executor switching.
  ///
  /// This check is used when performing ``Actor/assertIsolated()``, ``Actor/preconditionIsolated()``,
  /// ``Actor/assumeIsolated()`` and similar APIs which assert about the same "exclusive serial execution context".
  ///
  /// - Parameter other: the executor to compare with.
  /// - Returns: true, if `self` and the `other` executor actually are mutually exclusive
  ///            and it is safe–from a concurrency perspective–to execute code assuming one on the other.
  @available(SwiftStdlib 5.9, *)
  func isSameExclusiveExecutionContext(other: Self) -> Bool
}

#if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
@available(SwiftStdlib 5.9, *)
extension Executor {

  // Delegation goes like this:
  // Unowned Job -> Executor Job -> Job -> ---||---

  public func enqueue(_ job: UnownedJob) {
    self.enqueue(ExecutorJob(job))
  }

  public func enqueue(_ job: consuming ExecutorJob) {
    self.enqueue(Job(job))
  }

  public func enqueue(_ job: consuming Job) {
    self.enqueue(UnownedJob(job))
  }
}
#endif // !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY

@available(SwiftStdlib 5.9, *)
extension SerialExecutor {
  @available(SwiftStdlib 5.9, *)
  public func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    UnownedSerialExecutor(ordinary: self)
  }
}

@available(SwiftStdlib 5.9, *)
extension SerialExecutor {

  @available(SwiftStdlib 5.9, *)
  public func isSameExclusiveExecutionContext(other: Self) -> Bool {
    return self === other
  }

}

/// An unowned reference to a serial executor (a `SerialExecutor`
/// value).
///
/// This is an optimized type used internally by the core scheduling
/// operations.  It is an unowned reference to avoid unnecessary
/// reference-counting work even when working with actors abstractly.
/// Generally there are extra constraints imposed on core operations
/// in order to allow this.  For example, keeping an actor alive must
/// also keep the actor's associated executor alive; if they are
/// different objects, the executor must be referenced strongly by the
/// actor.
@available(SwiftStdlib 5.1, *)
@frozen
public struct UnownedSerialExecutor: Sendable {
  #if compiler(>=5.5) && $BuiltinExecutor
  @usableFromInline
  internal var executor: Builtin.Executor

  /// SPI: Do not use. Cannot be marked @_spi, since we need to use it from Distributed module
  /// which needs to reach for this from an @_transparent function which prevents @_spi use.
  @available(SwiftStdlib 5.9, *)
  public var _executor: Builtin.Executor {
    self.executor
  }
  #endif

  @inlinable
  public init(_ executor: Builtin.Executor) {
    #if compiler(>=5.5) && $BuiltinExecutor
    self.executor = executor
    #endif
  }

  @inlinable
  public init<E: SerialExecutor>(ordinary executor: __shared E) {
    #if compiler(>=5.5) && $BuiltinBuildExecutor
    self.executor = Builtin.buildOrdinarySerialExecutorRef(executor)
    #else
    fatalError("Swift compiler is incompatible with this SDK version")
    #endif
  }

  /// Opts the executor into complex "same exclusive execution context" equality checks.
  ///
  /// This means what when asserting or assuming executors, and the current and expected
  /// executor are not the same instance (by object equality), the runtime may invoke
  /// `isSameExclusiveExecutionContext` in order to compare the executors for equality.
  ///
  /// Implementing such complex equality can be useful if multiple executor instances
  /// actually use the same underlying serialization context and can be therefore
  /// safely treated as the same serial exclusive execution context (e.g. multiple
  /// dispatch queues targeting the same serial queue).
  @available(SwiftStdlib 5.9, *)
  @inlinable
  public init<E: SerialExecutor>(complexEquality executor: __shared E) {
    #if compiler(>=5.9) && $BuiltinBuildComplexEqualityExecutor
    self.executor = Builtin.buildComplexEqualitySerialExecutorRef(executor)
    #else
    fatalError("Swift compiler is incompatible with this SDK version")
    #endif
  }

  @_spi(ConcurrencyExecutors)
  @available(SwiftStdlib 5.9, *)
  public var _isComplexEquality: Bool {
    _executor_isComplexEquality(self)
  }

}

/// Checks if the current task is running on the expected executor.
///
/// Generally, Swift programs should be constructed such that it is statically
/// known that a specific executor is used, for example by using global actors or
/// custom executors. However, in some APIs it may be useful to provide an
/// additional runtime check for this, especially when moving towards Swift
/// concurrency from other runtimes which frequently use such assertions.
/// - Parameter executor: The expected executor.
@available(SwiftStdlib 5.9, *)
@_silgen_name("swift_task_isOnExecutor")
public func _taskIsOnExecutor<Executor: SerialExecutor>(_ executor: Executor) -> Bool

@_spi(ConcurrencyExecutors)
@available(SwiftStdlib 5.9, *)
@_silgen_name("swift_executor_isComplexEquality")
public func _executor_isComplexEquality(_ executor: UnownedSerialExecutor) -> Bool

@available(SwiftStdlib 5.1, *)
@_transparent
public // COMPILER_INTRINSIC
func _checkExpectedExecutor(_filenameStart: Builtin.RawPointer,
                            _filenameLength: Builtin.Word,
                            _filenameIsASCII: Builtin.Int1,
                            _line: Builtin.Word,
                            _executor: Builtin.Executor) {
  if _taskIsCurrentExecutor(_executor) {
    return
  }

  _reportUnexpectedExecutor(
      _filenameStart, _filenameLength, _filenameIsASCII, _line, _executor)
}

/// Primarily a debug utility.
///
/// If the passed in ExecutorJob is a Task, returns the complete 64bit TaskId,
/// otherwise returns only the job's 32bit Id.
///
/// - Returns: the Id stored in this ExecutorJob or Task, for purposes of debug printing
@available(SwiftStdlib 5.9, *)
@_silgen_name("swift_task_getJobTaskId")
internal func _getJobTaskId(_ job: UnownedJob) -> UInt64

@available(SwiftStdlib 5.9, *)
@_silgen_name("_task_serialExecutor_isSameExclusiveExecutionContext")
internal func _task_serialExecutor_isSameExclusiveExecutionContext<E>(current currentExecutor: E, executor: E) -> Bool
    where E: SerialExecutor {
  currentExecutor.isSameExclusiveExecutionContext(other: executor)
}

/// Obtain the executor ref by calling the executor's `asUnownedSerialExecutor()`.
/// The obtained executor ref will have all the user-defined flags set on the executor.
@available(SwiftStdlib 5.9, *)
@_silgen_name("_task_serialExecutor_getExecutorRef")
internal func _task_serialExecutor_getExecutorRef<E>(_ executor: E) -> Builtin.Executor
    where E: SerialExecutor {
  return executor.asUnownedSerialExecutor().executor
}

// Used by the concurrency runtime
@available(SwiftStdlib 5.1, *)
@_silgen_name("_swift_task_enqueueOnExecutor")
internal func _enqueueOnExecutor<E>(job unownedJob: UnownedJob, executor: E)
where E: SerialExecutor {
  #if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  if #available(SwiftStdlib 5.9, *) {
    executor.enqueue(ExecutorJob(context: unownedJob._context))
  } else {
    executor.enqueue(unownedJob)
  }
  #else // SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  executor.enqueue(unownedJob)
  #endif // !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
}

#if !SWIFT_STDLIB_SINGLE_THREADED_CONCURRENCY && !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
// This must take a DispatchQueueShim, not something like AnyObject,
// or else SILGen will emit a retain/release in unoptimized builds,
// which won't work because DispatchQueues aren't actually
// Swift-retainable.
@available(SwiftStdlib 5.1, *)
@_silgen_name("swift_task_enqueueOnDispatchQueue")
internal func _enqueueOnDispatchQueue(_ job: UnownedJob,
                                      queue: DispatchQueueShim)

/// Used by the runtime solely for the witness table it produces.
/// FIXME: figure out some way to achieve that which doesn't generate
/// all the other metadata
///
/// Expected to work for any primitive dispatch queue; note that this
/// means a dispatch_queue_t, which is not the same as DispatchQueue
/// on platforms where that is an instance of a wrapper class.
@available(SwiftStdlib 5.1, *)
internal final class DispatchQueueShim: @unchecked Sendable, SerialExecutor {
  func enqueue(_ job: UnownedJob) {
    _enqueueOnDispatchQueue(job, queue: self)
  }

  func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    return UnownedSerialExecutor(ordinary: self)
  }
}
#endif // SWIFT_STDLIB_SINGLE_THREADED_CONCURRENCY
