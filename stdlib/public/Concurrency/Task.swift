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
@_implementationOnly import _SwiftConcurrencyShims

// ==== Task -------------------------------------------------------------------

/// An asynchronous task (just "Task" hereafter) is the analogue of a thread for
/// asynchronous functions. All asynchronous functions run as part of some task.
///
/// An instance of `Task` always represents a top-level task. The instance
/// can be used to await its completion, cancel the task, etc., The task will
/// run to completion even if there are no other instances of the `Task`.
///
/// `Task` also provides appropriate context-sensitive static functions which
/// operate on the "current" task, which might either be a detached task or
/// a child task. Because all such functions are `async` they can only
/// be invoked as part of an existing task, and therefore are guaranteed to be
/// effective.
///
/// A task's execution can be seen as a series of periods where the task was
/// running. Each such period ends at a suspension point or -- finally -- the
/// completion of the task.
///
/// These partial periods towards the task's completion are
/// individually schedulable as jobs.  Jobs are generally not interacted
/// with by end-users directly, unless implementing a scheduler.
@available(SwiftStdlib 5.5, *)
@frozen
public struct Task<Success, Failure: Error>: Sendable {
  @usableFromInline
  internal let _task: Builtin.NativeObject

  @_alwaysEmitIntoClient
  internal init(_ task: Builtin.NativeObject) {
    self._task = task
  }
}

@available(SwiftStdlib 5.5, *)
extension Task {
  /// Wait for the task to complete, returning (or throwing) its result.
  ///
  /// ### Priority
  /// If the task has not completed yet, its priority will be elevated to the
  /// priority of the current task. Note that this may not be as effective as
  /// creating the task with the "right" priority to in the first place.
  ///
  /// ### Cancellation
  /// If the awaited on task gets cancelled externally the `get()` will throw
  /// a cancellation error.
  ///
  /// If the task gets cancelled internally, e.g. by checking for cancellation
  /// and throwing a specific error or using `checkCancellation` the error
  /// thrown out of the task will be re-thrown here.
  public var value: Success {
    get async throws {
      return try await _taskFutureGetThrowing(_task)
    }
  }

  /// Wait for the task to complete, returning (or throwing) its result.
  ///
  /// ### Priority
  /// If the task has not completed yet, its priority will be elevated to the
  /// priority of the current task. Note that this may not be as effective as
  /// creating the task with the "right" priority to in the first place.
  ///
  /// ### Cancellation
  /// If the awaited on task gets cancelled externally the `get()` will throw
  /// a cancellation error.
  ///
  /// If the task gets cancelled internally, e.g. by checking for cancellation
  /// and throwing a specific error or using `checkCancellation` the error
  /// thrown out of the task will be re-thrown here.

  /// Wait for the task to complete, returning its `Result`.
  ///
  /// ### Priority
  /// If the task has not completed yet, its priority will be elevated to the
  /// priority of the current task. Note that this may not be as effective as
  /// creating the task with the "right" priority to in the first place.
  ///
  /// ### Cancellation
  /// If the awaited on task gets cancelled externally the `get()` will throw
  /// a cancellation error.
  ///
  /// If the task gets cancelled internally, e.g. by checking for cancellation
  /// and throwing a specific error or using `checkCancellation` the error
  /// thrown out of the task will be re-thrown here.
  public var result: Result<Success, Failure> {
    get async {
      do {
        return .success(try await value)
      } catch {
        return .failure(error as! Failure) // as!-safe, guaranteed to be Failure
      }
    }
  }

  /// Attempt to cancel the task.
  ///
  /// Whether this function has any effect is task-dependent.
  ///
  /// For a task to respect cancellation it must cooperatively check for it
  /// while running. Many tasks will check for cancellation before beginning
  /// their "actual work", however this is not a requirement nor is it guaranteed
  /// how and when tasks check for cancellation in general.
  public func cancel() {
    Builtin.cancelAsyncTask(_task)
  }
}

@available(SwiftStdlib 5.5, *)
extension Task where Failure == Never {
  /// Wait for the task to complete, returning its result.
  ///
  /// ### Priority
  /// If the task has not completed yet, its priority will be elevated to the
  /// priority of the current task. Note that this may not be as effective as
  /// creating the task with the "right" priority to in the first place.
  ///
  /// ### Cancellation
  /// The task this refers to may check for cancellation, however
  /// since it is not-throwing it would have to handle it using some other
  /// way than throwing a `CancellationError`, e.g. it could provide a neutral
  /// value of the `Success` type, or encode that cancellation has occurred in
  /// that type itself.
  public var value: Success {
    get async {
      return await _taskFutureGet(_task)
    }
  }
}

@available(SwiftStdlib 5.5, *)
extension Task: Hashable {
  public func hash(into hasher: inout Hasher) {
    UnsafeRawPointer(Builtin.bridgeToRawPointer(_task)).hash(into: &hasher)
  }
}

@available(SwiftStdlib 5.5, *)
extension Task: Equatable {
  public static func ==(lhs: Self, rhs: Self) -> Bool {
    UnsafeRawPointer(Builtin.bridgeToRawPointer(lhs._task)) ==
      UnsafeRawPointer(Builtin.bridgeToRawPointer(rhs._task))
  }
}

// ==== Task Priority ----------------------------------------------------------

/// Task priority may inform decisions an `Executor` makes about how and when
/// to schedule tasks submitted to it.
///
/// ### Priority scheduling
/// An executor MAY utilize priority information to attempt running higher
/// priority tasks first, and then continuing to serve lower priority tasks.
///
/// The exact semantics of how priority is treated are left up to each
/// platform and `Executor` implementation.
///
/// ### Priority inheritance
/// Child tasks automatically inherit their parent task's priority.
///
/// Detached tasks (created by `Task.detached`) DO NOT inherit task priority,
/// as they are "detached" from their parent tasks after all.
///
/// ### Priority elevation
/// In some situations the priority of a task must be elevated (or "escalated", "raised"):
///
/// - if a `Task` running on behalf of an actor, and a new higher-priority
///   task is enqueued to the actor, its current task must be temporarily
///   elevated to the priority of the enqueued task, in order to allow the new
///   task to be processed at--effectively-- the priority it was enqueued with.
///   - this DOES NOT affect `Task.currentPriority()`.
/// - if a task is created with a `Task.Handle`, and a higher-priority task
///   calls the `await handle.get()` function the priority of this task must be
///   permanently increased until the task completes.
///   - this DOES affect `Task.currentPriority()`.
///
/// TODO: Define the details of task priority; It is likely to be a concept
///       similar to Darwin Dispatch's QoS; bearing in mind that priority is not as
///       much of a thing on other platforms (i.e. server side Linux systems).
@available(SwiftStdlib 5.5, *)
public struct TaskPriority: RawRepresentable, Sendable {
  public typealias RawValue = UInt8
  public var rawValue: UInt8

  public init(rawValue: UInt8) {
    self.rawValue = rawValue
  }

  public static let high: TaskPriority = .init(rawValue: 0x19)
  public static let userInitiated: TaskPriority = high
  public static let `default`: TaskPriority = .init(rawValue: 0x15)
  public static let low: TaskPriority = .init(rawValue: 0x11)
  public static let utility: TaskPriority = low
  public static let background: TaskPriority = .init(rawValue: 0x09)
}

@available(SwiftStdlib 5.5, *)
extension TaskPriority: Equatable {
  public static func == (lhs: TaskPriority, rhs: TaskPriority) -> Bool {
    lhs.rawValue == rhs.rawValue
  }

  public static func != (lhs: TaskPriority, rhs: TaskPriority) -> Bool {
    lhs.rawValue != rhs.rawValue
  }
}

@available(SwiftStdlib 5.5, *)
extension TaskPriority: Comparable {
  public static func < (lhs: TaskPriority, rhs: TaskPriority) -> Bool {
    lhs.rawValue < rhs.rawValue
  }

  public static func <= (lhs: TaskPriority, rhs: TaskPriority) -> Bool {
    lhs.rawValue <= rhs.rawValue
  }

  public static func > (lhs: TaskPriority, rhs: TaskPriority) -> Bool {
    lhs.rawValue > rhs.rawValue
  }

  public static func >= (lhs: TaskPriority, rhs: TaskPriority) -> Bool {
    lhs.rawValue >= rhs.rawValue
  }
}

@available(SwiftStdlib 5.5, *)
extension TaskPriority: Codable { }

@available(SwiftStdlib 5.5, *)
extension Task where Success == Never, Failure == Never {

  /// Returns the `current` task's priority.
  ///
  /// If no current `Task` is available, queries the system to determine the
  /// priority at which the current function is running. If the system cannot
  /// provide an appropriate priority, returns `Priority.default`.
  ///
  /// - SeeAlso: `TaskPriority`
  public static var currentPriority: TaskPriority {
    withUnsafeCurrentTask { task in
      // If we are running on behalf of a task, use that task's priority.
      if let task = task {
        return task.priority
      }

      // Otherwise, query the system.
      return TaskPriority(rawValue: UInt8(0))
    }
  }
}

@available(SwiftStdlib 5.5, *)
extension TaskPriority {
  /// Downgrade user-interactive to user-initiated.
  var _downgradeUserInteractive: TaskPriority {
    return self
  }
}

// ==== Job Flags --------------------------------------------------------------

/// Flags for schedulable jobs.
///
/// This is a port of the C++ FlagSet.
@available(SwiftStdlib 5.5, *)
struct JobFlags {
  /// Kinds of schedulable jobs.
  enum Kind: Int32 {
    case task = 0
  }

  /// The actual bit representation of these flags.
  var bits: Int32 = 0

  /// The kind of job described by these flags.
  var kind: Kind {
    get {
      Kind(rawValue: bits & 0xFF)!
    }

    set {
      bits = (bits & ~0xFF) | newValue.rawValue
    }
  }

  /// Whether this is an asynchronous task.
  var isAsyncTask: Bool { kind == .task }

  /// The priority given to the job.
  var priority: TaskPriority? {
    get {
      let value = (Int(bits) & 0xFF00) >> 8

      if value == 0 {
        return nil
      }

      return TaskPriority(rawValue: UInt8(value))
    }

    set {
      bits = (bits & ~0xFF00) | Int32((Int(newValue?.rawValue ?? 0) << 8))
    }
  }

  /// Whether this is a child task.
  var isChildTask: Bool {
    get {
      (bits & (1 << 24)) != 0
    }

    set {
      if newValue {
        bits = bits | 1 << 24
      } else {
        bits = (bits & ~(1 << 24))
      }
    }
  }

  /// Whether this is a future.
  var isFuture: Bool {
    get {
      (bits & (1 << 25)) != 0
    }

    set {
      if newValue {
        bits = bits | 1 << 25
      } else {
        bits = (bits & ~(1 << 25))
      }
    }
  }

  /// Whether this is a group child.
  var isGroupChildTask: Bool {
    get {
      (bits & (1 << 26)) != 0
    }

    set {
      if newValue {
        bits = bits | 1 << 26
      } else {
        bits = (bits & ~(1 << 26))
      }
    }
  }

  /// Whether this is a task created by the 'async' operation, which
  /// conceptually continues the work of the synchronous code that invokes
  /// it.
  var isContinuingAsyncTask: Bool {
    get {
      (bits & (1 << 27)) != 0
    }

    set {
      if newValue {
        bits = bits | 1 << 27
      } else {
        bits = (bits & ~(1 << 27))
      }
    }
  }
}

// ==== Task Creation Flags --------------------------------------------------

/// Form task creation flags for use with the createAsyncTask builtins.
@available(SwiftStdlib 5.5, *)
@_alwaysEmitIntoClient
func taskCreateFlags(
  priority: TaskPriority?, isChildTask: Bool, copyTaskLocals: Bool,
  inheritContext: Bool, enqueueJob: Bool,
  addPendingGroupTaskUnconditionally: Bool
) -> Int {
  var bits = 0
  bits |= (bits & ~0xFF) | Int(priority?.rawValue ?? 0)
  if isChildTask {
    bits |= 1 << 8
  }
  if copyTaskLocals {
    bits |= 1 << 10
  }
  if inheritContext {
    bits |= 1 << 11
  }
  if enqueueJob {
    bits |= 1 << 12
  }
  if addPendingGroupTaskUnconditionally {
    bits |= 1 << 13
  }
  return bits
}

// ==== Task Creation ----------------------------------------------------------
@available(SwiftStdlib 5.5, *)
extension Task where Failure == Never {
  /// Run given `operation` as asynchronously in its own top-level task.
  ///
  /// The `async` function should be used when creating asynchronous work
  /// that operates on behalf of the synchronous function that calls it.
  /// Like `Task.detached`, the async function creates a separate, top-level
  /// task.
  ///
  /// Unlike `Task.detached`, the task creating by the `Task` initializer
  /// inherits the priority and actor context of the caller, so the `operation`
  /// is treated more like an asynchronous extension to the synchronous
  /// operation.
  ///
  /// - Parameters:
  ///   - priority: priority of the task. If nil, the priority will come from
  ///     Task.currentPriority.
  ///   - operation: the operation to execute
  @discardableResult
  @_alwaysEmitIntoClient
  public init(
    priority: TaskPriority? = nil,
    @_inheritActorContext @_implicitSelfCapture operation: __owned @Sendable @escaping () async -> Success
  ) {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    // Set up the job flags for a new task.
    let flags = taskCreateFlags(
      priority: priority, isChildTask: false, copyTaskLocals: true,
      inheritContext: true, enqueueJob: true,
      addPendingGroupTaskUnconditionally: false)

    // Create the asynchronous task.
    let (task, _) = Builtin.createAsyncTask(flags, operation)

    self._task = task
#else
    fatalError("Unsupported Swift compiler")
#endif
  }

  /// Run given `operation` as asynchronously in its own top-level task.
  ///
  /// The `async` function should be used when creating asynchronous work
  /// that operates on behalf of the synchronous function that calls it.
  /// Like `Task.detached`, the async function creates a separate, top-level
  /// task.
  ///
  /// Unlike `Task.detached`, the task creating by the `Task` initializer
  /// inherits the priority and actor context of the caller, so the `operation`
  /// is treated more like an asynchronous extension to the synchronous
  /// operation.
  ///
  /// - Parameters:
  ///   - priority: priority of the task. If nil, the priority will come from
  ///     Task.currentPriority.
  ///   - operation: the operation to execute
  @discardableResult
  @available(*, deprecated, message: "Experimental entry point--do not use")
  @_alwaysEmitIntoClient
  public init(
    priority: TaskPriority? = nil,
    on executor: Executor,
    @_inheritActorContext @_implicitSelfCapture operation: __owned @Sendable @escaping () async -> Success
  ) {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    // Set up the job flags for a new task.
    let flags = taskCreateFlags(
      priority: priority, isChildTask: false, copyTaskLocals: true,
      inheritContext: true, enqueueJob: true,
      addPendingGroupTaskUnconditionally: false)

    // Create the asynchronous task.
    let (task, _) = Builtin.createAsyncTaskWithExecutor(
        flags, unsafeBitCast(executor, to: Builtin.Executor.self), operation)

    self._task = task
#else
    fatalError("Unsupported Swift compiler")
#endif
  }}

@available(SwiftStdlib 5.5, *)
extension Task where Failure == Error {
  /// Run given `operation` as asynchronously in its own top-level task.
  ///
  /// This initializer creates asynchronous work on behalf of the synchronous function that calls it.
  /// Like `Task.detached`, this initializer creates a separate, top-level task.
  /// Unlike `Task.detached`, the task created inherits the priority and
  /// actor context of the caller, so the `operation` is treated more like an
  /// asynchronous extension to the synchronous operation.
  ///
  /// - Parameters:
  ///   - priority: priority of the task. If nil, the priority will come from
  ///     Task.currentPriority.
  ///   - operation: the operation to execute
  @discardableResult
  @_alwaysEmitIntoClient
  public init(
    priority: TaskPriority? = nil,
    @_inheritActorContext @_implicitSelfCapture operation: __owned @Sendable @escaping () async throws -> Success
  ) {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    // Set up the task flags for a new task.
    let flags = taskCreateFlags(
      priority: priority, isChildTask: false, copyTaskLocals: true,
      inheritContext: true, enqueueJob: true,
      addPendingGroupTaskUnconditionally: false
    )

    // Create the asynchronous task future.
    let (task, _) = Builtin.createAsyncTask(flags, operation)

    self._task = task
#else
    fatalError("Unsupported Swift compiler")
#endif
  }
}

// ==== Detached Tasks ---------------------------------------------------------
@available(SwiftStdlib 5.5, *)
extension Task where Failure == Never {
  /// Run given throwing `operation` as part of a new top-level task.
  ///
  /// Creating detached tasks should, generally, be avoided in favor of using
  /// `async` functions, `async let` declarations and `await` expressions - as
  /// those benefit from structured, bounded concurrency which is easier to reason
  /// about, as well as automatically inheriting the parent tasks priority,
  /// task-local storage, deadlines, as well as being cancelled automatically
  /// when their parent task is cancelled. Detached tasks do not get any of those
  /// benefits, and thus should only be used when an operation is impossible to
  /// be modelled with child tasks.
  ///
  /// ### Cancellation
  /// A detached task always runs to completion unless it is explicitly cancelled.
  /// Specifically, dropping a detached tasks `Task` does _not_ automatically
  /// cancel given task.
  ///
  /// Cancelling a task must be performed explicitly via `cancel()`.
  ///
  /// - Note: it is generally preferable to use child tasks rather than detached
  ///   tasks. Child tasks automatically carry priorities, task-local state,
  ///   deadlines and have other benefits resulting from the structured
  ///   concurrency concepts that they model. Consider using detached tasks only
  ///   when strictly necessary and impossible to model operations otherwise.
  ///
  /// - Parameters:
  ///   - priority: priority of the task
  ///   - operation: the operation to execute
  /// - Returns: handle to the task, allowing to `await get()` on the
  ///     tasks result or `cancel` it. If the operation fails the handle will
  ///     throw the error the operation has thrown when awaited on.
  @discardableResult
  @_alwaysEmitIntoClient
  public static func detached(
    priority: TaskPriority? = nil,
    operation: __owned @Sendable @escaping () async -> Success
  ) -> Task<Success, Failure> {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    // Set up the job flags for a new task.
    let flags = taskCreateFlags(
      priority: priority, isChildTask: false, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: false)

    // Create the asynchronous task future.
    let (task, _) = Builtin.createAsyncTask(flags, operation)

    return Task(task)
#else
    fatalError("Unsupported Swift compiler")
#endif
  }
}

@available(SwiftStdlib 5.5, *)
extension Task where Failure == Error {
  /// Run given throwing `operation` as part of a new top-level task.
  ///
  /// Creating detached tasks should, generally, be avoided in favor of using
  /// `async` functions, `async let` declarations and `await` expressions - as
  /// those benefit from structured, bounded concurrency which is easier to reason
  /// about, as well as automatically inheriting the parent tasks priority,
  /// task-local storage, deadlines, as well as being cancelled automatically
  /// when their parent task is cancelled. Detached tasks do not get any of those
  /// benefits, and thus should only be used when an operation is impossible to
  /// be modelled with child tasks.
  ///
  /// ### Cancellation
  /// A detached task always runs to completion unless it is explicitly cancelled.
  /// Specifically, dropping a detached tasks `Task.Handle` does _not_ automatically
  /// cancel given task.
  ///
  /// Cancelling a task must be performed explicitly via `handle.cancel()`.
  ///
  /// - Note: it is generally preferable to use child tasks rather than detached
  ///   tasks. Child tasks automatically carry priorities, task-local state,
  ///   deadlines and have other benefits resulting from the structured
  ///   concurrency concepts that they model. Consider using detached tasks only
  ///   when strictly necessary and impossible to model operations otherwise.
  ///
  /// - Parameters:
  ///   - priority: priority of the task
  ///   - executor: the executor on which the detached closure should start
  ///               executing on.
  ///   - operation: the operation to execute
  /// - Returns: handle to the task, allowing to `await handle.get()` on the
  ///     tasks result or `cancel` it. If the operation fails the handle will
  ///     throw the error the operation has thrown when awaited on.
  @discardableResult
  @_alwaysEmitIntoClient
  public static func detached(
    priority: TaskPriority? = nil,
    operation: __owned @Sendable @escaping () async throws -> Success
  ) -> Task<Success, Failure> {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    // Set up the job flags for a new task.
    let flags = taskCreateFlags(
      priority: priority, isChildTask: false, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: false
    )

    // Create the asynchronous task future.
    let (task, _) = Builtin.createAsyncTask(flags, operation)

    return Task(task)
#else
    fatalError("Unsupported Swift compiler")
#endif
  }
}

// ==== Async Sleep ------------------------------------------------------------

@available(SwiftStdlib 5.5, *)
extension Task where Success == Never, Failure == Never {
  /// Suspends the current task for _at least_ the given duration
  /// in nanoseconds.
  ///
  /// This function does _not_ block the underlying thread.
  public static func sleep(_ duration: UInt64) async {
    let currentTask = Builtin.getCurrentAsyncTask()
    let priority = getJobFlags(currentTask).priority ?? Task.currentPriority._downgradeUserInteractive

    return await Builtin.withUnsafeContinuation { (continuation: Builtin.RawUnsafeContinuation) -> Void in
      let job = _taskCreateNullaryContinuationJob(priority: Int(priority.rawValue), continuation: continuation)
      _enqueueJobGlobalWithDelay(duration, job)
    }
  }
}

// ==== Voluntary Suspension -----------------------------------------------------

@available(SwiftStdlib 5.5, *)
extension Task where Success == Never, Failure == Never {

  /// Explicitly suspend the current task, potentially giving up execution actor
  /// of current actor/task, allowing other tasks to execute.
  ///
  /// This is not a perfect cure for starvation;
  /// if the task is the highest-priority task in the system, it might go
  /// immediately back to executing.
  public static func yield() async {
    let currentTask = Builtin.getCurrentAsyncTask()
    let priority = getJobFlags(currentTask).priority ?? Task.currentPriority._downgradeUserInteractive

    return await Builtin.withUnsafeContinuation { (continuation: Builtin.RawUnsafeContinuation) -> Void in
      let job = _taskCreateNullaryContinuationJob(priority: Int(priority.rawValue), continuation: continuation)
      _enqueueJobGlobal(job)
    }
  }
}

// ==== UnsafeCurrentTask ------------------------------------------------------

/// Calls the given closure with the with the "current" task in which this
/// function was invoked.
///
/// If invoked from an asynchronous function the task will always be non-nil,
/// as an asynchronous function is always running within some task.
/// However if invoked from a synchronous function the task may be nil,
/// meaning that the function is not executing within a task, i.e. there is no
/// asynchronous context available in the call stack.
///
/// It is generally not safe to escape/store the `UnsafeCurrentTask` for future
/// use, as some operations on it may only be performed from the same task
/// that it is representing.
///
/// It is possible to obtain a `Task` fom the `UnsafeCurrentTask` which is safe
/// to access from other tasks or even store for future reference e.g. equality
/// checks.
@available(SwiftStdlib 5.5, *)
public func withUnsafeCurrentTask<T>(body: (UnsafeCurrentTask?) throws -> T) rethrows -> T {
  guard let _task = _getCurrentAsyncTask() else {
    return try body(nil)
  }

  // FIXME: This retain seems pretty wrong, however if we don't we WILL crash
  //        with "destroying a task that never completed" in the task's destroy.
  //        How do we solve this properly?
  Builtin.retain(_task)

  return try body(UnsafeCurrentTask(_task))
}

/// An *unsafe* 'current' task handle.
///
/// An `UnsafeCurrentTask` should not be stored for "later" access.
///
/// Storing an `UnsafeCurrentTask` has no implication on the task's actual lifecycle.
///
/// The sub-set of APIs of `UnsafeCurrentTask` which also exist on `Task` are
/// generally safe to be invoked from any task/thread.
///
/// All other APIs must not, be called 'from' any other task than the one
/// represented by this handle itself. Doing so may result in undefined behavior,
/// and most certainly will break invariants in other places of the program
/// actively running on this task.
@available(SwiftStdlib 5.5, *)
public struct UnsafeCurrentTask {
  internal let _task: Builtin.NativeObject

  // May only be created by the standard library.
  internal init(_ task: Builtin.NativeObject) {
    self._task = task
  }

  /// Returns `true` if the task is cancelled, and should stop executing.
  ///
  /// - SeeAlso: `checkCancellation()`
  public var isCancelled: Bool {
    _taskIsCancelled(_task)
  }

  /// Returns the `current` task's priority.
  ///
  /// - SeeAlso: `TaskPriority`
  /// - SeeAlso: `Task.currentPriority`
  public var priority: TaskPriority {
    getJobFlags(_task).priority ?? .unspecified
  }
}

@available(SwiftStdlib 5.5, *)
extension UnsafeCurrentTask: Hashable {
  public func hash(into hasher: inout Hasher) {
    UnsafeRawPointer(Builtin.bridgeToRawPointer(_task)).hash(into: &hasher)
  }
}

@available(SwiftStdlib 5.5, *)
extension UnsafeCurrentTask: Equatable {
  public static func ==(lhs: Self, rhs: Self) -> Bool {
    UnsafeRawPointer(Builtin.bridgeToRawPointer(lhs._task)) ==
      UnsafeRawPointer(Builtin.bridgeToRawPointer(rhs._task))
  }
}

// ==== Internal ---------------------------------------------------------------
@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_task_getCurrent")
func _getCurrentAsyncTask() -> Builtin.NativeObject?

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_task_getJobFlags")
func getJobFlags(_ task: Builtin.NativeObject) -> JobFlags

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_task_enqueueGlobal")
@usableFromInline
func _enqueueJobGlobal(_ task: Builtin.Job)

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_task_enqueueGlobalWithDelay")
@usableFromInline
func _enqueueJobGlobalWithDelay(_ delay: UInt64, _ task: Builtin.Job)

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_task_asyncMainDrainQueue")
public func _asyncMainDrainQueue() -> Never

@available(SwiftStdlib 5.5, *)
public func _runAsyncMain(_ asyncFun: @escaping () async throws -> ()) {
  Task.detached {
    do {
#if !os(Windows)
      Builtin.hopToActor(MainActor.shared)
#endif
      try await asyncFun()
      exit(0)
    } catch {
      _errorInMain(error)
    }
  }
  _asyncMainDrainQueue()
}

// FIXME: both of these ought to take their arguments _owned so that
//        we can do a move out of the future in the common case where it's
//        unreferenced
@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_task_future_wait")
public func _taskFutureGet<T>(_ task: Builtin.NativeObject) async -> T

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_task_future_wait_throwing")
public func _taskFutureGetThrowing<T>(_ task: Builtin.NativeObject) async throws -> T

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_task_cancel")
func _taskCancel(_ task: Builtin.NativeObject)

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_task_isCancelled")
func _taskIsCancelled(_ task: Builtin.NativeObject) -> Bool

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_task_createNullaryContinuationJob")
func _taskCreateNullaryContinuationJob(priority: Int, continuation: Builtin.RawUnsafeContinuation) -> Builtin.Job

@available(SwiftStdlib 5.5, *)
@usableFromInline
@_silgen_name("swift_task_isCurrentExecutor")
func _taskIsCurrentExecutor(_ executor: Builtin.Executor) -> Bool

@available(SwiftStdlib 5.5, *)
@usableFromInline
@_silgen_name("swift_task_reportUnexpectedExecutor")
func _reportUnexpectedExecutor(_ _filenameStart: Builtin.RawPointer,
                               _ _filenameLength: Builtin.Word,
                               _ _filenameIsASCII: Builtin.Int1,
                               _ _line: Builtin.Word,
                               _ _executor: Builtin.Executor)

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_task_getCurrentThreadPriority")
func _getCurrentThreadPriority() -> Int

#if _runtime(_ObjC)

/// Intrinsic used by SILGen to launch a task for bridging a Swift async method
/// which was called through its ObjC-exported completion-handler-based API.
@available(SwiftStdlib 5.5, *)
@_alwaysEmitIntoClient
@usableFromInline
internal func _runTaskForBridgedAsyncMethod(@_inheritActorContext _ body: __owned @Sendable @escaping () async -> Void) {
  Task(operation: body)
}

#endif
