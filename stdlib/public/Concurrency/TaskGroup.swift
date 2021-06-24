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

// ==== TaskGroup --------------------------------------------------------------

/// Starts a new task group which provides a scope in which a dynamic number of
/// tasks may be spawned.
///
/// Tasks added to the group by `group.spawn()` will automatically be awaited on
/// when the scope exits. If the group exits by throwing, all added tasks will
/// be cancelled and their results discarded.
///
/// ### Implicit awaiting
/// When the group returns it will implicitly await for all spawned tasks to
/// complete. The tasks are only cancelled if `cancelAll()` was invoked before
/// returning, the groups' task was cancelled, or the group body has thrown.
///
/// When results of tasks added to the group need to be collected, one can
/// gather their results using the following pattern:
///
///     while let result = await group.next() {
///       // some accumulation logic (e.g. sum += result)
///     }
///
/// It is also possible to collect results from the group by using its
/// `AsyncSequence` conformance, which enables its use in an asynchronous for-loop,
/// like this:
///
///     for await result in group {
///       // some accumulation logic (e.g. sum += result)
///      }
///
/// ### Cancellation
/// If the task that the group is running in is cancelled, the group becomes 
/// cancelled and all child tasks spawned in the group are cancelled as well.
/// 
/// Since the `withTaskGroup` provided group is specifically non-throwing,
/// child tasks (or the group) cannot react to cancellation by throwing a 
/// `CancellationError`, however they may interrupt their work and e.g. return 
/// some best-effort approximation of their work. 
///
/// If throwing is a good option for the kinds of tasks spawned by the group,
/// consider using the `withThrowingTaskGroup` function instead.
///
/// Postcondition:
/// Once `withTaskGroup` returns it is guaranteed that the `group` is *empty*.
///
/// This is achieved in the following way:
/// - if the body returns normally:
///   - the group will await any not yet complete tasks,
///   - once the `withTaskGroup` returns the group is guaranteed to be empty.
@available(SwiftStdlib 5.5, *)
@inlinable
public func withTaskGroup<ChildTaskResult, GroupResult>(
  of childTaskResultType: ChildTaskResult.Type,
  returning returnType: GroupResult.Type = GroupResult.self,
  body: (inout TaskGroup<ChildTaskResult>) async -> GroupResult
) async -> GroupResult {
  #if compiler(>=5.5) && $BuiltinTaskGroupWithArgument

  let _group = Builtin.createTaskGroup(ChildTaskResult.self)
  var group = TaskGroup<ChildTaskResult>(group: _group)

  // Run the withTaskGroup body.
  let result = await body(&group)

  await group.awaitAllRemainingTasks()

  Builtin.destroyTaskGroup(_group)
  return result

  #else
  fatalError("Swift compiler is incompatible with this SDK version")
  #endif
}

/// Starts a new throwing task group which provides a scope in which a dynamic 
/// number of tasks may be spawned.
///
/// Tasks added to the group by `group.spawn()` will automatically be awaited on
/// when the scope exits. If the group exits by throwing, all added tasks will
/// be cancelled and their results discarded.
///
/// ### Implicit awaiting
/// When the group returns it will implicitly await for all spawned tasks to
/// complete. The tasks are only cancelled if `cancelAll()` was invoked before
/// returning, the groups' task was cancelled, or the group body has thrown.
///
/// When results of tasks added to the group need to be collected, one can
/// gather their results using the following pattern:
///
///     while let result = await try group.next() {
///       // some accumulation logic (e.g. sum += result)
///     }
///
/// It is also possible to collect results from the group by using its
/// `AsyncSequence` conformance, which enables its use in an asynchronous for-loop,
/// like this:
///
///     for try await result in group {
///       // some accumulation logic (e.g. sum += result)
///      }
///
/// ### Thrown errors
/// When tasks are added to the group using the `group.spawn` function, they may
/// immediately begin executing. Even if their results are not collected explicitly
/// and such task throws, and was not yet cancelled, it may result in the `withTaskGroup`
/// throwing.
///
/// ### Cancellation
/// If the task that the group is running in is cancelled, the group becomes 
/// cancelled and all child tasks spawned in the group are cancelled as well.
/// 
/// If an error is thrown out of the task group, all of its remaining tasks
/// will be cancelled and the `withTaskGroup` call will rethrow that error.
///
/// Individual tasks throwing results in their corresponding `try group.next()`
/// call throwing, giving a chance to handle individual errors or letting the
/// error be rethrown by the group.
///
/// Postcondition:
/// Once `withThrowingTaskGroup` returns it is guaranteed that the `group` is *empty*.
///
/// This is achieved in the following way:
/// - if the body returns normally:
///   - the group will await any not yet complete tasks,
///     - if any of those tasks throws, the remaining tasks will be cancelled,
///   - once the `withTaskGroup` returns the group is guaranteed to be empty.
/// - if the body throws:
///   - all tasks remaining in the group will be automatically cancelled.
@available(SwiftStdlib 5.5, *)
@inlinable
public func withThrowingTaskGroup<ChildTaskResult, GroupResult>(
  of childTaskResultType: ChildTaskResult.Type,
  returning returnType: GroupResult.Type = GroupResult.self,
  body: (inout ThrowingTaskGroup<ChildTaskResult, Error>) async throws -> GroupResult
) async rethrows -> GroupResult {
  #if compiler(>=5.5) && $BuiltinTaskGroupWithArgument

  let _group = Builtin.createTaskGroup(ChildTaskResult.self)
  var group = ThrowingTaskGroup<ChildTaskResult, Error>(group: _group)

  do {
    // Run the withTaskGroup body.
    let result = try await body(&group)

    await group.awaitAllRemainingTasks()
    Builtin.destroyTaskGroup(_group)

    return result
  } catch {
    group.cancelAll()

    await group.awaitAllRemainingTasks()
    Builtin.destroyTaskGroup(_group)

    throw error
  }

  #else
  fatalError("Swift compiler is incompatible with this SDK version")
  #endif
}

/// A task group serves as storage for dynamically spawned tasks.
///
/// It is created by the `withTaskGroup` function.
@available(SwiftStdlib 5.5, *)
@frozen
public struct TaskGroup<ChildTaskResult> {

  /// Group task into which child tasks offer their results,
  /// and the `next()` function polls those results from.
  @usableFromInline
  internal let _group: Builtin.RawPointer

  /// No public initializers
  @inlinable
  init(group: Builtin.RawPointer) {
    self._group = group
  }

  /// Add a child task to the group.
  ///
  /// ### Error handling
  /// Operations are allowed to `throw`, in which case the `try await next()`
  /// invocation corresponding to the failed task will re-throw the given task.
  ///
  /// The `add` function will never (re-)throw errors from the `operation`.
  /// Instead, the corresponding `next()` call will throw the error when necessary.
  ///
  /// - Parameters:
  ///   - overridingPriority: override priority of the operation task
  ///   - operation: operation to execute and add to the group
  /// - Returns:
  ///   - `true` if the operation was added to the group successfully,
  ///     `false` otherwise (e.g. because the group `isCancelled`)
  @_alwaysEmitIntoClient
  public mutating func async(
    priority: TaskPriority? = nil,
    operation: __owned @Sendable @escaping () async -> ChildTaskResult
  ) {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: true
    )

    // Create the task in this group.
    _ = Builtin.createAsyncTaskInGroup(flags, _group, operation)
#else
    fatalError("Unsupported Swift compiler")
#endif
  }

  /// Add a child task to the group.
  ///
  /// ### Error handling
  /// Operations are allowed to `throw`, in which case the `try await next()`
  /// invocation corresponding to the failed task will re-throw the given task.
  ///
  /// The `add` function will never (re-)throw errors from the `operation`.
  /// Instead, the corresponding `next()` call will throw the error when necessary.
  ///
  /// - Parameters:
  ///   - overridingPriority: override priority of the operation task
  ///   - operation: operation to execute and add to the group
  /// - Returns:
  ///   - `true` if the operation was added to the group successfully,
  ///     `false` otherwise (e.g. because the group `isCancelled`)
  @_alwaysEmitIntoClient
  public mutating func asyncUnlessCancelled(
    priority: TaskPriority? = nil,
    operation: __owned @Sendable @escaping () async -> ChildTaskResult
  ) -> Bool {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    let canAdd = _taskGroupAddPendingTask(group: _group, unconditionally: false)

    guard canAdd else {
      // the group is cancelled and is not accepting any new work
      return false
    }

    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: false
    )

    // Create the task in this group.
    _ = Builtin.createAsyncTaskInGroup(flags, _group, operation)

    return true
#else
    fatalError("Unsupported Swift compiler")
#endif
  }

  /// Wait for the a child task that was added to the group to complete,
  /// and return (or rethrow) the value it completed with. If no tasks are
  /// pending in the task group this function returns `nil`, allowing the
  /// following convenient expressions to be written for awaiting for one
  /// or all tasks to complete:
  ///
  /// Await on a single completion:
  ///
  ///     if let first = try await group.next() {
  ///        return first
  ///     }
  ///
  /// Wait and collect all group child task completions:
  ///
  ///     while let first = try await group.next() {
  ///        collected += value
  ///     }
  ///     return collected
  ///
  /// Awaiting on an empty group results in the immediate return of a `nil`
  /// value, without the group task having to suspend.
  ///
  /// It is also possible to use `for await` to collect results of a task groups:
  ///
  ///     for await try value in group {
  ///         collected += value
  ///     }
  ///
  /// ### Thread-safety
  /// Please note that the `group` object MUST NOT escape into another task.
  /// The `group.next()` MUST be awaited from the task that had originally
  /// created the group. It is not allowed to escape the group reference.
  ///
  /// Note also that this is generally prevented by Swift's type-system,
  /// as the `add` operation is `mutating`, and those may not be performed
  /// from concurrent execution contexts, such as child tasks.
  ///
  /// ### Ordering
  /// Order of values returned by next() is *completion order*, and not
  /// submission order. I.e. if tasks are added to the group one after another:
  ///
  ///     group.spawn { 1 }
  ///     group.spawn { 2 }
  ///
  ///     print(await group.next())
  ///     /// Prints "1" OR "2"
  ///
  /// ### Errors
  /// If an operation added to the group throws, that error will be rethrown
  /// by the next() call corresponding to that operation's completion.
  ///
  /// It is possible to directly rethrow such error out of a `withTaskGroup` body
  /// function's body, causing all remaining tasks to be implicitly cancelled.
  public mutating func next() async -> ChildTaskResult? {
    // try!-safe because this function only exists for Failure == Never,
    // and as such, it is impossible to spawn a throwing child task.
    return try! await _taskGroupWaitNext(group: _group)
  }

  /// Await all the remaining tasks on this group.
  @usableFromInline
  internal mutating func awaitAllRemainingTasks() async {
    while let _ = await next() {}
  }
  
  /// Query whether the group has any remaining tasks.
  ///
  /// Task groups are always empty upon entry to the `withTaskGroup` body, and
  /// become empty again when `withTaskGroup` returns (either by awaiting on all
  /// pending tasks or cancelling them).
  ///
  /// - Returns: `true` if the group has no pending tasks, `false` otherwise.
  public var isEmpty: Bool {
    _taskGroupIsEmpty(_group)
  }

  /// Cancel all the remaining, and future, tasks in the group.
  ///
  /// A cancelled group will not will create new tasks when the `asyncUnlessCancelled`,
  /// function is used. It will, however, continue to create tasks when the plain `async`
  /// function is used. Such tasks will be created yet immediately cancelled, allowing
  /// the tasks to perform some short-cut implementation, if they are responsive to cancellation.
  ///
  /// This function may be called even from within child (or any other) tasks,
  /// and will reliably cause the group to become cancelled.
  ///
  /// - SeeAlso: `Task.isCancelled`
  /// - SeeAlso: `TaskGroup.isCancelled`
  public func cancelAll() {
    _taskGroupCancelAll(group: _group)
  }

  /// Returns `true` if the group was cancelled, e.g. by `cancelAll`.
  ///
  /// If the task currently running this group was cancelled, the group will
  /// also be implicitly cancelled, which will be reflected in the return
  /// value of this function as well.
  ///
  /// - Returns: `true` if the group (or its parent task) was cancelled,
  ///            `false` otherwise.
  public var isCancelled: Bool {
    return _taskGroupIsCancelled(group: _group)
  }
}

// Implementation note:
// We are unable to just™ abstract over Failure == Error / Never because of the
// complicated relationship between `group.spawn` which dictates if `group.next`
// AND the AsyncSequence conformances would be throwing or not.
//
// We would be able to abstract over TaskGroup<..., Failure> equal to Never
// or Error, and specifically only add the `spawn` and `next` functions for
// those two cases. However, we are not able to conform to AsyncSequence "twice"
// depending on if the Failure is Error or Never, as we'll hit:
//    conflicting conformance of 'TaskGroup<ChildTaskResult, Failure>' to protocol
//    'AsyncSequence'; there cannot be more than one conformance, even with
//    different conditional bounds
// So, sadly we're forced to duplicate the entire implementation of TaskGroup
// to TaskGroup and ThrowingTaskGroup.
//
// The throwing task group is parameterized with failure only because of future
// proofing, in case we'd ever have typed errors, however unlikely this may be.
// Today the throwing task group failure is simply automatically bound to `Error`.

/// A task group serves as storage for dynamically spawned, potentially throwing,
/// child tasks.
///
/// It is created by the `withTaskGroup` function.
@available(SwiftStdlib 5.5, *)
@frozen
public struct ThrowingTaskGroup<ChildTaskResult, Failure: Error> {

  /// Group task into which child tasks offer their results,
  /// and the `next()` function polls those results from.
  @usableFromInline
  internal let _group: Builtin.RawPointer

  /// No public initializers
  @inlinable
  init(group: Builtin.RawPointer) {
    self._group = group
  }

  /// Await all the remaining tasks on this group.
  @usableFromInline
  internal mutating func awaitAllRemainingTasks() async {
    while true {
      do {
        guard let _ = try await next() else {
          return
        }
      } catch {}
    }
  }

  /// Spawn, unconditionally, a child task in the group.
  ///
  /// ### Error handling
  /// Operations are allowed to `throw`, in which case the `try await next()`
  /// invocation corresponding to the failed task will re-throw the given task.
  ///
  /// The `add` function will never (re-)throw errors from the `operation`.
  /// Instead, the corresponding `next()` call will throw the error when necessary.
  ///
  /// - Parameters:
  ///   - overridingPriority: override priority of the operation task
  ///   - operation: operation to execute and add to the group
  /// - Returns:
  ///   - `true` if the operation was added to the group successfully,
  ///     `false` otherwise (e.g. because the group `isCancelled`)
  @_alwaysEmitIntoClient
  public mutating func async(
    priority: TaskPriority? = nil,
    operation: __owned @Sendable @escaping () async throws -> ChildTaskResult
  ) {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: true
    )

    // Create the task in this group.
    _ = Builtin.createAsyncTaskInGroup(flags, _group, operation)
#else
    fatalError("Unsupported Swift compiler")
#endif
  }

  /// Add a child task to the group.
  ///
  /// ### Error handling
  /// Operations are allowed to `throw`, in which case the `try await next()`
  /// invocation corresponding to the failed task will re-throw the given task.
  ///
  /// The `add` function will never (re-)throw errors from the `operation`.
  /// Instead, the corresponding `next()` call will throw the error when necessary.
  ///
  /// - Parameters:
  ///   - overridingPriority: override priority of the operation task
  ///   - operation: operation to execute and add to the group
  /// - Returns:
  ///   - `true` if the operation was added to the group successfully,
  ///     `false` otherwise (e.g. because the group `isCancelled`)
  @_alwaysEmitIntoClient
  public mutating func asyncUnlessCancelled(
    priority: TaskPriority? = nil,
    operation: __owned @Sendable @escaping () async throws -> ChildTaskResult
  ) -> Bool {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    let canAdd = _taskGroupAddPendingTask(group: _group, unconditionally: false)

    guard canAdd else {
      // the group is cancelled and is not accepting any new work
      return false
    }

    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: false
    )

    // Create the task in this group.
    _ = Builtin.createAsyncTaskInGroup(flags, _group, operation)

    return true
#else
    fatalError("Unsupported Swift compiler")
#endif
  }

  /// Wait for the a child task that was added to the group to complete,
  /// and return (or rethrow) the value it completed with. If no tasks are
  /// pending in the task group this function returns `nil`, allowing the
  /// following convenient expressions to be written for awaiting for one
  /// or all tasks to complete:
  ///
  /// Await on a single completion:
  ///
  ///     if let first = try await group.next() {
  ///        return first
  ///     }
  ///
  /// Wait and collect all group child task completions:
  ///
  ///     while let first = try await group.next() {
  ///        collected += value
  ///     }
  ///     return collected
  ///
  /// Awaiting on an empty group results in the immediate return of a `nil`
  /// value, without the group task having to suspend.
  ///
  /// It is also possible to use `for await` to collect results of a task groups:
  ///
  ///     for await try value in group {
  ///         collected += value
  ///     }
  ///
  /// ### Thread-safety
  /// Please note that the `group` object MUST NOT escape into another task.
  /// The `group.next()` MUST be awaited from the task that had originally
  /// created the group. It is not allowed to escape the group reference.
  ///
  /// Note also that this is generally prevented by Swift's type-system,
  /// as the `add` operation is `mutating`, and those may not be performed
  /// from concurrent execution contexts, such as child tasks.
  ///
  /// ### Ordering
  /// Order of values returned by next() is *completion order*, and not
  /// submission order. I.e. if tasks are added to the group one after another:
  ///
  ///     group.spawn { 1 }
  ///     group.spawn { 2 }
  ///
  ///     print(await group.next())
  ///     /// Prints "1" OR "2"
  ///
  /// ### Errors
  /// If an operation added to the group throws, that error will be rethrown
  /// by the next() call corresponding to that operation's completion.
  ///
  /// It is possible to directly rethrow such error out of a `withTaskGroup` body
  /// function's body, causing all remaining tasks to be implicitly cancelled.
  public mutating func next() async throws -> ChildTaskResult? {
    return try await _taskGroupWaitNext(group: _group)
  }

  /// - SeeAlso: `next()`
  public mutating func nextResult() async throws -> Result<ChildTaskResult, Failure>? {
    do {
      guard let success: ChildTaskResult = try await _taskGroupWaitNext(group: _group) else {
        return nil
      }

      return .success(success)
    } catch {
      return .failure(error as! Failure) // as!-safe, because we are only allowed to throw Failure (Error)
    }
  }

  /// Query whether the group has any remaining tasks.
  ///
  /// Task groups are always empty upon entry to the `withTaskGroup` body, and
  /// become empty again when `withTaskGroup` returns (either by awaiting on all
  /// pending tasks or cancelling them).
  ///
  /// - Returns: `true` if the group has no pending tasks, `false` otherwise.
  public var isEmpty: Bool {
    _taskGroupIsEmpty(_group)
  }

  /// Cancel all the remaining tasks in the group.
  ///
  /// A cancelled group will not will NOT accept new tasks being added into it.
  ///
  /// Any results, including errors thrown by tasks affected by this
  /// cancellation, are silently discarded.
  ///
  /// This function may be called even from within child (or any other) tasks,
  /// and will reliably cause the group to become cancelled.
  ///
  /// - SeeAlso: `Task.isCancelled`
  /// - SeeAlso: `TaskGroup.isCancelled`
  public func cancelAll() {
    _taskGroupCancelAll(group: _group)
  }

  /// Returns `true` if the group was cancelled, e.g. by `cancelAll`.
  ///
  /// If the task currently running this group was cancelled, the group will
  /// also be implicitly cancelled, which will be reflected in the return
  /// value of this function as well.
  ///
  /// - Returns: `true` if the group (or its parent task) was cancelled,
  ///            `false` otherwise.
  public var isCancelled: Bool {
    return _taskGroupIsCancelled(group: _group)
  }
}

/// ==== TaskGroup: AsyncSequence ----------------------------------------------

@available(SwiftStdlib 5.5, *)
extension TaskGroup: AsyncSequence {
  public typealias AsyncIterator = Iterator
  public typealias Element = ChildTaskResult

  public func makeAsyncIterator() -> Iterator {
    return Iterator(group: self)
  }

  /// Allows iterating over results of tasks added to the group.
  ///
  /// The order of elements returned by this iterator is the same as manually
  /// invoking the `group.next()` function in a loop, meaning that results
  /// are returned in *completion order*.
  ///
  /// This iterator terminates after all tasks have completed successfully, or
  /// after any task completes by throwing an error.
  ///
  /// - SeeAlso: `TaskGroup.next()`
  @available(SwiftStdlib 5.5, *)
  public struct Iterator: AsyncIteratorProtocol {
    public typealias Element = ChildTaskResult

    @usableFromInline
    var group: TaskGroup<ChildTaskResult>

    @usableFromInline
    var finished: Bool = false

    // no public constructors
    init(group: TaskGroup<ChildTaskResult>) {
      self.group = group
    }

    /// Once this function returns `nil` this specific iterator is guaranteed to
    /// never produce more values.
    /// - SeeAlso: `TaskGroup.next()` for a detailed discussion its semantics.
    public mutating func next() async -> Element? {
      guard !finished else { return nil }
      guard let element = await group.next() else {
        finished = true
        return nil
      }
      return element
    }

    public mutating func cancel() {
      finished = true
      group.cancelAll()
    }
  }
}

@available(SwiftStdlib 5.5, *)
extension ThrowingTaskGroup: AsyncSequence {
  public typealias AsyncIterator = Iterator
  public typealias Element = ChildTaskResult

  public func makeAsyncIterator() -> Iterator {
    return Iterator(group: self)
  }

  /// Allows iterating over results of tasks added to the group.
  ///
  /// The order of elements returned by this iterator is the same as manually
  /// invoking the `group.next()` function in a loop, meaning that results
  /// are returned in *completion order*.
  ///
  /// This iterator terminates after all tasks have completed successfully, or
  /// after any task completes by throwing an error. If a task completes by
  /// throwing an error, no further task results are returned.
  ///
  /// - SeeAlso: `ThrowingTaskGroup.next()`
  @available(SwiftStdlib 5.5, *)
  public struct Iterator: AsyncIteratorProtocol {
    public typealias Element = ChildTaskResult

    @usableFromInline
    var group: ThrowingTaskGroup<ChildTaskResult, Failure>

    @usableFromInline
    var finished: Bool = false

    // no public constructors
    init(group: ThrowingTaskGroup<ChildTaskResult, Failure>) {
      self.group = group
    }

    /// - SeeAlso: `ThrowingTaskGroup.next()` for a detailed discussion its semantics.
    public mutating func next() async throws -> Element? {
      guard !finished else { return nil }
      do {
        guard let element = try await group.next() else {
          finished = true
          return nil
        }
        return element
      } catch {
        finished = true
        throw error
      }
    }

    public mutating func cancel() {
      finished = true
      group.cancelAll()
    }
  }
}

/// ==== -----------------------------------------------------------------------

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_taskGroup_destroy")
func _taskGroupDestroy(group: __owned Builtin.RawPointer)

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_taskGroup_addPending")
@usableFromInline
func _taskGroupAddPendingTask(
  group: Builtin.RawPointer,
  unconditionally: Bool
) -> Bool

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_taskGroup_cancelAll")
func _taskGroupCancelAll(group: Builtin.RawPointer)

/// Checks ONLY if the group was specifically cancelled.
/// The task itself being cancelled must be checked separately.
@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_taskGroup_isCancelled")
func _taskGroupIsCancelled(group: Builtin.RawPointer) -> Bool

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_taskGroup_wait_next_throwing")
func _taskGroupWaitNext<T>(group: Builtin.RawPointer) async throws -> T?

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_task_hasTaskGroupStatusRecord")
func _taskHasTaskGroupStatusRecord() -> Bool

@available(SwiftStdlib 5.5, *)
enum PollStatus: Int {
  case empty   = 0
  case waiting = 1
  case success = 2
  case error   = 3
}

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_taskGroup_isEmpty")
func _taskGroupIsEmpty(
  _ group: Builtin.RawPointer
) -> Bool
