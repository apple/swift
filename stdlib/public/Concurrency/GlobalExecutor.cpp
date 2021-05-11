///===--- GlobalExecutor.cpp - Global concurrent executor ------------------===///
///
/// This source file is part of the Swift.org open source project
///
/// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
/// Licensed under Apache License v2.0 with Runtime Library Exception
///
/// See https:///swift.org/LICENSE.txt for license information
/// See https:///swift.org/CONTRIBUTORS.txt for the list of Swift project authors
///
///===----------------------------------------------------------------------===///
///
/// Routines related to the global concurrent execution service.
///
/// The execution side of Swift's concurrency model centers around
/// scheduling work onto various execution services ("executors").
/// Executors vary in several different dimensions:
///
/// First, executors may be exclusive or concurrent.  An exclusive
/// executor can only execute one job at once; a concurrent executor
/// can execute many.  Exclusive executors are usually used to achieve
/// some higher-level requirement, like exclusive access to some
/// resource or memory.  Concurrent executors are usually used to
/// manage a pool of threads and prevent the number of allocated
/// threads from growing without limit.
/// 
/// Second, executors may own dedicated threads, or they may schedule
/// work onto some some underlying executor.  Dedicated threads can
/// improve the responsiveness of a subsystem *locally*, but they impose
/// substantial costs which can drive down performance *globally*
/// if not used carefully.  When an executor relies on running work
/// on its own dedicated threads, jobs that need to run briefly on
/// that executor may need to suspend and restart.  Dedicating threads
/// to an executor is a decision that should be made carefully
/// and holistically.
///
/// If most executors should not have dedicated threads, they must
/// be backed by some underlying executor, typically a concurrent
/// executor.  The purpose of most concurrent executors is to
/// manage threads and prevent excessive growth in the number
/// of threads.  Having multiple independent concurrent executors
/// with their own dedicated threads would undermine that.
/// Therefore, it is sensible to have a single, global executor
/// that will ultimately schedule most of the work in the system.  
/// With that as a baseline, special needs can be recognized and
/// carved out from the global executor with its cooperation.
///
/// This file defines Swift's interface to that global executor.
///
/// The default implementation is backed by libdispatch, but there
/// may be good reasons to provide alternatives (e.g. when building
/// a single-threaded runtime).
///
///===----------------------------------------------------------------------===///

#include "../CompatibilityOverride/CompatibilityOverride.h"
#include "swift/Runtime/Concurrency.h"
#include "swift/Runtime/EnvironmentVariables.h"
#include "TaskPrivate.h"
#include "Error.h"

#if !SWIFT_CONCURRENCY_COOPERATIVE_GLOBAL_EXECUTOR
#include <dispatch/dispatch.h>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#endif

using namespace swift;

SWIFT_CC(swift)
void (*swift::swift_task_enqueueGlobal_hook)(
    Job *job, swift_task_enqueueGlobal_original original) = nullptr;

SWIFT_CC(swift)
void (*swift::swift_task_enqueueGlobalWithDelay_hook)(
    unsigned long long delay, Job *job,
    swift_task_enqueueGlobalWithDelay_original original) = nullptr;

SWIFT_CC(swift)
void (*swift::swift_task_enqueueMainExecutor_hook)(
    Job *job, swift_task_enqueueMainExecutor_original original) = nullptr;

#if SWIFT_CONCURRENCY_COOPERATIVE_GLOBAL_EXECUTOR

#include <chrono>
#include <thread>

static Job *JobQueue = nullptr;

class DelayedJob {
public:
  Job *job;
  unsigned long long when;
  DelayedJob *next;

  DelayedJob(Job *job, unsigned long long when) : job(job), when(when), next(nullptr) {}
};

static DelayedJob *DelayedJobQueue = nullptr;

/// Get the next-in-queue storage slot.
static Job *&nextInQueue(Job *cur) {
  return reinterpret_cast<Job*&>(cur->SchedulerPrivate);
}

/// Insert a job into the cooperative global queue.
static void insertIntoJobQueue(Job *newJob) {
  Job **position = &JobQueue;
  while (auto cur = *position) {
    // If we find a job with lower priority, insert here.
    if (cur->getPriority() < newJob->getPriority()) {
      nextInQueue(newJob) = cur;
      *position = newJob;
      return;
    }

    // Otherwise, keep advancing through the queue.
    position = &nextInQueue(cur);
  }
  nextInQueue(newJob) = nullptr;
  *position = newJob;
}

static unsigned long long currentNanos() {
  auto now = std::chrono::steady_clock::now();
  auto nowNanos = std::chrono::time_point_cast<std::chrono::nanoseconds>(now);
  auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(nowNanos.time_since_epoch());
  return value.count();
}

/// Insert a job into the cooperative global queue.
static void insertIntoDelayedJobQueue(unsigned long long delay, Job *job) {
  DelayedJob **position = &DelayedJobQueue;
  DelayedJob *newJob = new DelayedJob(job, currentNanos() + delay);

  while (auto cur = *position) {
    // If we find a job with lower priority, insert here.
    if (cur->when > newJob->when) {
      newJob->next = cur;
      *position = newJob;
      return;
    }

    // Otherwise, keep advancing through the queue.
    position = &cur->next;
  }
  *position = newJob;
}

/// Claim the next job from the cooperative global queue.
static Job *claimNextFromJobQueue() {
  // Check delayed jobs first
  while (true) {
    if (auto delayedJob = DelayedJobQueue) {
      if (delayedJob->when < currentNanos()) {
        DelayedJobQueue = delayedJob->next;
        auto job = delayedJob->job;
        
        delete delayedJob;

        return job;
      }
    }
    if (auto job = JobQueue) {
      JobQueue = nextInQueue(job);
      return job;
    }
    // there are only delayed jobs left, but they are not ready,
    // so we sleep until the first one is
    if (auto delayedJob = DelayedJobQueue) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(delayedJob->when - currentNanos()));
      continue;
    }
    return nullptr;
  }
}

void swift::donateThreadToGlobalExecutorUntil(bool (*condition)(void *),
                                              void *conditionContext) {
  while (!condition(conditionContext)) {
    auto job = claimNextFromJobQueue();
    if (!job) return;
    swift_job_run(job, ExecutorRef::generic());
  }
}

#else

// Ensure that Job's layout is compatible with what Dispatch expects.
// Note: MinimalDispatchObjectHeader just has the fields we care about, it is
// not complete and should not be used for anything other than these asserts.
struct MinimalDispatchObjectHeader {
  const void *VTable;
  int Opaque0;
  int Opaque1;
  void *Linkage;
};
static_assert(
    offsetof(Job, metadata) == offsetof(MinimalDispatchObjectHeader, VTable),
    "Job Metadata field must match location of Dispatch VTable field.");
static_assert(offsetof(Job, SchedulerPrivate[Job::DispatchLinkageIndex]) ==
                  offsetof(MinimalDispatchObjectHeader, Linkage),
              "Dispatch Linkage field must match Job "
              "SchedulerPrivate[DispatchLinkageIndex].");

/// The function passed to dispatch_async_f to execute a job.
static void __swift_run_job(void *_job) {
  Job *job = (Job*) _job;
  auto metadata =
      reinterpret_cast<const DispatchClassMetadata *>(job->metadata);
  metadata->VTableInvoke(job, nullptr, 0);
}

/// The type of a function pointer for enqueueing a Job object onto a dispatch
/// queue.
typedef void (*dispatchEnqueueFuncType)(dispatch_queue_t queue, void *obj,
                                        dispatch_qos_class_t qos);

/// Initialize dispatchEnqueueFunc and then call through to the proper
/// implementation.
static void initializeDispatchEnqueueFunc(dispatch_queue_t queue, void *obj,
                                          dispatch_qos_class_t qos);

/// A function pointer to the function used to enqueue a Job onto a dispatch
/// queue. Initially set to initializeDispatchEnqueueFunc, so that the first
/// call will initialize it. initializeDispatchEnqueueFunc sets it to point
/// either to dispatch_async_swift_job when it's available, otherwise to
/// dispatchEnqueueDispatchAsync.
static std::atomic<dispatchEnqueueFuncType> dispatchEnqueueFunc{
    initializeDispatchEnqueueFunc};

/// A small adapter that dispatches a Job onto a queue using dispatch_async_f.
static void dispatchEnqueueDispatchAsync(dispatch_queue_t queue, void *obj,
                                         dispatch_qos_class_t qos) {
  dispatch_async_f(queue, obj, __swift_run_job);
}

static void initializeDispatchEnqueueFunc(dispatch_queue_t queue, void *obj,
                                          dispatch_qos_class_t qos) {
  dispatchEnqueueFuncType func = nullptr;

  // Always fall back to plain dispatch_async_f on Windows for now.
#if !defined(_WIN32)
  if (runtime::environment::concurrencyEnableJobDispatchIntegration())
    func = reinterpret_cast<dispatchEnqueueFuncType>(
        dlsym(RTLD_NEXT, "dispatch_async_swift_job"));
#endif

  if (!func)
    func = dispatchEnqueueDispatchAsync;

  dispatchEnqueueFunc.store(func, std::memory_order_relaxed);

  func(queue, obj, qos);
}

/// Enqueue a Job onto a dispatch queue using dispatchEnqueueFunc.
static void dispatchEnqueue(dispatch_queue_t queue, Job *job,
                            dispatch_qos_class_t qos, void *executorQueue) {
  job->SchedulerPrivate[Job::DispatchQueueIndex] = executorQueue;
  dispatchEnqueueFunc.load(std::memory_order_relaxed)(queue, job, qos);
}

static constexpr size_t globalQueueCacheCount =
    static_cast<size_t>(JobPriority::UserInteractive) + 1;
static std::atomic<dispatch_queue_t> globalQueueCache[globalQueueCacheCount];

static dispatch_queue_t getGlobalQueue(JobPriority priority) {
  size_t numericPriority = static_cast<size_t>(priority);
  if (numericPriority >= globalQueueCacheCount)
    swift_Concurrency_fatalError(0, "invalid job priority %#zx");

  auto *ptr = &globalQueueCache[numericPriority];
  auto queue = ptr->load(std::memory_order_relaxed);
  if (SWIFT_LIKELY(queue))
    return queue;

  // If we don't have a queue cached for this priority, cache it now. This may
  // race with other threads doing this at the same time for this priority, but
  // that's OK, they'll all end up writing the same value.
  queue = dispatch_get_global_queue((dispatch_qos_class_t)priority,
                                    /*flags*/ 0);

  // Unconditionally store it back in the cache. If we raced with another
  // thread, we'll just overwrite the entry with the same value.
  ptr->store(queue, std::memory_order_relaxed);

  return queue;
}

#endif

SWIFT_CC(swift)
static void swift_task_enqueueGlobalImpl(Job *job) {
  assert(job && "no job provided");

#if SWIFT_CONCURRENCY_COOPERATIVE_GLOBAL_EXECUTOR
  insertIntoJobQueue(job);
#else
  // We really want four things from the global execution service:
  //  - Enqueuing work should have minimal runtime and memory overhead.
  //  - Adding work should never result in an "explosion" where many
  //    more threads are created than the available cores.
  //  - Jobs should run on threads with an appropriate priority.
  //  - Thread priorities should temporarily elevatable to avoid
  //    priority inversions.
  //
  // Of these, the first two are the most important.  Many programs
  // do not rely on high-usage priority scheduling, and many priority
  // inversions can be avoided at a higher level (albeit with some
  // performance cost, e.g. by creating higher-priority tasks to run
  // critical sections that contend with high-priority work).  In
  // contrast, if the async feature adds too much overhead, or if
  // heavy use of it leads to thread explosions and memory exhaustion,
  // programmers will have no choice but to stop using it.  So if
  // goals are in conflict, it's best to focus on core properties over
  // priority-inversion avoidance.

  // We currently use Dispatch for our thread pool on all platforms.
  // Dispatch currently backs its serial queues with a global
  // concurrent queue that is prone to thread explosions when a flood
  // of jobs are added to it.  That problem does not apply equally
  // to the global concurrent queues returned by dispatch_get_global_queue,
  // which are not strictly CPU-limited but are at least much more
  // cautious about adding new threads.  We cannot safely elevate
  // the priorities of work added to this queue using Dispatch's public
  // API, but as discussed above, that is less important than avoiding
  // performance problems.
  JobPriority priority = job->getPriority();

  auto queue = getGlobalQueue(priority);

  dispatchEnqueue(queue, job, (dispatch_qos_class_t)priority,
                  DISPATCH_QUEUE_GLOBAL_EXECUTOR);
#endif
}

void swift::swift_task_enqueueGlobal(Job *job) {
  if (swift_task_enqueueGlobal_hook)
    swift_task_enqueueGlobal_hook(job, swift_task_enqueueGlobal);
  else
    swift_task_enqueueGlobalImpl(job);
}

SWIFT_CC(swift)
static void swift_task_enqueueGlobalWithDelayImpl(unsigned long long delay,
                                                  Job *job) {
  assert(job && "no job provided");

#if SWIFT_CONCURRENCY_COOPERATIVE_GLOBAL_EXECUTOR
  insertIntoDelayedJobQueue(delay, job);
#else

  dispatch_function_t dispatchFunction = &__swift_run_job;
  void *dispatchContext = job;

  JobPriority priority = job->getPriority();

  auto queue = getGlobalQueue(priority);

  job->SchedulerPrivate[Job::DispatchQueueIndex] =
      DISPATCH_QUEUE_GLOBAL_EXECUTOR;

  dispatch_time_t when = dispatch_time(DISPATCH_TIME_NOW, delay);
  dispatch_after_f(when, queue, dispatchContext, dispatchFunction);
#endif
}

void swift::swift_task_enqueueGlobalWithDelay(unsigned long long delay,
                                              Job *job) {
  if (swift_task_enqueueGlobalWithDelay_hook)
    swift_task_enqueueGlobalWithDelay_hook(
        delay, job, swift_task_enqueueGlobalWithDelayImpl);
  else
    swift_task_enqueueGlobalWithDelayImpl(delay, job);
}

/// Enqueues a task on the main executor.
/// FIXME: only exists for the quick-and-dirty MainActor implementation.
SWIFT_CC(swift)
static void swift_task_enqueueMainExecutorImpl(Job *job) {
  assert(job && "no job provided");

#if SWIFT_CONCURRENCY_COOPERATIVE_GLOBAL_EXECUTOR
  insertIntoJobQueue(job);
#else

  JobPriority priority = job->getPriority();

  // This is an inline function that compiles down to a pointer to a global.
  auto mainQueue = dispatch_get_main_queue();

  dispatchEnqueue(mainQueue, job, (dispatch_qos_class_t)priority,
                  DISPATCH_QUEUE_MAIN_EXECUTOR);

#endif
}

void swift::swift_task_enqueueMainExecutor(Job *job) {
  if (swift_task_enqueueMainExecutor_hook)
    swift_task_enqueueMainExecutor_hook(job,
                                        swift_task_enqueueMainExecutorImpl);
  else
    swift_task_enqueueMainExecutorImpl(job);
}

#define OVERRIDE_GLOBAL_EXECUTOR COMPATIBILITY_OVERRIDE
#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH
