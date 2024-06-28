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

// None of TaskExecutor APIs are available in task-to-thread concurrency model.
#if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY

@_unavailableInEmbedded
@available(SwiftStdlib 6.0, *)
extension TaskGroup {
  /// Adds a child task to the group and enqueue it on the specified executor.
  ///
  /// - Parameters:
  ///   - taskExecutor: The task executor that the child task should be started on and keep using.
  ///      Explicitly passing `nil` as the executor preference is equivalent to
  ///      calling the `addTask` method without a preference, and effectively
  ///      means to inherit the outer context's executor preference.
  ///   - priority: The priority of the operation task.
  ///     Omit this parameter or pass `.unspecified`
  ///     to set the child task's priority to the priority of the group.
  ///   - operation: The operation to execute as part of the task group.
  @_alwaysEmitIntoClient
  @_allowFeatureSuppression(IsolatedAny)
  public mutating func addTask(
    executorPreference taskExecutor: (any TaskExecutor)?,
    priority: TaskPriority? = nil,
    operation: sending @escaping @isolated(any) () async -> ChildTaskResult
  ) {
    guard let taskExecutor else {
      return self.addTask(priority: priority, operation: operation)
    }
    #if $BuiltinCreateAsyncTaskInGroupWithExecutor
    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: true,
      isDiscardingTask: false)

    // Create the task in this group with an executor preference.
    #if $BuiltinCreateTask
    let builtinSerialExecutor =
      Builtin.extractFunctionIsolation(operation)?.unownedExecutor.executor

    _ = Builtin.createTask(flags: flags,
                           initialSerialExecutor: builtinSerialExecutor,
                           taskGroup: _group,
                           initialTaskExecutorConsuming: taskExecutor,
                           operation: operation)
    #else
    _ = Builtin.createAsyncTaskInGroupWithExecutor(flags, _group, executorBuiltin, operation)
    #endif
    #else
    fatalError("Unsupported Swift compiler")
    #endif
  }

  /// Adds a child task to the group and enqueue it on the specified executor, unless the group has been canceled.
  ///
  /// - Parameters:
  ///   - taskExecutor: The task executor that the child task should be started on and keep using.
  ///                   If `nil` is passed explicitly, tht parent task's executor preference (if any),
  ///                   will be ignored. In order to inherit the parent task's executor preference
  ///                   invoke `addTaskUnlessCancelled()` without passing a value to the `taskExecutor` parameter,
  ///                   and it will be inherited automatically.
  ///   - priority: The priority of the operation task.
  ///     Omit this parameter or pass `.unspecified`
  ///     to set the child task's priority to the priority of the group.
  ///   - operation: The operation to execute as part of the task group.
  /// - Returns: `true` if the child task was added to the group;
  ///   otherwise `false`.
  @_alwaysEmitIntoClient
  @_allowFeatureSuppression(IsolatedAny)
  public mutating func addTaskUnlessCancelled(
    executorPreference taskExecutor: (any TaskExecutor)?,
    priority: TaskPriority? = nil,
    operation: sending @escaping @isolated(any) () async -> ChildTaskResult
  ) -> Bool {
    guard let taskExecutor else {
      return self.addTaskUnlessCancelled(priority: priority, operation: operation)
    }
    #if $BuiltinCreateAsyncTaskInGroupWithExecutor
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

    // Create the task in this group with an executor preference.
    #if $BuiltinCreateTask
    let builtinSerialExecutor =
      Builtin.extractFunctionIsolation(operation)?.unownedExecutor.executor

    _ = Builtin.createTask(flags: flags,
                           initialSerialExecutor: builtinSerialExecutor,
                           taskGroup: _group,
                           initialTaskExecutorConsuming: taskExecutor,
                           operation: operation)
    #else
    _ = Builtin.createAsyncTaskInGroupWithExecutor(flags, _group, executorBuiltin, operation)
    #endif
    return true
    #else
    fatalError("Unsupported Swift compiler")
    #endif
  }
}

// ==== ThrowingTaskGroup ------------------------------------------------------------------------------------------------------

@_unavailableInEmbedded
@available(SwiftStdlib 6.0, *)
extension ThrowingTaskGroup {
  /// Adds a child task to the group and enqueue it on the specified executor.
  ///
  /// - Parameters:
  ///   - taskExecutor: The task executor that the child task should be started on and keep using.
  ///                   If `nil` is passed explicitly, tht parent task's executor preference (if any),
  ///                   will be ignored. In order to inherit the parent task's executor preference
  ///                   invoke `addTask()` without passing a value to the `taskExecutor` parameter,
  ///                   and it will be inherited automatically.
  ///   - priority: The priority of the operation task.
  ///     Omit this parameter or pass `.unspecified`
  ///     to set the child task's priority to the priority of the group.
  ///   - operation: The operation to execute as part of the task group.
  @_alwaysEmitIntoClient
  @_allowFeatureSuppression(IsolatedAny)
  public mutating func addTask(
    executorPreference taskExecutor: (any TaskExecutor)?,
    priority: TaskPriority? = nil,
    operation: sending @escaping @isolated(any) () async throws -> ChildTaskResult
  ) {
    guard let taskExecutor else {
      return self.addTask(priority: priority, operation: operation)
    }
    #if $BuiltinCreateAsyncTaskInGroupWithExecutor
    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: true,
      isDiscardingTask: false)

    // Create the task in this group with an executor preference.
    #if $BuiltinCreateTask
    let builtinSerialExecutor =
      Builtin.extractFunctionIsolation(operation)?.unownedExecutor.executor

    _ = Builtin.createTask(flags: flags,
                           initialSerialExecutor: builtinSerialExecutor,
                           taskGroup: _group,
                           initialTaskExecutorConsuming: taskExecutor,
                           operation: operation)
    #else
    _ = Builtin.createAsyncTaskInGroupWithExecutor(flags, _group, executorBuiltin, operation)
    #endif
    #else
    fatalError("Unsupported Swift compiler")
    #endif
  }

  /// Adds a child task to the group and enqueue it on the specified executor, unless the group has been canceled.
  ///
  /// - Parameters:
  ///   - taskExecutor: The task executor that the child task should be started on and keep using.
  ///   - priority: The priority of the operation task.
  ///     Omit this parameter or pass `.unspecified`
  ///     to set the child task's priority to the priority of the group.
  ///   - operation: The operation to execute as part of the task group.
  /// - Returns: `true` if the child task was added to the group;
  ///   otherwise `false`.
  @_alwaysEmitIntoClient
  @_allowFeatureSuppression(IsolatedAny)
  public mutating func addTaskUnlessCancelled(
    executorPreference taskExecutor: (any TaskExecutor)?,
    priority: TaskPriority? = nil,
    operation: sending @escaping @isolated(any) () async throws -> ChildTaskResult
  ) -> Bool {
    guard let taskExecutor else {
      return self.addTaskUnlessCancelled(priority: priority, operation: operation)
    }
    #if $BuiltinCreateAsyncTaskInGroupWithExecutor
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

    // Create the task in this group with an executor preference.
    #if $BuiltinCreateTask
    let builtinSerialExecutor =
      Builtin.extractFunctionIsolation(operation)?.unownedExecutor.executor

    _ = Builtin.createTask(flags: flags,
                           initialSerialExecutor: builtinSerialExecutor,
                           taskGroup: _group,
                           initialTaskExecutorConsuming: taskExecutor,
                           operation: operation)
    #else
    _ = Builtin.createAsyncTaskInGroupWithExecutor(flags, _group, executorBuiltin, operation)
    #endif
    return true
    #else
    fatalError("Unsupported Swift compiler")
    #endif
  }
}

// ==== DiscardingTaskGroup ------------------------------------------------------------------------------------------------------

@_unavailableInEmbedded
@available(SwiftStdlib 6.0, *)
extension DiscardingTaskGroup {
  /// Adds a child task to the group and enqueue it on the specified executor.
  ///
  /// - Parameters:
  ///   - taskExecutor: The task executor that the child task should be started on and keep using.
  ///                   If `nil` is passed explicitly, tht parent task's executor preference (if any),
  ///                   will be ignored. In order to inherit the parent task's executor preference
  ///                   invoke `addTask()` without passing a value to the `taskExecutor` parameter,
  ///                   and it will be inherited automatically.
  ///   - priority: The priority of the operation task.
  ///     Omit this parameter or pass `.unspecified`
  ///     to set the child task's priority to the priority of the group.
  ///   - operation: The operation to execute as part of the task group.
  @_alwaysEmitIntoClient
  @_allowFeatureSuppression(IsolatedAny)
  public mutating func addTask(
    executorPreference taskExecutor: (any TaskExecutor)?,
    priority: TaskPriority? = nil,
    operation: sending @escaping @isolated(any) () async -> Void
  ) {
    guard let taskExecutor else {
      return self.addTask(priority: priority, operation: operation)
    }
    #if $BuiltinCreateAsyncDiscardingTaskInGroupWithExecutor
    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: true,
      isDiscardingTask: true)

    // Create the task in this group with an executor preference.
    #if $BuiltinCreateTask
    let builtinSerialExecutor =
      Builtin.extractFunctionIsolation(operation)?.unownedExecutor.executor

    _ = Builtin.createTask(flags: flags,
                           initialSerialExecutor: builtinSerialExecutor,
                           taskGroup: _group,
                           initialTaskExecutorConsuming: taskExecutor,
                           operation: operation)
    #else
    _ = Builtin.createAsyncDiscardingTaskInGroupWithExecutor(flags, _group, executorBuiltin, operation)
    #endif
    #else
    fatalError("Unsupported Swift compiler")
    #endif
  }

  /// Adds a child task to the group and set it up with the passed in task executor preference,
  /// unless the group has been canceled.
  ///
  /// - Parameters:
  ///   - taskExecutor: The task executor that the child task should be started on and keep using.
  ///                   If `nil` is passed explicitly, tht parent task's executor preference (if any),
  ///                   will be ignored. In order to inherit the parent task's executor preference
  ///                   invoke `addTask()` without passing a value to the `taskExecutor` parameter,
  ///                   and it will be inherited automatically.
  ///   - priority: The priority of the operation task.
  ///     Omit this parameter or pass `.unspecified`
  ///     to set the child task's priority to the priority of the group.
  ///   - operation: The operation to execute as part of the task group.
  /// - Returns: `true` if the child task was added to the group;
  ///   otherwise `false`.
  @_alwaysEmitIntoClient
  @_allowFeatureSuppression(IsolatedAny)
  public mutating func addTaskUnlessCancelled(
    executorPreference taskExecutor: (any TaskExecutor)?,
    priority: TaskPriority? = nil,
    operation: sending @escaping @isolated(any) () async -> Void
  ) -> Bool {
    guard let taskExecutor else {
      return self.addTaskUnlessCancelled(priority: priority, operation: operation)
    }
    #if $BuiltinCreateAsyncDiscardingTaskInGroupWithExecutor
    let canAdd = _taskGroupAddPendingTask(group: _group, unconditionally: false)

    guard canAdd else {
      // the group is cancelled and is not accepting any new work
      return false
    }
    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: false, isDiscardingTask: true
    )

    // Create the task in this group with an executor preference.
    #if $BuiltinCreateTask
    let builtinSerialExecutor =
      Builtin.extractFunctionIsolation(operation)?.unownedExecutor.executor

    _ = Builtin.createTask(flags: flags,
                           initialSerialExecutor: builtinSerialExecutor,
                           taskGroup: _group,
                           initialTaskExecutorConsuming: taskExecutor,
                           operation: operation)
    #else
    _ = Builtin.createAsyncDiscardingTaskInGroupWithExecutor(flags, _group, executorBuiltin, operation)
    #endif
    return true
    #else
    fatalError("Unsupported Swift compiler")
    #endif
  }
}

// ==== ThrowingDiscardingTaskGroup ------------------------------------------------------------------------------------------------------

@_unavailableInEmbedded
@available(SwiftStdlib 6.0, *)
extension ThrowingDiscardingTaskGroup {
  /// Adds a child task to the group and set it up with the passed in task executor preference.
  ///
  /// - Parameters:
  ///   - taskExecutor: The task executor that the child task should be started on and keep using.
  ///                   If `nil` is passed explicitly, tht parent task's executor preference (if any),
  ///                   will be ignored. In order to inherit the parent task's executor preference
  ///                   invoke `addTask()` without passing a value to the `taskExecutor` parameter,
  ///                   and it will be inherited automatically.
  ///   - priority: The priority of the operation task.
  ///     Omit this parameter or pass `.unspecified`
  ///     to set the child task's priority to the priority of the group.
  ///   - operation: The operation to execute as part of the task group.
  @_alwaysEmitIntoClient
  @_allowFeatureSuppression(IsolatedAny)
  public mutating func addTask(
    executorPreference taskExecutor: (any TaskExecutor)?,
    priority: TaskPriority? = nil,
    operation: sending @escaping @isolated(any) () async throws -> Void
  ) {
    guard let taskExecutor else {
      return self.addTask(priority: priority, operation: operation)
    }
    #if $BuiltinCreateAsyncDiscardingTaskInGroupWithExecutor
    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: true,
      isDiscardingTask: true)

    // Create the task in this group with an executor preference.
    #if $BuiltinCreateTask
    let builtinSerialExecutor =
      Builtin.extractFunctionIsolation(operation)?.unownedExecutor.executor

    _ = Builtin.createTask(flags: flags,
                           initialSerialExecutor: builtinSerialExecutor,
                           taskGroup: _group,
                           initialTaskExecutorConsuming: taskExecutor,
                           operation: operation)
    #else
    _ = Builtin.createAsyncDiscardingTaskInGroupWithExecutor(flags, _group, executorBuiltin, operation)
    #endif
    #else
    fatalError("Unsupported Swift compiler")
    #endif
  }

  /// Adds a child task to the group and set it up with the passed in task executor preference,
  /// unless the group has been canceled.
  ///
  /// - Parameters:
  ///   - taskExecutor: The task executor that the child task should be started on and keep using.
  ///                   If `nil` is passed explicitly, tht parent task's executor preference (if any),
  ///                   will be ignored. In order to inherit the parent task's executor preference
  ///                   invoke `addTask()` without passing a value to the `taskExecutor` parameter,
  ///                   and it will be inherited automatically.
  ///   - priority: The priority of the operation task.
  ///     Omit this parameter or pass `.unspecified`
  ///     to set the child task's priority to the priority of the group.
  ///   - operation: The operation to execute as part of the task group.
  /// - Returns: `true` if the child task was added to the group;
  ///   otherwise `false`.
  @_alwaysEmitIntoClient
  @_allowFeatureSuppression(IsolatedAny)
  public mutating func addTaskUnlessCancelled(
    executorPreference taskExecutor: (any TaskExecutor)?,
    priority: TaskPriority? = nil,
    operation: sending @escaping @isolated(any) () async throws -> Void
  ) -> Bool {
    guard let taskExecutor else {
      return self.addTaskUnlessCancelled(priority: priority, operation: operation)
    }
    #if $BuiltinCreateAsyncDiscardingTaskInGroupWithExecutor
    let canAdd = _taskGroupAddPendingTask(group: _group, unconditionally: false)

    guard canAdd else {
      // the group is cancelled and is not accepting any new work
      return false
    }
    let flags = taskCreateFlags(
      priority: priority, isChildTask: true, copyTaskLocals: false,
      inheritContext: false, enqueueJob: true,
      addPendingGroupTaskUnconditionally: false, isDiscardingTask: true
    )

    // Create the task in this group with an executor preference.
    #if $BuiltinCreateTask
    let builtinSerialExecutor =
      Builtin.extractFunctionIsolation(operation)?.unownedExecutor.executor

    _ = Builtin.createTask(flags: flags,
                           initialSerialExecutor: builtinSerialExecutor,
                           taskGroup: _group,
                           initialTaskExecutorConsuming: taskExecutor,
                           operation: operation)
    #else
    _ = Builtin.createAsyncDiscardingTaskInGroupWithExecutor(flags, _group, executorBuiltin, operation)
    #endif
    return true
    #else
    fatalError("Unsupported Swift compiler")
    #endif
  }
}

#endif
