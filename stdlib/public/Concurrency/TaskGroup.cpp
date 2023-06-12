//===--- TaskGroup.cpp - Task Groups --------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
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

#include "Debug.h"
#include "TaskGroupPrivate.h"
#include "TaskPrivate.h"
#include "bitset"
#include "queue" // TODO: remove and replace with usage of our mpsc queue
#include "string"
#include "swift/ABI/HeapObject.h"
#include "swift/ABI/Metadata.h"
#include "swift/ABI/Task.h"
#include "swift/ABI/TaskGroup.h"
#include "swift/Basic/RelativePointer.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Runtime/Concurrency.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Threading/Mutex.h"
#include <atomic>
#include <new>

#if !SWIFT_STDLIB_SINGLE_THREADED_CONCURRENCY
#include <mutex>
#endif

#if SWIFT_STDLIB_HAS_ASL
#include <asl.h>
#elif defined(__ANDROID__)
#include <android/log.h>
#endif

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#if defined(_WIN32)
#include <io.h>
#endif

#include <assert.h>
#if SWIFT_CONCURRENCY_ENABLE_DISPATCH
#include <dispatch/dispatch.h>
#endif

#if !defined(_WIN32) && !defined(__wasi__) && __has_include(<dlfcn.h>)
#include <dlfcn.h>
#endif

using namespace swift;

#if 0
#define SWIFT_TASK_GROUP_DEBUG_LOG(group, fmt, ...)                     \
fprintf(stderr, "[%#lx] [%s:%d][group(%p%s)] (%s) " fmt "\n",           \
      (unsigned long)Thread::current().platformThreadId(),              \
      __FILE__, __LINE__,                                               \
      group, group->isDiscardingResults() ? ",discardResults" : "",     \
      __FUNCTION__,                                                     \
      __VA_ARGS__)

#define SWIFT_TASK_GROUP_DEBUG_LOG_0(group, fmt, ...)                   \
fprintf(stderr, "[%#lx] [%s:%d][group(%p)] (%s) " fmt "\n",             \
      (unsigned long)Thread::current().platformThreadId(),              \
      __FILE__, __LINE__,                                               \
      group,                                                            \
      __FUNCTION__,                                                     \
      __VA_ARGS__)
#else
#define SWIFT_TASK_GROUP_DEBUG_LOG(group, fmt, ...) (void)0
#define SWIFT_TASK_GROUP_DEBUG_LOG_0(group, fmt, ...) (void)0
#endif

using FutureFragment = AsyncTask::FutureFragment;

/// During evolution discussions we opted to implement the following semantic of
/// a discarding task-group throw:
/// - the error thrown out of withThrowingDiscardingTaskGroup { ... } always "wins",
///   even if the group already had an error stored within.
///
/// This is harder to implement, since we have to always store the "first error from children",
/// and keep it around until body completes, and only then are we able to decide which error to
/// re-throw; If we threw the body task, we must swift_release the stored "first child error" (if it was present).
///
/// Implementation of "rethrow the first child error" just works in `waitAll`,
/// since we poll the error and resume the waiting task with it immediately.
///
/// Change this flag, or expose a boolean to offer developers a choice of behavior.
#define SWIFT_TASK_GROUP_BODY_THROWN_ERROR_WINS 1

namespace {
class TaskStatusRecord;
struct TaskGroupStatus;

class AccumulatingTaskGroup;
class DiscardingTaskGroup;

/*****************************************************************************/
/************************** QUEUE IMPL ***************************************/
/*****************************************************************************/

template<typename T>
class NaiveTaskGroupQueue {
  std::queue <T> queue;

public:
  NaiveTaskGroupQueue() = default;

  NaiveTaskGroupQueue(const NaiveTaskGroupQueue<T> &) = delete;

  NaiveTaskGroupQueue &operator=(const NaiveTaskGroupQueue<T> &) = delete;

  NaiveTaskGroupQueue(NaiveTaskGroupQueue<T> &&other) {
    queue = std::move(other.queue);
  }

  ~NaiveTaskGroupQueue() {}

  bool dequeue(T &output) {
    if (queue.empty()) {
      return false;
    }
    output = queue.front();
    queue.pop();
    return true;
  }

  bool isEmpty() const {
    return queue.empty();
  }

  void enqueue(const T item) {
    queue.push(item);
  }
};

/******************************************************************************/
/*************************** TASK GROUP BASE **********************************/
/******************************************************************************/

class TaskGroupBase : public TaskGroupTaskStatusRecord {
public:

  /// Describes the status of the group.
  enum class ReadyStatus : uintptr_t {
    /// The task group is empty, no tasks are pending.
    /// Return immediately, there is no point in suspending.
    ///
    /// The storage is not accessible.
    Empty = 0b00,

    /// A raw SwiftError is stored in the item's storage, rather than a Task with an Error inside.
    ///
    /// Only used by DiscardingTaskGroup.
    RawError = 0b01,

    /// The future has completed with result (of type \c resultType).
    ///
    /// Only used by AccumulatingTaskGroup.
    Success = 0b10,

    /// The future has completed by throwing an error (an \c Error existential).
    ///
    /// Only used by AccumulatingTaskGroup.
    Error = 0b11,
  };

  /// Status of a poll, i.e. is there a result we can return, or do we have to suspend.
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

  /// The result of waiting on a task group.
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

    static PollResult get(AsyncTask *asyncTask, bool hadErrorResult) {
      auto fragment = asyncTask->futureFragment();
      return PollResult{
          /*status=*/hadErrorResult ?
                     PollStatus::Error :
                     PollStatus::Success,
          /*storage=*/hadErrorResult ?
                      reinterpret_cast<OpaqueValue *>(fragment->getError()) :
                      fragment->getStoragePtr(),
          /*successType=*/fragment->getResultType(),
          /*retainedTask==*/asyncTask
      };
    }

    static PollResult getEmpty(const Metadata *successType) {
      return PollResult{
          /*status*/PollStatus::Empty,
          /*storage*/nullptr,
          /*successType*/successType,
          /*task*/nullptr
      };
    }

    static PollResult getError(SwiftError *error) {
      assert(error);
      return PollResult{
          /*status*/PollStatus::Error,
          /*storage*/reinterpret_cast<OpaqueValue *>(error),
          /*successType*/nullptr,
          /*task*/nullptr
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
      assert(getStatus() != ReadyStatus::RawError && "storage did contain raw error pointer, not task!");
      return reinterpret_cast<AsyncTask *>(storage & ~statusMask);
    }

    SwiftError *getRawError(DiscardingTaskGroup *group) const {
      assert(group && "only a discarding task group uses raw errors in the ready queue");
      assert(getStatus() == ReadyStatus::RawError && "storage did not contain raw error pointer!");
      return reinterpret_cast<SwiftError *>(storage & ~statusMask);
    }

    static ReadyQueueItem get(ReadyStatus status, AsyncTask *task) {
      assert(task == nullptr || task->isFuture());
      return ReadyQueueItem{
          reinterpret_cast<uintptr_t>(task) | static_cast<uintptr_t>(status)};
    }

    static ReadyQueueItem getRawError(DiscardingTaskGroup *group, SwiftError *error) {
      assert(group && "only a discarding task group uses raw errors in the ready queue");
      return ReadyQueueItem{
          reinterpret_cast<uintptr_t>(error) | static_cast<uintptr_t>(ReadyStatus::RawError)};
    }
  };

protected:
#if SWIFT_STDLIB_SINGLE_THREADED_CONCURRENCY || SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
  // Synchronization is simple here. In a single threaded mode, all swift tasks
  // run on a single thread so no coordination is needed. In a task-to-thread
  // model, only the parent task which created the task group can
  //
  //   (a) add child tasks to a group
  //   (b) run the child tasks
  //
  // So we shouldn't need to worry about coordinating between child tasks and
  // parents in a task group
  void lock() const {}
  void unlock() const {}
#else
  // TODO: move to lockless via the status atomic (make readyQueue an mpsc_queue_t<ReadyQueueItem>)
  mutable std::mutex mutex_;

  void lock() const { mutex_.lock(); }
  void unlock() const { mutex_.unlock(); }
#endif

  /// Used for queue management, counting number of waiting and ready tasks
  std::atomic<uint64_t> status;

  /// The task currently waiting on `group.next()`.  Since only the owning
  /// task can ever be waiting on a group, this is just either a reference
  /// to that task or null.
  std::atomic<AsyncTask *> waitQueue;

  /// Queue containing completed tasks offered into this group.
  ///
  /// The low bits contain the status, the rest of the pointer is the
  /// AsyncTask.
  NaiveTaskGroupQueue<ReadyQueueItem> readyQueue;

  const Metadata *successType;

  explicit TaskGroupBase(const Metadata* T, uint64_t initialStatus)
    : TaskGroupTaskStatusRecord(),
      status(initialStatus),
      waitQueue(nullptr),
      readyQueue(),
      successType(T) {}

  TaskGroupBase(const TaskGroupBase &) = delete;

public:
  virtual ~TaskGroupBase() {}

  TaskStatusRecordKind getKind() const {
    return Flags.getKind();
  }

  /// Destroy the storage associated with the group.
  virtual void destroy() = 0;

  bool isAccumulatingResults() const {
    return !isDiscardingResults();
  }
  virtual bool isDiscardingResults() const = 0;

  /// Any TaskGroup always IS its own TaskRecord.
  /// This allows us to easily get the group while cancellation is propagated throughout the task tree.
  TaskGroupTaskStatusRecord *getTaskRecord() {
    return static_cast<TaskGroupTaskStatusRecord *>(this);
  }

  // ==== Queue operations ----------------------------------------------------

  /// Offer result of a task into this task group.
  ///
  /// If possible, and an existing task is already waiting on next(), this will
  /// schedule it immediately. If not, the result is enqueued and will be picked
  /// up whenever a task calls next() the next time.
  virtual void offer(AsyncTask *completed, AsyncContext *context) = 0;

  /// Attempt to park the `waitingTask` in the waiting queue.
  ///
  /// If unable to complete the waiting task immediately (with an readily
  /// available completed task), either returns an `PollStatus::Empty`
  /// result if it is known that there are no pending tasks in the group,
  /// or a `PollStatus::MustWait` result if there are tasks in flight
  /// and the waitingTask eventually be woken up by a completion.
  ///
  /// A `discardResults` TaskGroup is not able to wait on individual completions,
  /// instead, it can only await on "all pending tasks have been processed".
  ///
  /// There can be only at-most-one waiting task on a group at any given time,
  /// and the waiting task is expected to be the parent task in which the group
  /// body is running.
  ///
  /// \param bodyError error thrown by the body of a with...TaskGroup method
  /// \param waitingTask the task waiting on the group
  /// \param rawContext used to resume the waiting task
  void waitAll(SwiftError* bodyError, AsyncTask *waitingTask,
                     OpaqueValue *resultPointer, SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
                     ThrowingTaskFutureWaitContinuationFunction *resumeFunction,
                     AsyncContext *rawContext);

  // Enqueue the completed task onto ready queue if there are no waiting tasks yet
  virtual void enqueueCompletedTask(AsyncTask *completedTask, bool hadErrorResult) = 0;

  /// Resume waiting task with result from `completedTask`
  void resumeWaitingTask(AsyncTask *completedTask,
                         TaskGroupStatus &assumed,
                         bool hadErrorResult,
                         bool alreadyDecremented = false,
                         bool taskWasRetained = false);

  // ==== Status manipulation -------------------------------------------------

  TaskGroupStatus statusLoadRelaxed() const;
  TaskGroupStatus statusLoadAcquire() const;

  std::string statusString() const;

  bool isEmpty() const;

  uint64_t pendingTasks() const;

  /// Compare-and-set old status to a status derived from the old one,
  /// by simultaneously decrementing one Pending and one Waiting tasks.
  ///
  /// This is used to atomically perform a waiting task completion.
  /// The change is made 'relaxed' and may have to be retried.
  ///
  /// This can be safely used in a discarding task group as well,
  /// where the "ready" change will simply be ignored, since there
  /// are no ready bits to change.
  bool statusCompletePendingReadyWaiting(TaskGroupStatus &old);

  /// Cancel the task group and all tasks within it.
  ///
  /// Returns `true` if this is the first time cancelling the group, false otherwise.
  bool isCancelled() const;

  /// Set waiting status bit.
  ///
  /// Returns *assumed* new status, including the just performed +1.
  TaskGroupStatus statusMarkWaitingAssumeAcquire();

  /// Remove waiting status bit.
  TaskGroupStatus statusRemoveWaitingRelease();

  /// Mark the waiting status bit.
  /// A waiting task MUST have been already enqueued in the `waitQueue`.
  TaskGroupStatus statusMarkWaitingAssumeRelease();

  TaskGroupStatus statusAddPendingTaskAssumeRelaxed(bool unconditionally);

  /// Cancels the group and returns true if was already cancelled before.
  /// After this function returns, the group is guaranteed to be cancelled.
  ///
  /// Prefer calling cancelAll if the intent is to cancel the group and all of its children.
  ///
  /// \return true, if the group was already cancelled before, and false if it wasn't cancelled before (but now is).
  bool statusCancel();

  /// Cancel the group and all of its child tasks recursively.
  /// This also sets
  bool cancelAll();

};

[[maybe_unused]]
static std::string to_string(TaskGroupBase::PollStatus status) {
  switch (status) {
    case TaskGroupBase::PollStatus::Empty: return "Empty";
    case TaskGroupBase::PollStatus::MustWait: return "MustWait";
    case TaskGroupBase::PollStatus::Success: return "Success";
    case TaskGroupBase::PollStatus::Error: return "Error";
  }
}

/// The status of a task group.
///
/// Its exact structure depends on the type of group, and therefore a group must be passed to operations
/// which may be touching the 'ready' bits; Only an "accumulating" task group maintains the 'ready' count,
/// while all kinds of group use the 'pending' count (with varying width though).
///
/// Accumulating group status:
///     [1:cancelled][1:waiting][31:ready count][31:pending count]
/// Discarding group status:
///     [1:cancelled][1:waiting][62:pending count]
struct TaskGroupStatus {
  static const uint64_t cancelled               = 0b1000000000000000000000000000000000000000000000000000000000000000;
  static const uint64_t waiting                 = 0b0100000000000000000000000000000000000000000000000000000000000000;

  // 31 bits for ready tasks counter
  static const uint64_t maskReady               = 0b0011111111111111111111111111111110000000000000000000000000000000;
  static const uint64_t oneReadyTask            = 0b0000000000000000000000000000000010000000000000000000000000000000;

  // 31 bits for pending tasks counter, while accumulating results (default mode)
  static const uint64_t maskAccumulatingPending = 0b0000000000000000000000000000000001111111111111111111111111111111;
  // 62 bits for pending tasks counter, while discarding results (discardResults)
  static const uint64_t maskDiscardingPending   = 0b0011111111111111111111111111111111111111111111111111111111111111;
  static const uint64_t onePendingTask          = 0b0000000000000000000000000000000000000000000000000000000000000001;

  /// Depending on kind of task group, we can either support 2^31 or 2^62 pending tasks.
  ///
  /// While a discarding task group's max pending count is unrealistic to be exceeded, the lower
  /// maximum number used in an accumulating task group has potential to be exceeded, and thus we must crash
  /// rather than start overflowing status if this were to happen.
  static uint64_t maximumPendingTasks(TaskGroupBase* group) {
    if (group->isAccumulatingResults()) {
      return maskAccumulatingPending;
    } else {
      return maskDiscardingPending;
    }
  }

  uint64_t status;

  bool isCancelled() {
    return (status & cancelled) > 0;
  }

  bool hasWaitingTask() {
    return (status & waiting) > 0;
  }

  unsigned int readyTasks(const TaskGroupBase* _Nonnull group) {
    assert(group->isAccumulatingResults()
           && "attempted to check ready tasks on group that does not accumulate results!");
    return (status & maskReady) >> 31;
  }

  uint64_t pendingTasks(const TaskGroupBase* _Nonnull group) {
    if (group->isAccumulatingResults()) {
      return (status & maskAccumulatingPending);
    } else {
      return (status & maskDiscardingPending);
    }
  }

  bool isEmpty(const TaskGroupBase *group) {
    return pendingTasks(group) == 0;
  }

  /// Status value decrementing the Ready, Pending and Waiting counters by one.
  TaskGroupStatus completingPendingReadyWaiting(const TaskGroupBase* _Nonnull group) {
    assert(pendingTasks(group) &&
           "can only complete waiting task when pending tasks available");
    assert(group->isDiscardingResults() || readyTasks(group) &&
                                           "can only complete waiting task when ready tasks available");
    assert(hasWaitingTask() &&
           "can only complete waiting task when waiting task available");
    uint64_t change = waiting + onePendingTask;
    // only while accumulating results does the status contain "ready" bits;
    // so if we're in "discard results" mode, we must not decrement the ready count,
    // as there is no ready count in the status.
    change += group->isAccumulatingResults() ? oneReadyTask : 0;
    return TaskGroupStatus{status - change};
  }

  TaskGroupStatus completingPendingReady(const TaskGroupBase* _Nonnull group) {
    assert(pendingTasks(group) &&
           "can only complete waiting task when pending tasks available");
    assert(group->isDiscardingResults() || readyTasks(group) &&
                                           "can only complete waiting task when ready tasks available");
    auto change = onePendingTask;
    change += group->isAccumulatingResults() ? oneReadyTask : 0;
    return TaskGroupStatus{status - change};
  }

  TaskGroupStatus asCancelled(bool cancel) {
    return TaskGroupStatus{status | (cancel ? cancelled : 0)};
  }

  static void reportPendingTaskOverflow(TaskGroupBase* group, TaskGroupStatus status) {
    char *message;
    swift_asprintf(
        &message,
        "error: %sTaskGroup: detected pending task count overflow, in task group %p! Status: %s",
        group->isDiscardingResults() ? "Discarding" : "", group, status.to_string(group).c_str());

    if (_swift_shouldReportFatalErrorsToDebugger()) {
      RuntimeErrorDetails details = {
          .version = RuntimeErrorDetails::currentVersion,
          .errorType = "task-group-violation",
          .currentStackDescription = "TaskGroup exceeded supported pending task count",
          .framesToSkip = 1,
      };
      _swift_reportToDebugger(RuntimeErrorFlagFatal, message, &details);
    }

#if defined(_WIN32)
    #define STDERR_FILENO 2
   _write(STDERR_FILENO, message, strlen(message));
#elif defined(STDERR_FILENO)
    write(STDERR_FILENO, message, strlen(message));
#endif
#if defined(SWIFT_STDLIB_HAS_ASL)
    asl_log(nullptr, nullptr, ASL_LEVEL_ERR, "%s", message);
#elif defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_FATAL, "SwiftRuntime", "%s", message);
#endif

    free(message);
    abort();
  }

  /// Pretty prints the status, as follows:
  /// If accumulating results:
  ///     TaskGroupStatus{ C:{cancelled} W:{waiting task} R:{ready tasks} P:{pending tasks} {binary repr} }
  /// If discarding results:
  ///     TaskGroupStatus{ C:{cancelled} W:{waiting task} P:{pending tasks} {binary repr} }
  std::string to_string(const TaskGroupBase* _Nullable group) {
    std::string str;
    str.append("TaskGroupStatus{ ");
    str.append("C:"); // cancelled
    str.append(isCancelled() ? "y" : "n");
    str.append(" W:"); // has waiting task
    str.append(hasWaitingTask() ? "y" : "n");
    if (group && group->isAccumulatingResults()) {
      str.append(" R:"); // ready
      str.append(std::to_string(readyTasks(group)));
    }
    str.append(" P:"); // pending
    str.append(std::to_string(pendingTasks(group)));
    str.append(" " + std::bitset<64>(status).to_string());
    str.append(" }");
    return str;
  }

  /// Initially there are no waiting and no pending tasks.
  static const TaskGroupStatus initial() {
    return TaskGroupStatus{0};
  };
};

bool TaskGroupBase::statusCompletePendingReadyWaiting(TaskGroupStatus &old) {
  return status.compare_exchange_strong(
      old.status, old.completingPendingReadyWaiting(this).status,
      /*success*/ std::memory_order_relaxed,
      /*failure*/ std::memory_order_relaxed);
}

bool TaskGroupBase::isCancelled() const {
  auto old = TaskGroupStatus{status.load(std::memory_order_relaxed)};
  return old.isCancelled();
}

TaskGroupStatus TaskGroupBase::statusLoadRelaxed() const {
  return TaskGroupStatus{status.load(std::memory_order_relaxed)};
}

TaskGroupStatus TaskGroupBase::statusLoadAcquire() const {
  return TaskGroupStatus{status.load(std::memory_order_acquire)};
}

std::string TaskGroupBase::statusString() const {
  return statusLoadRelaxed().to_string(this);
}

bool TaskGroupBase::isEmpty() const {
  auto oldStatus = TaskGroupStatus{status.load(std::memory_order_relaxed)};
  return oldStatus.pendingTasks(this) == 0;
}

uint64_t TaskGroupBase::pendingTasks() const {
  auto s = TaskGroupStatus{status.load(std::memory_order_relaxed)};
  return s.pendingTasks(this);
}

TaskGroupStatus TaskGroupBase::statusMarkWaitingAssumeAcquire() {
  auto old = status.fetch_or(TaskGroupStatus::waiting, std::memory_order_acquire);
  return TaskGroupStatus{old | TaskGroupStatus::waiting};
}

TaskGroupStatus TaskGroupBase::statusMarkWaitingAssumeRelease() {
  auto old = status.fetch_or(TaskGroupStatus::waiting,
                             std::memory_order_release);
  return TaskGroupStatus{old | TaskGroupStatus::waiting};
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
TaskGroupStatus TaskGroupBase::statusAddPendingTaskAssumeRelaxed(bool unconditionally) {
  auto old = status.fetch_add(TaskGroupStatus::onePendingTask,
                              std::memory_order_relaxed);
  auto s = TaskGroupStatus{old + TaskGroupStatus::onePendingTask};

  if (s.pendingTasks(this) == TaskGroupStatus::maximumPendingTasks(this)) {
    TaskGroupStatus::reportPendingTaskOverflow(this, s); // this will abort()
  }

  if (!unconditionally && s.isCancelled()) {
    // revert that add, it was meaningless
    auto o = status.fetch_sub(TaskGroupStatus::onePendingTask,
                              std::memory_order_relaxed);
    s = TaskGroupStatus{o - TaskGroupStatus::onePendingTask};
  }

  SWIFT_TASK_GROUP_DEBUG_LOG(this, "addPending, after: %s", s.to_string(this).c_str());
  return s;
}

TaskGroupStatus TaskGroupBase::statusRemoveWaitingRelease() {
  auto old = status.fetch_and(~TaskGroupStatus::waiting,
                              std::memory_order_release);
  return TaskGroupStatus{old};
}

bool TaskGroupBase::statusCancel() {
  /// The cancelled bit is always the same, the first one, between all task group implementations:
  const uint64_t cancelled = TaskGroupStatus::cancelled;
  auto old = status.fetch_or(cancelled, std::memory_order_relaxed);

  // return if the status was already cancelled before we flipped it or not
  return old & cancelled;
}

/******************************************************************************/
/*************** ACCUMULATING (DEFAULT) TASK GROUP ****************************/
/******************************************************************************/

/// The default TaskGroup implementation, which accumulates results until they are consumed using `await next()`.
class AccumulatingTaskGroup: public TaskGroupBase {
  friend class ::swift::AsyncTask;

public:

  explicit AccumulatingTaskGroup(const Metadata *T)
    : TaskGroupBase(T, TaskGroupStatus::initial().status) {}

  virtual void destroy() override;

  virtual ~AccumulatingTaskGroup() {}

  virtual bool isDiscardingResults() const override {
    return false;
  }

  /// Returns *assumed* new status.
  ///
  /// If the group is not accumulating results, the "ready" count does not exist,
  /// and this is just a plan load().
  TaskGroupStatus statusAddReadyAssumeAcquire() {
    auto old = status.fetch_add(TaskGroupStatus::oneReadyTask,
                                std::memory_order_acquire);
    auto s = TaskGroupStatus{old + TaskGroupStatus::oneReadyTask};
    assert(s.readyTasks(this) <= s.pendingTasks(this));
    return s;
  }

  virtual void offer(AsyncTask *completed, AsyncContext *context) override;

  virtual void enqueueCompletedTask(AsyncTask *completedTask, bool hadErrorResult) override;

  /// Attempt to dequeue ready tasks and complete the waitingTask.
  ///
  /// If unable to complete the waiting task immediately (with an readily
  /// available completed task), either returns an `PollStatus::Empty`
  /// result if it is known that no pending tasks in the group,
  /// or a `PollStatus::MustWait` result if there are tasks in flight
  /// and the waitingTask eventually be woken up by a completion.
  PollResult poll(AsyncTask *waitingTask);

};

/******************************************************************************/
/********************** DISCARDING TASK GROUP *********************************/
/******************************************************************************/

class DiscardingTaskGroup: public TaskGroupBase {
  friend class ::swift::AsyncTask;

public:

  explicit DiscardingTaskGroup(const Metadata *T)
    : TaskGroupBase(T, TaskGroupStatus::initial().status) {}

  virtual void destroy() override;

  virtual ~DiscardingTaskGroup() {}

  virtual bool isDiscardingResults() const override {
    return true;
  }

  /// Returns *assumed* new status.
  TaskGroupStatus statusAddReadyAssumeAcquire(const DiscardingTaskGroup *group) {
    assert(group->isDiscardingResults());
    return TaskGroupStatus{status.load(std::memory_order_acquire)};
  }

  TaskGroupStatus statusLoadRelaxed() {
    return TaskGroupStatus{status.load(std::memory_order_relaxed)};
  }

  TaskGroupStatus statusLoadAcquire() {
    return TaskGroupStatus{status.load(std::memory_order_acquire)};
  }

  /// Compare-and-set old status to a status derived from the old one,
  /// by simultaneously decrementing one Pending and one Waiting tasks.
  ///
  /// This is used to atomically perform a waiting task completion.
  bool statusCompletePendingReadyWaiting(TaskGroupStatus &old) {
    return status.compare_exchange_strong(
      old.status, old.completingPendingReadyWaiting(this).status,
      /*success*/ std::memory_order_relaxed,
      /*failure*/ std::memory_order_relaxed);
  }

  /// Decrement the pending status count.
  /// Returns the *assumed* new status, including the just performed -1.
  TaskGroupStatus statusCompletePendingAssumeRelease() {
    auto old = status.fetch_sub(TaskGroupStatus::onePendingTask,
                                std::memory_order_release);
    assert(TaskGroupStatus{old}.pendingTasks(this) > 0 && "attempted to decrement pending count when it was 0 already");
    return TaskGroupStatus{old - TaskGroupStatus::onePendingTask};
  }

  virtual void offer(AsyncTask *completed, AsyncContext *context) override;

  virtual void enqueueCompletedTask(AsyncTask *completedTask, bool hadErrorResult) override;

  /// Attempt to dequeue ready tasks and complete the waitingTask.
  ///
  /// If unable to complete the waiting task immediately (with an readily
  /// available completed task), either returns an `PollStatus::Empty`
  /// result if it is known that no pending tasks in the group,
  /// or a `PollStatus::MustWait` result if there are tasks in flight
  /// and the waitingTask eventually be woken up by a completion.
  PollResult poll(AsyncTask *waitingTask);

private:
  /// Resume waiting task with specified error
  void resumeWaitingTaskWithError(SwiftError *error,
                                  TaskGroupStatus &assumed,
                                  bool alreadyDecremented);
};

} // end anonymous namespace

/******************************************************************************/
/************************ TASK GROUP PUBLIC API *******************************/
/******************************************************************************/

using ReadyQueueItem = TaskGroupBase::ReadyQueueItem;
using ReadyStatus = TaskGroupBase::ReadyStatus;
using PollResult = TaskGroupBase::PollResult;
using PollStatus = TaskGroupBase::PollStatus;

static_assert(sizeof(AccumulatingTaskGroup) <= sizeof(TaskGroup) &&
              alignof(AccumulatingTaskGroup) <= alignof(TaskGroup),
              "TaskGroupBase doesn't fit in TaskGroup");

static_assert(sizeof(DiscardingTaskGroup) <= sizeof(TaskGroup) &&
              alignof(DiscardingTaskGroup) <= alignof(TaskGroup),
              "DiscardingTaskGroup doesn't fit in TaskGroup");

static TaskGroupBase *asBaseImpl(TaskGroup *group) {
  return reinterpret_cast<TaskGroupBase*>(group);
}
static AccumulatingTaskGroup *asAccumulatingImpl(TaskGroupBase *group) {
  assert(group->isAccumulatingResults());
  return static_cast<AccumulatingTaskGroup*>(group);
}
static AccumulatingTaskGroup *asAccumulatingImpl(TaskGroup *group) {
  assert(group->isAccumulatingResults());
  return asAccumulatingImpl(asBaseImpl(group));
}
static DiscardingTaskGroup *asDiscardingImpl(TaskGroupBase *group) {
  assert(group->isDiscardingResults());
  return static_cast<DiscardingTaskGroup*>(group);
}
[[maybe_unused]]
static DiscardingTaskGroup *asDiscardingImpl(TaskGroup *group) {
  assert(group->isDiscardingResults());
  return asDiscardingImpl(asBaseImpl(group));
}

static TaskGroup *asAbstract(TaskGroupBase *group) {
  return reinterpret_cast<TaskGroup*>(group);
}
static TaskGroup *asAbstract(AccumulatingTaskGroup *group) {
  return reinterpret_cast<TaskGroup*>(group);
}
static TaskGroup *asAbstract(DiscardingTaskGroup *group) {
  return reinterpret_cast<TaskGroup*>(group);
}

TaskGroupTaskStatusRecord *TaskGroup::getTaskRecord() {
  return asBaseImpl(this)->getTaskRecord();
}

bool TaskGroup::isDiscardingResults() {
  return asBaseImpl(this)->isDiscardingResults();
}

TaskGroup* TaskGroupTaskStatusRecord::getGroup() {
  return reinterpret_cast<TaskGroup *>(static_cast<TaskGroupBase*>(this));
}

// =============================================================================
// ==== initialize -------------------------------------------------------------

// Initializes into the preallocated _group an actual TaskGroupBase.
SWIFT_CC(swift)
static void swift_taskGroup_initializeImpl(TaskGroup *group, const Metadata *T) {
  swift_taskGroup_initializeWithFlags(0, group, T);
}

// Initializes into the preallocated _group an actual instance.
SWIFT_CC(swift)
static void swift_taskGroup_initializeWithFlagsImpl(size_t rawGroupFlags,
                                                    TaskGroup *group, const Metadata *T) {

  TaskGroupFlags groupFlags(rawGroupFlags);
  SWIFT_TASK_GROUP_DEBUG_LOG_0(group, "create group; flags: isDiscardingResults=%d",
                               groupFlags.isDiscardResults());

  TaskGroupBase *impl;
  if (groupFlags.isDiscardResults()) {
    impl = ::new(group) DiscardingTaskGroup(T);
  } else {
    impl = ::new(group) AccumulatingTaskGroup(T);
  }

  TaskGroupTaskStatusRecord *record = impl->getTaskRecord();
  assert(record->getKind() == swift::TaskStatusRecordKind::TaskGroup);

  // ok, now that the group actually is initialized: attach it to the task
  addStatusRecordToSelf(record, [&](ActiveTaskStatus oldStatus, ActiveTaskStatus& newStatus) {
    // If the task has already been cancelled, reflect that immediately in
    // the group's status.
    if (oldStatus.isCancelled()) {
      impl->statusCancel();
    }
    return true;
  });
}

// =============================================================================
// ==== child task management --------------------------------------------------

void TaskGroup::addChildTask(AsyncTask *child) {
  SWIFT_TASK_DEBUG_LOG("attach child task = %p to group = %p", child, this);

  // Add the child task to this task group.  The corresponding removal
  // won't happen until the parent task successfully polls for this child
  // task, either synchronously in poll (if a task is available
  // synchronously) or asynchronously in offer (otherwise).  In either
  // case, the work ends up being non-concurrent with the parent task.

  // The task status record lock is held during this operation, which
  // prevents us from racing with cancellation or escalation.  We don't
  // need to acquire the task group lock because the child list is only
  // accessed under the task status record lock.
  auto base = asBaseImpl(this);
  auto record = base->getTaskRecord();
  record->attachChild(child);
}

void TaskGroup::removeChildTask(AsyncTask *child) {
  SWIFT_TASK_DEBUG_LOG("detach child task = %p from group = %p", child, this);

  auto groupRecord = asBaseImpl(this)->getTaskRecord();

  // The task status record lock is held during this operation, which
  // prevents us from racing with cancellation or escalation.  We don't
  // need to acquire the task group lock because the child list is only
  // accessed under the task status record lock.
  groupRecord->detachChild(child);
}

// =============================================================================
// ==== destroy ----------------------------------------------------------------

SWIFT_CC(swift)
static void swift_taskGroup_destroyImpl(TaskGroup *group) {
  asBaseImpl(group)->destroy();
}

void AccumulatingTaskGroup::destroy() {
#if SWIFT_TASK_DEBUG_LOG_ENABLED
  if (!this->isEmpty()) {
    auto status = this->statusLoadRelaxed();
    SWIFT_TASK_GROUP_DEBUG_LOG(this, "destroy, tasks .ready = %d, .pending = %llu",
                         status.readyTasks(this), status.pendingTasks(this));
  } else {
    SWIFT_TASK_DEBUG_LOG("destroying task group = %p", this);
  }
#endif
  assert(this->isEmpty() && "Attempted to destroy non-empty task group!");

  // First, remove the group from the task and deallocate the record
  removeStatusRecordFromSelf(getTaskRecord());

  // No need to drain our queue here, as by the time we call destroy,
  // all tasks inside the group must have been awaited on already.
  // This is done in Swift's withTaskGroup function explicitly.

  // destroy the group's storage
  this->~AccumulatingTaskGroup();
}

void DiscardingTaskGroup::destroy() {
#if SWIFT_TASK_DEBUG_LOG_ENABLED
  if (!this->isEmpty()) {
    auto status = this->statusLoadRelaxed();
    SWIFT_TASK_GROUP_DEBUG_LOG(this, "destroy, tasks .ready = %d, .pending = %llu",
                         status.readyTasks(this), status.pendingTasks(this));
  } else {
    SWIFT_TASK_DEBUG_LOG("destroying discarding task group = %p", this);
  }
#endif
  assert(this->isEmpty() && "Attempted to destroy non-empty task group!");

  // First, remove the group from the task and deallocate the record
  removeStatusRecordFromSelf(getTaskRecord());

  // No need to drain our queue here, as by the time we call destroy,
  // all tasks inside the group must have been awaited on already.
  // This is done in Swift's withTaskGroup function explicitly.

  // destroy the group's storage
  this->~DiscardingTaskGroup();
}

bool TaskGroup::isCancelled() {
  return asBaseImpl(this)->isCancelled();
}

// =============================================================================
// ==== offer ------------------------------------------------------------------

static void fillGroupNextErrorResult(TaskFutureWaitAsyncContext *context,
                                     SwiftError *error) {
  context->fillWithError(error);
}

static void fillGroupNextResult(TaskFutureWaitAsyncContext *context,
                                PollResult result) {
  /// Fill in the result value
  switch (result.status) {
  case PollStatus::MustWait:
    assert(false && "filling a waiting status?");
    return;

  case PollStatus::Error: {
    auto error = reinterpret_cast<SwiftError *>(result.storage);
    fillGroupNextErrorResult(context, error);
    return;
  }

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
    // Initialize the result as a .none Optional<Success>.
    const Metadata *successType = result.successType;
    OpaqueValue *destPtr = context->successResultPointer;
    successType->vw_storeEnumTagSinglePayload(destPtr, 1, 1);
    return;
  }
  }
}

static void _enqueueCompletedTask(NaiveTaskGroupQueue<ReadyQueueItem> *readyQueue,
                                  AsyncTask *completedTask,
                                  bool hadErrorResult) {
  auto readyItem = ReadyQueueItem::get(
      hadErrorResult ? ReadyStatus::Error : ReadyStatus::Success,
      completedTask
  );

  assert(completedTask == readyItem.getTask());
  assert(readyItem.getTask()->isFuture());
  readyQueue->enqueue(readyItem);
}

#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
static void _enqueueRawError(DiscardingTaskGroup *group,
                             NaiveTaskGroupQueue<ReadyQueueItem> *readyQueue,
                             SwiftError *error) {
  auto readyItem = ReadyQueueItem::getRawError(group, error);
  readyQueue->enqueue(readyItem);
}
#endif

// TaskGroup is locked upon entry and exit
void AccumulatingTaskGroup::enqueueCompletedTask(AsyncTask *completedTask, bool hadErrorResult) {
  // Retain the task while it is in the queue; it must remain alive until
  // it is found by poll.  This retain will be balanced by the release in poll.
  swift_retain(completedTask);

  _enqueueCompletedTask(&readyQueue, completedTask, hadErrorResult);
}

// TaskGroup is locked upon entry and exit
void DiscardingTaskGroup::enqueueCompletedTask(AsyncTask *completedTask, bool hadErrorResult) {
  if (!readyQueue.isEmpty()) {
    SWIFT_TASK_GROUP_DEBUG_LOG(this, "discard task, we already have an error stored, completedTask:%p",
                               completedTask);
  }

  if (hadErrorResult) {
    // we only store the FIRST error in discardResults mode
    SWIFT_TASK_GROUP_DEBUG_LOG(this, "store first error, completedTask:%p", completedTask);
    // continue handling as usual, which will perform the enqueue
  } else {
    SWIFT_TASK_GROUP_DEBUG_LOG(this, "discard successful result, %p", completedTask);
    // DO NOT RETAIN THE TASK.
    // We know it is Void, so we don't need to store the result;
    // By releasing tasks eagerly we're able to keep "infinite" task groups,
    // running, that never consume their values. Even more-so,
    return;
  }

  // Retain the task while it is in the queue; it must remain alive until
  // it is found by poll.  This retain will be balanced by the release in waitAll.
  assert(hadErrorResult); // a discarding group may only store an errored task.
  swift_retain(completedTask);

  _enqueueCompletedTask(&readyQueue, completedTask, hadErrorResult);
}

void TaskGroup::offer(AsyncTask *completedTask, AsyncContext *context) {
  asBaseImpl(this)->offer(completedTask, context);
}

void AccumulatingTaskGroup::offer(AsyncTask *completedTask, AsyncContext *context) {
  assert(completedTask);
  assert(completedTask->isFuture());
  assert(completedTask->hasChildFragment());
  assert(completedTask->hasGroupChildFragment());
  assert(completedTask->groupChildFragment()->getGroup() == asAbstract(this));

  // The current ownership convention is that we are *not* given ownership
  // of a retain on completedTask; we're called from the task completion
  // handler, and the task will release itself.  So if we need the task
  // to survive this call (e.g. because there isn't an immediate waiting
  // task), we will need to retain it, which we do in enqueueCompletedTask.
  // This is wasteful, and the task completion function should be fixed to
  // transfer ownership of a retain into this function, in which case we
  // will need to release in the other path.
  lock(); // TODO: remove fragment lock, and use status for synchronization

  SWIFT_TASK_GROUP_DEBUG_LOG(this, "offer, completedTask:%p, status:%s",
                             completedTask,
                             statusString().c_str());

  // Immediately increment ready count and acquire the status
  //
  // NOTE: If the group is `discardResults` this becomes a plain load(),
  //       since there is no ready count to maintain.
  //
  // Examples:
  //   W:n R:0 P:3 -> W:n R:1 P:3 // no waiter, 2 more pending tasks
  //   W:n R:0 P:1 -> W:n R:1 P:1 // no waiter, no more pending tasks
  //   W:n R:0 P:1 -> W:y R:1 P:1 // complete immediately
  //   W:n R:0 P:1 -> W:y R:1 P:3 // complete immediately, 2 more pending tasks
  TaskGroupStatus assumed = statusAddReadyAssumeAcquire();

  auto asyncContextPrefix = reinterpret_cast<FutureAsyncContextPrefix *>(
      reinterpret_cast<char *>(context) - sizeof(FutureAsyncContextPrefix));
  bool hadErrorResult = false;
  auto errorObject = asyncContextPrefix->errorResult;
  if (errorObject) {
    // instead, we need to enqueue this result:
    hadErrorResult = true;
  }

  SWIFT_TASK_GROUP_DEBUG_LOG(this, "ready: %d, pending: %llu",
                       assumed.readyTasks(this), assumed.pendingTasks(this));

  // ==== a) has waiting task, so let us complete it right away
  if (assumed.hasWaitingTask()) {
    resumeWaitingTask(completedTask, assumed, hadErrorResult);
    unlock(); // TODO: remove fragment lock, and use status for synchronization
    return;
  } else {
    // ==== b) enqueue completion ------------------------------------------------
    //
    // else, no-one was waiting (yet), so we have to instead enqueue to the message
    // queue when a task polls during next() it will notice that we have a value
    // ready for it, and will process it immediately without suspending.
    assert(!waitQueue.load(std::memory_order_relaxed));

    enqueueCompletedTask(completedTask, hadErrorResult);
    unlock(); // TODO: remove fragment lock, and use status for synchronization
  }
}

void DiscardingTaskGroup::offer(AsyncTask *completedTask, AsyncContext *context) {
  assert(completedTask);
  assert(completedTask->isFuture());
  assert(completedTask->hasChildFragment());
  assert(completedTask->hasGroupChildFragment());
  assert(completedTask->groupChildFragment()->getGroup() == asAbstract(this));

  lock(); // TODO: remove fragment lock, and use status for synchronization

  // Since we don't maintain ready counts in a discarding group, only load the status.
  TaskGroupStatus assumed = statusLoadAcquire();

  auto asyncContextPrefix = reinterpret_cast<FutureAsyncContextPrefix *>(
      reinterpret_cast<char *>(context) - sizeof(FutureAsyncContextPrefix));
  bool hadErrorResult = false;
  auto errorObject = asyncContextPrefix->errorResult;
  if (errorObject) {
    // instead, we need to enqueue this result:
    hadErrorResult = true;
  }

  SWIFT_TASK_GROUP_DEBUG_LOG(this, "offer, completedTask:%p, error:%d, status:%s",
                             completedTask, hadErrorResult, assumed.to_string(this).c_str());

  // Immediately decrement the pending count.
  // We can do this, since in this mode there is no ready count to keep track of,
  // and we immediately discard the result.
  auto afterComplete = statusCompletePendingAssumeRelease();
  const bool alreadyDecrementedStatus = true;
  SWIFT_TASK_GROUP_DEBUG_LOG(this, "offer, complete, status afterComplete:%s", afterComplete.to_string(this).c_str());

  // Errors need special treatment
  if (hadErrorResult) {
    // Discarding results mode immediately treats a child failure as group cancellation.
    // "All for one, one for all!" - any task failing must cause the group and all sibling tasks to be cancelled,
    // such that the discarding group can exit as soon as possible.
    cancelAll();

    if (afterComplete.hasWaitingTask() && afterComplete.pendingTasks(this) == 0) {
      // This is the last pending task, and we must resume the waiting task.
      // - if there already was a previous error stored, we resume using it,
      // - otherwise, we resume using this current (failed) completedTask
      ReadyQueueItem readyErrorItem;
      if (readyQueue.dequeue(readyErrorItem)) {
        switch (readyErrorItem.getStatus()) {
          case ReadyStatus::RawError:
            SWIFT_TASK_GROUP_DEBUG_LOG(this, "offer, complete, resume with raw error:%p", readyErrorItem.getRawError(this));
            resumeWaitingTaskWithError(readyErrorItem.getRawError(this), assumed,
                                       alreadyDecrementedStatus);
            break;
          case ReadyStatus::Error:
            SWIFT_TASK_GROUP_DEBUG_LOG(this, "offer, complete, resume with errorItem.task:%p", readyErrorItem.getTask());
            resumeWaitingTask(readyErrorItem.getTask(), assumed,
                              /*hadErrorResult=*/true,
                              alreadyDecrementedStatus,
                              /*taskWasRetained=*/true);
            break;
          default:
            swift_Concurrency_fatalError(0,
                                         "only errors can be stored by a discarding task group, yet it wasn't an error! 1");
        }
      } else {
        // There was no prior failed task stored, so we should resume the waitingTask with this (failed) completedTask
        resumeWaitingTask(completedTask, assumed, hadErrorResult, alreadyDecrementedStatus);
      }
    } else if (readyQueue.isEmpty()) {
      // There was no waiting task, or other tasks are still pending, so we cannot
      // it is the first error we encountered, thus we need to store it for future throwing
      SWIFT_TASK_GROUP_DEBUG_LOG(this, "offer, enqueue child task:%p", completedTask);
      enqueueCompletedTask(completedTask, hadErrorResult);
    } else {
      SWIFT_TASK_GROUP_DEBUG_LOG(this, "offer, complete, discard child task:%p", completedTask);
      _swift_taskGroup_detachChild(asAbstract(this), completedTask);
    }

    unlock();
    return;
  }

  assert(!hadErrorResult && "only successfully completed tasks can reach here");
  if (afterComplete.hasWaitingTask() && afterComplete.pendingTasks(this) == 0) {
    SWIFT_TASK_GROUP_DEBUG_LOG(this,
                               "offer, last pending task completed successfully, resume waitingTask with completedTask:%p",
                               completedTask);

    /// If there was an error previously stored, we must resume the waitingTask using that error.
    ReadyQueueItem readyErrorItem;
    if (readyQueue.dequeue(readyErrorItem)) {
      _swift_taskGroup_detachChild(asAbstract(this), completedTask);
      switch (readyErrorItem.getStatus()) {
        case ReadyStatus::RawError:
          resumeWaitingTaskWithError(readyErrorItem.getRawError(this), assumed, alreadyDecrementedStatus);
          break;
        case ReadyStatus::Error:
          resumeWaitingTask(readyErrorItem.getTask(), assumed,
                            /*hadErrorResult=*/true,
                            alreadyDecrementedStatus,
                            /*taskWasRetained=*/true);
          break;
        default:
          swift_Concurrency_fatalError(0,
                                       "only errors can be stored by a discarding task group, yet it wasn't an error! 2");
      }
    } else {
      // This is the last task, we have a waiting task and there was no error stored previously;
      // We must resume the waiting task with a success, so let us return here.
      resumeWaitingTask(completedTask, assumed, /*hadErrorResult=*/false, alreadyDecrementedStatus);
    }
  } else {
    // it wasn't the last pending task, and there is no-one to resume;
    // Since this is a successful result, and we're a discarding task group -- always just ignore this task.
    _swift_taskGroup_detachChild(asAbstract(this), completedTask);
  }

  unlock();
  return;
}

/// Must be called while holding the TaskGroup lock.
void TaskGroupBase::resumeWaitingTask(
    AsyncTask *completedTask,
    TaskGroupStatus &assumed,
    bool hadErrorResult,
    bool alreadyDecremented,
    bool taskWasRetained) {
  auto waitingTask = waitQueue.load(std::memory_order_acquire);
  assert(waitingTask && "waitingTask must not be null when attempting to resume it");
  assert(assumed.hasWaitingTask());
  SWIFT_TASK_GROUP_DEBUG_LOG(this, "resume waiting task = %p, alreadyDecremented:%d, error:%d, complete with = %p",
                       waitingTask, alreadyDecremented, hadErrorResult, completedTask);
  while (true) {
    SWIFT_TASK_GROUP_DEBUG_LOG(this, "resumeWaitingTask, attempt CAS, waiting task = %p, waitQueue.head = %p, error:%d, complete with = %p",
                               waitingTask, waitQueue.load(std::memory_order_relaxed), hadErrorResult, completedTask);

    // ==== a) run waiting task directly -------------------------------------
      // assert(assumed.pendingTasks(this) && "offered to group with no pending tasks!");
      // We are the "first" completed task to arrive,
      // and since there is a task waiting we immediately claim and complete it.
      if (waitQueue.compare_exchange_strong(
          waitingTask, nullptr,
          /*success*/ std::memory_order_release,
          /*failure*/ std::memory_order_acquire)) {

#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
        // In the task-to-thread model, child tasks are always actually
        // run synchronously on the parent task's thread.  For task groups
        // specifically, this means that poll() will pick a child task
        // that was added to the group and run it to completion as a
        // subroutine.  Therefore, when we enter offer(), we know that
        // the parent task is waiting and we can just return to it.

        // The task-to-thread logic in poll() currently expects the child
        // task to enqueue itself instead of just filling in the result in
        // the waiting task.  This is a little wasteful; there's no reason
        // we can't just have the parent task set itself up as a waiter.
        // But since it's what we're doing, we basically take the same
        // path as we would if there wasn't a waiter.
        enqueueCompletedTask(completedTask, hadErrorResult);
        return;

#else /* SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL */
        if (!alreadyDecremented) {
          (void) statusCompletePendingReadyWaiting(assumed);
        }

        // Run the task.
        auto result = PollResult::get(completedTask, hadErrorResult);
        SWIFT_TASK_GROUP_DEBUG_LOG(this,
                                   "resume waiting DONE, task = %p, error:%d, complete with = %p, status = %s",
                                   waitingTask, hadErrorResult, completedTask, statusString().c_str());

        auto waitingContext =
            static_cast<TaskFutureWaitAsyncContext *>(
                waitingTask->ResumeContext);
        fillGroupNextResult(waitingContext, result);

        // Remove the child from the task group's running tasks list.
        // The parent task isn't currently running (we're about to wake
        // it up), so we're still synchronous with it.  We can safely
        // acquire our parent's status record lock here (which would
        // ordinarily run the risk of deadlock, since e.g. cancellation
        // does a parent -> child traversal while recursively holding
        // locks) because we know that the child task is completed and
        // we can't be holding its locks ourselves.
        _swift_taskGroup_detachChild(asAbstract(this), completedTask);
        if (isDiscardingResults() && hadErrorResult && taskWasRetained) {
          // We only used the task to keep the error in the future fragment around
          // so now that we emitted the error and detached the task, we are free to release the task immediately.
          swift_release(completedTask);
        }

        _swift_tsan_acquire(static_cast<Job *>(waitingTask));
        // TODO: allow the caller to suggest an executor
        waitingTask->flagAsAndEnqueueOnExecutor(ExecutorRef::generic());
        return;
#endif /* SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL */
    } else {
      SWIFT_TASK_GROUP_DEBUG_LOG(this, "CAS failed, task = %p, backup = %p, complete with = %p, status = %s",
                                 waitingTask, completedTask, statusString().c_str());
    }
  }
}

/// Must be called while holding the TaskGroup lock.
void DiscardingTaskGroup::resumeWaitingTaskWithError(
    SwiftError *error,
    TaskGroupStatus &assumed,
    bool alreadyDecremented) {
  auto waitingTask = waitQueue.load(std::memory_order_acquire);
  assert(waitingTask && "cannot resume 'null' waiting task!");
  SWIFT_TASK_GROUP_DEBUG_LOG(this, "resume waiting task = %p, with error = %p",
                       waitingTask, error);
  while (true) {
    // ==== a) run waiting task directly -------------------------------------
    assert(assumed.hasWaitingTask());
    // assert(assumed.pendingTasks(this) && "offered to group with no pending tasks!");
    // We are the "first" completed task to arrive,
    // and since there is a task waiting we immediately claim and complete it.
    if (waitQueue.compare_exchange_strong(
        waitingTask, nullptr,
        /*success*/ std::memory_order_release,
        /*failure*/ std::memory_order_acquire)) {

#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
      // In the task-to-thread model, child tasks are always actually
      // run synchronously on the parent task's thread.  For task groups
      // specifically, this means that poll() will pick a child task
      // that was added to the group and run it to completion as a
      // subroutine.  Therefore, when we enter offer(), we know that
      // the parent task is waiting and we can just return to it.

      // The task-to-thread logic in poll() currently expects the child
      // task to enqueue itself instead of just filling in the result in
      // the waiting task.  This is a little wasteful; there's no reason
      // we can't just have the parent task set itself up as a waiter.
      // But since it's what we're doing, we basically take the same
      // path as we would if there wasn't a waiter.
      _enqueueRawError(this, &readyQueue, error);
      return;

#else /* SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL */
      if (alreadyDecremented || statusCompletePendingReadyWaiting(assumed)) {
        // Run the task.
        auto result = PollResult::getError(error);

        auto waitingContext =
            static_cast<TaskFutureWaitAsyncContext *>(
                waitingTask->ResumeContext);

        fillGroupNextResult(waitingContext, result);

        _swift_tsan_acquire(static_cast<Job *>(waitingTask));
        // TODO: allow the caller to suggest an executor
        waitingTask->flagAsAndEnqueueOnExecutor(ExecutorRef::generic());
        return;
      } // else, try again
#endif
    }
  }
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

__attribute__((noinline))
SWIFT_CC(swiftasync) static void workaround_function_swift_taskGroup_waitAllImpl(
    OpaqueValue *result, SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
    TaskGroup *_group,
    SwiftError *bodyError,
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
static void swift_taskGroup_wait_next_throwingImpl(
    OpaqueValue *resultPointer, SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
    TaskGroup *_group,
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

  auto group = asAccumulatingImpl(_group);
  assert(group && "swift_taskGroup_wait_next_throwing was passed context without group!");

  PollResult polled = group->poll(waitingTask);
  switch (polled.status) {
  case PollStatus::MustWait:
    SWIFT_TASK_DEBUG_LOG("poll group = %p, no ready tasks, waiting task = %p",
                         group, waitingTask);
    // The waiting task has been queued on the channel,
    // there were pending tasks so it will be woken up eventually.
#ifdef __ARM_ARCH_7K__
    return workaround_function_swift_taskGroup_wait_next_throwingImpl(
        resultPointer, callerContext, _group, resumeFunction, rawContext);
#else /* __ARM_ARCH_7K__ */
    return;
#endif /* __ARM_ARCH_7K__ */

  case PollStatus::Empty:
  case PollStatus::Error:
  case PollStatus::Success:
    SWIFT_TASK_GROUP_DEBUG_LOG(group, "poll, task = %p, ready task available = %p",
                         waitingTask, polled.retainedTask);
    fillGroupNextResult(context, polled);
    if (auto completedTask = polled.retainedTask) {
      // Remove the child from the task group's running tasks list.
      _swift_taskGroup_detachChild(asAbstract(group), completedTask);

      // Balance the retain done by enqueueCompletedTask.
      swift_release(completedTask);
    }

    return waitingTask->runInFullyEstablishedContext();
  }
}

PollResult AccumulatingTaskGroup::poll(AsyncTask *waitingTask) {
  SWIFT_TASK_GROUP_DEBUG_LOG(this, "poll, waitingTask:%p", waitingTask);
  lock(); // TODO: remove group lock, and use status for synchronization
  assert(isAccumulatingResults() &&
         "attempted to poll TaskGroup in discard-results mode!");

  PollResult result;
  result.storage = nullptr;
  result.successType = nullptr;
  result.retainedTask = nullptr;

  // Have we suspended the task?
  bool hasSuspended = false;
  bool haveRunOneChildTaskInline = false;

#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
reevaluate_if_taskgroup_has_results:;
#endif
  auto assumed = statusMarkWaitingAssumeAcquire();
  if (haveRunOneChildTaskInline) {
    assert(assumed.readyTasks(this));
  }

  // ==== 1) bail out early if no tasks are pending ----------------------------
  if (assumed.isEmpty(this)) {
    SWIFT_TASK_DEBUG_LOG("poll group = %p, group is empty, no pending tasks", this);
    // No tasks in flight, we know no tasks were submitted before this poll
    // was issued, and if we parked here we'd potentially never be woken up.
    // Bail out and return `nil` from `group.next()`.
    statusRemoveWaitingRelease();
    result.status = PollStatus::Empty;
    result.successType = this->successType;
    unlock(); // TODO: remove group lock, and use status for synchronization
    return result;
  }

  auto waitHead = waitQueue.load(std::memory_order_acquire);

  // ==== 2) Ready task was polled, return with it immediately -----------------
  if (assumed.readyTasks(this)) {
    SWIFT_TASK_DEBUG_LOG("poll group = %p, tasks .ready = %d, .pending = %llu",
                         this, assumed.readyTasks(this), assumed.pendingTasks(this));

    auto assumedStatus = assumed.status;
    auto newStatus = TaskGroupStatus{assumedStatus};
    if (status.compare_exchange_strong(
        assumedStatus, newStatus.completingPendingReadyWaiting(this).status,
        /*success*/ std::memory_order_release,
        /*failure*/ std::memory_order_acquire)) {

      // We're going back to running the task, so if we suspended before,
      // we need to flag it as running again.
      if (hasSuspended) {
        waitingTask->flagAsRunning();
      }

      // Success! We are allowed to poll.
      ReadyQueueItem item;
      bool taskDequeued = readyQueue.dequeue(item);
      assert(taskDequeued); (void) taskDequeued;

      auto futureFragment =
          item.getStatus() == ReadyStatus::RawError ?
          nullptr :
          item.getTask()->futureFragment();

      // Store the task in the result, so after we're done processing it may
      // be swift_release'd; we kept it alive while it was in the readyQueue by
      // an additional retain issued as we enqueued it there.

      // Note that the task was detached from the task group when it
      // completed, so we don't need to do that bit of record-keeping here.

      switch (item.getStatus()) {
        case ReadyStatus::Success:
          // Immediately return the polled value
          result.status = PollStatus::Success;
          result.storage = futureFragment->getStoragePtr();
          result.successType = futureFragment->getResultType();
          result.retainedTask = item.getTask();
          assert(result.retainedTask && "polled a task, it must be not null");
          _swift_tsan_acquire(static_cast<Job *>(result.retainedTask));
          unlock(); // TODO: remove fragment lock, and use status for synchronization
          return result;

        case ReadyStatus::Error:
          // Immediately return the polled value
          result.status = PollStatus::Error;
          result.storage =
              reinterpret_cast<OpaqueValue *>(futureFragment->getError());
          result.successType = nullptr;
          result.retainedTask = item.getTask();
          assert(result.retainedTask && "polled a task, it must be not null");
          _swift_tsan_acquire(static_cast<Job *>(result.retainedTask));
          unlock(); // TODO: remove fragment lock, and use status for synchronization
          return result;

        case ReadyStatus::Empty:
          result.status = PollStatus::Empty;
          result.storage = nullptr;
          result.retainedTask = nullptr;
          result.successType = this->successType;
          unlock(); // TODO: remove fragment lock, and use status for synchronization
          return result;

        case ReadyStatus::RawError:
          swift_Concurrency_fatalError(0, "accumulating task group should never use raw-errors!");
      }
      swift_Concurrency_fatalError(0, "must return result when status compare-and-swap was successful");
    } // else, we failed status-cas (some other waiter claimed a ready pending task, try again)
  }

  // ==== 3) Add to wait queue -------------------------------------------------
  assert(assumed.readyTasks(this) == 0);
  _swift_tsan_release(static_cast<Job *>(waitingTask));
  while (true) {
    if (!hasSuspended) {
      hasSuspended = true;
      waitingTask->flagAsSuspendedOnTaskGroup(asAbstract(this));
    }
    // Put the waiting task at the beginning of the wait queue.
    SWIFT_TASK_GROUP_DEBUG_LOG(this, "WATCH OUT, SET WAITER ONTO waitQueue.head = %p", waitQueue.load(std::memory_order_relaxed));
    if (waitQueue.compare_exchange_strong(
        waitHead, waitingTask,
        /*success*/ std::memory_order_release,
        /*failure*/ std::memory_order_acquire)) {
      unlock(); // TODO: remove fragment lock, and use status for synchronization
#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
      // The logic here is paired with the logic in TaskGroupBase::offer. Once
      // we run the
      auto oldTask = _swift_task_clearCurrent();
      assert(oldTask == waitingTask);

      auto childTask = getTaskRecord()->getFirstChild();
      assert(childTask != NULL);

      SWIFT_TASK_DEBUG_LOG("[RunInline] Switching away from running %p to now running %p", oldTask, childTask);
      // Run the new task on the same thread now - this should run the new task to
      // completion. All swift tasks in task-to-thread model run on generic
      // executor
      swift_job_run(childTask, ExecutorRef::generic());
      haveRunOneChildTaskInline = true;

      SWIFT_TASK_DEBUG_LOG("[RunInline] Switching back from running %p to now running %p", childTask, oldTask);
      // We are back to being the parent task and now that we've run the child
      // task, we should reevaluate parent task
      _swift_task_setCurrent(oldTask);
      goto reevaluate_if_taskgroup_has_results;
#endif
      // no ready tasks, so we must wait.
      result.status = PollStatus::MustWait;
      _swift_task_clearCurrent();
      return result;
    } // else, try again
  }
}

// =============================================================================
// ==== _taskGroupWaitAll implementation ---------------------------------------

SWIFT_CC(swiftasync)
static void swift_taskGroup_waitAllImpl(
    OpaqueValue *resultPointer, SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
    TaskGroup *_group,
    SwiftError *bodyError,
    ThrowingTaskFutureWaitContinuationFunction *resumeFunction,
    AsyncContext *rawContext) {
  auto waitingTask = swift_task_getCurrent();

  auto group = asBaseImpl(_group);
  return group->waitAll(
      bodyError, waitingTask,
      resultPointer, callerContext, resumeFunction, rawContext);
}

void TaskGroupBase::waitAll(SwiftError* bodyError, AsyncTask *waitingTask,
                                  OpaqueValue *resultPointer, SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
                                  ThrowingTaskFutureWaitContinuationFunction *resumeFunction,
                                  AsyncContext *rawContext) {
  lock();

  // must mutate the waiting task while holding the group lock,
  // so we don't get an offer concurrently trying to do so
  waitingTask->ResumeTask = task_group_wait_resume_adapter;
  waitingTask->ResumeContext = rawContext;

  auto context = static_cast<TaskFutureWaitAsyncContext *>(rawContext);
  context->ResumeParent =
      reinterpret_cast<TaskContinuationFunction *>(resumeFunction);
  context->Parent = callerContext;
  context->errorResult = nullptr;
  context->successResultPointer = resultPointer;

  SWIFT_TASK_GROUP_DEBUG_LOG(this, "waitAll, bodyError = %p, status = %s", bodyError, statusString().c_str());
  PollResult result = PollResult::getEmpty(this->successType);
  result.status = PollStatus::Empty;
  result.storage = nullptr;
  result.retainedTask = nullptr;

  // Have we suspended the task?
  bool hasSuspended = false;

#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
  bool haveRunOneChildTaskInline = false;
  reevaluate_if_TaskGroup_has_results:;
#endif
  // Paired with a release when marking Waiting,
  // otherwise we don't modify the status
  auto assumed = statusLoadAcquire();

  SWIFT_TASK_GROUP_DEBUG_LOG(this, "waitAll, status = %s", assumed.to_string(this).c_str());

  // ==== 1) may be able to bail out early if no tasks are pending -------------
  if (assumed.isEmpty(this)) {
    /// A discarding task group may be empty already, but have stored an error that must be thrown
    /// out of waitAll - providing the "the first error gets thrown" semantics of the group.
    /// The readyQueue is allowed to have exactly one error element in this case.
    if (isDiscardingResults()) {
      // ---- 1.1) A discarding group needs to check if there is a stored error to throw
      auto discardingGroup = asDiscardingImpl(this);
      ReadyQueueItem firstErrorItem;
      if (readyQueue.dequeue(firstErrorItem)) {
        if (firstErrorItem.getStatus() == ReadyStatus::Error) {
          result = PollResult::get(firstErrorItem.getTask(), /*hadErrorResult=*/true);
        } else if (firstErrorItem.getStatus() == ReadyStatus::RawError) {
          result.storage = reinterpret_cast<OpaqueValue*>(firstErrorItem.getRawError(discardingGroup));
          result.status = PollStatus::Error;
        }
      } // else, we're definitely Empty
    } // else (in an accumulating group), a waitAll can bail out early Empty

    SWIFT_TASK_GROUP_DEBUG_LOG(this, "waitAll, early return, no pending tasks, bodyError:%p, status = %s",
                               bodyError,  assumed.to_string(this).c_str());
    // No tasks in flight, we know no tasks were submitted before this poll
    // was issued, and if we parked here we'd potentially never be woken up.

#if SWIFT_TASK_GROUP_BODY_THROWN_ERROR_WINS
    if (bodyError) {
      fillGroupNextErrorResult(context, bodyError);
    } else {
      fillGroupNextResult(context, result);
    }
#else // so, not SWIFT_TASK_GROUP_BODY_THROWN_ERROR_WINS
    fillGroupNextResult(context, polled);
#endif // SWIFT_TASK_GROUP_BODY_THROWN_ERROR_WINS
    if (auto completedTask = result.retainedTask) {
      // Remove the child from the task group's running tasks list.
      _swift_taskGroup_detachChild(asAbstract(this), completedTask);

      // Balance the retain done by enqueueCompletedTask.
      swift_release(completedTask);
    }

    waitingTask->runInFullyEstablishedContext();

    unlock();
    return;
  }

  // ==== 2) Add to wait queue -------------------------------------------------

  // ---- 2.1) Discarding task group may need to story the bodyError before we park
  if (bodyError && isDiscardingResults() && readyQueue.isEmpty()) {
    auto discardingGroup = asDiscardingImpl(this);
    auto readyItem = ReadyQueueItem::getRawError(discardingGroup, bodyError);
    readyQueue.enqueue(readyItem);
  }

  auto waitHead = waitQueue.load(std::memory_order_acquire);
  _swift_tsan_release(static_cast<Job *>(waitingTask));
  while (true) {
    if (!hasSuspended) {
      hasSuspended = true;
      waitingTask->flagAsSuspendedOnTaskGroup(asAbstract(this));
    }
    // Put the waiting task at the beginning of the wait queue.
    if (waitQueue.compare_exchange_strong(
        waitHead, waitingTask,
        /*success*/ std::memory_order_release,
        /*failure*/ std::memory_order_acquire)) {
      statusMarkWaitingAssumeRelease();
      SWIFT_TASK_GROUP_DEBUG_LOG(this, "waitAll, marked waiting status = %s", statusString().c_str());

#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
      // The logic here is paired with the logic in TaskGroupBase::offer. Once
      // we run the
      auto oldTask = _swift_task_clearCurrent();
      assert(oldTask == waitingTask);

      auto childTask = getTaskRecord()->getFirstChild();
      assert(childTask != NULL);

      SWIFT_TASK_DEBUG_LOG("[RunInline] Switching away from running %p to now running %p", oldTask, childTask);
      // Run the new task on the same thread now - this should run the new task to
      // completion. All swift tasks in task-to-thread model run on generic
      // executor
      swift_job_run(childTask, ExecutorRef::generic());
      haveRunOneChildTaskInline = true;

      SWIFT_TASK_DEBUG_LOG("[RunInline] Switching back from running %p to now running %p", childTask, oldTask);
      // We are back to being the parent task and now that we've run the child
      // task, we should reevaluate parent task
      _swift_task_setCurrent(oldTask);
      goto reevaluate_if_TaskGroup_has_results;
#endif
      // The waiting task has been queued on the channel,
      // there were pending tasks so it will be woken up eventually.
#ifdef __ARM_ARCH_7K__
      workaround_function_swift_taskGroup_waitAllImpl(
         resultPointer, callerContext, asAbstract(this), bodyError, resumeFunction, rawContext);
#endif /* __ARM_ARCH_7K__ */

      _swift_task_clearCurrent();
      unlock();
      return;
    } // else, try again
  }
}

// =============================================================================
// ==== Task Group status and flag checks  -------------------------------------

SWIFT_CC(swift)
static bool swift_taskGroup_isEmptyImpl(TaskGroup *group) {
  return asBaseImpl(group)->isEmpty();
}

SWIFT_CC(swift)
static bool swift_taskGroup_isCancelledImpl(TaskGroup *group) {
  return asBaseImpl(group)->isCancelled();
}

// =============================================================================
// ==== cancelAll --------------------------------------------------------------

SWIFT_CC(swift)
static void swift_taskGroup_cancelAllImpl(TaskGroup *group) {
  asBaseImpl(group)->cancelAll();
}

bool TaskGroupBase::cancelAll() {
  SWIFT_TASK_DEBUG_LOG("cancel all tasks in group = %p", this);

  // Flag the task group itself as cancelled.  If this was already
  // done, any existing child tasks should already have been cancelled,
  // and cancellation should automatically flow to any new child tasks,
  // so there's nothing else for us to do.
  auto wasCancelledBefore = statusCancel();
  if (wasCancelledBefore) {
    return false;
  }

  // Cancel all the child tasks.  TaskGroup is not a Sendable type,
  // so cancelAll() can only be called from the owning task.  This
  // satisfies the precondition on cancelAllChildren().
  _swift_taskGroup_cancelAllChildren(asAbstract(this));

  return true;
}

SWIFT_CC(swift)
static void swift_task_cancel_group_child_tasksImpl(TaskGroup *group) {
  // TaskGroup is not a Sendable type, and so this operation (which is not
  // currently exposed in the API) can only be called from the owning
  // task.  This satisfies the precondition on cancelAllChildren().
  _swift_taskGroup_cancelAllChildren(group);
}

/// Cancel all the children of the given task group.
///
/// The caller must guarantee that this is either called from the
/// owning task of the task group or while holding the owning task's
/// status record lock.
void swift::_swift_taskGroup_cancelAllChildren(TaskGroup *group) {
  // Because only the owning task of the task group can modify the
  // child list of a task group status record, and it can only do so
  // while holding the owning task's status record lock, we do not need
  // any additional synchronization within this function.
  for (auto childTask: group->getTaskRecord()->children())
    swift_task_cancel(childTask);
}

// =============================================================================
// ==== addPending -------------------------------------------------------------

SWIFT_CC(swift)
static bool swift_taskGroup_addPendingImpl(TaskGroup *_group, bool unconditionally) {
  auto group = asBaseImpl(_group);
  auto assumed = group->statusAddPendingTaskAssumeRelaxed(unconditionally);
  SWIFT_TASK_DEBUG_LOG("add pending %s to group(%p), tasks pending = %d",
                       unconditionally ? "unconditionally" : "",
                       group, assumed.pendingTasks(group));
  return !assumed.isCancelled();
}

#define OVERRIDE_TASK_GROUP COMPATIBILITY_OVERRIDE
#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH
