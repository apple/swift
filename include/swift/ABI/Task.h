//===--- Task.h - ABI structures for asynchronous tasks ---------*- C++ -*-===//
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
// Swift ABI describing tasks.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ABI_TASK_H
#define SWIFT_ABI_TASK_H

#include "swift/ABI/TaskLocal.h"
#include "swift/ABI/Executor.h"
#include "swift/ABI/HeapObject.h"
#include "swift/ABI/Metadata.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/VoucherShims.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Threading/ConditionVariable.h"
#include "swift/Threading/Mutex.h"
#include "bitset"
#include "queue" // TODO: remove and replace with our own mpsc

namespace swift {
class AsyncTask;
class AsyncContext;
class Job;
struct OpaqueValue;
struct SwiftError;
class TaskStatusRecord;
class TaskDependencyStatusRecord;
class TaskExecutorPreferenceStatusRecord;
class TaskOptionRecord;
class TaskGroup;
class ContinuationAsyncContext;

extern FullMetadata<DispatchClassMetadata> jobHeapMetadata;

/// A schedulable job.
class alignas(2 * alignof(void*)) Job :
  // For async-let tasks, the refcount bits are initialized as "immortal"
  // because such a task is allocated with the parent's stack allocator.
  public HeapObject {
public:
  // Indices into SchedulerPrivate, for use by the runtime.
  enum {
    /// The next waiting task link, an AsyncTask that is waiting on a future.
    NextWaitingTaskIndex = 0,

    // The Dispatch object header is one pointer and two ints, which is
    // equivalent to three pointers on 32-bit and two pointers 64-bit. Set the
    // indexes accordingly so that DispatchLinkageIndex points to where Dispatch
    // expects.
    DispatchHasLongObjectHeader = sizeof(void *) == sizeof(int),

    /// An opaque field used by Dispatch when enqueueing Jobs directly.
    DispatchLinkageIndex = DispatchHasLongObjectHeader ? 1 : 0,

    /// The dispatch queue being used when enqueueing a Job directly with
    /// Dispatch.
    DispatchQueueIndex = DispatchHasLongObjectHeader ? 0 : 1,
  };

  // Reserved for the use of the scheduler.
  void *SchedulerPrivate[2];

  /// WARNING: DO NOT MOVE.
  /// Schedulers may assume the memory location of the Flags in order to avoid a runtime call
  /// to get the priority of a job.
  JobFlags Flags;

  /// Derived classes can use this to store a Job Id.
  uint32_t Id = 0;

  /// The voucher associated with the job. Note: this is currently unused on
  /// non-Darwin platforms, with stub implementations of the functions for
  /// consistency.
  voucher_t Voucher = nullptr;

  /// Reserved for future use.
  void *Reserved = nullptr;

  // We use this union to avoid having to do a second indirect branch
  // when resuming an asynchronous task, which we expect will be the
  // common case.
  union {
    // A function to run a job that isn't an AsyncTask.
    JobInvokeFunction * __ptrauth_swift_job_invoke_function RunJob;

    // A function to resume an AsyncTask.
    TaskContinuationFunction * __ptrauth_swift_task_resume_function ResumeTask;
  };

  Job(JobFlags flags, JobInvokeFunction *invoke,
      const HeapMetadata *metadata = &jobHeapMetadata)
      : HeapObject(metadata), Flags(flags), RunJob(invoke) {
    Voucher = voucher_copy();
    assert(!isAsyncTask() && "wrong constructor for a task");
  }

  Job(JobFlags flags, TaskContinuationFunction *invoke,
      const HeapMetadata *metadata = &jobHeapMetadata,
      bool captureCurrentVoucher = true)
      : HeapObject(metadata), Flags(flags), ResumeTask(invoke) {
    if (captureCurrentVoucher)
      Voucher = voucher_copy();
    assert(isAsyncTask() && "wrong constructor for a non-task job");
  }

  /// Create a job with "immortal" reference counts.
  /// Used for async let tasks.
  Job(JobFlags flags, TaskContinuationFunction *invoke,
      const HeapMetadata *metadata, InlineRefCounts::Immortal_t immortal,
      bool captureCurrentVoucher = true)
      : HeapObject(metadata, immortal), Flags(flags), ResumeTask(invoke) {
    if (captureCurrentVoucher)
      Voucher = voucher_copy();
    assert(isAsyncTask() && "wrong constructor for a non-task job");
  }

  ~Job() { swift_voucher_release(Voucher); }

  bool isAsyncTask() const {
    return Flags.isAsyncTask();
  }

  JobPriority getPriority() const {
    return Flags.getPriority();
  }

  uint32_t getJobId() const {
    return Id;
  }

  /// Given that we've fully established the job context in the current
  /// thread, actually start running this job.  To establish the context
  /// correctly, call swift_job_run or runJobInExecutorContext.
  SWIFT_CC(swiftasync)
  void runInFullyEstablishedContext();

  /// Given that we've fully established the job context in the
  /// current thread, and that the job is a simple (non-task) job,
  /// actually start running this job.
  SWIFT_CC(swiftasync)
  void runSimpleInFullyEstablishedContext() {
    return RunJob(this); // 'return' forces tail call
  }
};

// The compiler will eventually assume these.
#if SWIFT_POINTER_IS_8_BYTES
static_assert(sizeof(Job) == 8 * sizeof(void*),
              "Job size is wrong");
#else
static_assert(sizeof(Job) == 10 * sizeof(void*),
              "Job size is wrong");
#endif
static_assert(alignof(Job) == 2 * alignof(void*),
              "Job alignment is wrong");

class NullaryContinuationJob : public Job {

private:
  SWIFT_ATTRIBUTE_UNUSED AsyncTask *Task;
  AsyncTask *Continuation;

public:
  NullaryContinuationJob(AsyncTask *task, JobPriority priority, AsyncTask *continuation)
    : Job({JobKind::NullaryContinuation, priority}, &process),
      Task(task), Continuation(continuation) {}

  SWIFT_CC(swiftasync)
  static void process(Job *job);

  static bool classof(const Job *job) {
    return job->Flags.getKind() == JobKind::NullaryContinuation;
  }
};

/// Describes type information and offers value methods for an arbitrary concrete
/// type in a way that's compatible with regular Swift and embedded Swift. In
/// regular Swift, just holds a Metadata pointer and dispatches to the value
/// witness table. In embedded Swift, because we do not have any value witness
/// tables present at runtime, the witnesses are stored and referenced directly.
///
/// This structure is created from swift_task_create, where in regular Swift, the
/// compiler provides the Metadata pointer, and in embedded Swift, a
/// TaskOptionRecord is used to provide the witnesses.
struct ResultTypeInfo {
#if !SWIFT_CONCURRENCY_EMBEDDED
  const Metadata *metadata = nullptr;
  bool isNull() {
    return metadata == nullptr;
  }
  size_t vw_size() {
    return metadata->vw_size();
  }
  size_t vw_alignment() {
    return metadata->vw_alignment();
  }
  void vw_initializeWithCopy(OpaqueValue *result, OpaqueValue *src) {
    metadata->vw_initializeWithCopy(result, src);
  }
  void vw_storeEnumTagSinglePayload(OpaqueValue *v, unsigned whichCase,
                                    unsigned emptyCases) {
    metadata->vw_storeEnumTagSinglePayload(v, whichCase, emptyCases);
  }
  void vw_destroy(OpaqueValue *v) {
    metadata->vw_destroy(v);
  }
#else
  size_t size = 0;
  size_t alignMask = 0;
  void (*initializeWithCopy)(OpaqueValue *result, OpaqueValue *src) = nullptr;
  void (*storeEnumTagSinglePayload)(OpaqueValue *v, unsigned whichCase,
                                    unsigned emptyCases) = nullptr;
  void (*destroy)(OpaqueValue *) = nullptr;

  bool isNull() {
    return initializeWithCopy == nullptr;
  }
  size_t vw_size() {
    return size;
  }
  size_t vw_alignment() {
    return alignMask + 1;
  }
  void vw_initializeWithCopy(OpaqueValue *result, OpaqueValue *src) {
    initializeWithCopy(result, src);
  }
  void vw_storeEnumTagSinglePayload(OpaqueValue *v, unsigned whichCase,
                                    unsigned emptyCases) {
    storeEnumTagSinglePayload(v, whichCase, emptyCases);
  }
  void vw_destroy(OpaqueValue *v) {
    destroy(v);
  }
#endif
};

/// An asynchronous task.  Tasks are the analogue of threads for
/// asynchronous functions: that is, they are a persistent identity
/// for the overall async computation.
///
/// ### Fragments
/// An AsyncTask may have the following fragments:
///
///    +--------------------------+
///    | childFragment?           |
///    | groupChildFragment?      |
///    | futureFragment?          |*
///    +--------------------------+
///
/// * The future fragment is dynamic in size, based on the future result type
///   it can hold, and thus must be the *last* fragment.
class AsyncTask : public Job {
public:
  // On 32-bit targets, there is a word of tail padding remaining
  // in Job, and ResumeContext will fit into that, at offset 28.
  // Private then has offset 32.
  // On 64-bit targets, there is no tail padding in Job, and so
  // ResumeContext has offset 48.  There is therefore another word
  // of reserved storage prior to Private (which needs to have
  // double-word alignment), which has offset 64.
  // We therefore converge and end up with 16 words of storage on
  // all platforms.

  /// The context for resuming the job.  When a task is scheduled
  /// as a job, the next continuation should be installed as the
  /// ResumeTask pointer in the job header, with this serving as
  /// the context pointer.
  ///
  /// We can't protect the data in the context from being overwritten
  /// by attackers, but we can at least sign the context pointer to
  /// prevent it from being corrupted in flight.
  AsyncContext * __ptrauth_swift_task_resume_context ResumeContext;

#if SWIFT_POINTER_IS_8_BYTES
  void *Reserved64;
#endif

  struct PrivateStorage;

  /// Private storage for the use of the runtime.
  struct alignas(2 * alignof(void*)) OpaquePrivateStorage {
    void *Storage[14];

    /// Initialize this storage during the creation of a task.
    void initialize(JobPriority basePri);
    void initializeWithSlab(JobPriority basePri, void *slab,
                            size_t slabCapacity);

    /// React to the completion of the enclosing task's execution.
    void complete(AsyncTask *task);

    /// React to the final destruction of the enclosing task.
    void destroy();

    PrivateStorage &get();
    const PrivateStorage &get() const;
  };
  PrivateStorage &_private();
  const PrivateStorage &_private() const;

  OpaquePrivateStorage Private;

  /// Create a task.
  /// This does not initialize Private; callers must call
  /// Private.initialize separately.
  AsyncTask(const HeapMetadata *metadata, JobFlags flags,
            TaskContinuationFunction *run,
            AsyncContext *initialContext,
            bool captureCurrentVoucher)
    : Job(flags, run, metadata, captureCurrentVoucher),
      ResumeContext(initialContext) {
    assert(flags.isAsyncTask());
    setTaskId();
  }

  /// Create a task with "immortal" reference counts.
  /// Used for async let tasks.
  /// This does not initialize Private; callers must call
  /// Private.initialize separately.
  AsyncTask(const HeapMetadata *metadata, InlineRefCounts::Immortal_t immortal,
            JobFlags flags,
            TaskContinuationFunction *run,
            AsyncContext *initialContext,
            bool captureCurrentVoucher)
    : Job(flags, run, metadata, immortal, captureCurrentVoucher),
      ResumeContext(initialContext) {
    assert(flags.isAsyncTask());
    setTaskId();
  }

  ~AsyncTask();

  /// Set the task's ID field to the next task ID.
  void setTaskId();
  uint64_t getTaskId();

  /// Get the task's resume function, for logging purposes only. This will
  /// attempt to see through the various adapters that are sometimes used, and
  /// failing that will return ResumeTask. The returned function pointer may
  /// have a different signature than ResumeTask, and it's only for identifying
  /// code associated with the task.
  ///
  /// If isStarting is true, look into the resume context when appropriate
  /// to pull out a wrapped resume function. If isStarting is false, assume the
  /// resume context may not be valid and just return the wrapper.
  const void *getResumeFunctionForLogging(bool isStarting);

  /// Given that we've already fully established the job context
  /// in the current thread, start running this task.  To establish
  /// the job context correctly, call swift_job_run or
  /// runInExecutorContext.
  SWIFT_CC(swiftasync)
  void runInFullyEstablishedContext() {
    return ResumeTask(ResumeContext); // 'return' forces tail call
  }

  /// A task can have the following states:
  ///   * suspended: In this state, a task is considered not runnable
  ///   * enqueued: In this state, a task is considered runnable
  ///   * running on a thread
  ///   * completed
  ///
  /// The following state transitions are possible:
  ///       suspended -> enqueued
  ///       suspended -> running
  ///       enqueued -> running
  ///       running -> suspended
  ///       running -> completed
  ///       running -> enqueued
  ///
  /// The 4 methods below are how a task switches from one state to another.

  /// Flag that this task is now running.  This can update
  /// the priority stored in the job flags if the priority has been
  /// escalated.
  ///
  /// Generally this should be done immediately after updating
  /// ActiveTask.
  void flagAsRunning();

  /// Flag that this task is now suspended with information about what it is
  /// waiting on.
  void flagAsSuspendedOnTask(AsyncTask *task);
  void flagAsSuspendedOnContinuation(ContinuationAsyncContext *context);
  void flagAsSuspendedOnTaskGroup(TaskGroup *taskGroup);

private:
  // Helper function
  void flagAsSuspended(TaskDependencyStatusRecord *dependencyStatusRecord);
  void destroyTaskDependency(TaskDependencyStatusRecord *dependencyRecord);

public:
  /// Flag that the task is to be enqueued on the provided executor and actually
  /// enqueue it.
  void flagAsAndEnqueueOnExecutor(SerialExecutorRef newExecutor);

  /// Flag that this task is now completed. This normally does not do anything
  /// but can be used to locally insert logging.
  void flagAsDestroyed();

  /// Check whether this task has been cancelled.
  /// Checking this is, of course, inherently race-prone on its own.
  bool isCancelled() const;

  // ==== Task Executor Preference ---------------------------------------------

  /// Get the preferred task executor reference if there is one set for this
  /// task.
  TaskExecutorRef getPreferredTaskExecutor(bool assumeHasRecord = false);

  /// WARNING: Only to be used during task creation, in other situations prefer
  /// to use `swift_task_pushTaskExecutorPreference` and
  /// `swift_task_popTaskExecutorPreference`.
  ///
  /// The `owned` parameter indicates if the executor is owned by the task,
  /// and must be released when the task completes.
  void pushInitialTaskExecutorPreference(
      TaskExecutorRef preferred, bool owned);

  /// WARNING: Only to be used during task completion (destroy).
  ///
  /// This is because between task creation and its destroy, we cannot carry the
  /// exact record to `pop(record)`, and instead assume that there will be
  /// exactly one record remaining -- the "initial" record (added during
  /// creating the task), and it must be that record that is removed by this
  /// api.
  ///
  /// All other situations from user code should be using the
  /// `swift_task_pushTaskExecutorPreference`, and
  /// `swift_task_popTaskExecutorPreference(record)` method pair.
  void dropInitialTaskExecutorPreferenceRecord();

  // ==== Task Local Values ----------------------------------------------------

  void localValuePush(const HeapObject *key,
                      /* +1 */ OpaqueValue *value,
                      const Metadata *valueType);

  OpaqueValue *localValueGet(const HeapObject *key);

  /// Returns true if storage has still more bindings.
  bool localValuePop();

  // ==== Child Fragment -------------------------------------------------------

  /// A fragment of an async task structure that happens to be a child task.
  class ChildFragment {
    /// The parent task of this task.
    AsyncTask *Parent;

    // TODO: Document more how this is used from the `TaskGroupTaskStatusRecord`

    /// The next task in the singly-linked list of child tasks.
    /// The list must start in a `ChildTaskStatusRecord` registered
    /// with the parent task.
    ///
    /// Note that the parent task may have multiple such records.
    ///
    /// WARNING: Access can only be performed by the `Parent` of this task.
    AsyncTask *NextChild = nullptr;

  public:
    ChildFragment(AsyncTask *parent) : Parent(parent) {}

    AsyncTask *getParent() const {
      return Parent;
    }

    AsyncTask *getNextChild() const {
      return NextChild;
    }

    /// Set the `NextChild` to to the passed task.
    ///
    /// WARNING: This must ONLY be invoked from the parent of both
    /// (this and the passed-in) tasks for thread-safety reasons.
    void setNextChild(AsyncTask *task) {
      NextChild = task;
    }
  };

  bool hasChildFragment() const {
    return Flags.task_isChildTask();
  }

  ChildFragment *childFragment() {
    assert(hasChildFragment());

    auto offset = reinterpret_cast<char*>(this);
    offset += sizeof(AsyncTask);

    return reinterpret_cast<ChildFragment*>(offset);
  }

  // ==== TaskGroup Child ------------------------------------------------------

  /// A child task created by `group.add` is called a "task group child."
  /// Upon completion, in addition to the usual future notifying all its waiters,
  /// it must also `group->offer` itself to the group.
  ///
  /// This signalling is necessary to correctly implement the group's `next()`.
  class GroupChildFragment {
  private:
    TaskGroup* Group;

    friend class AsyncTask;
    friend class TaskGroup;

  public:
    explicit GroupChildFragment(TaskGroup *group)
        : Group(group) {}

    /// Return the group this task should offer into when it completes.
    TaskGroup* getGroup() {
      return Group;
    }
  };

  // Checks if task is a child of a TaskGroup task.
  //
  // A child task that is a group child knows that it's parent is a group
  // and therefore may `groupOffer` to it upon completion.
  bool hasGroupChildFragment() const { return Flags.task_isGroupChildTask(); }

  GroupChildFragment *groupChildFragment() {
    assert(hasGroupChildFragment());

    auto offset = reinterpret_cast<char*>(this);
    offset += sizeof(AsyncTask);
    if (hasChildFragment())
      offset += sizeof(ChildFragment);

    return reinterpret_cast<GroupChildFragment *>(offset);
  }

  // ==== Task Executor Preference --------------------------------------------

  /// Returns true if the task has a task executor preference set,
  /// specifically at creation time of the task. This may be from
  /// inheriting the preference from a parent task, or by explicitly
  /// setting it during creation (`Task(_on:...)`).
  ///
  /// This means that during task tear down the record should be deallocated
  /// because it will not be taken care of by a paired "pop" as the normal
  /// user-land "push / pop" pair of setting a task executor preference would
  /// have been.
  bool hasInitialTaskExecutorPreferenceRecord() const {
    return Flags.task_hasInitialTaskExecutorPreference();
  }

  /// Returns true if the current task has any task preference record,
  /// including if it has an initial task preference record or onces
  /// set during the lifetime of the task.
  bool hasTaskExecutorPreferenceRecord() const;

  // ==== Future ---------------------------------------------------------------

  class FutureFragment {
  public:
    /// Describes the status of the future.
    ///
    /// Futures always begin in the "Executing" state, and will always
    /// make a single state change to either Success or Error.
    enum class Status : uintptr_t {
      /// The future is executing or ready to execute. The storage
      /// is not accessible.
      Executing = 0,

      /// The future has completed with result (of type \c resultType).
      Success,

      /// The future has completed by throwing an error (an \c Error
      /// existential).
      Error,
    };

    /// An item within the wait queue, which includes the status and the
    /// head of the list of tasks.
    struct WaitQueueItem {
      /// Mask used for the low status bits in a wait queue item.
      static const uintptr_t statusMask = 0x03;

      uintptr_t storage;

      Status getStatus() const {
        return static_cast<Status>(storage & statusMask);
      }

      AsyncTask *getTask() const {
        return reinterpret_cast<AsyncTask *>(storage & ~statusMask);
      }

      static WaitQueueItem get(Status status, AsyncTask *task) {
        return WaitQueueItem{
          reinterpret_cast<uintptr_t>(task) | static_cast<uintptr_t>(status)};
      }
    };

  private:
    /// Queue containing all of the tasks that are waiting in `get()`.
    ///
    /// The low bits contain the status, the rest of the pointer is the
    /// AsyncTask.
    std::atomic<WaitQueueItem> waitQueue;

    /// The type of the result that will be produced by the future.
    ResultTypeInfo resultType;

    SwiftError *error = nullptr;

    // Trailing storage for the result itself. The storage will be
    // uninitialized, contain an instance of \c resultType.

    friend class AsyncTask;

  public:
    explicit FutureFragment(ResultTypeInfo resultType)
      : waitQueue(WaitQueueItem::get(Status::Executing, nullptr)),
        resultType(resultType) { }

    /// Destroy the storage associated with the future.
    void destroy();

    ResultTypeInfo getResultType() const {
      return resultType;
    }

    /// Retrieve a pointer to the storage of the result.
    OpaqueValue *getStoragePtr() {
      // The result storage starts at the first aligned offset following
      // the fragment header.  This offset will agree with the abstract
      // calculation for `resultOffset` in the fragmentSize function below
      // because the entire task is aligned to at least the target
      // alignment (because it's aligned to MaxAlignment), which means
      // `this` must have the same value modulo that alignment as
      // `fragmentOffset` has in that function.
      char *fragmentAddr = reinterpret_cast<char *>(this);
      uintptr_t alignment = resultType.vw_alignment();
      char *resultAddr = fragmentAddr + sizeof(FutureFragment);
      uintptr_t unalignedResultAddrInt =
        reinterpret_cast<uintptr_t>(resultAddr);
      uintptr_t alignedResultAddrInt =
        (unalignedResultAddrInt + alignment - 1) & ~(alignment - 1);
      // We could just cast alignedResultAddrInt back to a pointer, but
      // doing pointer arithmetic is more strictly conformant and less
      // likely to annoy the optimizer.
      resultAddr += (alignedResultAddrInt - unalignedResultAddrInt);
      return reinterpret_cast<OpaqueValue *>(resultAddr);
    }

    /// Retrieve the error.
    SwiftError *&getError() { return error; }

    /// Determine the size of the future fragment given the result type
    /// of the future.
    static size_t fragmentSize(size_t fragmentOffset,
                               ResultTypeInfo resultType) {
      assert((fragmentOffset & (alignof(FutureFragment) - 1)) == 0);
      size_t alignment = resultType.vw_alignment();
      size_t resultOffset = fragmentOffset + sizeof(FutureFragment);
      resultOffset = (resultOffset + alignment - 1) & ~(alignment - 1);
      size_t endOffset = resultOffset + resultType.vw_size();
      return (endOffset - fragmentOffset);
    }
  };

  bool isFuture() const { return Flags.task_isFuture(); }

  FutureFragment *futureFragment() {
    assert(isFuture());
    auto offset = reinterpret_cast<char*>(this);
    offset += sizeof(AsyncTask);
    if (hasChildFragment())
      offset += sizeof(ChildFragment);
    if (hasGroupChildFragment())
      offset += sizeof(GroupChildFragment);

    return reinterpret_cast<FutureFragment *>(offset);
  }

  /// Wait for this future to complete.
  ///
  /// \returns the status of the future. If this result is
  /// \c Executing, then \c waitingTask has been added to the
  /// wait queue and will be scheduled when the future completes. Otherwise,
  /// the future has completed and can be queried.
  /// The waiting task's async context will be initialized with the parameters if
  /// the current's task state is executing.
  FutureFragment::Status waitFuture(AsyncTask *waitingTask,
                                    AsyncContext *waitingTaskContext,
                                    TaskContinuationFunction *resumeFn,
                                    AsyncContext *callerContext,
                                    OpaqueValue *result);

  /// Complete this future.
  ///
  /// Upon completion, any waiting tasks will be scheduled on the given
  /// executor.
  void completeFuture(AsyncContext *context);

  // ==== ----------------------------------------------------------------------

  static bool classof(const Job *job) {
    return job->isAsyncTask();
  }

private:
  /// Access the next waiting task, which establishes a singly linked list of
  /// tasks that are waiting on a future.
  AsyncTask *&getNextWaitingTask() {
    return reinterpret_cast<AsyncTask *&>(
        SchedulerPrivate[NextWaitingTaskIndex]);
  }
};

// The compiler will eventually assume these.
static_assert(sizeof(AsyncTask) == NumWords_AsyncTask * sizeof(void*),
              "AsyncTask size is wrong");
static_assert(alignof(AsyncTask) == 2 * alignof(void*),
              "AsyncTask alignment is wrong");
// Libc hardcodes this offset to extract the TaskID
static_assert(offsetof(AsyncTask, Id) == 4 * sizeof(void *) + 4,
              "AsyncTask::Id offset is wrong");

SWIFT_CC(swiftasync)
inline void Job::runInFullyEstablishedContext() {
  if (auto task = dyn_cast<AsyncTask>(this))
    return task->runInFullyEstablishedContext(); // 'return' forces tail call
  else
    return runSimpleInFullyEstablishedContext(); // 'return' forces tail call
}

// ==== ------------------------------------------------------------------------

/// An asynchronous context within a task.  Generally contexts are
/// allocated using the task-local stack alloc/dealloc operations, but
/// there's no guarantee of that, and the ABI is designed to permit
/// contexts to be allocated within their caller's frame.
class alignas(MaximumAlignment) AsyncContext {
public:
  /// The parent context.
  AsyncContext * __ptrauth_swift_async_context_parent Parent;

  /// The function to call to resume running in the parent context.
  /// Generally this means a semantic return, but for some temporary
  /// translation contexts it might mean initiating a call.
  ///
  /// Eventually, the actual type here will depend on the types
  /// which need to be passed to the parent.  For now, arguments
  /// are always written into the context, and so the type is
  /// always the same.
  TaskContinuationFunction * __ptrauth_swift_async_context_resume
    ResumeParent;

  AsyncContext(TaskContinuationFunction *resumeParent,
               AsyncContext *parent)
    : Parent(parent), ResumeParent(resumeParent) {}

  AsyncContext(const AsyncContext &) = delete;
  AsyncContext &operator=(const AsyncContext &) = delete;

  /// Perform a return from this context.
  ///
  /// Generally this should be tail-called.
  SWIFT_CC(swiftasync)
  void resumeParent() {
    // TODO: destroy context before returning?
    // FIXME: force tail call
    return ResumeParent(Parent);
  }
};

/// An async context that supports yielding.
class YieldingAsyncContext : public AsyncContext {
public:
  /// The function to call to temporarily resume running in the
  /// parent context.  Generally this means a semantic yield.
  TaskContinuationFunction * __ptrauth_swift_async_context_yield
    YieldToParent;

  YieldingAsyncContext(TaskContinuationFunction *resumeParent,
                       TaskContinuationFunction *yieldToParent,
                       AsyncContext *parent)
    : AsyncContext(resumeParent, parent),
      YieldToParent(yieldToParent) {}
};

/// An async context that can be resumed as a continuation.
class ContinuationAsyncContext : public AsyncContext {
public:
  class FlagsType : public FlagSet<size_t> {
  public:
    enum {
      CanThrow = 0,
      IsExecutorSwitchForced = 1,
    };

    explicit FlagsType(size_t bits) : FlagSet(bits) {}
    constexpr FlagsType() {}

    /// Whether this is a throwing continuation.
    FLAGSET_DEFINE_FLAG_ACCESSORS(CanThrow,
                                  canThrow,
                                  setCanThrow)

    /// See AsyncContinuationFlags::isExecutorSwitchForced().
    FLAGSET_DEFINE_FLAG_ACCESSORS(IsExecutorSwitchForced,
                                  isExecutorSwitchForced,
                                  setIsExecutorSwitchForced)
  };

  /// Flags for the continuation.  Not public ABI.
  FlagsType Flags;

  /// An atomic object used to ensure that a continuation is not
  /// scheduled immediately during a resume if it hasn't yet been
  /// awaited by the function which set it up.  Not public ABI.
  std::atomic<ContinuationStatus> AwaitSynchronization;

  /// The error result value of the continuation.
  /// This should be null-initialized when setting up the continuation.
  /// Throwing resumers must overwrite this with a non-null value.
  /// Public ABI.
  SwiftError *ErrorResult;

  /// A pointer to the normal result value of the continuation.
  /// Normal resumers must initialize this before resuming.
  /// Public ABI.
  OpaqueValue *NormalResult;

  /// The executor that should be resumed to.
  /// Public ABI.
  SerialExecutorRef ResumeToExecutor;

#if defined(SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY)
  /// In a task-to-thread model, instead of voluntarily descheduling the task
  /// from the thread, we will block the thread (and therefore task).
  /// This condition variable is lazily allocated on the stack only if the
  /// continuation has not been resumed by the point of await. The mutex in the
  /// condition variable is therefore not really protecting any state as all
  /// coordination is done via the AwaitSynchronization atomic
  ConditionVariable *Cond;
#endif

  void setErrorResult(SwiftError *error) {
    ErrorResult = error;
  }

  bool isExecutorSwitchForced() const {
    return Flags.isExecutorSwitchForced();
  }
};

/// An asynchronous context within a task that describes a general "Future".
/// task.
///
/// This type matches the ABI of a function `<T> () async throws -> T`, which
/// is the type used by `detach` and `Task.group.add` to create
/// futures.
class FutureAsyncContext : public AsyncContext {
public:
  using AsyncContext::AsyncContext;
};

/// This matches the ABI of a closure `() async throws -> ()`
using AsyncVoidClosureEntryPoint =
  SWIFT_CC(swiftasync)
  void (SWIFT_ASYNC_CONTEXT AsyncContext *, SWIFT_CONTEXT void *);

/// This matches the ABI of a closure `<T>() async throws -> T`
using AsyncGenericClosureEntryPoint =
    SWIFT_CC(swiftasync)
    void(OpaqueValue *,
         SWIFT_ASYNC_CONTEXT AsyncContext *, SWIFT_CONTEXT void *);

/// This matches the ABI of the resume function of a closure
///  `() async throws -> ()`.
using AsyncVoidClosureResumeEntryPoint =
  SWIFT_CC(swiftasync)
  void(SWIFT_ASYNC_CONTEXT AsyncContext *, SWIFT_CONTEXT SwiftError *);

class AsyncContextPrefix {
public:
  // Async closure entry point adhering to compiler calling conv (e.g directly
  // passing the closure context instead of via the async context)
  AsyncVoidClosureEntryPoint *__ptrauth_swift_task_resume_function
      asyncEntryPoint;
  void *closureContext;
  SwiftError *errorResult;
};

/// Storage that is allocated before the AsyncContext to be used by an adapter
/// of Swift's async convention and the ResumeTask interface.
class FutureAsyncContextPrefix {
public:
  OpaqueValue *indirectResult;
  // Async closure entry point adhering to compiler calling conv (e.g directly
  // passing the closure context instead of via the async context)
  AsyncGenericClosureEntryPoint *__ptrauth_swift_task_resume_function
      asyncEntryPoint;
  void *closureContext;
  SwiftError *errorResult;
};

} // end namespace swift

#endif
