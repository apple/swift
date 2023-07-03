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

/// Starts a new scope that can contain a dynamic number of child tasks.
///
/// A group waits for all of its child tasks
/// to complete or be canceled before it returns.
/// After this function returns, the task group is always empty.
///
/// To collect the results of the group's child tasks,
/// you can use a `for`-`await`-`in` loop:
///
///     var sum = 0
///     for await result in group {
///         sum += result
///     }
///
/// If you need more control or only a few results,
/// you can call `next()` directly:
///
///     guard let first = await group.next() else {
///         group.cancelAll()
///         return 0
///     }
///     let second = await group.next() ?? 0
///     group.cancelAll()
///     return first + second
///
/// Task Group Cancellation
/// =======================
///
/// You can cancel a task group and all of its child tasks
/// by calling the `cancelAll()` method on the task group,
/// or by canceling the task in which the group is running.
///
/// If you call `addTask(priority:operation:)` to create a new task in a canceled group,
/// that task is immediately canceled after creation.
/// Alternatively, you can call `addTaskUnlessCancelled(priority:operation:)`,
/// which doesn't create the task if the group has already been canceled
/// Choosing between these two functions
/// lets you control how to react to cancellation within a group:
/// some child tasks need to run regardless of cancellation,
/// but other tasks are better not even being created
/// when you know they can't produce useful results.
///
/// Because the tasks you add to a group with this method are nonthrowing,
/// those tasks can't respond to cancellation by throwing `CancellationError`.
/// The tasks must handle cancellation in some other way,
/// such as returning the work completed so far, returning an empty result, or returning `nil`.
/// For tasks that need to handle cancellation by throwing an error,
/// use the `withThrowingTaskGroup(of:returning:body:)` method instead.
@available(SwiftStdlib 5.1, *)
@_silgen_name("$ss13withTaskGroup2of9returning4bodyq_xm_q_mq_ScGyxGzYaXEtYar0_lF")
@_unsafeInheritExecutor
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

/// Starts a new scope that can contain a dynamic number of throwing child tasks.
///
/// A group waits for all of its child tasks
/// to complete before it returns. Even cancelled tasks must run until
/// completion before this function returns.
/// Cancelled child tasks cooperatively react to cancellation and attempt
/// to return as early as possible.
/// After this function returns, the task group is always empty.
///
/// To collect the results of the group's child tasks,
/// you can use a `for`-`await`-`in` loop:
///
///     var sum = 0
///     for try await result in group {
///         sum += result
///     }
///
/// If you need more control or only a few results,
/// you can call `next()` directly:
///
///     guard let first = try await group.next() else {
///         group.cancelAll()
///         return 0
///     }
///     let second = await group.next() ?? 0
///     group.cancelAll()
///     return first + second
///
/// Task Group Cancellation
/// =======================
///
/// You can cancel a task group and all of its child tasks
/// by calling the `cancelAll()` method on the task group,
/// or by canceling the task in which the group is running.
///
/// If you call `addTask(priority:operation:)` to create a new task in a canceled group,
/// that task is immediately canceled after creation.
/// Alternatively, you can call `addTaskUnlessCancelled(priority:operation:)`,
/// which doesn't create the task if the group has already been canceled
/// Choosing between these two functions
/// lets you control how to react to cancellation within a group:
/// some child tasks need to run regardless of cancellation,
/// but other tasks are better not even being created
/// when you know they can't produce useful results.
///
/// Error Handling
/// ==============
///
/// Throwing an error in one of the child tasks of a task group
/// doesn't immediately cancel the other tasks in that group.
/// However,
/// throwing out of the `body` of the `withThrowingTaskGroup` method does cancel
/// the group, and all of its child tasks.
/// For example,
/// if you call `next()` in the task group and propagate its error,
/// all other tasks are canceled.
/// For example, in the code below,
/// nothing is canceled and the group doesn't throw an error:
///
///     try await withThrowingTaskGroup(of: Void.self) { group in
///         group.addTask { throw SomeError() }
///     }
///
/// In contrast, this example throws `SomeError`
/// and cancels all of the tasks in the group:
///
///     try await withThrowingTaskGroup(of: Void.self) { group in
///         group.addTask { throw SomeError() }
///         try await group.next()
///     }
///
/// An individual task throws its error
/// in the corresponding call to `Group.next()`,
/// which gives you a chance to handle the individual error
/// or to let the group rethrow the error.
@available(SwiftStdlib 5.1, *)
@_silgen_name("$ss21withThrowingTaskGroup2of9returning4bodyq_xm_q_mq_Scgyxs5Error_pGzYaKXEtYaKr0_lF")
@_unsafeInheritExecutor
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

/// A group that contains dynamically created child tasks.
///
/// To create a task group,
/// call the `withTaskGroup(of:returning:body:)` method.
///
/// Don't use a task group from outside the task where you created it.
/// In most cases,
/// the Swift type system prevents a task group from escaping like that
/// because adding a child task to a task group is a mutating operation,
/// and mutation operations can't be performed
/// from a concurrent execution context like a child task.
///
/// ### Task execution order
///
/// Tasks added to a task group execute concurrently, and may be scheduled in
/// any order.
///
/// ### Cancellation behavior
/// A task group becomes cancelled in one of the following ways:
///
/// - when ``cancelAll()`` is invoked on it,
/// - when the ``Task`` running this task group is cancelled.
///
/// Since a `TaskGroup` is a structured concurrency primitive, cancellation is
/// automatically propagated through all of its child-tasks (and their child
/// tasks).
///
/// A cancelled task group can still keep adding tasks, however they will start
/// being immediately cancelled, and may act accordingly to this. To avoid adding
/// new tasks to an already cancelled task group, use ``addTaskUnlessCancelled(priority:body:)``
/// rather than the plain ``addTask(priority:body:)`` which adds tasks unconditionally.
///
/// For information about the language-level concurrency model that `TaskGroup` is part of,
/// see [Concurrency][concurrency] in [The Swift Programming Language][tspl].
///
/// [concurrency]: https://docs.swift.org/swift-book/LanguageGuide/Concurrency.html
/// [tspl]: https://docs.swift.org/swift-book/
///
@available(SwiftStdlib 5.1, *)
@frozen
public struct TaskGroup<ChildTaskResult: Sendable> {

  /// Group task into which child tasks offer their results,
  /// and the `next()` function polls those results from.
  @usableFromInline
  internal let _group: Builtin.RawPointer

  // No public initializers
  @inlinable
  init(group: Builtin.RawPointer) {
    self._group = group
  }

#if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  /// Adds a child task to the group.
  ///
  /// - Parameters:
  ///   - priority: The priority of the operation task.
  ///     Omit this parameter or pass `.unspecified`
  ///     to set the child task's priority to the priority of the group.
  ///   - operation: The operation to execute as part of the task group.
  @_alwaysEmitIntoClient
  public mutating func addTask(
    priority: TaskPriority? = nil,
    operation: __owned @Sendable @escaping () async -> ChildTaskResult
  ) {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
#if SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: false,
      addPendingGroupTaskUnconditionally: true,
      isDiscardingTask: false
    )
#else
    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: true,
      isDiscardingTask: false)
#endif

    // Create the task in this group.
    _ = Builtin.createAsyncTaskInGroup(flags, _group, operation)
#else
    fatalError("Unsupported Swift compiler")
#endif
  }

  /// Adds a child task to the group, unless the group has been canceled.
  ///
  /// - Parameters:
  ///   - priority: The priority of the operation task.
  ///     Omit this parameter or pass `.unspecified`
  ///     to set the child task's priority to the priority of the group.
  ///   - operation: The operation to execute as part of the task group.
  /// - Returns: `true` if the child task was added to the group;
  ///   otherwise `false`.
  @_alwaysEmitIntoClient
  public mutating func addTaskUnlessCancelled(
    priority: TaskPriority? = nil,
    operation: __owned @Sendable @escaping () async -> ChildTaskResult
  ) -> Bool {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    let canAdd = _taskGroupAddPendingTask(group: _group, unconditionally: false)

    guard canAdd else {
      // the group is cancelled and is not accepting any new work
      return false
    }
#if SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: false,
      addPendingGroupTaskUnconditionally: false,
      isDiscardingTask: false)
#else
    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: false,
      isDiscardingTask: false)
#endif

    // Create the task in this group.
    _ = Builtin.createAsyncTaskInGroup(flags, _group, operation)

    return true
#else
    fatalError("Unsupported Swift compiler")
#endif
  }
#else // if SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  @available(SwiftStdlib 5.7, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model", renamed: "addTask(operation:)")
  public mutating func addTask(
    priority: TaskPriority? = nil,
    operation: __owned @Sendable @escaping () async -> ChildTaskResult
  ) {
    fatalError("Unavailable in task-to-thread concurrency model")
  }

  /// Adds a child task to the group.
  ///
  /// - Parameters:
  ///   - operation: The operation to execute as part of the task group.
  @_alwaysEmitIntoClient
  public mutating func addTask(
    operation: __owned @Sendable @escaping () async -> ChildTaskResult
  ) {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    let flags = taskCreateFlags(
      priority: nil, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: true,
      isDiscardingTask: false)

    // Create the task in this group.
    _ = Builtin.createAsyncTaskInGroup(flags, _group, operation)
#else
    fatalError("Unsupported Swift compiler")
#endif
  }

  @available(SwiftStdlib 5.7, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model", renamed: "addTaskUnlessCancelled(operation:)")
  public mutating func addTaskUnlessCancelled(
    priority: TaskPriority? = nil,
    operation: __owned @Sendable @escaping () async -> ChildTaskResult
  ) -> Bool {
    fatalError("Unavailable in task-to-thread concurrency model")
  }

  /// Adds a child task to the group, unless the group has been canceled.
  ///
  /// - Parameters:
  ///   - operation: The operation to execute as part of the task group.
  /// - Returns: `true` if the child task was added to the group;
  ///   otherwise `false`.
  @_alwaysEmitIntoClient
  public mutating func addTaskUnlessCancelled(
    operation: __owned @Sendable @escaping () async -> ChildTaskResult
  ) -> Bool {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    let canAdd = _taskGroupAddPendingTask(group: _group, unconditionally: false)

    guard canAdd else {
      // the group is cancelled and is not accepting any new work
      return false
    }

    let flags = taskCreateFlags(
      priority: nil, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: false,
      isDiscardingTask: false)

    // Create the task in this group.
    _ = Builtin.createAsyncTaskInGroup(flags, _group, operation)

    return true
#else
    fatalError("Unsupported Swift compiler")
#endif
  }
#endif

  /// Wait for the next child task to complete,
  /// and return the value it returned.
  ///
  /// The values returned by successive calls to this method
  /// appear in the order that the tasks *completed*,
  /// not in the order that those tasks were added to the task group.
  /// For example:
  ///
  ///     group.addTask { 1 }
  ///     group.addTask { 2 }
  ///
  ///     print(await group.next())
  ///     // Prints either "2" or "1".
  ///
  /// If there aren't any pending tasks in the task group,
  /// this method returns `nil`,
  /// which lets you write the following
  /// to wait for a single task to complete:
  ///
  ///     if let first = try await group.next() {
  ///        return first
  ///     }
  ///
  /// It also lets you write code like the following
  /// to wait for all the child tasks to complete,
  /// collecting the values they returned:
  ///
  ///     while let first = try await group.next() {
  ///        collected += value
  ///     }
  ///     return collected
  ///
  /// Awaiting on an empty group
  /// immediate returns `nil` without suspending.
  ///
  /// You can also use a `for`-`await`-`in` loop to collect results of a task group:
  ///
  ///     for await try value in group {
  ///         collected += value
  ///     }
  ///
  /// Don't call this method from outside the task
  /// where you created this task group.
  /// In most cases, the Swift type system prevents this mistake.
  /// For example, because the `add(priority:operation:)` method is mutating,
  /// that method can't be called from a concurrent execution context like a child task.
  ///
  /// - Returns: The value returned by the next child task that completes.
  public mutating func next() async -> ChildTaskResult? {
    // try!-safe because this function only exists for Failure == Never,
    // and as such, it is impossible to spawn a throwing child task.
    return try! await _taskGroupWaitNext(group: _group) // !-safe cannot throw, we're a non-throwing TaskGroup
  }

  /// Await all of the pending tasks added this group.
  @usableFromInline
  internal mutating func awaitAllRemainingTasks() async {
    while let _ = await next() {}
  }

  /// Wait for all of the group's remaining tasks to complete.
  @_alwaysEmitIntoClient
  public mutating func waitForAll() async {
    await awaitAllRemainingTasks()
  }

  /// A Boolean value that indicates whether the group has any remaining tasks.
  ///
  /// At the start of the body of a `withTaskGroup(of:returning:body:)` call,
  /// the task group is always empty.
  /// It`s guaranteed to be empty when returning from that body
  /// because a task group waits for all child tasks to complete before returning.
  ///
  /// - Returns: `true` if the group has no pending tasks; otherwise `false`.
  public var isEmpty: Bool {
    _taskGroupIsEmpty(_group)
  }

  /// Cancel all of the remaining tasks in the group.
  ///
  /// If you add a task to a group after canceling the group,
  /// that task is canceled immediately after being added to the group.
  ///
  /// Immediately cancelled child tasks should therefore cooperatively check for and
  /// react  to cancellation, e.g. by throwing an `CancellationError` at their
  /// earliest convenience, or otherwise handling the cancellation.
  ///
  /// There are no restrictions on where you can call this method.
  /// Code inside a child task or even another task can cancel a group,
  /// however one should be very careful to not keep a reference to the
  /// group longer than the `with...TaskGroup(...) { ... }` method body is executing.
  ///
  /// - SeeAlso: `Task.isCancelled`
  /// - SeeAlso: `TaskGroup.isCancelled`
  public func cancelAll() {
    _taskGroupCancelAll(group: _group)
  }

  /// A Boolean value that indicates whether the group was canceled.
  ///
  /// To cancel a group, call the `TaskGroup.cancelAll()` method.
  ///
  /// If the task that's currently running this group is canceled,
  /// the group is also implicitly canceled,
  /// which is also reflected in this property's value.
  public var isCancelled: Bool {
    return _taskGroupIsCancelled(group: _group)
  }
}

@available(SwiftStdlib 5.1, *)
@available(*, unavailable)
extension TaskGroup: Sendable { }

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

/// A group that contains throwing, dynamically created child tasks.
///
/// To create a throwing task group,
/// call the `withThrowingTaskGroup(of:returning:body:)` method.
///
/// Don't use a task group from outside the task where you created it.
/// In most cases,
/// the Swift type system prevents a task group from escaping like that
/// because adding a child task to a task group is a mutating operation,
/// and mutation operations can't be performed
/// from concurrent execution contexts like a child task.
///
/// ### Task execution order
/// Tasks added to a task group execute concurrently, and may be scheduled in
/// any order.
///
/// ### Cancellation behavior
/// A task group becomes cancelled in one of the following ways:
///
/// - when ``cancelAll()`` is invoked on it,
/// - when an error is thrown out of the `withThrowingTaskGroup(...) { }` closure,
/// - when the ``Task`` running this task group is cancelled.
///
/// Since a `ThrowingTaskGroup` is a structured concurrency primitive, cancellation is
/// automatically propagated through all of its child-tasks (and their child
/// tasks).
///
/// A cancelled task group can still keep adding tasks, however they will start
/// being immediately cancelled, and may act accordingly to this. To avoid adding
/// new tasks to an already cancelled task group, use ``addTaskUnlessCancelled(priority:body:)``
/// rather than the plain ``addTask(priority:body:)`` which adds tasks unconditionally.
///
/// For information about the language-level concurrency model that `ThrowingTaskGroup` is part of,
/// see [Concurrency][concurrency] in [The Swift Programming Language][tspl].
///
/// [concurrency]: https://docs.swift.org/swift-book/LanguageGuide/Concurrency.html
/// [tspl]: https://docs.swift.org/swift-book/
///
@available(SwiftStdlib 5.1, *)
@frozen
public struct ThrowingTaskGroup<ChildTaskResult: Sendable, Failure: Error> {

  /// Group task into which child tasks offer their results,
  /// and the `next()` function polls those results from.
  @usableFromInline
  internal let _group: Builtin.RawPointer

  // No public initializers
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

  @usableFromInline
  internal mutating func _waitForAll() async throws {
    await self.awaitAllRemainingTasks()
  }

  /// Wait for all of the group's remaining tasks to complete.
  ///
  /// If any of the tasks throw, the *first* error thrown is captured
  /// and re-thrown by this method although the task group is *not* cancelled
  /// when this happens.
  ///
  /// ### Cancelling the task group on first error
  ///
  /// If you want to cancel the task group, and all "sibling" tasks,
  /// whenever any of child tasks throws an error, use the following pattern instead:
  ///
  /// ```
  /// while !group.isEmpty {
  ///     do {
  ///         try await group.next()
  ///     } catch is CancellationError {
  ///         // we decide that cancellation errors thrown by children,
  ///         // should not cause cancellation of the entire group.
  ///         continue;
  ///     } catch {
  ///         // other errors though we print and cancel the group,
  ///         // and all of the remaining child tasks within it.
  ///         print("Error: \(error)")
  ///         group.cancelAll()
  ///     }
  /// }
  /// assert(group.isEmpty())
  /// ```
  ///
  /// - Throws: The *first* error that was thrown by a child task during draining all the tasks.
  ///           This first error is stored until all other tasks have completed, and is re-thrown afterwards.
  @_alwaysEmitIntoClient
  public mutating func waitForAll() async throws {
    var firstError: Error? = nil

    // Make sure we loop until all child tasks have completed
    while !isEmpty {
      do {
        while let _ = try await next() {}
      } catch {
        // Upon error throws, capture the first one
        if firstError == nil {
          firstError = error
        }
      }
    }

    if let firstError {
      throw firstError
    }
  }

#if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
  /// Adds a child task to the group.
  ///
  /// This method doesn't throw an error, even if the child task does.
  /// Instead, the corresponding call to `ThrowingTaskGroup.next()` rethrows that error.
  ///
  /// - Parameters:
  ///   - overridingPriority: The priority of the operation task.
  ///     Omit this parameter or pass `.unspecified`
  ///     to set the child task's priority to the priority of the group.
  ///   - operation: The operation to execute as part of the task group.
  @_alwaysEmitIntoClient
  public mutating func addTask(
    priority: TaskPriority? = nil,
    operation: __owned @Sendable @escaping () async throws -> ChildTaskResult
  ) {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: true,
      isDiscardingTask: false
    )

    // Create the task in this group.
    _ = Builtin.createAsyncTaskInGroup(flags, _group, operation)
#else
    fatalError("Unsupported Swift compiler")
#endif
  }

  /// Adds a child task to the group, unless the group has been canceled.
  ///
  /// This method doesn't throw an error, even if the child task does.
  /// Instead, the corresponding call to `ThrowingTaskGroup.next()` rethrows that error.
  ///
  /// - Parameters:
  ///   - overridingPriority: The priority of the operation task.
  ///     Omit this parameter or pass `.unspecified`
  ///     to set the child task's priority to the priority of the group.
  ///   - operation: The operation to execute as part of the task group.
  /// - Returns: `true` if the child task was added to the group;
  ///   otherwise `false`.
  @_alwaysEmitIntoClient
  public mutating func addTaskUnlessCancelled(
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
      addPendingGroupTaskUnconditionally: false,
      isDiscardingTask: false)

    // Create the task in this group.
    _ = Builtin.createAsyncTaskInGroup(flags, _group, operation)

    return true
#else
    fatalError("Unsupported Swift compiler")
#endif
  }
#else
  @available(SwiftStdlib 5.7, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model", renamed: "addTask(operation:)")
  public mutating func addTask(
    priority: TaskPriority? = nil,
    operation: __owned @Sendable @escaping () async throws -> ChildTaskResult
  ) {
    fatalError("Unavailable in task-to-thread concurrency model")
  }

  /// Adds a child task to the group.
  ///
  /// This method doesn't throw an error, even if the child task does.
  /// Instead, the corresponding call to `ThrowingTaskGroup.next()` rethrows that error.
  ///
  /// - Parameters:
  ///   - operation: The operation to execute as part of the task group.
  @_alwaysEmitIntoClient
  public mutating func addTask(
    operation: __owned @Sendable @escaping () async throws -> ChildTaskResult
  ) {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    let flags = taskCreateFlags(
      priority: nil, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: true,
      isDiscardingTask: false)

    // Create the task in this group.
    _ = Builtin.createAsyncTaskInGroup(flags, _group, operation)
#else
    fatalError("Unsupported Swift compiler")
#endif
  }

  @available(SwiftStdlib 5.7, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model", renamed: "addTaskUnlessCancelled(operation:)")
  public mutating func addTaskUnlessCancelled(
    priority: TaskPriority? = nil,
    operation: __owned @Sendable @escaping () async throws -> ChildTaskResult
  ) -> Bool {
    fatalError("Unavailable in task-to-thread concurrency model")
  }

  /// Adds a child task to the group, unless the group has been canceled.
  ///
  /// This method doesn't throw an error, even if the child task does.
  /// Instead, the corresponding call to `ThrowingTaskGroup.next()` rethrows that error.
  ///
  /// - Parameters:
  ///   - operation: The operation to execute as part of the task group.
  /// - Returns: `true` if the child task was added to the group;
  ///   otherwise `false`.
  @_alwaysEmitIntoClient
  public mutating func addTaskUnlessCancelled(
    operation: __owned @Sendable @escaping () async throws -> ChildTaskResult
  ) -> Bool {
#if compiler(>=5.5) && $BuiltinCreateAsyncTaskInGroup
    let canAdd = _taskGroupAddPendingTask(group: _group, unconditionally: false)

    guard canAdd else {
      // the group is cancelled and is not accepting any new work
      return false
    }

    let flags = taskCreateFlags(
      priority: nil, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: false,
      isDiscardingTask: false)

    // Create the task in this group.
    _ = Builtin.createAsyncTaskInGroup(flags, _group, operation)

    return true
#else
    fatalError("Unsupported Swift compiler")
#endif
  }
#endif

  /// Wait for the next child task to complete,
  /// and return the value it returned or rethrow the error it threw.
  ///
  /// The values returned by successive calls to this method
  /// appear in the order that the tasks *completed*,
  /// not in the order that those tasks were added to the task group.
  /// For example:
  ///
  ///     group.addTask { 1 }
  ///     group.addTask { 2 }
  ///
  ///     print(await group.next())
  ///     // Prints either "2" or "1".
  ///
  /// If there aren't any pending tasks in the task group,
  /// this method returns `nil`,
  /// which lets you write the following
  /// to wait for a single task to complete:
  ///
  ///     if let first = try await group.next() {
  ///        return first
  ///     }
  ///
  /// It also lets you write code like the following
  /// to wait for all the child tasks to complete,
  /// collecting the values they returned:
  ///
  ///     while let first = try await group.next() {
  ///        collected += value
  ///     }
  ///     return collected
  ///
  /// Awaiting on an empty group
  /// immediately returns `nil` without suspending.
  ///
  /// You can also use a `for`-`await`-`in` loop to collect results of a task group:
  ///
  ///     for try await value in group {
  ///         collected += value
  ///     }
  ///
  /// If the next child task throws an error
  /// and you propagate that error from this method
  /// out of the body of a call to the
  /// `ThrowingTaskGroup.withThrowingTaskGroup(of:returning:body:)` method,
  /// then all remaining child tasks in that group are implicitly canceled.
  ///
  /// Don't call this method from outside the task
  /// where this task group was created.
  /// In most cases, the Swift type system prevents this mistake;
  /// for example, because the `add(priority:operation:)` method is mutating,
  /// that method can't be called from a concurrent execution context like a child task.
  ///
  /// - Returns: The value returned by the next child task that completes.
  ///
  /// - Throws: The error thrown by the next child task that completes.
  ///
  /// - SeeAlso: `nextResult()`
  public mutating func next() async throws -> ChildTaskResult? {
    return try await _taskGroupWaitNext(group: _group)
  }

  @_silgen_name("$sScg10nextResults0B0Oyxq_GSgyYaKF")
  @usableFromInline
  mutating func nextResultForABI() async throws -> Result<ChildTaskResult, Failure>? {
    do {
      guard let success: ChildTaskResult = try await _taskGroupWaitNext(group: _group) else {
        return nil
      }

      return .success(success)
    } catch {
      return .failure(error as! Failure) // as!-safe, because we are only allowed to throw Failure (Error)
    }
  }

  /// Wait for the next child task to complete,
  /// and return a result containing either
  /// the value that the child task returned or the error that it threw.
  ///
  /// The values returned by successive calls to this method
  /// appear in the order that the tasks *completed*,
  /// not in the order that those tasks were added to the task group.
  /// For example:
  ///
  ///     group.addTask { 1 }
  ///     group.addTask { 2 }
  ///
  ///     guard let result = await group.nextResult() else {
  ///         return  // No task to wait on, which won't happen in this example.
  ///     }
  ///
  ///     switch result {
  ///     case .success(let value): print(value)
  ///     case .failure(let error): print("Failure: \(error)")
  ///     }
  ///     // Prints either "2" or "1".
  ///
  /// If the next child task throws an error
  /// and you propagate that error from this method
  /// out of the body of a call to the
  /// `ThrowingTaskGroup.withThrowingTaskGroup(of:returning:body:)` method,
  /// then all remaining child tasks in that group are implicitly canceled.
  ///
  /// - Returns: A `Result.success` value
  ///   containing the value that the child task returned,
  ///   or a `Result.failure` value
  ///   containing the error that the child task threw.
  ///
  /// - SeeAlso: `next()`
  @_alwaysEmitIntoClient
  public mutating func nextResult() async -> Result<ChildTaskResult, Failure>? {
    return try! await nextResultForABI()
  }

  /// A Boolean value that indicates whether the group has any remaining tasks.
  ///
  /// At the start of the body of a `withThrowingTaskGroup(of:returning:body:)` call,
  /// the task group is always empty.
  ///
  /// It's guaranteed to be empty when returning from that body
  /// because a task group waits for all child tasks to complete before returning.
  ///
  /// - Returns: `true` if the group has no pending tasks; otherwise `false`.
  public var isEmpty: Bool {
    _taskGroupIsEmpty(_group)
  }

  /// Cancel all of the remaining tasks in the group.
  ///
  /// If you add a task to a group after canceling the group,
  /// that task is canceled immediately after being added to the group.
  ///
  /// Immediately cancelled child tasks should therefore cooperatively check for and
  /// react  to cancellation, e.g. by throwing an `CancellationError` at their
  /// earliest convenience, or otherwise handling the cancellation.
  ///
  /// There are no restrictions on where you can call this method.
  /// Code inside a child task or even another task can cancel a group,
  /// however one should be very careful to not keep a reference to the
  /// group longer than the `with...TaskGroup(...) { ... }` method body is executing.
  ///
  /// - SeeAlso: `Task.isCancelled`
  /// - SeeAlso: `ThrowingTaskGroup.isCancelled`
  public func cancelAll() {
    _taskGroupCancelAll(group: _group)
  }

  /// A Boolean value that indicates whether the group was canceled.
  ///
  /// To cancel a group, call the `ThrowingTaskGroup.cancelAll()` method.
  ///
  /// If the task that's currently running this group is canceled,
  /// the group is also implicitly canceled,
  /// which is also reflected in this property's value.
  public var isCancelled: Bool {
    return _taskGroupIsCancelled(group: _group)
  }
}

@available(SwiftStdlib 5.1, *)
@available(*, unavailable)
extension ThrowingTaskGroup: Sendable { }

/// ==== TaskGroup: AsyncSequence ----------------------------------------------

@available(SwiftStdlib 5.1, *)
extension TaskGroup: AsyncSequence {
  public typealias AsyncIterator = Iterator
  public typealias Element = ChildTaskResult

  public func makeAsyncIterator() -> Iterator {
    return Iterator(group: self)
  }

  /// A type that provides an iteration interface
  /// over the results of tasks added to the group.
  ///
  /// The elements returned by this iterator
  /// appear in the order that the tasks *completed*,
  /// not in the order that those tasks were added to the task group.
  ///
  /// This iterator terminates after all tasks have completed.
  /// After iterating over the results of each task,
  /// it's valid to make a new iterator for the task group,
  /// which you can use to iterate over the results of new tasks you add to the group.
  /// For example:
  ///
  ///     group.addTask { 1 }
  ///     for await r in group { print(r) }
  ///
  ///     // Add a new child task and iterate again.
  ///     group.addTask { 2 }
  ///     for await r in group { print(r) }
  ///
  /// - SeeAlso: `TaskGroup.next()`
  @available(SwiftStdlib 5.1, *)
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

    /// Advances to and returns the result of the next child task.
    ///
    /// The elements returned from this method
    /// appear in the order that the tasks *completed*,
    /// not in the order that those tasks were added to the task group.
    /// After this method returns `nil`,
    /// this iterator is guaranteed to never produce more values.
    ///
    /// For more information about the iteration order and semantics,
    /// see `TaskGroup.next()`.
    ///
    /// - Returns: The value returned by the next child task that completes,
    ///   or `nil` if there are no remaining child tasks,
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

@available(SwiftStdlib 5.1, *)
extension ThrowingTaskGroup: AsyncSequence {
  public typealias AsyncIterator = Iterator
  public typealias Element = ChildTaskResult

  public func makeAsyncIterator() -> Iterator {
    return Iterator(group: self)
  }

  /// A type that provides an iteration interface
  /// over the results of tasks added to the group.
  ///
  /// The elements returned by this iterator
  /// appear in the order that the tasks *completed*,
  /// not in the order that those tasks were added to the task group.
  ///
  /// This iterator terminates after all tasks have completed successfully,
  /// or after any task completes by throwing an error.
  /// If a task completes by throwing an error,
  /// it doesn't return any further task results.
  /// After iterating over the results of each task,
  /// it's valid to make a new iterator for the task group,
  /// which you can use to iterate over the results of new tasks you add to the group.
  /// You can also make a new iterator to resume iteration
  /// after a child task throws an error.
  /// For example:
  ///
  ///     group.addTask { 1 }
  ///     group.addTask { throw SomeError }
  ///     group.addTask { 2 }
  ///
  ///     do {
  ///         // Assuming the child tasks complete in order, this prints "1"
  ///         // and then throws an error.
  ///         for try await r in group { print(r) }
  ///     } catch {
  ///         // Resolve the error.
  ///     }
  ///
  ///     // Assuming the child tasks complete in order, this prints "2".
  ///     for try await r in group { print(r) }
  ///
  /// - SeeAlso: `ThrowingTaskGroup.next()`
  @available(SwiftStdlib 5.1, *)
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

    /// Advances to and returns the result of the next child task.
    ///
    /// The elements returned from this method
    /// appear in the order that the tasks *completed*,
    /// not in the order that those tasks were added to the task group.
    /// After this method returns `nil`,
    /// this iterator is guaranteed to never produce more values.
    ///
    /// For more information about the iteration order and semantics,
    /// see `ThrowingTaskGroup.next()`
    ///
    /// - Throws: The error thrown by the next child task that completes.
    ///
    /// - Returns: The value returned by the next child task that completes,
    ///   or `nil` if there are no remaining child tasks,
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

// ==== -----------------------------------------------------------------------
// MARK: Runtime functions

@available(SwiftStdlib 5.1, *)
@_silgen_name("swift_taskGroup_destroy")
func _taskGroupDestroy(group: __owned Builtin.RawPointer)

@available(SwiftStdlib 5.1, *)
@_silgen_name("swift_taskGroup_addPending")
@usableFromInline
func _taskGroupAddPendingTask(
  group: Builtin.RawPointer,
  unconditionally: Bool
) -> Bool

@available(SwiftStdlib 5.1, *)
@_silgen_name("swift_taskGroup_cancelAll")
func _taskGroupCancelAll(group: Builtin.RawPointer)

/// Checks ONLY if the group was specifically canceled.
/// The task itself being canceled must be checked separately.
@available(SwiftStdlib 5.1, *)
@_silgen_name("swift_taskGroup_isCancelled")
func _taskGroupIsCancelled(group: Builtin.RawPointer) -> Bool

@available(SwiftStdlib 5.1, *)
@_silgen_name("swift_taskGroup_wait_next_throwing")
func _taskGroupWaitNext<T>(group: Builtin.RawPointer) async throws -> T?

@available(SwiftStdlib 5.1, *)
@_silgen_name("swift_task_hasTaskGroupStatusRecord")
func _taskHasTaskGroupStatusRecord() -> Bool

@available(SwiftStdlib 5.1, *)
enum PollStatus: Int {
  case empty   = 0
  case waiting = 1
  case success = 2
  case error   = 3
}

@available(SwiftStdlib 5.1, *)
@_silgen_name("swift_taskGroup_isEmpty")
func _taskGroupIsEmpty(
  _ group: Builtin.RawPointer
) -> Bool


// ==== TaskGroup Flags --------------------------------------------------------------

/// Flags for task groups.
///
/// This is a port of the C++ FlagSet.
@available(SwiftStdlib 5.8, *)
struct TaskGroupFlags {
  /// The actual bit representation of these flags.
  var bits: Int32 = 0

  /// The priority given to the job.
  var discardResults: Bool? {
    get {
      let value = (Int(bits) & 1 << 24)

      return value > 0
    }

    set {
      if newValue == true {
        bits = bits | 1 << 24
      } else {
        bits = (bits & ~(1 << 23))
      }
    }
  }
}

// ==== Task Creation Flags --------------------------------------------------

/// Form task creation flags for use with the createAsyncTask builtins.
@available(SwiftStdlib 5.8, *)
@_alwaysEmitIntoClient
func taskGroupCreateFlags(
        discardResults: Bool) -> Int {
  var bits = 0
  if discardResults {
    bits |= 1 << 8
  }
  return bits
}
