//===--- Task.cpp - Task object and management ----------------------------===//
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
// Object management routines for asynchronous task objects.
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "../CompatibilityOverride/CompatibilityOverride.h"
#include "Debug.h"
#include "Error.h"
#include "TaskGroupPrivate.h"
#include "TaskPrivate.h"
#include "Tracing.h"
#include "swift/ABI/Metadata.h"
#include "swift/ABI/Task.h"
#include "swift/ABI/TaskLocal.h"
#include "swift/ABI/TaskOptions.h"
#include "swift/Basic/Lazy.h"
#include "swift/Runtime/Concurrency.h"
#include "swift/Runtime/EnvironmentVariables.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Threading/Mutex.h"
#include <atomic>
#include <new>
#include <unordered_set>

#if SWIFT_CONCURRENCY_ENABLE_DISPATCH
#include <dispatch/dispatch.h>
#endif

#if !defined(_WIN32) && !defined(__wasi__) && __has_include(<dlfcn.h>)
#include <dlfcn.h>
#endif

#if defined(SWIFT_CONCURRENCY_BACK_DEPLOYMENT)
#include <Availability.h>
#include <TargetConditionals.h>
#if TARGET_OS_WATCH
// Bitcode compilation for the watch device precludes defining the following asm
// symbols, so we don't use them... but simulators are okay.
#if TARGET_OS_SIMULATOR
asm("\n .globl _swift_async_extendedFramePointerFlags" \
    "\n _swift_async_extendedFramePointerFlags = 0x0");
#endif
#else
asm("\n .globl _swift_async_extendedFramePointerFlags" \
    "\n _swift_async_extendedFramePointerFlags = 0x0");
#endif
#else
#ifdef __APPLE__
#if __POINTER_WIDTH__ == 64
asm("\n .globl _swift_async_extendedFramePointerFlags" \
    "\n _swift_async_extendedFramePointerFlags = 0x1000000000000000");
#elif __ARM64_ARCH_8_32__
asm("\n .globl _swift_async_extendedFramePointerFlags" \
    "\n _swift_async_extendedFramePointerFlags = 0x10000000");
#else
asm("\n .globl _swift_async_extendedFramePointerFlags" \
    "\n _swift_async_extendedFramePointerFlags = 0x0");
#endif
#endif // __APPLE__
#endif // !defined(SWIFT_CONCURRENCY_BACK_DEPLOYMENT)

using namespace swift;
using FutureFragment = AsyncTask::FutureFragment;
using TaskGroup = swift::TaskGroup;

Metadata swift::TaskAllocatorSlabMetadata;
const void *const swift::_swift_concurrency_debug_asyncTaskSlabMetadata =
    &TaskAllocatorSlabMetadata;

bool swift::_swift_concurrency_debug_supportsPriorityEscalation =
    SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION;

void FutureFragment::destroy() {
  auto queueHead = waitQueue.load(std::memory_order_acquire);
  switch (queueHead.getStatus()) {
  case Status::Executing:
    swift_unreachable("destroying a task that never completed");

  case Status::Success:
    resultType->vw_destroy(getStoragePtr());
    break;

  case Status::Error:
    swift_errorRelease(getError());
    break;
  }
}

FutureFragment::Status AsyncTask::waitFuture(AsyncTask *waitingTask,
                                             AsyncContext *waitingTaskContext,
                                             TaskContinuationFunction *resumeFn,
                                             AsyncContext *callerContext,
                                             OpaqueValue *result) {
  using Status = FutureFragment::Status;
  using WaitQueueItem = FutureFragment::WaitQueueItem;

  assert(isFuture());
  auto fragment = futureFragment();

  // NOTE: this acquire synchronizes with `completeFuture`.
  auto queueHead = fragment->waitQueue.load(std::memory_order_acquire);
  bool contextInitialized = false;
  while (true) {
    switch (queueHead.getStatus()) {
    case Status::Error:
    case Status::Success:
      SWIFT_TASK_DEBUG_LOG("task %p waiting on task %p, completed immediately",
                           waitingTask, this);
      _swift_tsan_acquire(static_cast<Job *>(this));
      if (contextInitialized) waitingTask->flagAsRunning();
      // The task is done; we don't need to wait.
      return queueHead.getStatus();

    case Status::Executing:
      SWIFT_TASK_DEBUG_LOG("task %p waiting on task %p, going to sleep",
                           waitingTask, this);
      _swift_tsan_release(static_cast<Job *>(waitingTask));
      concurrency::trace::task_wait(
          waitingTask, this, static_cast<uintptr_t>(queueHead.getStatus()));
      // Task is not complete. We'll need to add ourselves to the queue.
      break;
    }

    if (!contextInitialized) {
      contextInitialized = true;
      auto context =
          reinterpret_cast<TaskFutureWaitAsyncContext *>(waitingTaskContext);
      context->errorResult = nullptr;
      context->successResultPointer = result;
      context->ResumeParent = resumeFn;
      context->Parent = callerContext;
      waitingTask->flagAsSuspendedOnTask(this);
    }

#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
    // In the task to thread model, we will execute the task that we are waiting
    // on, on the current thread itself. As a result, do not bother adding the
    // waitingTask to any thread queue. Instead, we will clear the old task, run
    // the new one and then reattempt to continue running the old task

    auto oldTask = _swift_task_clearCurrent();
    assert(oldTask == waitingTask);

    SWIFT_TASK_DEBUG_LOG("[RunInline] Switching away from running %p to now running %p", oldTask, this);
    // Run the new task on the same thread now - this should run the new task to
    // completion. All swift tasks in task-to-thread model run on generic
    // executor
    swift_job_run(this, ExecutorRef::generic());

    SWIFT_TASK_DEBUG_LOG("[RunInline] Switching back from running %p to now running %p", this, oldTask);

    // We now are back in the context of the waiting task and need to reevaluate
    // our state
    _swift_task_setCurrent(oldTask);
    queueHead = fragment->waitQueue.load(std::memory_order_acquire);
    continue;
#else
    // Put the waiting task at the beginning of the wait queue.
    // NOTE: this acquire-release synchronizes with `completeFuture`.
    waitingTask->getNextWaitingTask() = queueHead.getTask();
    auto newQueueHead = WaitQueueItem::get(Status::Executing, waitingTask);
    if (fragment->waitQueue.compare_exchange_weak(
            queueHead, newQueueHead,
            /*success*/ std::memory_order_release,
            /*failure*/ std::memory_order_acquire)) {

      _swift_task_clearCurrent();
      return FutureFragment::Status::Executing;
    }
#endif /* SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL */
  }
}

void NullaryContinuationJob::process(Job *_job) {
  auto *job = cast<NullaryContinuationJob>(_job);

  auto *continuation = job->Continuation;

  delete job;

  auto *context =
    static_cast<ContinuationAsyncContext*>(continuation->ResumeContext);

  context->setErrorResult(nullptr);
  swift_continuation_resume(continuation);
}

void AsyncTask::completeFuture(AsyncContext *context) {
  using Status = FutureFragment::Status;
  using WaitQueueItem = FutureFragment::WaitQueueItem;
  SWIFT_TASK_DEBUG_LOG("complete future = %p", this);
  assert(isFuture());
  auto fragment = futureFragment();

  // If an error was thrown, save it in the future fragment.
  auto asyncContextPrefix = reinterpret_cast<FutureAsyncContextPrefix *>(
      reinterpret_cast<char *>(context) - sizeof(FutureAsyncContextPrefix));
  bool hadErrorResult = false;
  auto errorObject = asyncContextPrefix->errorResult;
  fragment->getError() = errorObject;
  if (errorObject) {
    hadErrorResult = true;
  }

  _swift_tsan_release(static_cast<Job *>(this));

  // Update the status to signal completion.
  auto newQueueHead = WaitQueueItem::get(
    hadErrorResult ? Status::Error : Status::Success,
    nullptr
  );

  // NOTE: this acquire-release synchronizes with `waitFuture`.
  auto queueHead = fragment->waitQueue.exchange(
      newQueueHead, std::memory_order_acq_rel);
  assert(queueHead.getStatus() == Status::Executing);

  // If this is task group child, notify the parent group about the completion.
  if (hasGroupChildFragment()) {
    // then we must offer into the parent group that we completed,
    // so it may `next()` poll completed child tasks in completion order.
    auto group = groupChildFragment()->getGroup();
    group->offer(this, context);
  }

  // Schedule every waiting task on the executor.
  auto waitingTask = queueHead.getTask();

  if (!waitingTask) {
    SWIFT_TASK_DEBUG_LOG("task %p had no waiting tasks", this);
  } else {
#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
    assert(false && "Task should have no waiters in task-to-thread model");
#endif
  }

  while (waitingTask) {
    // Find the next waiting task before we invalidate it by resuming
    // the task.
    auto nextWaitingTask = waitingTask->getNextWaitingTask();

    SWIFT_TASK_DEBUG_LOG("waking task %p from future of task %p", waitingTask,
                         this);

    // Fill in the return context.
    auto waitingContext =
      static_cast<TaskFutureWaitAsyncContext *>(waitingTask->ResumeContext);
    if (hadErrorResult) {
      waitingContext->fillWithError(fragment);
    } else {
      waitingContext->fillWithSuccess(fragment);
    }

    _swift_tsan_acquire(static_cast<Job *>(waitingTask));

    concurrency::trace::task_resume(waitingTask);

    // Enqueue the waiter on the global executor.
    // TODO: allow waiters to fill in a suggested executor
    waitingTask->flagAsAndEnqueueOnExecutor(ExecutorRef::generic());

    // Move to the next task.
    waitingTask = nextWaitingTask;
  }
}

SWIFT_CC(swift)
static void destroyJob(SWIFT_CONTEXT HeapObject *obj) {
  assert(false && "A non-task job should never be destroyed as heap metadata.");
}

AsyncTask::~AsyncTask() {
  flagAsDestroyed();

  // For a future, destroy the result.
  if (isFuture()) {
    futureFragment()->destroy();
  }

  Private.destroy();

  concurrency::trace::task_destroy(this);
}

void AsyncTask::setTaskId() {
  static std::atomic<uint64_t> NextId(1);

  // We want the 32-bit Job::Id to be non-zero, so loop if we happen upon zero.
  uint64_t Fetched;
  do {
    Fetched = NextId.fetch_add(1, std::memory_order_relaxed);
    Id = Fetched & 0xffffffff;
  } while (Id == 0);

  _private().Id = (Fetched >> 32) & 0xffffffff;
}

uint64_t AsyncTask::getTaskId() {
  // Reconstitute a full 64-bit task ID from the 32-bit job ID and the upper
  // 32 bits held in _private().
  return (uint64_t)Id << _private().Id;
}

SWIFT_CC(swift)
static void destroyTask(SWIFT_CONTEXT HeapObject *obj) {
  auto task = static_cast<AsyncTask*>(obj);
  task->~AsyncTask();

  // The task execution itself should always hold a reference to it, so
  // if we get here, we know the task has finished running, which means
  // swift_task_complete should have been run, which will have torn down
  // the task-local allocator.  There's actually nothing else to clean up
  // here.

  SWIFT_TASK_DEBUG_LOG("Destroyed task %p", task);
  free(task);
}

static ExecutorRef executorForEnqueuedJob(Job *job) {
#if !SWIFT_CONCURRENCY_ENABLE_DISPATCH
  return ExecutorRef::generic();
#else
  void *jobQueue = job->SchedulerPrivate[Job::DispatchQueueIndex];
  if (jobQueue == DISPATCH_QUEUE_GLOBAL_EXECUTOR)
    return ExecutorRef::generic();
  else
    return ExecutorRef::forOrdinary(reinterpret_cast<HeapObject*>(jobQueue),
                    _swift_task_getDispatchQueueSerialExecutorWitnessTable());
#endif
}

static void jobInvoke(void *obj, void *unused, uint32_t flags) {
  (void)unused;
  Job *job = reinterpret_cast<Job *>(obj);

  swift_job_run(job, executorForEnqueuedJob(job));
}

// Magic constant to identify Swift Job vtables to Dispatch.
static const unsigned long dispatchSwiftObjectType = 1;

FullMetadata<DispatchClassMetadata> swift::jobHeapMetadata = {
  {
    {
      /*type layout*/ nullptr,
    },
    {
      &destroyJob
    },
    {
      /*value witness table*/ nullptr
    }
  },
  {
    MetadataKind::Job,
    dispatchSwiftObjectType,
    jobInvoke
  }
};

/// Heap metadata for an asynchronous task.
static FullMetadata<DispatchClassMetadata> taskHeapMetadata = {
  {
    {
      /*type layout*/ nullptr
    },
    {
      &destroyTask
    },
    {
      /*value witness table*/ nullptr
    }
  },
  {
    MetadataKind::Task,
    dispatchSwiftObjectType,
    jobInvoke
  }
};

const void *const swift::_swift_concurrency_debug_jobMetadata =
    static_cast<Metadata *>(&jobHeapMetadata);
const void *const swift::_swift_concurrency_debug_asyncTaskMetadata =
    static_cast<Metadata *>(&taskHeapMetadata);

static void completeTaskImpl(AsyncTask *task,
                             AsyncContext *context,
                             SwiftError *error) {
  assert(task && "completing task, but there is no active task registered");

  // Store the error result.
  auto asyncContextPrefix = reinterpret_cast<AsyncContextPrefix *>(
      reinterpret_cast<char *>(context) - sizeof(AsyncContextPrefix));
  asyncContextPrefix->errorResult = error;

  task->Private.complete(task);

  SWIFT_TASK_DEBUG_LOG("task %p completed", task);

  // Complete the future.
  // Warning: This deallocates the task in case it's an async let task.
  // The task must not be accessed afterwards.
  if (task->isFuture()) {
    task->completeFuture(context);
  }

  // TODO: set something in the status?
  // if (task->hasChildFragment()) {
    // TODO: notify the parent somehow?
    // TODO: remove this task from the child-task chain?
  // }
}

/// The function that we put in the context of a simple task
/// to handle the final return.
SWIFT_CC(swiftasync)
static void completeTask(SWIFT_ASYNC_CONTEXT AsyncContext *context,
                         SWIFT_CONTEXT SwiftError *error) {
  // Set that there's no longer a running task in the current thread.
  auto task = _swift_task_clearCurrent();
  assert(task && "completing task, but there is no active task registered");

  completeTaskImpl(task, context, error);
}

/// The function that we put in the context of a simple task
/// to handle the final return.
SWIFT_CC(swiftasync)
static void completeTaskAndRelease(SWIFT_ASYNC_CONTEXT AsyncContext *context,
                                   SWIFT_CONTEXT SwiftError *error) {
  // Set that there's no longer a running task in the current thread.
  auto task = _swift_task_clearCurrent();
  assert(task && "completing task, but there is no active task registered");

  completeTaskImpl(task, context, error);

  // Release the task, balancing the retain that a running task has on itself.
  // If it was a group child task, it will remain until the group returns it.
  swift_release(task);
}

/// The function that we put in the context of a simple task
/// to handle the final return from a closure.
SWIFT_CC(swiftasync)
static void completeTaskWithClosure(SWIFT_ASYNC_CONTEXT AsyncContext *context,
                                    SWIFT_CONTEXT SwiftError *error) {
  // Release the closure context.
  auto asyncContextPrefix = reinterpret_cast<AsyncContextPrefix *>(
      reinterpret_cast<char *>(context) - sizeof(AsyncContextPrefix));

  swift_release((HeapObject *)asyncContextPrefix->closureContext);

  // Clean up the rest of the task.
  return completeTaskAndRelease(context, error);
}

/// The function that we put in the context of an inline task to handle the
/// final return.
///
/// Because inline tasks can't produce errors, this function doesn't have an
/// error parameter.
///
/// Because inline tasks' closures are noescaping, their closure contexts are
/// stack allocated; so this function doesn't release them.
SWIFT_CC(swiftasync)
static void completeInlineTask(SWIFT_ASYNC_CONTEXT AsyncContext *context) {
  // Set that there's no longer a running task in the current thread.
  auto task = _swift_task_clearCurrent();
  assert(task && "completing task, but there is no active task registered");

  completeTaskImpl(task, context, /*error=*/nullptr);
}

SWIFT_CC(swiftasync)
static void non_future_adapter(SWIFT_ASYNC_CONTEXT AsyncContext *_context) {
  auto asyncContextPrefix = reinterpret_cast<AsyncContextPrefix *>(
      reinterpret_cast<char *>(_context) - sizeof(AsyncContextPrefix));
  return asyncContextPrefix->asyncEntryPoint(
      _context, asyncContextPrefix->closureContext);
}

SWIFT_CC(swiftasync)
static void future_adapter(SWIFT_ASYNC_CONTEXT AsyncContext *_context) {
  auto asyncContextPrefix = reinterpret_cast<FutureAsyncContextPrefix *>(
      reinterpret_cast<char *>(_context) - sizeof(FutureAsyncContextPrefix));
  return asyncContextPrefix->asyncEntryPoint(
      asyncContextPrefix->indirectResult, _context,
      asyncContextPrefix->closureContext);
}

SWIFT_CC(swiftasync)
static void task_wait_throwing_resume_adapter(SWIFT_ASYNC_CONTEXT AsyncContext *_context) {

  auto context = static_cast<TaskFutureWaitAsyncContext *>(_context);
  auto resumeWithError =
      reinterpret_cast<AsyncVoidClosureEntryPoint *>(context->ResumeParent);
  return resumeWithError(context->Parent, context->errorResult);
}

SWIFT_CC(swiftasync)
static void
task_future_wait_resume_adapter(SWIFT_ASYNC_CONTEXT AsyncContext *_context) {
  return _context->ResumeParent(_context->Parent);
}

const void *const swift::_swift_concurrency_debug_non_future_adapter =
    reinterpret_cast<void *>(non_future_adapter);
const void *const swift::_swift_concurrency_debug_future_adapter =
    reinterpret_cast<void *>(future_adapter);
const void
    *const swift::_swift_concurrency_debug_task_wait_throwing_resume_adapter =
        reinterpret_cast<void *>(task_wait_throwing_resume_adapter);
const void
    *const swift::_swift_concurrency_debug_task_future_wait_resume_adapter =
        reinterpret_cast<void *>(task_future_wait_resume_adapter);

const void *AsyncTask::getResumeFunctionForLogging() {
  const void *result = reinterpret_cast<const void *>(ResumeTask);

  if (ResumeTask == non_future_adapter) {
    auto asyncContextPrefix = reinterpret_cast<AsyncContextPrefix *>(
        reinterpret_cast<char *>(ResumeContext) - sizeof(AsyncContextPrefix));
    result =
        reinterpret_cast<const void *>(asyncContextPrefix->asyncEntryPoint);
  } else if (ResumeTask == future_adapter) {
    auto asyncContextPrefix = reinterpret_cast<FutureAsyncContextPrefix *>(
        reinterpret_cast<char *>(ResumeContext) -
        sizeof(FutureAsyncContextPrefix));
    result =
        reinterpret_cast<const void *>(asyncContextPrefix->asyncEntryPoint);
  } else if (ResumeTask == task_wait_throwing_resume_adapter) {
    auto context = static_cast<TaskFutureWaitAsyncContext *>(ResumeContext);
    result = reinterpret_cast<const void *>(context->ResumeParent);
  } else if (ResumeTask == task_future_wait_resume_adapter) {
    result = reinterpret_cast<const void *>(ResumeContext->ResumeParent);
  }

  return __ptrauth_swift_runtime_function_entry_strip(result);
}

JobPriority swift::swift_task_currentPriority(AsyncTask *task) {
  // This is racey but this is to be used in an API is inherently racey anyways.
  auto oldStatus = task->_private()._status().load(std::memory_order_relaxed);
  return oldStatus.getStoredPriority();
}

JobPriority swift::swift_task_basePriority(AsyncTask *task) {
  JobPriority pri = task->_private().BasePriority;
  SWIFT_TASK_DEBUG_LOG("Task %p has base priority = %zu", task, pri);
  return pri;
}

JobPriority swift::swift_concurrency_jobPriority(Job *job) {
  return job->getPriority();
}

static inline bool isUnspecified(JobPriority priority) {
  return priority == JobPriority::Unspecified;
}

static inline bool taskIsStructured(JobFlags jobFlags) {
  return jobFlags.task_isAsyncLetTask() || jobFlags.task_isGroupChildTask();
}

static inline bool taskIsUnstructured(TaskCreateFlags createFlags, JobFlags jobFlags) {
  return !taskIsStructured(jobFlags) && !createFlags.isInlineTask();
}

static inline bool taskIsDetached(TaskCreateFlags createFlags, JobFlags jobFlags) {
  return taskIsUnstructured(createFlags, jobFlags) && !createFlags.copyTaskLocals();
}

static std::pair<size_t, size_t> amountToAllocateForHeaderAndTask(
    const AsyncTask *parent, const TaskGroup *group,
    const Metadata *futureResultType, size_t initialContextSize) {
  // Figure out the size of the header.
  size_t headerSize = sizeof(AsyncTask);
  if (parent) {
    headerSize += sizeof(AsyncTask::ChildFragment);
  }
  if (group) {
    headerSize += sizeof(AsyncTask::GroupChildFragment);
  }
  if (futureResultType) {
    headerSize += FutureFragment::fragmentSize(headerSize, futureResultType);
    // Add the future async context prefix.
    headerSize += sizeof(FutureAsyncContextPrefix);
  } else {
    // Add the async context prefix.
    headerSize += sizeof(AsyncContextPrefix);
  }

  headerSize = llvm::alignTo(headerSize, llvm::Align(alignof(AsyncContext)));
  // Allocate the initial context together with the job.
  // This means that we never get rid of this allocation.
  size_t amountToAllocate = headerSize + initialContextSize;

  assert(amountToAllocate % MaximumAlignment == 0);

  return {headerSize, amountToAllocate};
}

/// Implementation of task creation.
SWIFT_CC(swift)
static AsyncTaskAndContext swift_task_create_commonImpl(
    size_t rawTaskCreateFlags,
    TaskOptionRecord *options,
    const Metadata *futureResultType,
    TaskContinuationFunction *function, void *closureContext,
    size_t initialContextSize) {

  TaskCreateFlags taskCreateFlags(rawTaskCreateFlags);
  JobFlags jobFlags(JobKind::Task, JobPriority::Unspecified);

  // Propagate task-creation flags to job flags as appropriate.
  jobFlags.task_setIsChildTask(taskCreateFlags.isChildTask());
  if (futureResultType) {
    jobFlags.task_setIsFuture(true);
    assert(initialContextSize >= sizeof(FutureAsyncContext));
  }

  // Collect the options we know about.
  ExecutorRef executor = ExecutorRef::generic();
  TaskGroup *group = nullptr;
  AsyncLet *asyncLet = nullptr;
  bool hasAsyncLetResultBuffer = false;
  RunInlineTaskOptionRecord *runInlineOption = nullptr;
  for (auto option = options; option; option = option->getParent()) {
    switch (option->getKind()) {
    case TaskOptionRecordKind::Executor:
      executor = cast<ExecutorTaskOptionRecord>(option)->getExecutor();
      break;

    case TaskOptionRecordKind::TaskGroup:
      group = cast<TaskGroupTaskOptionRecord>(option)->getGroup();
      assert(group && "Missing group");
      jobFlags.task_setIsGroupChildTask(true);
      break;

    case TaskOptionRecordKind::AsyncLet:
      asyncLet = cast<AsyncLetTaskOptionRecord>(option)->getAsyncLet();
      assert(asyncLet && "Missing async let storage");
      jobFlags.task_setIsAsyncLetTask(true);
      jobFlags.task_setIsChildTask(true);
      break;

    case TaskOptionRecordKind::AsyncLetWithBuffer: {
      auto *aletRecord = cast<AsyncLetWithBufferTaskOptionRecord>(option);
      asyncLet = aletRecord->getAsyncLet();
      // TODO: Actually digest the result buffer into the async let task
      // context, so that we can emplace the eventual result there instead
      // of in a FutureFragment.
      hasAsyncLetResultBuffer = true;
      assert(asyncLet && "Missing async let storage");

      jobFlags.task_setIsAsyncLetTask(true);
      jobFlags.task_setIsChildTask(true);
      break;
    }
    case TaskOptionRecordKind::RunInline: {
      runInlineOption = cast<RunInlineTaskOptionRecord>(option);
      // TODO (rokhinip): We seem to be creating runInline tasks like detached
      // tasks but they need to maintain the voucher and priority of calling
      // thread and therefore need to behave a bit more like SC child tasks.
      break;
    }
    }
  }

  // Add to the task group, if requested.
  if (taskCreateFlags.addPendingGroupTaskUnconditionally()) {
    assert(group && "Missing group");
    swift_taskGroup_addPending(group, /*unconditionally=*/true);
  }

  AsyncTask *parent = nullptr;
  AsyncTask *currentTask = swift_task_getCurrent();
  if (jobFlags.task_isChildTask()) {
    parent = currentTask;
    assert(parent != nullptr && "creating a child task with no active task");
  }

  // Start with user specified priority at creation time (if any)
  JobPriority basePriority = (taskCreateFlags.getRequestedPriority());

  if (taskCreateFlags.isInlineTask()) {
     SWIFT_TASK_DEBUG_LOG("Creating an inline task from %p", currentTask);

     // We'll take the current priority and set it as base and escalated
     // priority of the task. No UI->IN downgrade needed.
     basePriority = swift_task_getCurrentThreadPriority();

  } else if (taskIsDetached(taskCreateFlags, jobFlags)) {
     SWIFT_TASK_DEBUG_LOG("Creating a detached task from %p", currentTask);
     // Case 1: No priority specified
     //    Base priority = UN
     //    Escalated priority = UN
     // Case 2: Priority specified
     //    Base priority = user specified priority
     //    Escalated priority = UN
     //
     // Task will be created with max priority = max(base priority, UN) = base
     // priority. We shouldn't need to do any additional manipulations here since
     // basePriority should already be the right value

  } else if (taskIsUnstructured(taskCreateFlags, jobFlags)) {
     SWIFT_TASK_DEBUG_LOG("Creating an unstructured task from %p", currentTask);

    if (isUnspecified(basePriority)) {
      // Case 1: No priority specified
      //    Base priority = Base priority of parent with a UI -> IN downgrade
      //    Escalated priority = UN
      if (currentTask) {
        basePriority = currentTask->_private().BasePriority;
      } else {
        basePriority = swift_task_getCurrentThreadPriority();
      }
      basePriority = withUserInteractivePriorityDowngrade(basePriority);
    } else {
      // Case 2: User specified a priority
      //    Base priority = user specified priority
      //    Escalated priority = UN
    }

    // Task will be created with max priority = max(base priority, UN) = base
    // priority
  } else {
    // Is a structured concurrency child task. Must have a parent.
    assert((asyncLet || group) && parent);
    SWIFT_TASK_DEBUG_LOG("Creating an structured concurrency task from %p", currentTask);

    if (isUnspecified(basePriority)) {
      // Case 1: No priority specified
      //    Base priority = Base priority of parent with a UI -> IN downgrade
      //    Escalated priority = Escalated priority of parent with a UI -> IN
      //    downgrade
      JobPriority parentBasePri = parent->_private().BasePriority;
      basePriority = withUserInteractivePriorityDowngrade(parentBasePri);
    } else {
      // Case 2: User priority specified
      //    Base priority = User specified priority
      //    Escalated priority = Escalated priority of parent with a UI -> IN
      //    downgrade
    }

    // Task will be created with escalated priority = base priority. We will
    // update the escalated priority with the right rules in
    // updateNewChildWithParentAndGroupState when we link the child into
    // the parent task/task group since we'll have the right
    // synchronization then.
  }

  if (isUnspecified(basePriority)) {
     basePriority = JobPriority::Default;
  }

  SWIFT_TASK_DEBUG_LOG("Task's base priority = %#zx", basePriority);

  size_t headerSize, amountToAllocate;
  std::tie(headerSize, amountToAllocate) = amountToAllocateForHeaderAndTask(
      parent, group, futureResultType, initialContextSize);

  unsigned initialSlabSize = 512;

  void *allocation = nullptr;
  if (asyncLet) {
    assert(parent);

    // If there isn't enough room in the fixed async let allocation to
    // set up the initial context, then we'll have to allocate more space
    // from the parent.
    if (asyncLet->getSizeOfPreallocatedSpace() < amountToAllocate) {
      hasAsyncLetResultBuffer = false;
    }

    // DEPRECATED. This is separated from the above condition because we
    // also have to handle an older async let ABI that did not provide
    // space for the initial slab in the compiler-generated preallocation.
    if (!hasAsyncLetResultBuffer) {
      allocation = _swift_task_alloc_specific(parent,
                                          amountToAllocate + initialSlabSize);
    } else {
      allocation = asyncLet->getPreallocatedSpace();
      assert(asyncLet->getSizeOfPreallocatedSpace() >= amountToAllocate
             && "async let does not preallocate enough space for child task");
      initialSlabSize = asyncLet->getSizeOfPreallocatedSpace()
                          - amountToAllocate;
    }
  } else if (runInlineOption && runInlineOption->getAllocation()) {
    // NOTE: If the space required for the task and initial context was
    //       greater than SWIFT_TASK_RUN_INLINE_INITIAL_CONTEXT_BYTES,
    //       getAllocation will return nullptr and we'll fall back to malloc to
    //       allocate the buffer.
    //
    // This was already checked in swift_task_run_inline.
    size_t runInlineBufferBytes = runInlineOption->getAllocationBytes();
    assert(amountToAllocate <= runInlineBufferBytes);
    allocation = runInlineOption->getAllocation();
    initialSlabSize = runInlineBufferBytes - amountToAllocate;
  } else {
    allocation = malloc(amountToAllocate);
  }
  SWIFT_TASK_DEBUG_LOG("allocate task %p, parent = %p, slab %u", allocation,
                       parent, initialSlabSize);

  AsyncContext *initialContext =
    reinterpret_cast<AsyncContext*>(
      reinterpret_cast<char*>(allocation) + headerSize);

  //  We can't just use `function` because it uses the new async function entry
  //  ABI -- passing parameters, closure context, indirect result addresses
  //  directly -- but AsyncTask->ResumeTask expects the signature to be
  //  `void (*, *, swiftasync *)`.
  //  Instead we use an adapter. This adaptor should use the storage prefixed to
  //  the async context to get at the parameters.
  //  See e.g. FutureAsyncContextPrefix.

  if (!futureResultType || taskCreateFlags.isDiscardingTask()) {
    auto asyncContextPrefix = reinterpret_cast<AsyncContextPrefix *>(
        reinterpret_cast<char *>(allocation) + headerSize -
        sizeof(AsyncContextPrefix));
    asyncContextPrefix->asyncEntryPoint =
        reinterpret_cast<AsyncVoidClosureEntryPoint *>(function);
    asyncContextPrefix->closureContext = closureContext;
    function = non_future_adapter;
    assert(sizeof(AsyncContextPrefix) == 3 * sizeof(void *));
  } else {
    auto asyncContextPrefix = reinterpret_cast<FutureAsyncContextPrefix *>(
        reinterpret_cast<char *>(allocation) + headerSize -
        sizeof(FutureAsyncContextPrefix));
    asyncContextPrefix->asyncEntryPoint =
        reinterpret_cast<AsyncGenericClosureEntryPoint *>(function);
    function = future_adapter;
    asyncContextPrefix->closureContext = closureContext;
    assert(sizeof(FutureAsyncContextPrefix) == 4 * sizeof(void *));
  }

  // Initialize the task so that resuming it will run the given
  // function on the initial context.
  AsyncTask *task = nullptr;
  bool captureCurrentVoucher = !taskIsDetached(taskCreateFlags, jobFlags);
  if (asyncLet) {
    // Initialize the refcount bits to "immortal", so that
    // ARC operations don't have any effect on the task.
    task = new(allocation) AsyncTask(&taskHeapMetadata,
                             InlineRefCounts::Immortal, jobFlags,
                             function, initialContext,
                             captureCurrentVoucher);
  } else {
    task = new(allocation) AsyncTask(&taskHeapMetadata, jobFlags,
                                    function, initialContext,
                                    captureCurrentVoucher);
  }

  // Initialize the child fragment if applicable.
  if (parent) {
    auto childFragment = task->childFragment();
    ::new (childFragment) AsyncTask::ChildFragment(parent);
  }

  // Initialize the group child fragment if applicable.
  if (group) {
    auto groupChildFragment = task->groupChildFragment();
    ::new (groupChildFragment) AsyncTask::GroupChildFragment(group);
  }

  // Initialize the future fragment if applicable.
  if (futureResultType) {
    assert(task->isFuture());
    auto futureFragment = task->futureFragment();
    ::new (futureFragment) FutureFragment(futureResultType);

    // Set up the context for the future so there is no error, and a successful
    // result will be written into the future fragment's storage.
    auto futureAsyncContextPrefix =
        reinterpret_cast<FutureAsyncContextPrefix *>(
            reinterpret_cast<char *>(allocation) + headerSize -
            sizeof(FutureAsyncContextPrefix));
    futureAsyncContextPrefix->indirectResult = futureFragment->getStoragePtr();
  }

  SWIFT_TASK_DEBUG_LOG("creating task %p ID %" PRIu64
                       " with parent %p at base pri %zu",
                       task, task->getTaskId(), parent, basePriority);

  // Initialize the task-local allocator.
  initialContext->ResumeParent =
      runInlineOption ? &completeInlineTask
                      : reinterpret_cast<TaskContinuationFunction *>(
                            asyncLet         ? &completeTask
                            : closureContext ? &completeTaskWithClosure
                                             : &completeTaskAndRelease);
  if ((asyncLet || (runInlineOption && runInlineOption->getAllocation())) &&
      initialSlabSize > 0) {
    assert(parent || (runInlineOption && runInlineOption->getAllocation()));
    void *initialSlab = (char*)allocation + amountToAllocate;
    task->Private.initializeWithSlab(basePriority, initialSlab,
                                     initialSlabSize);
  } else {
    task->Private.initialize(basePriority);
  }

  // Perform additional linking between parent and child task.
  if (parent) {
    // If the parent was already cancelled, we carry this flag forward to the child.
    //
    // In a task group we would not have allowed the `add` to create a child anymore,
    // however better safe than sorry and `async let` are not expressed as task groups,
    // so they may have been spawned in any case still.
    if (swift_task_isCancelled(parent) ||
        (group && group->isCancelled()))
      swift_task_cancel(task);

    // Initialize task locals with a link to the parent task.
    task->_private().Local.initializeLinkParent(task, parent);
  }

  // Configure the initial context.
  //
  // FIXME: if we store a null pointer here using the standard ABI for
  // signed null pointers, then we'll have to authenticate context pointers
  // as if they might be null, even though the only time they ever might
  // be is the final hop.  Store a signed null instead.
  initialContext->Parent = nullptr;

  // FIXME: add discarding flag
  concurrency::trace::task_create(
      task, parent, group, asyncLet,
      static_cast<uint8_t>(task->Flags.getPriority()),
      task->Flags.task_isChildTask(), task->Flags.task_isFuture(),
      task->Flags.task_isGroupChildTask(), task->Flags.task_isAsyncLetTask());

  // Attach to the group, if needed.
  if (group) {
    swift_taskGroup_attachChild(group, task);
#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
    // We need to take a retain here to keep the child task for the task group
    // alive. In the non-task-to-thread model, we'd always take this retain
    // below since we'd enqueue the child task. But since we're not going to be
    // enqueueing the child task in this model, we need to take this +1 to
    // balance out the release that exists after the task group child task
    // creation
    swift_retain(task);
#endif
  }

  // If we're supposed to copy task locals, do so now.
  if (taskCreateFlags.copyTaskLocals()) {
    swift_task_localsCopyTo(task);
  }

  // Push the async let task status record.
  if (asyncLet) {
    asyncLet_addImpl(task, asyncLet, !hasAsyncLetResultBuffer);
  }

  // If we're supposed to enqueue the task, do so now.
  if (taskCreateFlags.enqueueJob()) {
#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
    assert(false && "Should not be enqueuing tasks in task-to-thread model");
#endif
    swift_retain(task);
    task->flagAsAndEnqueueOnExecutor(executor);
  }

  return {task, initialContext};
}

/// Extract the entry point address and initial context size from an async closure value.
template<typename AsyncSignature, uint16_t AuthDiscriminator>
SWIFT_ALWAYS_INLINE // so this doesn't hang out as a ptrauth gadget
std::pair<typename AsyncSignature::FunctionType *, size_t>
getAsyncClosureEntryPointAndContextSize(void *function) {
  auto fnPtr =
      reinterpret_cast<const AsyncFunctionPointer<AsyncSignature> *>(function);
#if SWIFT_PTRAUTH
  fnPtr = (const AsyncFunctionPointer<AsyncSignature> *)ptrauth_auth_data(
      (void *)fnPtr, ptrauth_key_process_independent_data, AuthDiscriminator);
#endif
  return {reinterpret_cast<typename AsyncSignature::FunctionType *>(
              fnPtr->Function.get()),
          fnPtr->ExpectedContextSize};
}

#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
SWIFT_CC(swift)
void swift::swift_task_run_inline(OpaqueValue *result, void *closureAFP,
                                  OpaqueValue *closureContext,
                                  const Metadata *futureResultType) {
  // Ensure that we're currently in a synchronous context.
  if (swift_task_getCurrent()) {
    swift_Concurrency_fatalError(0, "called runInline within an async context");
  }

  // Unpack the asynchronous function pointer.
  FutureAsyncSignature::FunctionType *closure;
  size_t closureContextSize;
  std::tie(closure, closureContextSize) =
      getAsyncClosureEntryPointAndContextSize<
          FutureAsyncSignature,
          SpecialPointerAuthDiscriminators::AsyncFutureFunction>(closureAFP);

  // If the initial task and initial async frame aren't too big, allocate enough
  // stack space for them and for use as the initial task slab.
  //
  // If they are too big, swift_task_create_common will fall back to malloc.
  size_t candidateAllocationBytes = SWIFT_TASK_RUN_INLINE_INITIAL_CONTEXT_BYTES;
  size_t minimumAllocationSize =
      amountToAllocateForHeaderAndTask(/*parent=*/nullptr, /*group=*/nullptr,
                                       futureResultType, closureContextSize)
          .second;
  void *allocation = nullptr;
  size_t allocationBytes = 0;
  if (minimumAllocationSize <= candidateAllocationBytes) {
    allocationBytes = candidateAllocationBytes;
    allocation = alloca(allocationBytes);
  }

  // Create a task to run the closure.  Pass a RunInlineTaskOptionRecord
  // containing a pointer to the allocation enabling us to provide our stack
  // allocation rather than swift_task_create_common having to malloc it.
  RunInlineTaskOptionRecord option(allocation, allocationBytes);
  size_t taskCreateFlags = 1 << TaskCreateFlags::Task_IsInlineTask;

  auto taskAndContext = swift_task_create_common(
      taskCreateFlags, &option, futureResultType,
      reinterpret_cast<TaskContinuationFunction *>(closure), closureContext,
      /*initialContextSize=*/closureContextSize);

  // Run the task.
  swift_job_run(taskAndContext.Task, ExecutorRef::generic());
  // Under the task-to-thread concurrency model, the task should always have
  // completed by this point.

  // Copy the result out to our caller.
  auto *futureResult = taskAndContext.Task->futureFragment()->getStoragePtr();
  futureResultType->getValueWitnesses()->initializeWithCopy(
      result, futureResult, futureResultType);

  // Destroy the task.
  taskAndContext.Task->~AsyncTask();
}
#endif

SWIFT_CC(swift)
AsyncTaskAndContext swift::swift_task_create(
    size_t rawTaskCreateFlags,
    TaskOptionRecord *options,
    const Metadata *futureResultType,
    void *closureEntry, HeapObject *closureContext) {
  TaskCreateFlags taskCreateFlags(rawTaskCreateFlags);

  if (taskCreateFlags.isDiscardingTask()) {
    ThinNullaryAsyncSignature::FunctionType *taskEntry;
    size_t initialContextSize;

    std::tie(taskEntry, initialContextSize) =
      getAsyncClosureEntryPointAndContextSize<
        ThinNullaryAsyncSignature,
        SpecialPointerAuthDiscriminators::AsyncThinNullaryFunction>(closureEntry);

    return swift_task_create_common(
        rawTaskCreateFlags, options, futureResultType,
        reinterpret_cast<TaskContinuationFunction *>(taskEntry), closureContext,
        initialContextSize);

  } else {
    FutureAsyncSignature::FunctionType *taskEntry;
    size_t initialContextSize;

    std::tie(taskEntry, initialContextSize) =
        getAsyncClosureEntryPointAndContextSize<
            FutureAsyncSignature,
            SpecialPointerAuthDiscriminators::AsyncFutureFunction>(closureEntry);

    return swift_task_create_common(
        rawTaskCreateFlags, options, futureResultType,
        reinterpret_cast<TaskContinuationFunction *>(taskEntry), closureContext,
        initialContextSize);
  }
}

#ifdef __ARM_ARCH_7K__
__attribute__((noinline))
SWIFT_CC(swiftasync) static void workaround_function_swift_task_future_waitImpl(
    OpaqueValue *result, SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
    AsyncTask *task, TaskContinuationFunction resumeFunction,
    AsyncContext *callContext) {
  // Make sure we don't eliminate calls to this function.
  asm volatile("" // Do nothing.
               :  // Output list, empty.
               : "r"(result), "r"(callerContext), "r"(task) // Input list.
               : // Clobber list, empty.
  );
  return;
}
#endif

SWIFT_CC(swiftasync)
static void swift_task_future_waitImpl(
  OpaqueValue *result,
  SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
  AsyncTask *task,
  TaskContinuationFunction *resumeFn,
  AsyncContext *callContext) {
  // Suspend the waiting task.
  auto waitingTask = swift_task_getCurrent();
  waitingTask->ResumeTask = task_future_wait_resume_adapter;
  waitingTask->ResumeContext = callContext;

  // Wait on the future.
  assert(task->isFuture());

  switch (task->waitFuture(waitingTask, callContext, resumeFn, callerContext,
                           result)) {
  case FutureFragment::Status::Executing:
    // The waiting task has been queued on the future.
#ifdef __ARM_ARCH_7K__
    return workaround_function_swift_task_future_waitImpl(
        result, callerContext, task, resumeFn, callContext);
#else
    return;
#endif

  case FutureFragment::Status::Success: {
    // Run the task with a successful result.
    auto future = task->futureFragment();
    future->getResultType()->vw_initializeWithCopy(result,
                                                   future->getStoragePtr());
    return resumeFn(callerContext);
  }

  case FutureFragment::Status::Error:
    swift_Concurrency_fatalError(0, "future reported an error, but wait cannot throw");
  }
}

#ifdef __ARM_ARCH_7K__
__attribute__((noinline))
SWIFT_CC(swiftasync) static void workaround_function_swift_task_future_wait_throwingImpl(
    OpaqueValue *result, SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
    AsyncTask *task, ThrowingTaskFutureWaitContinuationFunction resumeFunction,
    AsyncContext *callContext) {
  // Make sure we don't eliminate calls to this function.
  asm volatile("" // Do nothing.
               :  // Output list, empty.
               : "r"(result), "r"(callerContext), "r"(task) // Input list.
               : // Clobber list, empty.
  );
  return;
}
#endif

SWIFT_CC(swiftasync)
void swift_task_future_wait_throwingImpl(
    OpaqueValue *result, SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
    AsyncTask *task,
    ThrowingTaskFutureWaitContinuationFunction *resumeFunction,
    AsyncContext *callContext) {
  auto waitingTask = swift_task_getCurrent();
  // Suspend the waiting task.
  waitingTask->ResumeTask = task_wait_throwing_resume_adapter;
  waitingTask->ResumeContext = callContext;

  auto resumeFn = reinterpret_cast<TaskContinuationFunction *>(resumeFunction);

  // Wait on the future.
  assert(task->isFuture());

  switch (task->waitFuture(waitingTask, callContext, resumeFn, callerContext,
                           result)) {
  case FutureFragment::Status::Executing:
    // The waiting task has been queued on the future.
#ifdef __ARM_ARCH_7K__
    return workaround_function_swift_task_future_wait_throwingImpl(
        result, callerContext, task, resumeFunction, callContext);
#else
    return;
#endif

  case FutureFragment::Status::Success: {
    auto future = task->futureFragment();
    future->getResultType()->vw_initializeWithCopy(result,
                                                   future->getStoragePtr());
    return resumeFunction(callerContext, nullptr /*error*/);
  }

  case FutureFragment::Status::Error: {
    // Run the task with an error result.
    auto future = task->futureFragment();
    auto error = future->getError();
    swift_errorRetain(error);
    return resumeFunction(callerContext, error);
  }
  }
}

size_t swift::swift_task_getJobFlags(AsyncTask *task) {
  return task->Flags.getOpaqueValue();
}

// This function exists primarily for the purpose of the concurrency runtime
// unit tests and does not serve a functional purpose.
SWIFT_CC(swift)
static AsyncTask *swift_task_suspendImpl() {
  auto task = swift_task_getCurrent();
  task->flagAsSuspendedOnContinuation(nullptr);
  _swift_task_clearCurrent();
  return task;
}

SWIFT_CC(swift)
static void
swift_task_enqueueTaskOnExecutorImpl(AsyncTask *task, ExecutorRef executor)
{
  task->flagAsAndEnqueueOnExecutor(executor);
}

namespace continuationChecking {

enum class State : uint8_t { Uninitialized, On, Off };

static std::atomic<State> CurrentState;

static LazyMutex ActiveContinuationsLock;
static Lazy<std::unordered_set<ContinuationAsyncContext *>> ActiveContinuations;

static bool isEnabled() {
  auto state = CurrentState.load(std::memory_order_relaxed);
  if (state == State::Uninitialized) {
    bool enabled =
        runtime::environment::concurrencyValidateUncheckedContinuations();
    state = enabled ? State::On : State::Off;
    CurrentState.store(state, std::memory_order_relaxed);
  }
  return state == State::On;
}

static void init(ContinuationAsyncContext *context) {
  if (!isEnabled())
    return;

  LazyMutex::ScopedLock guard(ActiveContinuationsLock);
  auto result = ActiveContinuations.get().insert(context);
  auto inserted = std::get<1>(result);
  if (!inserted)
    swift_Concurrency_fatalError(
        0,
        "Initializing continuation context %p that was already initialized.\n",
        context);
}

static void willResume(ContinuationAsyncContext *context) {
  if (!isEnabled())
    return;

  LazyMutex::ScopedLock guard(ActiveContinuationsLock);
  auto removed = ActiveContinuations.get().erase(context);
  if (!removed)
    swift_Concurrency_fatalError(0,
                      "Resuming continuation context %p that was not awaited "
                      "(may have already been resumed).\n",
                      context);
}

} // namespace continuationChecking

SWIFT_CC(swift)
static AsyncTask *swift_continuation_initImpl(ContinuationAsyncContext *context,
                                              AsyncContinuationFlags flags) {
  continuationChecking::init(context);
  context->Flags = ContinuationAsyncContext::FlagsType();
  if (flags.canThrow()) context->Flags.setCanThrow(true);
  if (flags.isExecutorSwitchForced())
    context->Flags.setIsExecutorSwitchForced(true);
  context->ErrorResult = nullptr;

  // Set the generic executor as the target executor unless there's
  // an executor override.
  if (!flags.hasExecutorOverride())
    context->ResumeToExecutor = ExecutorRef::generic();

  // We can initialize this with a relaxed store because resumption
  // must happen-after this call.
  context->AwaitSynchronization.store(flags.isPreawaited()
                                        ? ContinuationStatus::Awaited
                                        : ContinuationStatus::Pending,
                                      std::memory_order_relaxed);
#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
  context->Cond = nullptr;
#endif
  AsyncTask *task;

  // A preawait immediately suspends the task.
  if (flags.isPreawaited()) {
    task = swift_task_getCurrent();
    assert(task && "initializing a continuation with no current task");
    task->flagAsSuspendedOnContinuation(context);
    _swift_task_clearCurrent();
  } else {
    task = swift_task_getCurrent();
    assert(task && "initializing a continuation with no current task");
  }

  task->ResumeContext = context;
  task->ResumeTask = context->ResumeParent;

  concurrency::trace::task_continuation_init(task, context);

  return task;
}

SWIFT_CC(swiftasync)
static void swift_continuation_awaitImpl(ContinuationAsyncContext *context) {
#ifndef NDEBUG
  auto task = swift_task_getCurrent();
  assert(task && "awaiting continuation without a task");
  assert(task->ResumeContext == context);
  assert(task->ResumeTask == context->ResumeParent);
#endif

  concurrency::trace::task_continuation_await(context);

  auto &sync = context->AwaitSynchronization;

  auto oldStatus = sync.load(std::memory_order_acquire);
  assert((oldStatus == ContinuationStatus::Pending ||
          oldStatus == ContinuationStatus::Resumed) &&
         "awaiting a corrupt or already-awaited continuation");

  // If the status is already Resumed, we can resume immediately.
  if (oldStatus == ContinuationStatus::Resumed) {
    if (context->isExecutorSwitchForced())
      return swift_task_switch(context, context->ResumeParent,
                               context->ResumeToExecutor);
    return context->ResumeParent(context);
  }

  // Load the current task (we already did this in assertions builds).
#ifdef NDEBUG
  auto task = swift_task_getCurrent();
#endif

#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
  // In the task to thread model, we do not suspend the task that is waiting on
  // the continuation resumption. Instead we simply block the thread on a
  // condition variable keep the task alive on the thread.
  //
  // This condition variable can be allocated on the stack of the blocking
  // thread - with the address of it published to the resuming thread via the
  // context.
  ConditionVariable Cond;

  context->Cond = &Cond;
#else /* SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL */
  // Flag the task as suspended on the continuation.
  task->flagAsSuspendedOnContinuation(context);
#endif /* SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL */

#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
  // If the cmpxchg is successful, the store release also publishes the write to
  // the Cond in the ContinuationAsyncContext to any concurrent accessing
  // thread.
  //
  // If it failed, then someone concurrently resumed the continuation in which
  // case, we don't care about publishing the Cond in the
  // ContinuationAsyncContext anyway.
#endif
  // Try to transition to Awaited
  bool success =
    sync.compare_exchange_strong(oldStatus, ContinuationStatus::Awaited,
                                 /*success*/ std::memory_order_release,
                                 /*failure*/ std::memory_order_acquire);

  if (success) {
#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
    // This lock really protects nothing but we need to hold it
    // while calling the condition wait
    Cond.lock();

    // Condition variables can have spurious wakeups so we need to check this in
    // a do-while loop.
    do {
      Cond.wait();
      oldStatus = sync.load(std::memory_order_relaxed);
    } while (oldStatus != ContinuationStatus::Resumed);

    Cond.unlock();
#else
    // If that succeeded, we have nothing to do since we've successfully
    // suspended the task
    _swift_task_clearCurrent();
    return;
#endif /* SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL */
  }

  // If it failed, it should be because someone concurrently resumed
  // (note that the compare-exchange above is strong).
  assert(oldStatus == ContinuationStatus::Resumed &&
         "continuation was concurrently corrupted or awaited");

#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
  // Since the condition variable is stack allocated, we don't need to do
  // anything here to clean up
#else
  // Restore the running state of the task and resume it.
  task->flagAsRunning();
#endif /* SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL */

  if (context->isExecutorSwitchForced())
    return swift_task_switch(context, context->ResumeParent,
                             context->ResumeToExecutor);
  return context->ResumeParent(context);
}

static void resumeTaskAfterContinuation(AsyncTask *task,
                                        ContinuationAsyncContext *context) {
  continuationChecking::willResume(context);

  auto &sync = context->AwaitSynchronization;

  auto status = sync.load(std::memory_order_acquire);
  assert(status != ContinuationStatus::Resumed &&
         "continuation was already resumed");

  // Make sure TSan knows that the resume call happens-before the task
  // restarting.
  _swift_tsan_release(static_cast<Job *>(task));

  // The status should be either Pending or Awaited.
  //
  // Case 1: Status is Pending
  // No one has awaited us, we just need to set it to Resumed; if that fails
  // (with a strong cmpxchg), it should be because the original thread
  // concurrently set it to Awaited, in which case, we fall into Case 2.
  //
  // Case 2: Status is Awaited
  // This is probably the more frequently hit case.
  // In task-to-thread model, we update status to be Resumed and signal the
  // waiting thread. In regular model, we immediately enqueue the task and can
  // skip updates to the continuation state since there shouldn't be a racing
  // attempt to resume the continuation.
  if (status == ContinuationStatus::Pending &&
      sync.compare_exchange_strong(status, ContinuationStatus::Resumed,
                                   /*success*/ std::memory_order_release,
                                   /*failure*/ std::memory_order_acquire)) {
    return;
  }
  assert(status == ContinuationStatus::Awaited &&
         "detected concurrent attempt to resume continuation");
#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
  // If we see status == ContinuationStatus::Awaited, then we should also be
  // seeing a pointer to the cond var since we're doing a load acquire on sync
  // which pairs with the store release in swift_continuation_awaitImpl
  assert(context->Cond != nullptr);

  sync.store(ContinuationStatus::Resumed, std::memory_order_relaxed);
  context->Cond->signal();
#else
  // TODO: maybe in some mode we should set the status to Resumed here
  // to make a stronger best-effort attempt to catch racing attempts to
  // resume the continuation?
  task->flagAsAndEnqueueOnExecutor(context->ResumeToExecutor);
#endif /* SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL */
}

SWIFT_CC(swift)
static void swift_continuation_resumeImpl(AsyncTask *task) {
  auto context = static_cast<ContinuationAsyncContext*>(task->ResumeContext);
  concurrency::trace::task_continuation_resume(context, false);
  resumeTaskAfterContinuation(task, context);
}

SWIFT_CC(swift)
static void swift_continuation_throwingResumeImpl(AsyncTask *task) {
  auto context = static_cast<ContinuationAsyncContext*>(task->ResumeContext);
  concurrency::trace::task_continuation_resume(context, false);
  resumeTaskAfterContinuation(task, context);
}


SWIFT_CC(swift)
static void swift_continuation_throwingResumeWithErrorImpl(AsyncTask *task,
                                                /* +1 */ SwiftError *error) {
  auto context = static_cast<ContinuationAsyncContext*>(task->ResumeContext);
  concurrency::trace::task_continuation_resume(context, true);
  context->ErrorResult = error;
  resumeTaskAfterContinuation(task, context);
}

bool swift::swift_task_isCancelled(AsyncTask *task) {
  return task->isCancelled();
}

SWIFT_CC(swift)
static CancellationNotificationStatusRecord*
swift_task_addCancellationHandlerImpl(
    CancellationNotificationStatusRecord::FunctionType handler,
    void *context) {
  void *allocation =
      swift_task_alloc(sizeof(CancellationNotificationStatusRecord));
  auto unsigned_handler = swift_auth_code(handler, 3848);
  auto *record = ::new (allocation)
      CancellationNotificationStatusRecord(unsigned_handler, context);

  bool fireHandlerNow = false;

  addStatusRecordToSelf(record, [&](ActiveTaskStatus oldStatus, ActiveTaskStatus& newStatus) {
    if (oldStatus.isCancelled()) {
      fireHandlerNow = true;
      // We don't fire the cancellation handler here since this function needs
      // to be idempotent
    }
    return true;
  });

  if (fireHandlerNow) {
    record->run();
  }
  return record;
}

SWIFT_CC(swift)
static void swift_task_removeCancellationHandlerImpl(
    CancellationNotificationStatusRecord *record) {
  removeStatusRecordFromSelf(record);
  swift_task_dealloc(record);
}

SWIFT_CC(swift)
static NullaryContinuationJob*
swift_task_createNullaryContinuationJobImpl(
    size_t priority,
    AsyncTask *continuation) {
  auto *job = new NullaryContinuationJob(swift_task_getCurrent(),
        static_cast<JobPriority>(priority), continuation);

  return job;
}

SWIFT_CC(swift)
void swift::swift_continuation_logFailedCheck(const char *message) {
  swift_reportError(0, message);
}

SWIFT_RUNTIME_ATTRIBUTE_NORETURN
SWIFT_CC(swift)
static void swift_task_asyncMainDrainQueueImpl() {
#if SWIFT_CONCURRENCY_COOPERATIVE_GLOBAL_EXECUTOR
  bool Finished = false;
  swift_task_donateThreadToGlobalExecutorUntil([](void *context) {
    return *reinterpret_cast<bool*>(context);
  }, &Finished);
#elif !SWIFT_CONCURRENCY_ENABLE_DISPATCH
  // FIXME: consider implementing a concurrent global main queue for
  // these environments?
  swift_reportError(0, "operation unsupported without libdispatch: "
                       "swift_task_asyncMainDrainQueue");
#else
#if defined(_WIN32)
  HMODULE hModule = LoadLibraryW(L"dispatch.dll");
  if (hModule == NULL) {
    swift_Concurrency_fatalError(0,
      "unable to load dispatch.dll: %lu", GetLastError());
  }

  auto pfndispatch_main = reinterpret_cast<void (FAR *)(void)>(
    GetProcAddress(hModule, "dispatch_main"));
  if (pfndispatch_main == NULL) {
    swift_Concurrency_fatalError(0,
      "unable to locate dispatch_main in dispatch.dll: %lu", GetLastError());
  }

  pfndispatch_main();
  swift_unreachable("Returned from dispatch_main()");
#else
  // CFRunLoop is not available on non-Darwin targets.  Foundation has an
  // implementation, but CoreFoundation is not meant to be exposed.  We can only
  // assume the existence of `CFRunLoopRun` on Darwin platforms, where the
  // system provides an implementation of CoreFoundation.
#if defined(__APPLE__)
  auto runLoop =
      reinterpret_cast<void (*)(void)>(dlsym(RTLD_DEFAULT, "CFRunLoopRun"));
  if (runLoop) {
    runLoop();
    exit(0);
  }
#endif

    dispatch_main();
#endif
#endif
}

SWIFT_CC(swift)
void (*swift::swift_task_asyncMainDrainQueue_hook)(
    swift_task_asyncMainDrainQueue_original original,
    swift_task_asyncMainDrainQueue_override compatOverride) = nullptr;

SWIFT_CC(swift)
static void swift_task_startOnMainActorImpl(AsyncTask* task) {
  AsyncTask * originalTask = _swift_task_clearCurrent();
  ExecutorRef mainExecutor = swift_task_getMainExecutor();
  if (!swift_task_isCurrentExecutor(mainExecutor))
    swift_Concurrency_fatalError(0, "Not on the main executor");
  swift_retain(task);
  swift_job_run(task, mainExecutor);
  _swift_task_setCurrent(originalTask);
}

#define OVERRIDE_TASK COMPATIBILITY_OVERRIDE

#ifdef SWIFT_STDLIB_SUPPORT_BACK_DEPLOYMENT
/// The original COMPATIBILITY_OVERRIDE defined in CompatibilityOverride.h
/// returns the result of the impl function and override function. This results
/// in a warning emitted for noreturn functions. Overriding the override macro
/// to not return.
#define HOOKED_OVERRIDE_TASK_NORETURN(name, attrs, ccAttrs, namespace,         \
                                      typedArgs, namedArgs)                    \
  attrs ccAttrs void namespace swift_##name COMPATIBILITY_PAREN(typedArgs) {   \
    static Override_##name Override;                                           \
    static swift_once_t Predicate;                                             \
    swift_once(                                                                \
        &Predicate, [](void *) { Override = getOverride_##name(); }, nullptr); \
    if (swift_##name##_hook) {                                                 \
      swift_##name##_hook(COMPATIBILITY_UNPAREN_WITH_COMMA(namedArgs)          \
                              swift_##name##Impl,                              \
                          Override);                                           \
      abort();                                                                 \
    }                                                                          \
    if (Override != nullptr)                                                   \
      Override(COMPATIBILITY_UNPAREN_WITH_COMMA(namedArgs)                     \
                   swift_##name##Impl);                                        \
    swift_##name##Impl COMPATIBILITY_PAREN(namedArgs);                         \
  }

#else // ifndef SWIFT_STDLIB_SUPPORT_BACK_DEPLOYMENT
// Call directly through to the original implementation when we don't support
// overrides.
#define HOOKED_OVERRIDE_TASK_NORETURN(name, attrs, ccAttrs, namespace,         \
                                      typedArgs, namedArgs)                    \
  attrs ccAttrs void namespace swift_##name COMPATIBILITY_PAREN(typedArgs) {   \
    if (swift_##name##_hook) {                                                 \
      swift_##name##_hook(swift_##name##Impl, nullptr);                        \
      abort();                                                                 \
    }                                                                          \
    swift_##name##Impl COMPATIBILITY_PAREN(namedArgs);                         \
  }
#endif // #else SWIFT_STDLIB_SUPPORT_BACK_DEPLOYMENT

#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH
