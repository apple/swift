//===--- Concurrency.h - Runtime interface for concurrency ------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// The runtime interface for concurrency.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_RUNTIME_CONCURRENCY_H
#define SWIFT_RUNTIME_CONCURRENCY_H

#include "swift/ABI/Task.h"
#include "swift/ABI/TaskGroup.h"
#include "swift/ABI/AsyncLet.h"
#include "swift/ABI/TaskStatus.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"

namespace swift {
class DefaultActor;
class TaskOptionRecord;

struct SwiftError;

struct AsyncTaskAndContext {
  AsyncTask *Task;
  AsyncContext *InitialContext;
};

/// Create a task object.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
AsyncTaskAndContext swift_task_create(
    size_t taskCreateFlags,
    TaskOptionRecord *options,
    const Metadata *futureResultType,
    void *closureEntry, HeapObject *closureContext);

/// Caution: not all future-initializing functions actually throw, so
/// this signature may be incorrect.
using FutureAsyncSignature =
  AsyncSignature<void(void*), /*throws*/ true>;

/// Create a task object.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
AsyncTaskAndContext swift_task_create_common(
    size_t taskCreateFlags,
    TaskOptionRecord *options,
    const Metadata *futureResultType,
    FutureAsyncSignature::FunctionType *function, void *closureContext,
    size_t initialContextSize);

/// Allocate memory in a task.
///
/// This must be called synchronously with the task.
///
/// All allocations will be rounded to a multiple of MAX_ALIGNMENT.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void *swift_task_alloc(size_t size);

/// Deallocate memory in a task.
///
/// The pointer provided must be the last pointer allocated on
/// this task that has not yet been deallocated; that is, memory
/// must be allocated and deallocated in a strict stack discipline.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_dealloc(void *ptr);

/// Cancel a task and all of its child tasks.
///
/// This can be called from any thread.
///
/// This has no effect if the task is already cancelled.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_cancel(AsyncTask *task);

/// Cancel all the child tasks that belong to the `group`.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_cancel_group_child_tasks(TaskGroup *group);

/// Escalate the priority of a task and all of its child tasks.
///
/// This can be called from any thread.
///
/// This has no effect if the task already has at least the given priority.
/// Returns the priority of the task.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
JobPriority
swift_task_escalate(AsyncTask *task, JobPriority newPriority);

// TODO: "async let wait" and "async let destroy" would be expressed
//       similar to like TaskFutureWait;

/// Wait for a non-throwing future task to complete.
///
/// This can be called from any thread. Its Swift signature is
///
/// \code
/// func swift_task_future_wait(on task: _owned Builtin.NativeObject) async
///     -> Success
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swiftasync)
void swift_task_future_wait(OpaqueValue *,
         SWIFT_ASYNC_CONTEXT AsyncContext *, AsyncTask *,
         TaskContinuationFunction *,
         AsyncContext *);

/// Wait for a potentially-throwing future task to complete.
///
/// This can be called from any thread. Its Swift signature is
///
/// \code
/// func swift_task_future_wait_throwing(on task: _owned Builtin.NativeObject)
///    async throws -> Success
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swiftasync)
void swift_task_future_wait_throwing(
  OpaqueValue *,
  SWIFT_ASYNC_CONTEXT AsyncContext *,
  AsyncTask *,
  ThrowingTaskFutureWaitContinuationFunction *,
  AsyncContext *);

/// Wait for a readyQueue of a Channel to become non empty.
///
/// This can be called from any thread. Its Swift signature is
///
/// \code
/// func swift_taskGroup_wait_next_throwing(
///     waitingTask: Builtin.NativeObject, // current task
///     group: Builtin.RawPointer
/// ) async -> T
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency)
SWIFT_CC(swiftasync)
void swift_taskGroup_wait_next_throwing(
    OpaqueValue *resultPointer, SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
    TaskGroup *group, ThrowingTaskFutureWaitContinuationFunction *resumeFn,
    AsyncContext *callContext);

/// Initialize a `TaskGroup` in the passed `group` memory location.
/// The caller is responsible for retaining and managing the group's lifecycle.
///
/// Its Swift signature is
///
/// \code
/// func swift_taskGroup_initialize(group: Builtin.RawPointer)
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_taskGroup_initialize(TaskGroup *group, const Metadata *T);

/// Attach a child task to the parent task's task group record.
///
/// This function MUST be called from the AsyncTask running the task group.
///
/// Since the group (or rather, its record) is inserted in the parent task at
/// creation we do not need the parent task here, the group already is attached
/// to it.
/// Its Swift signature is
///
/// \code
/// func swift_taskGroup_attachChild(
///     group: Builtin.RawPointer,
///     child: Builtin.NativeObject
/// )
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_taskGroup_attachChild(TaskGroup *group, AsyncTask *child);

/// Its Swift signature is
///
/// This function MUST be called from the AsyncTask running the task group.
///
/// \code
/// func swift_taskGroup_destroy(_ group: Builtin.RawPointer)
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_taskGroup_destroy(TaskGroup *group);

/// Before starting a task group child task, inform the group that there is one
/// more 'pending' child to account for.
///
/// This function SHOULD be called from the AsyncTask running the task group,
/// however is generally thread-safe as it only only works with the group status.
///
/// Its Swift signature is
///
/// \code
/// func swift_taskGroup_addPending(
///     group: Builtin.RawPointer,
///     unconditionally: Bool
/// ) -> Bool
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
bool swift_taskGroup_addPending(TaskGroup *group, bool unconditionally);

/// Cancel all tasks in the group.
/// This also prevents new tasks from being added.
///
/// This can be called from any thread.
///
/// Its Swift signature is
///
/// \code
/// func swift_taskGroup_cancelAll(group: Builtin.RawPointer)
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_taskGroup_cancelAll(TaskGroup *group);

/// Check ONLY if the group was explicitly cancelled, e.g. by `cancelAll`.
///
/// This check DOES NOT take into account the task in which the group is running
/// being cancelled or not.
///
/// This can be called from any thread. Its Swift signature is
///
/// \code
/// func swift_taskGroup_isCancelled(group: Builtin.RawPointer)
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
bool swift_taskGroup_isCancelled(TaskGroup *group);

/// Check the readyQueue of a task group, return true if it has no pending tasks.
///
/// This can be called from any thread. Its Swift signature is
///
/// \code
/// func swift_taskGroup_isEmpty(
///     _ group: Builtin.RawPointer
/// ) -> Bool
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
bool swift_taskGroup_isEmpty(TaskGroup *group);

/// Its Swift signature is
///
/// \code
/// func swift_asyncLet_start<T>(
///     asyncLet: Builtin.RawPointer,
///     options: Builtin.RawPointer?,
///     operation: __owned @Sendable () async throws -> T
/// )
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_asyncLet_start(AsyncLet *alet,
                          TaskOptionRecord *options,
                          const Metadata *futureResultType,
                          void *closureEntryPoint, HeapObject *closureContext);

/// This matches the ABI of a closure `<T>(Builtin.RawPointer) async -> T`
using AsyncLetWaitSignature =
    SWIFT_CC(swiftasync)
    void(OpaqueValue *,
         SWIFT_ASYNC_CONTEXT AsyncContext *, AsyncTask *, Metadata *);

/// Wait for a non-throwing async-let to complete.
///
/// This can be called from any thread. Its Swift signature is
///
/// \code
/// func swift_asyncLet_wait(
///     _ asyncLet: _owned Builtin.RawPointer
/// ) async -> Success
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swiftasync)
void swift_asyncLet_wait(OpaqueValue *,
                         SWIFT_ASYNC_CONTEXT AsyncContext *,
                         AsyncLet *, TaskContinuationFunction *,
                         AsyncContext *);

/// Wait for a potentially-throwing async-let to complete.
///
/// This can be called from any thread. Its Swift signature is
///
/// \code
/// func swift_asyncLet_wait_throwing(
///     _ asyncLet: _owned Builtin.RawPointer
/// ) async throws -> Success
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swiftasync)
void swift_asyncLet_wait_throwing(OpaqueValue *,
                                  SWIFT_ASYNC_CONTEXT AsyncContext *,
                                  AsyncLet *,
                                  ThrowingTaskFutureWaitContinuationFunction *,
                                  AsyncContext *);

/// Its Swift signature is
///
/// \code
/// func swift_asyncLet_end(_ alet: Builtin.RawPointer)
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_asyncLet_end(AsyncLet *alet);

/// Returns true if the currently executing AsyncTask has a
/// 'TaskGroupTaskStatusRecord' present.
///
/// This can be called from any thread.
///
/// Its Swift signature is
///
/// \code
/// func swift_taskGroup_hasTaskGroupRecord()
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
bool swift_taskGroup_hasTaskGroupRecord();

/// Add a status record to a task.  The record should not be
/// modified while it is registered with a task.
///
/// This must be called synchronously with the task.
///
/// If the task is already cancelled, returns `false` but still adds
/// the status record.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
bool swift_task_addStatusRecord(TaskStatusRecord *record);

/// Add a status record to a task if the task has not already
/// been cancelled.   The record should not be modified while it is
/// registered with a task.
///
/// This must be called synchronously with the task.
///
/// If the task is already cancelled, returns `false` and does not
/// add the status record.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
bool swift_task_tryAddStatusRecord(TaskStatusRecord *record);

/// Remove a status record from a task.  After this call returns,
/// the record's memory can be freely modified or deallocated.
///
/// This must be called synchronously with the task.  The record must
/// be registered with the task or else this may crash.
///
/// The given record need not be the last record added to
/// the task, but the operation may be less efficient if not.
///
/// Returns false if the task has been cancelled.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
bool swift_task_removeStatusRecord(TaskStatusRecord *record);

/// Signifies whether the current task is in the middle of executing the
/// operation block of a `with(Throwing)TaskGroup(...) { <operation> }`.
///
/// Task local values must use un-structured allocation for values bound in this
/// scope, as they may be referred to by `group.spawn`-ed tasks and therefore
/// out-life the scope of a task-local value binding.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
bool swift_task_hasTaskGroupStatusRecord();

/// Attach a child task to its parent task and return the newly created
/// `ChildTaskStatusRecord`.
///
/// The record must be removed with by the parent invoking
/// `swift_task_detachChild` when the child has completed.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
ChildTaskStatusRecord* swift_task_attachChild(AsyncTask *child);

/// Remove a child task from the parent tracking it.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_detachChild(ChildTaskStatusRecord *record);

SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
size_t swift_task_getJobFlags(AsyncTask* task);

SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
bool swift_task_isCancelled(AsyncTask* task);

/// Create and add an cancellation record to the task.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
CancellationNotificationStatusRecord*
swift_task_addCancellationHandler(
    CancellationNotificationStatusRecord::FunctionType handler,
    void *handlerContext);

/// Remove the passed cancellation record from the task.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_removeCancellationHandler(
    CancellationNotificationStatusRecord *record);

/// Create a NullaryContinuationJob from a continuation.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
NullaryContinuationJob*
swift_task_createNullaryContinuationJob(
    size_t priority,
    AsyncTask *continuation);

/// Report error about attempting to bind a task-local value from an illegal context.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_reportIllegalTaskLocalBindingWithinWithTaskGroup(
    const unsigned char *file, uintptr_t fileLength,
    bool fileIsASCII, uintptr_t line);

/// Get a task local value from either the current task, or fallback task-local
/// storage.
///
/// Its Swift signature is
///
/// \code
/// func _taskLocalValueGet<Key>(
///   keyType: Any.Type /*Key.Type*/
/// ) -> UnsafeMutableRawPointer? where Key: TaskLocalKey
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
OpaqueValue*
swift_task_localValueGet(const HeapObject *key);

/// Bind a task local key to a value in the context of either the current
/// AsyncTask if present, or in the thread-local fallback context if no task
/// available.
///
/// Its Swift signature is
///
/// \code
///  public func _taskLocalValuePush<Value>(
///    keyType: Any.Type/*Key.Type*/,
///    value: __owned Value
///  )
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_localValuePush(const HeapObject *key,
                                   /* +1 */ OpaqueValue *value,
                                   const Metadata *valueType);

/// Pop a single task local binding from the binding stack of the current task,
/// or the fallback thread-local storage if no task is available.
///
/// This operation must be paired up with a preceding "push" operation, as otherwise
/// it may attempt to "pop" off an empty value stuck which will lead to a crash.
///
/// The Swift surface API ensures proper pairing of push and pop operations.
///
/// Its Swift signature is
///
/// \code
///  public func _taskLocalValuePop()
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_localValuePop();

/// Copy all task locals from the current context to the target task.
///
/// Its Swift signature is
///
/// \code
/// func _taskLocalValueGet<Key>(AsyncTask* task)
/// \endcode
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_localsCopyTo(AsyncTask* target);

/// This should have the same representation as an enum like this:
///    enum NearestTaskDeadline {
///      case none
///      case alreadyCancelled
///      case active(TaskDeadline)
///    }
/// TODO: decide what this interface should really be.
struct NearestTaskDeadline {
  enum Kind : uint8_t {
    None,
    AlreadyCancelled,
    Active
  };

  TaskDeadline Value;
  Kind ValueKind;
};

/// Returns the nearest deadline that's been registered with this task.
///
/// This must be called synchronously with the task.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
NearestTaskDeadline
swift_task_getNearestDeadline(AsyncTask *task);

/// Run the given async function and block the current thread until
/// it returns.  This is a hack added for testing purposes; eventually
/// top-level code will be an async context, and the need for this in
/// tests should go away.  We *definitely* do not want this to be part
/// of the standard feature set.
///
/// The argument is a `() async -> ()` function, whose ABI is currently
/// quite complex.  Eventually this should use a different convention;
/// that's rdar://72105841.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_runAndBlockThread(const void *function,
                                  HeapObject *functionContext);

/// Switch the current task to a new executor if we aren't already
/// running on a compatible executor.
///
/// The resumption function pointer and continuation should be set
/// appropriately in the task.
///
/// Generally the compiler should inline a fast-path compatible-executor
/// check to avoid doing the suspension work.  This function should
/// generally be tail-called, as it may continue executing the task
/// synchronously if possible.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swiftasync)
void swift_task_switch(SWIFT_ASYNC_CONTEXT AsyncContext *resumeToContext,
                       TaskContinuationFunction *resumeFunction,
                       ExecutorRef newExecutor);

/// Enqueue the given job to run asynchronously on the given executor.
///
/// The resumption function pointer and continuation should be set
/// appropriately in the task.
///
/// Generally you should call swift_task_switch to switch execution
/// synchronously when possible.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_enqueue(Job *job, ExecutorRef executor);

/// Enqueue the given job to run asynchronously on the global
/// execution pool.
///
/// The resumption function pointer and continuation should be set
/// appropriately in the task.
///
/// Generally you should call swift_task_switch to switch execution
/// synchronously when possible.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_enqueueGlobal(Job *job);

SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_enqueueGlobalWithDelay(unsigned long long delay, Job *job);

/// Enqueue the given job on the main executor.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_enqueueMainExecutor(Job *job);

/// Enqueue the given job on the main executor.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_enqueueOnDispatchQueue(Job *job, HeapObject *queue);

/// A hook to take over global enqueuing.
typedef SWIFT_CC(swift) void (*swift_task_enqueueGlobal_original)(Job *job);
SWIFT_EXPORT_FROM(swift_Concurrency)
SWIFT_CC(swift) void (*swift_task_enqueueGlobal_hook)(
    Job *job, swift_task_enqueueGlobal_original original);

/// A hook to take over global enqueuing with delay.
typedef SWIFT_CC(swift) void (*swift_task_enqueueGlobalWithDelay_original)(
    unsigned long long delay, Job *job);
SWIFT_EXPORT_FROM(swift_Concurrency)
SWIFT_CC(swift) void (*swift_task_enqueueGlobalWithDelay_hook)(
    unsigned long long delay, Job *job,
    swift_task_enqueueGlobalWithDelay_original original);

/// A hook to take over main executor enqueueing.
typedef SWIFT_CC(swift) void (*swift_task_enqueueMainExecutor_original)(
    Job *job);
SWIFT_EXPORT_FROM(swift_Concurrency)
SWIFT_CC(swift) void (*swift_task_enqueueMainExecutor_hook)(
    Job *job, swift_task_enqueueMainExecutor_original original);

/// Initialize the runtime storage for a default actor.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_defaultActor_initialize(DefaultActor *actor);

/// Destroy the runtime storage for a default actor.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_defaultActor_destroy(DefaultActor *actor);

/// Deallocate an instance of a default actor.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_defaultActor_deallocate(DefaultActor *actor);

/// Deallocate an instance of what might be a default actor.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_defaultActor_deallocateResilient(HeapObject *actor);

/// Initialize the runtime storage for a distributed remote actor.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_distributedActor_remote_initialize(DefaultActor *actor);

/// Destroy the runtime storage for a default actor.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_distributedActor_destroy(DefaultActor *actor);

/// Enqueue a job on the default actor implementation.
///
/// The job must be ready to run.  Notably, if it's a task, that
/// means that the resumption function and context should have been
/// set appropriately.
///
/// Jobs are assumed to be "self-consuming": once it starts running,
/// the job memory is invalidated and the executor should not access it
/// again.
///
/// Jobs are generally expected to keep the actor alive during their
/// execution.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_defaultActor_enqueue(Job *job, DefaultActor *actor);

/// Check if the actor is a distributed 'remote' actor instance.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
bool swift_distributed_actor_is_remote(DefaultActor *actor);

/// Prepare a continuation in the current task.
///
/// The caller should initialize the Parent, ResumeParent,
/// and NormalResult fields.  This function will initialize the other
/// fields with appropriate defaaults; the caller may then overwrite
/// them if desired.
///
/// This function is provided as a code-size and runtime-usage
/// optimization; calling it is not required if code is willing to
/// do all its work inline.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
AsyncTask *swift_continuation_init(ContinuationAsyncContext *context,
                                   AsyncContinuationFlags flags);

/// Resume a task from a non-throwing continuation, given a normal
/// result which has already been stored into the continuation.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_continuation_resume(AsyncTask *continuation);

/// Resume a task from a potentially-throwing continuation, given a
/// normal result which has already been stored into the continuation.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_continuation_throwingResume(AsyncTask *continuation);

/// Resume a task from a potentially-throwing continuation by throwing
/// an error.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_continuation_throwingResumeWithError(AsyncTask *continuation,
                                                /* +1 */ SwiftError *error);

/// SPI helper to log a misuse of a `CheckedContinuation` to the appropriate places in the OS.
extern "C" SWIFT_CC(swift)
void swift_continuation_logFailedCheck(const char *message);

/// Drain the queue
/// If the binary links CoreFoundation, uses CFRunLoopRun
/// Otherwise it uses dispatchMain.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_asyncMainDrainQueue [[noreturn]]();

/// Establish that the current thread is running as the given
/// executor, then run a job.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_job_run(Job *job, ExecutorRef executor);

/// Return the current thread's active task reference.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
AsyncTask *swift_task_getCurrent(void);

/// Return the current thread's active executor reference.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
ExecutorRef swift_task_getCurrentExecutor(void);

/// Return the main-actor executor reference.
SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
ExecutorRef swift_task_getMainExecutor(void);

SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
bool swift_task_isCurrentExecutor(ExecutorRef executor);

SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
void swift_task_reportUnexpectedExecutor(
    const unsigned char *file, uintptr_t fileLength, bool fileIsASCII,
    uintptr_t line, ExecutorRef executor);

SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(swift)
JobPriority swift_task_getCurrentThreadPriority(void);

}

#pragma clang diagnostic pop

#endif
