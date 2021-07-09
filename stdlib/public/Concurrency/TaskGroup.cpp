//===--- TaskGroup.cpp - Task Groups --------------------------------------===//
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
// Object management for child tasks that are children of a task group.
//
//===----------------------------------------------------------------------===//

#include "../CompatibilityOverride/CompatibilityOverride.h"

#include "swift/ABI/TaskGroup.h"
#include "swift/ABI/Task.h"
#include "swift/ABI/Metadata.h"
#include "swift/ABI/HeapObject.h"
#include "TaskPrivate.h"
#include "TaskGroupPrivate.h"
#include "swift/Basic/RelativePointer.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Runtime/Concurrency.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/Mutex.h"
#include "swift/Runtime/HeapObject.h"
#include "AsyncCall.h"
#include "Debug.h"
#include "bitset"
#include "string"
#include "queue" // TODO: remove and replace with usage of our mpsc queue
#include <atomic>
#include <assert.h>
#include <dispatch/dispatch.h>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

using namespace swift;

/******************************************************************************/
/*************************** TASK GROUP ***************************************/
/******************************************************************************/

using FutureFragment = AsyncTask::FutureFragment;

namespace {
class TaskStatusRecord;

class TaskGroupImpl: public TaskGroupTaskStatusRecord {
public:
  /// Describes the status of the group.
  enum class ReadyStatus : uintptr_t {
    /// The task group is empty, no tasks are pending.
    /// Return immediately, there is no point in suspending.
    ///
    /// The storage is not accessible.
    Empty = 0b00,

    // not used: 0b01; same value as the PollStatus MustWait,
    //                 which does not make sense for the ReadyStatus

    /// The future has completed with result (of type \c resultType).
    Success = 0b10,

    /// The future has completed by throwing an error (an \c Error
    /// existential).
    Error = 0b11,
  };

  enum class PollStatus : uintptr_t {
    /// The group is known to be empty and we can immediately return nil.
    Empty = 0b00,

    /// The task has been enqueued to the groups wait queue.
    MustWait = 0b01,

    /// The task has completed with result (of type \c resultType).
    Success = 0b10,

    /// The task has completed by throwing an error (an \c Error existential).
    Error = 0b11,
  };

  /// The result of waiting on the TaskGroupImpl.
  struct PollResult {
    PollStatus status; // TODO: pack it into storage pointer or not worth it?

    /// Storage for the result of the future.
    ///
    /// When the future completed normally, this is a pointer to the storage
    /// of the result value, which lives inside the future task itself.
    ///
    /// When the future completed by throwing an error, this is the error
    /// object itself.
    OpaqueValue *storage;

    const Metadata *successType;

    /// The completed task, if necessary to keep alive until consumed by next().
    ///
    /// # Important: swift_release
    /// If if a task is returned here, the task MUST be swift_released
    /// once we are done with it, to balance out the retain made before
    /// when the task was enqueued into the ready queue to keep it alive
    /// until a next() call eventually picks it up.
    AsyncTask *retainedTask;

    bool isStorageAccessible() {
      return status == PollStatus::Success ||
             status == PollStatus::Error ||
             status == PollStatus::Empty;
    }

    static PollResult get(AsyncTask *asyncTask, bool hadErrorResult) {
      auto fragment = asyncTask->futureFragment();
      return PollResult{
        /*status*/ hadErrorResult ?
                   PollStatus::Error :
                   PollStatus::Success,
        /*storage*/ hadErrorResult ?
                    reinterpret_cast<OpaqueValue *>(fragment->getError()) :
                    fragment->getStoragePtr(),
        /*successType*/fragment->getResultType(),
        /*task*/ asyncTask
      };
    }
  };

  /// An item within the message queue of a group.
  struct ReadyQueueItem {
    /// Mask used for the low status bits in a message queue item.
    static const uintptr_t statusMask = 0x03;

    uintptr_t storage;

    ReadyStatus getStatus() const {
      return static_cast<ReadyStatus>(storage & statusMask);
    }

    AsyncTask *getTask() const {
      return reinterpret_cast<AsyncTask *>(storage & ~statusMask);
    }

    static ReadyQueueItem get(ReadyStatus status, AsyncTask *task) {
      assert(task == nullptr || task->isFuture());
      return ReadyQueueItem{
        reinterpret_cast<uintptr_t>(task) | static_cast<uintptr_t>(status)};
    }
  };

  /// An item within the pending queue.
  struct PendingQueueItem {
    uintptr_t storage;

    AsyncTask *getTask() const {
      return reinterpret_cast<AsyncTask *>(storage);
    }

    static ReadyQueueItem get(AsyncTask *task) {
      assert(task == nullptr || task->isFuture());
      return ReadyQueueItem{reinterpret_cast<uintptr_t>(task)};
    }
  };

  struct GroupStatus {
    static const uint64_t cancelled      = 0b1000000000000000000000000000000000000000000000000000000000000000;
    static const uint64_t waiting        = 0b0100000000000000000000000000000000000000000000000000000000000000;

    // 31 bits for ready tasks counter
    static const uint64_t maskReady      = 0b0011111111111111111111111111111110000000000000000000000000000000;
    static const uint64_t oneReadyTask   = 0b0000000000000000000000000000000010000000000000000000000000000000;

    // 31 bits for pending tasks counter
    static const uint64_t maskPending    = 0b0000000000000000000000000000000001111111111111111111111111111111;
    static const uint64_t onePendingTask = 0b0000000000000000000000000000000000000000000000000000000000000001;

    uint64_t status;

    bool isCancelled() {
      return (status & cancelled) > 0;
    }

    bool hasWaitingTask() {
      return (status & waiting) > 0;
    }

    unsigned int readyTasks() {
      return (status & maskReady) >> 31;
    }

    unsigned int pendingTasks() {
      return (status & maskPending);
    }

    bool isEmpty() {
      return pendingTasks() == 0;
    }

    /// Status value decrementing the Ready, Pending and Waiting counters by one.
    GroupStatus completingPendingReadyWaiting() {
      assert(pendingTasks() &&
             "can only complete waiting task when pending tasks available");
      assert(readyTasks() &&
             "can only complete waiting task when ready tasks available");
      assert(hasWaitingTask() &&
             "can only complete waiting task when waiting task available");
      return GroupStatus{status - waiting - oneReadyTask - onePendingTask};
    }

    GroupStatus completingPendingReady() {
      assert(pendingTasks() &&
             "can only complete waiting task when pending tasks available");
      assert(readyTasks() &&
             "can only complete waiting task when ready tasks available");
      return GroupStatus{status - oneReadyTask - onePendingTask};
    }

    /// Pretty prints the status, as follows:
    /// GroupStatus{ P:{pending tasks} W:{waiting tasks} {binary repr} }
    std::string to_string() {
      std::string str;
      str.append("GroupStatus{ ");
      str.append("C:"); // cancelled
      str.append(isCancelled() ? "y " : "n ");
      str.append("W:"); // has waiting task
      str.append(hasWaitingTask() ? "y " : "n ");
      str.append("R:"); // ready
      str.append(std::to_string(readyTasks()));
      str.append(" P:"); // pending
      str.append(std::to_string(pendingTasks()));
      str.append(" " + std::bitset<64>(status).to_string());
      str.append(" }");
      return str;
    }

    /// Initially there are no waiting and no pending tasks.
    static const GroupStatus initial() {
      return GroupStatus{0};
    };
  };

  template<typename T>
  class NaiveQueue {
    std::queue <T> queue;

  public:
    NaiveQueue() = default;

    NaiveQueue(const NaiveQueue<T> &) = delete;

    NaiveQueue &operator=(const NaiveQueue<T> &) = delete;

    NaiveQueue(NaiveQueue<T> &&other) {
      queue = std::move(other.queue);
    }

    virtual ~NaiveQueue() {}

    bool dequeue(T &output) {
      if (queue.empty()) {
        return false;
      }
      output = queue.front();
      queue.pop();
      return true;
    }

    void enqueue(const T item) {
      queue.push(item);
    }
  };

private:

//    // TODO: move to lockless via the status atomic
  mutable std::mutex mutex;

  /// Used for queue management, counting number of waiting and ready tasks
  std::atomic <uint64_t> status;

  /// Queue containing completed tasks offered into this group.
  ///
  /// The low bits contain the status, the rest of the pointer is the
  /// AsyncTask.
  NaiveQueue<ReadyQueueItem> readyQueue;
//     mpsc_queue_t<ReadyQueueItem> readyQueue; // TODO: can we get away with an MPSC queue here once actor executors land?

  /// Single waiting `AsyncTask` currently waiting on `group.next()`,
  /// or `nullptr` if no task is currently waiting.
  std::atomic<AsyncTask *> waitQueue;

  const Metadata *successType;

  friend class ::swift::AsyncTask;

public:
  explicit TaskGroupImpl(const Metadata *T)
    : TaskGroupTaskStatusRecord(),
      status(GroupStatus::initial().status),
      readyQueue(),
//          readyQueue(ReadyQueueItem::get(ReadyStatus::Empty, nullptr)),
      waitQueue(nullptr), successType(T) {}


  TaskGroupTaskStatusRecord *getTaskRecord() {
    return reinterpret_cast<TaskGroupTaskStatusRecord *>(this);
  }

  /// Destroy the storage associated with the group.
  void destroy();

  bool isEmpty() {
    auto oldStatus = GroupStatus{status.load(std::memory_order_relaxed)};
    return oldStatus.pendingTasks() == 0;
  }

  bool isCancelled() {
    auto oldStatus = GroupStatus{status.load(std::memory_order_relaxed)};
    return oldStatus.isCancelled();
  }

  /// Cancel the task group and all tasks within it.
  ///
  /// Returns `true` if this is the first time cancelling the group, false otherwise.
  bool cancelAll();

  GroupStatus statusCancel() {
    auto old = status.fetch_or(GroupStatus::cancelled,
                               std::memory_order_relaxed);
    return GroupStatus{old};
  }

  /// Returns *assumed* new status, including the just performed +1.
  GroupStatus statusMarkWaitingAssumeAcquire() {
    auto old = status.fetch_or(GroupStatus::waiting, std::memory_order_acquire);
    return GroupStatus{old | GroupStatus::waiting};
  }

  GroupStatus statusRemoveWaiting() {
    auto old = status.fetch_and(~GroupStatus::waiting,
                                std::memory_order_release);
    return GroupStatus{old};
  }

  /// Returns *assumed* new status, including the just performed +1.
  GroupStatus statusAddReadyAssumeAcquire() {
    auto old = status.fetch_add(GroupStatus::oneReadyTask,
                                std::memory_order_acquire);
    auto s = GroupStatus{old + GroupStatus::oneReadyTask};
    assert(s.readyTasks() <= s.pendingTasks());
    return s;
  }

  /// Add a single pending task to the status counter.
  /// This is used to implement next() properly, as we need to know if there
  /// are pending tasks worth suspending/waiting for or not.
  ///
  /// Note that the group does *not* store child tasks at all, as they are
  /// stored in the `TaskGroupTaskStatusRecord` inside the current task, that
  /// is currently executing the group. Here we only need the counts of
  /// pending/ready tasks.
  ///
  /// If the `unconditionally` parameter is `true` the operation always successfully
  /// adds a pending task, even if the group is cancelled. If the unconditionally
  /// flag is `false`, the added pending count will be *reverted* before returning.
  /// This is because we will NOT add a task to a cancelled group, unless doing
  /// so unconditionally.
  ///
  /// Returns *assumed* new status, including the just performed +1.
  GroupStatus statusAddPendingTaskRelaxed(bool unconditionally) {
    auto old = status.fetch_add(GroupStatus::onePendingTask,
                                std::memory_order_relaxed);
    auto s = GroupStatus{old + GroupStatus::onePendingTask};

    if (!unconditionally && s.isCancelled()) {
      // revert that add, it was meaningless
      auto o = status.fetch_sub(GroupStatus::onePendingTask,
                                std::memory_order_relaxed);
      s = GroupStatus{o - GroupStatus::onePendingTask};
    }

    return s;
  }

  GroupStatus statusLoadRelaxed() {
    return GroupStatus{status.load(std::memory_order_relaxed)};
  }

  /// Compare-and-set old status to a status derived from the old one,
  /// by simultaneously decrementing one Pending and one Waiting tasks.
  ///
  /// This is used to atomically perform a waiting task completion.
  bool statusCompletePendingReadyWaiting(GroupStatus &old) {
    return status.compare_exchange_weak(
      old.status, old.completingPendingReadyWaiting().status,
      /*success*/ std::memory_order_relaxed,
      /*failure*/ std::memory_order_relaxed);
  }

  bool statusCompletePendingReady(GroupStatus &old) {
    return status.compare_exchange_weak(
      old.status, old.completingPendingReady().status,
      /*success*/ std::memory_order_relaxed,
      /*failure*/ std::memory_order_relaxed);
  }


  /// Offer result of a task into this task group.
  ///
  /// If possible, and an existing task is already waiting on next(), this will
  /// schedule it immediately. If not, the result is enqueued and will be picked
  /// up whenever a task calls next() the next time.
  void offer(AsyncTask *completed, AsyncContext *context);

  /// Attempt to dequeue ready tasks and complete the waitingTask.
  ///
  /// If unable to complete the waiting task immediately (with an readily
  /// available completed task), either returns an `PollStatus::Empty`
  /// result if it is known that no pending tasks in the group,
  /// or a `PollStatus::MustWait` result if there are tasks in flight
  /// and the waitingTask eventually be woken up by a completion.
  PollResult poll(AsyncTask *waitingTask);
};

} // end anonymous namespace

/******************************************************************************/
/************************ TASK GROUP IMPLEMENTATION ***************************/
/******************************************************************************/

using ReadyQueueItem = TaskGroupImpl::ReadyQueueItem;
using ReadyStatus = TaskGroupImpl::ReadyStatus;
using PollResult = TaskGroupImpl::PollResult;
using PollStatus = TaskGroupImpl::PollStatus;

static_assert(sizeof(TaskGroupImpl) <= sizeof(TaskGroup) &&
              alignof(TaskGroupImpl) <= alignof(TaskGroup),
              "TaskGroupImpl doesn't fit in TaskGroup");

static TaskGroupImpl *asImpl(TaskGroup *group) {
  return reinterpret_cast<TaskGroupImpl*>(group);
}

static TaskGroup *asAbstract(TaskGroupImpl *group) {
  return reinterpret_cast<TaskGroup*>(group);
}

// =============================================================================
// ==== initialize -------------------------------------------------------------

// Initializes into the preallocated _group an actual TaskGroupImpl.
SWIFT_CC(swift)
static void swift_taskGroup_initializeImpl(TaskGroup *group, const Metadata *T) {
  TaskGroupImpl *impl = new (group) TaskGroupImpl(T);
  auto record = impl->getTaskRecord();
  assert(impl == record && "the group IS the task record");

  // ok, now that the group actually is initialized: attach it to the task
  bool notCancelled = swift_task_addStatusRecord(record);

  // If the task has already been cancelled, reflect that immediately in
  // the group status.
  if (!notCancelled) impl->statusCancel();
}

// =============================================================================
// ==== add / attachChild ------------------------------------------------------
SWIFT_CC(swift)
static void swift_taskGroup_attachChildImpl(TaskGroup *group,
                                            AsyncTask *child) {
  auto groupRecord = asImpl(group)->getTaskRecord();
  return groupRecord->attachChild(child);
}

// =============================================================================
// ==== destroy ----------------------------------------------------------------
SWIFT_CC(swift)
static void swift_taskGroup_destroyImpl(TaskGroup *group) {
  asImpl(group)->destroy();
}

void TaskGroupImpl::destroy() {
  // First, remove the group from the task and deallocate the record
  swift_task_removeStatusRecord(getTaskRecord());

  mutex.lock(); // TODO: remove lock, and use status for synchronization
  // Release all ready tasks which are kept retained, the group destroyed,
  // so no other task will ever await on them anymore;
  ReadyQueueItem item;
  bool taskDequeued = readyQueue.dequeue(item);
  while (taskDequeued) {
    swift_release(item.getTask());
    taskDequeued = readyQueue.dequeue(item);
  }
  mutex.unlock(); // TODO: remove fragment lock, and use status for synchronization
}

// =============================================================================
// ==== offer ------------------------------------------------------------------

void TaskGroup::offer(AsyncTask *completedTask, AsyncContext *context) {
  asImpl(this)->offer(completedTask, context);
}

bool TaskGroup::isCancelled() {
  return asImpl(this)->isCancelled();
}

static void fillGroupNextResult(TaskFutureWaitAsyncContext *context,
                                PollResult result) {
  /// Fill in the result value
  switch (result.status) {
  case PollStatus::MustWait:
    assert(false && "filling a waiting status?");
    return;

  case PollStatus::Error:
    context->fillWithError(reinterpret_cast<SwiftError*>(result.storage));
    return;

  case PollStatus::Success: {
    // Initialize the result as an Optional<Success>.
    const Metadata *successType = result.successType;
    OpaqueValue *destPtr = context->successResultPointer;
    // TODO: figure out a way to try to optimistically take the
    // value out of the finished task's future, if there are no
    // remaining references to it.
    successType->vw_initializeWithCopy(destPtr, result.storage);
    successType->vw_storeEnumTagSinglePayload(destPtr, 0, 1);
    return;
  }

  case PollStatus::Empty: {
    // Initialize the result as a nil Optional<Success>.
    const Metadata *successType = result.successType;
    OpaqueValue *destPtr = context->successResultPointer;
    successType->vw_storeEnumTagSinglePayload(destPtr, 1, 1);
    return;
  }
  }
}

void TaskGroupImpl::offer(AsyncTask *completedTask, AsyncContext *context) {
  assert(completedTask);
  assert(completedTask->isFuture());
  assert(completedTask->hasChildFragment());
  assert(completedTask->hasGroupChildFragment());
  assert(completedTask->groupChildFragment()->getGroup() == asAbstract(this));

  // We retain the completed task, because we will either:
  // - (a) schedule the waiter to resume on the next() that it is waiting on, or
  // - (b) will need to store this task until the group task enters next() and
  //       picks up this task.
  // either way, there is some time between us returning here, and the `completeTask`
  // issuing a swift_release on this very task. We need to keep it alive until
  // we have the chance to poll it from the queue (via the waiter task entering
  // calling next()).
  swift_retain(completedTask);

  mutex.lock(); // TODO: remove fragment lock, and use status for synchronization

  // Immediately increment ready count and acquire the status
  // Examples:
  //   W:n R:0 P:3 -> W:n R:1 P:3 // no waiter, 2 more pending tasks
  //   W:n R:0 P:1 -> W:n R:1 P:1 // no waiter, no more pending tasks
  //   W:n R:0 P:1 -> W:y R:1 P:1 // complete immediately
  //   W:n R:0 P:1 -> W:y R:1 P:3 // complete immediately, 2 more pending tasks
  auto assumed = statusAddReadyAssumeAcquire();

  auto asyncContextPrefix = reinterpret_cast<FutureAsyncContextPrefix *>(
      reinterpret_cast<char *>(context) - sizeof(FutureAsyncContextPrefix));
  bool hadErrorResult = false;
  auto errorObject = asyncContextPrefix->errorResult;
  if (errorObject) {
    // instead we need to enqueue this result:
    hadErrorResult = true;
  }

  // ==== a) has waiting task, so let us complete it right away
  if (assumed.hasWaitingTask()) {
    auto waitingTask = waitQueue.load(std::memory_order_acquire);
    while (true) {
      // ==== a) run waiting task directly -------------------------------------
      assert(assumed.hasWaitingTask());
      assert(assumed.pendingTasks() && "offered to group with no pending tasks!");
      // We are the "first" completed task to arrive,
      // and since there is a task waiting we immediately claim and complete it.
      if (waitQueue.compare_exchange_weak(
          waitingTask, nullptr,
          /*success*/ std::memory_order_release,
          /*failure*/ std::memory_order_acquire) &&
          statusCompletePendingReadyWaiting(assumed)) {
        // Run the task.
        auto result = PollResult::get(completedTask, hadErrorResult);

        mutex.unlock(); // TODO: remove fragment lock, and use status for synchronization

        auto waitingContext =
            static_cast<TaskFutureWaitAsyncContext *>(
                waitingTask->ResumeContext);

        fillGroupNextResult(waitingContext, result);

        _swift_tsan_acquire(static_cast<Job *>(waitingTask));

        // TODO: allow the caller to suggest an executor
        swift_task_enqueueGlobal(waitingTask);
        return;
      } // else, try again
    }
  }

  // ==== b) enqueue completion ------------------------------------------------
  //
  // else, no-one was waiting (yet), so we have to instead enqueue to the message
  // queue when a task polls during next() it will notice that we have a value
  // ready for it, and will process it immediately without suspending.
  assert(!waitQueue.load(std::memory_order_relaxed));

  // Retain the task while it is in the queue;
  // it must remain alive until the task group is alive.
  swift_retain(completedTask);
  auto readyItem = ReadyQueueItem::get(
      hadErrorResult ? ReadyStatus::Error : ReadyStatus::Success,
      completedTask
  );

  assert(completedTask == readyItem.getTask());
  assert(readyItem.getTask()->isFuture());
  readyQueue.enqueue(readyItem);
  mutex.unlock(); // TODO: remove fragment lock, and use status for synchronization
  return;
}

SWIFT_CC(swiftasync)
static void
task_group_wait_resume_adapter(SWIFT_ASYNC_CONTEXT AsyncContext *_context) {

  auto context = static_cast<TaskFutureWaitAsyncContext *>(_context);
  auto resumeWithError =
      reinterpret_cast<AsyncVoidClosureResumeEntryPoint *>(context->ResumeParent);
  return resumeWithError(context->Parent, context->errorResult);
}

#ifdef __ARM_ARCH_7K__
__attribute__((noinline))
SWIFT_CC(swiftasync) static void workaround_function_swift_taskGroup_wait_next_throwingImpl(
    OpaqueValue *result, SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
    TaskGroup *_group,
    ThrowingTaskFutureWaitContinuationFunction resumeFunction,
    AsyncContext *callContext) {
  // Make sure we don't eliminate calls to this function.
  asm volatile("" // Do nothing.
               :  // Output list, empty.
               : "r"(result), "r"(callerContext), "r"(_group) // Input list.
               : // Clobber list, empty.
  );
  return;
}
#endif

// =============================================================================
// ==== group.next() implementation (wait_next and groupPoll) ------------------

SWIFT_CC(swiftasync)
static void swift_taskGroup_wait_next_throwing_with_optionsImpl(
    OpaqueValue *resultPointer, SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
    TaskGroup *_group,
    TaskOptionRecord *options,
    ThrowingTaskFutureWaitContinuationFunction *resumeFunction,
    AsyncContext *rawContext) {
  auto waitingTask = swift_task_getCurrent();
  waitingTask->ResumeTask = task_group_wait_resume_adapter;
  waitingTask->ResumeContext = rawContext;

  auto context = static_cast<TaskFutureWaitAsyncContext *>(rawContext);
  context->ResumeParent =
      reinterpret_cast<TaskContinuationFunction *>(resumeFunction);
  context->Parent = callerContext;
  context->errorResult = nullptr;
  context->successResultPointer = resultPointer;

  auto group = asImpl(_group);
  assert(group && "swift_taskGroup_wait_next_throwing was passed context without group!");

  PollResult polled = group->poll(waitingTask);
  switch (polled.status) {
  case PollStatus::MustWait:
    // The waiting task has been queued on the channel,
    // there were pending tasks so it will be woken up eventually.
#ifdef __ARM_ARCH_7K__
    return workaround_function_swift_taskGroup_wait_next_throwingImpl(
        resultPointer, callerContext, _group, resumeFunction, rawContext);
#else
    return;
#endif

  case PollStatus::Empty:
  case PollStatus::Error:
  case PollStatus::Success:
    fillGroupNextResult(context, polled);
    return waitingTask->runInFullyEstablishedContext();
  }
}

SWIFT_CC(swiftasync)
static void swift_taskGroup_wait_next_throwingImpl(
    OpaqueValue *resultPointer, SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
    TaskGroup *_group,
    ThrowingTaskFutureWaitContinuationFunction *resumeFunction,
    AsyncContext *rawContext) {
  swift_taskGroup_wait_next_throwingImpl(
      resultPointer, callerContext,
      _group,
      resumeFunction,
      rawContext);
}

PollResult TaskGroupImpl::poll(AsyncTask *waitingTask) {
  mutex.lock(); // TODO: remove group lock, and use status for synchronization
  auto assumed = statusMarkWaitingAssumeAcquire();

  PollResult result;
  result.storage = nullptr;
  result.successType = nullptr;
  result.retainedTask = nullptr;

  // ==== 1) bail out early if no tasks are pending ----------------------------
  if (assumed.isEmpty()) {
    // No tasks in flight, we know no tasks were submitted before this poll
    // was issued, and if we parked here we'd potentially never be woken up.
    // Bail out and return `nil` from `group.next()`.
    statusRemoveWaiting();
    result.status = PollStatus::Empty;
    result.successType = this->successType;
    mutex.unlock(); // TODO: remove group lock, and use status for synchronization
    return result;
  }

  auto waitHead = waitQueue.load(std::memory_order_acquire);

  // ==== 2) Ready task was polled, return with it immediately -----------------
  if (assumed.readyTasks()) {
    auto assumedStatus = assumed.status;
    auto newStatus = TaskGroupImpl::GroupStatus{assumedStatus};
    if (status.compare_exchange_weak(
        assumedStatus, newStatus.completingPendingReadyWaiting().status,
        /*success*/ std::memory_order_relaxed,
        /*failure*/ std::memory_order_acquire)) {

      // Success! We are allowed to poll.
      ReadyQueueItem item;
      bool taskDequeued = readyQueue.dequeue(item);
      if (!taskDequeued) {
        result.status = PollStatus::MustWait;
        result.storage = nullptr;
        result.successType = nullptr;
        result.retainedTask = nullptr;
        mutex.unlock(); // TODO: remove group lock, and use status for synchronization
        return result;
      }

      assert(item.getTask()->isFuture());
      auto futureFragment = item.getTask()->futureFragment();

      // Store the task in the result, so after we're done processing it it may
      // be swift_release'd; we kept it alive while it was in the readyQueue by
      // an additional retain issued as we enqueued it there.
      result.retainedTask = item.getTask();
      switch (item.getStatus()) {
        case ReadyStatus::Success:
          // Immediately return the polled value
          result.status = PollStatus::Success;
          result.storage = futureFragment->getStoragePtr();
          result.successType = futureFragment->getResultType();
          assert(result.retainedTask && "polled a task, it must be not null");
          _swift_tsan_acquire(static_cast<Job *>(result.retainedTask));
          mutex.unlock(); // TODO: remove fragment lock, and use status for synchronization
          return result;

        case ReadyStatus::Error:
          // Immediately return the polled value
          result.status = PollStatus::Error;
          result.storage =
              reinterpret_cast<OpaqueValue *>(futureFragment->getError());
          result.successType = nullptr;
          assert(result.retainedTask && "polled a task, it must be not null");
          _swift_tsan_acquire(static_cast<Job *>(result.retainedTask));
          mutex.unlock(); // TODO: remove fragment lock, and use status for synchronization
          return result;

        case ReadyStatus::Empty:
          result.status = PollStatus::Empty;
          result.storage = nullptr;
          result.retainedTask = nullptr;
          result.successType = this->successType;
          mutex.unlock(); // TODO: remove fragment lock, and use status for synchronization
          return result;
      }
      assert(false && "must return result when status compare-and-swap was successful");
    } // else, we failed status-cas (some other waiter claimed a ready pending task, try again)
  }

  // ==== 3) Add to wait queue -------------------------------------------------
  assert(assumed.readyTasks() == 0);
  _swift_tsan_release(static_cast<Job *>(waitingTask));
  while (true) {
    // Put the waiting task at the beginning of the wait queue.
    if (waitQueue.compare_exchange_weak(
        waitHead, waitingTask,
        /*success*/ std::memory_order_release,
        /*failure*/ std::memory_order_acquire)) {
      mutex.unlock(); // TODO: remove fragment lock, and use status for synchronization
      // no ready tasks, so we must wait.
      result.status = PollStatus::MustWait;
      return result;
    } // else, try again
  }
  assert(false && "must successfully compare exchange the waiting task.");
}

// =============================================================================
// ==== isEmpty ----------------------------------------------------------------
SWIFT_CC(swift)
static bool swift_taskGroup_isEmptyImpl(TaskGroup *group) {
  return asImpl(group)->isEmpty();
}

// =============================================================================
// ==== isCancelled ------------------------------------------------------------
SWIFT_CC(swift)
static bool swift_taskGroup_isCancelledImpl(TaskGroup *group) {
  return asImpl(group)->isCancelled();
}

// =============================================================================
// ==== cancelAll --------------------------------------------------------------
SWIFT_CC(swift)
static void swift_taskGroup_cancelAllImpl(TaskGroup *group) {
  asImpl(group)->cancelAll();
}

bool TaskGroupImpl::cancelAll() {
  // store the cancelled bit
  auto old = statusCancel();
  if (old.isCancelled()) {
    // already was cancelled previously, nothing to do?
    return false;
  }

  // cancel all existing tasks within the group
  swift_task_cancel_group_child_tasks(asAbstract(this));
  return true;
}

// =============================================================================
// ==== addPending -------------------------------------------------------------
SWIFT_CC(swift)
static bool swift_taskGroup_addPendingImpl(TaskGroup *group, bool unconditionally) {
  auto assumedStatus = asImpl(group)->statusAddPendingTaskRelaxed(unconditionally);
  return !assumedStatus.isCancelled();
}

#define OVERRIDE_TASK_GROUP COMPATIBILITY_OVERRIDE
#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH
