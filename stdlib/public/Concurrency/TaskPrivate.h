//===--- TaskPrivate.h - Concurrency library internal interface -*- C++ -*-===//
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
// Internal functions for the concurrency library.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_CONCURRENCY_TASKPRIVATE_H
#define SWIFT_CONCURRENCY_TASKPRIVATE_H

#include "Error.h"
#include "Tracing.h"
#include "swift/ABI/Metadata.h"
#include "swift/ABI/Task.h"
#include "swift/ABI/TaskStatus.h"
#include "swift/Runtime/Atomic.h"
#include "swift/Runtime/Concurrency.h"
#include "swift/Runtime/DispatchShims.h"
#include "swift/Runtime/Error.h"
#include "swift/Runtime/Exclusivity.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Threading/Thread.h"
#include "swift/Threading/ThreadSanitizer.h"
#include <atomic>
#include <new>

#define SWIFT_FATAL_ERROR swift_Concurrency_fatalError
#include "../runtime/StackAllocator.h"

namespace swift {

// Set to 1 to enable helpful debug spew to stderr
// If this is enabled, tests with `swift_task_debug_log` requirement can run.
#if 0
#define SWIFT_TASK_DEBUG_LOG_ENABLED 1
#define SWIFT_TASK_DEBUG_LOG(fmt, ...)                                         \
  fprintf(stderr, "[%#lx] [%s:%d](%s) " fmt "\n",                              \
          (unsigned long)Thread::current().platformThreadId(), __FILE__,       \
          __LINE__, __FUNCTION__, __VA_ARGS__)
#else
#define SWIFT_TASK_DEBUG_LOG_ENABLED 0
#define SWIFT_TASK_DEBUG_LOG(fmt, ...) (void)0
#endif

class AsyncTask;
class TaskGroup;
class ActiveTaskStatus;

/// Allocate task-local memory on behalf of a specific task,
/// not necessarily the current one.  Generally this should only be
/// done on behalf of a child task.
void *_swift_task_alloc_specific(AsyncTask *task, size_t size);

/// dellocate task-local memory on behalf of a specific task,
/// not necessarily the current one.  Generally this should only be
/// done on behalf of a child task.
void _swift_task_dealloc_specific(AsyncTask *task, void *ptr);

/// Given that we've already set the right executor as the active
/// executor, run the given job.  This does additional bookkeeping
/// related to the active task.
void runJobInEstablishedExecutorContext(Job *job);

/// Adopt the voucher stored in `task`. This removes the voucher from the task
/// and adopts it on the current thread.
void adoptTaskVoucher(AsyncTask *task);

/// Restore the voucher for `task`. This un-adopts the current thread's voucher
/// and stores it back into the task again.
void restoreTaskVoucher(AsyncTask *task);

/// Initialize the async let storage for the given async-let child task.
void asyncLet_addImpl(AsyncTask *task, AsyncLet *asyncLet,
                      bool didAllocateInParentTask);

/// Clear the active task reference for the current thread.
AsyncTask *_swift_task_clearCurrent();
/// Set the active task reference for the current thread.
AsyncTask *_swift_task_setCurrent(AsyncTask *newTask);

/// Cancel all the child tasks that belong to `group`.
///
/// The caller must guarantee that this is either called from the
/// owning task of the task group or while holding the owning task's
/// status record lock.
void _swift_taskGroup_cancelAllChildren(TaskGroup *group);

/// Remove the given task from the given task group.
///
/// This is an internal API; clients outside of the TaskGroup implementation
/// should generally use a higher-level function.
void _swift_taskGroup_detachChild(TaskGroup *group, AsyncTask *child);

/// Tell TSan about an acquiring load
inline void _swift_tsan_acquire(void *addr) {
  swift::tsan::acquire(addr);
}
/// Tell TSan about a releasing store
inline void _swift_tsan_release(void *addr) {
  swift::tsan::release(addr);
}
/// Tell TSan about a consuming load
inline void _swift_tsan_consume(void *addr) {
  // TSan doesn't support consume, so pretend it's an acquire.
  //
  // Note that that means that TSan won't generate errors for non-dependent
  // reads, so this isn't entirely safe if you're relying solely on TSan to
  // spot bugs.
  swift::tsan::acquire(addr);
}

/// Special values used with DispatchQueueIndex to indicate the global and main
/// executors.
#define DISPATCH_QUEUE_GLOBAL_EXECUTOR (void *)1

#if SWIFT_CONCURRENCY_ENABLE_DISPATCH
inline SerialExecutorWitnessTable *
_swift_task_getDispatchQueueSerialExecutorWitnessTable() {
  extern SerialExecutorWitnessTable wtable
    SWIFT_ASM_LABEL_WITH_PREFIX("$ss17DispatchQueueShimCScfsWP");
  return &wtable;
}
#endif

// The task listed as argument is escalated to a new priority. Pass that
// information along to the executor that it is enqueued into.
SWIFT_CC(swift)
void
swift_executor_escalate(ExecutorRef executor, AsyncTask *task, JobPriority newPriority);

/*************** Methods for Status records manipulation ******************/

/// Add a status record to the input task.
///
/// Clients can optionally pass in the status of the task if they have already
/// done the load on it already or if they require the oldStatus on the task
/// prior to the atomic ActiveTaskStatus update that addStatusRecord will
/// perform. This status will be updated with the last status on the task prior
/// to updating it with the new status if the input function_ref allows so.
///
/// Clients can optionally pass in the status of the task if they have already
/// done the load on it or if they require the oldStatus on the task prior to
/// the atomic ActiveTaskStatus update that addStatusRecord will do. This status
/// will be updated with the last status on the task prior to updating it with
/// the new status if the input function_ref allows so.
///
/// This function also takes in a function_ref which is given the old
/// ActiveTaskStatus on the task and a reference to the new ActiveTaskStatus
/// that is to be set on the task that we are adding the record to.
///
/// This can be used by the function to
/// (1) to determine if the current status of the task permits adding the status
/// record
/// (2) modify the active task status that is to be set if needed
///
/// If the function_ref returns false, the status record is not added to the
/// task. This function_ref may be called multiple times and must be idempotent.
/// The new status passed to `fn` is freshly derived from the current status and
/// does not include modifications made by previous runs through the loop.
///
/// A convenience overload is also provided for clients who have not done the
/// load on the task status prior to adding the record
SWIFT_CC(swift)
bool addStatusRecord(AsyncTask *task, TaskStatusRecord *record,
     llvm::function_ref<bool(ActiveTaskStatus, ActiveTaskStatus&)> testAddRecord);

SWIFT_CC(swift)
bool addStatusRecord(AsyncTask *task, TaskStatusRecord *record,
     ActiveTaskStatus& taskStatus,
     llvm::function_ref<bool(ActiveTaskStatus, ActiveTaskStatus&)> testAddRecord);

/// Convenience functions for clients who want to just add a record to the task
/// on the current thread
SWIFT_CC(swift)
bool addStatusRecordToSelf(TaskStatusRecord *record,
     llvm::function_ref<bool(ActiveTaskStatus, ActiveTaskStatus&)> testAddRecord);

SWIFT_CC(swift)
bool addStatusRecordToSelf(TaskStatusRecord *record,  ActiveTaskStatus& taskStatus,
     llvm::function_ref<bool(ActiveTaskStatus, ActiveTaskStatus&)> testAddRecord);

/// Remove the status record from input task which may not be the current task.
/// This may be called asynchronously from the current task.  After this call
/// returns, the record's memory can be freely modified or deallocated.  The
/// record must be registered with the task. If it isn't, this function will
/// crash.
///
/// The given record need not be the last record added to
/// the task, but the operation may be less efficient if not.
///
/// This function also takes in a function_ref which is given the old
/// ActiveTaskStatus on the task and a reference to the new ActiveTaskStatus
/// that is to be set on the task that we are removing the record from. It may
/// modify the new ActiveTaskStatus that is to be set on the task. This function
/// may be called multiple times inside a RMW loop and must be therefore be
/// idempotent. The new status passed to `fn` is freshly derived from the
/// current status and does not include modifications made by previous runs
/// through the loop
SWIFT_CC(swift)
void removeStatusRecord(AsyncTask *task, TaskStatusRecord *record, ActiveTaskStatus& status,
     llvm::function_ref<void(ActiveTaskStatus, ActiveTaskStatus&)>fn = nullptr);

SWIFT_CC(swift)
void removeStatusRecord(AsyncTask *task, TaskStatusRecord *record,
     llvm::function_ref<void(ActiveTaskStatus, ActiveTaskStatus&)>fn = nullptr);

/// Remove a status record from the current task. This must be called
/// synchronously with the task.
SWIFT_CC(swift)
void removeStatusRecordFromSelf(TaskStatusRecord *record, ActiveTaskStatus &status,
     llvm::function_ref<void(ActiveTaskStatus, ActiveTaskStatus&)>fn = nullptr);

SWIFT_CC(swift)
void removeStatusRecordFromSelf(TaskStatusRecord *record,
     llvm::function_ref<void(ActiveTaskStatus, ActiveTaskStatus&)>fn = nullptr);

/// Update the specified input status record while holding the status record
/// lock of the task. The status record must already be registered with the
/// task - if it isn't, this API provides no additional protections.
///
/// This function also takes in a function_ref which is given the old
/// ActiveTaskStatus on the task and a reference to the new ActiveTaskStatus
/// that is to be set on the task when we are unlocking the task status record
/// lock. It may modify the new ActiveTaskStatus that is to be set on the task.
/// This function may be called multiple times inside a RMW loop and must be
/// therefore be idempotent. The new status passed to `fn` is freshly derived
/// from the current status and does not include modifications made by previous
/// runs through the loop
SWIFT_CC(swift)
void updateStatusRecord(AsyncTask *task, TaskStatusRecord *record,
     llvm::function_ref<void()>updateRecord,
     ActiveTaskStatus& status,
     llvm::function_ref<void(ActiveTaskStatus, ActiveTaskStatus&)>fn = nullptr);

/// A helper function for updating a new child task that is created with
/// information from the parent or the group that it was going to be added to.
SWIFT_CC(swift)
void updateNewChildWithParentAndGroupState(AsyncTask *child,
                                           ActiveTaskStatus parentStatus,
                                           TaskGroup *group);

// ==== ------------------------------------------------------------------------

namespace {

/// The layout of a context to call one of the following functions:
///
///   @_silgen_name("swift_task_future_wait")
///   func _taskFutureGet<T>(_ task: Builtin.NativeObject) async -> T
///
///   @_silgen_name("swift_task_future_wait_throwing")
///   func _taskFutureGetThrowing<T>(_ task: Builtin.NativeObject) async throws -> T
///
///   @_silgen_name("swift_asyncLet_wait")
///   func _asyncLetGet<T>(_ task: Builtin.RawPointer) async -> T
///
///   @_silgen_name("swift_asyncLet_waitThrowing")
///   func _asyncLetGetThrowing<T>(_ task: Builtin.RawPointer) async throws -> T
///
///   @_silgen_name("swift_taskGroup_wait_next_throwing")
///   func _taskGroupWaitNext<T>(group: Builtin.RawPointer) async throws -> T?
///
///   @_silgen_name("swift_taskGroup_waitAll")
///   func _taskGroupWaitAll<T>(
///     group: Builtin.RawPointer,
///     bodyError: Swift.Error?
///   ) async throws -> T?
///
class TaskFutureWaitAsyncContext : public AsyncContext {
public:
  // The ABI reserves three words of storage for these contexts, which
  // we currently use as follows.  These fields are not accessed by
  // generated code; they're purely internal to the runtime, and only
  // when the calling task actually suspends.
  //
  // (If you think three words is an odd choice, one of them used to be
  // the context flags.)
  SwiftError *errorResult;
  OpaqueValue *successResultPointer;
  void *_reserved;

  void fillWithSuccess(AsyncTask::FutureFragment *future) {
    fillWithSuccess(future->getStoragePtr(), future->getResultType(),
                    successResultPointer);
  }
  void fillWithSuccess(OpaqueValue *src, const Metadata *successType,
                       OpaqueValue *result) {
    successType->vw_initializeWithCopy(result, src);
  }

  void fillWithError(AsyncTask::FutureFragment *future) {
    fillWithError(future->getError());
  }
  void fillWithError(SwiftError *error) {
    errorResult = error;
    swift_errorRetain(error);
  }
};

} // end anonymous namespace

/// The current state of a task's status records.
///
/// The runtime needs to track the following state about the task in within the
/// same atomic:
///
/// * The current status of the task (whether it is running, waiting in a queue,
/// waiting on another task to finish, etc.)
/// * The cancellation state
/// * The locked state of the status record
/// * The current maximum priority of the task
/// * The identity of the thread executing the task
/// * Pointer to head of TaskStatusRecords
///
/// It is important for all of this information to be in the same atomic so that
/// when the task's state changes, the information is visible to all threads that
/// may be modifying the task, allowing the algorithm to eventually converge.
///
/// Some changes to task state, like cancellation are local to the concurrency
/// runtime library but others like priority escalation require deeper integration
/// with the OS in order to have the intended side effects. On Darwin, Swift
/// Concurrency Tasks runs on dispatch's queues. As such, we need to use an
/// encoding of thread identity vended by libdispatch called dispatch_lock_t,
/// and a futex-style dispatch API in order to escalate the priority of a thread.
/// When the thread gives up running a task, the priority escalation that may
/// have been applied on the thread, is removed. Henceforth, the dispatch_lock_t
/// tracked in the ActiveTaskStatus will be called the ExecutionLock.
///
/// In order to keep the entire ActiveTaskStatus size to 2 words, the thread
/// identity is only tracked on platforms which can support 128 bit atomic
/// operations. The ActiveTaskStatus's layout has thus been changed to have the
/// following layout depending on the system configuration supported:
///
/// 32 bit systems with SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION=1
///
///          Flags               Execution Lock           Unused           TaskStatusRecord *
/// |----------------------|----------------------|----------------------|-------------------|
///          32 bits                32 bits                32 bits              32 bits
///
/// 64 bit systems with SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION=1
///
///         Flags              Execution Lock      TaskStatusRecord *
/// |----------------------|-------------------|----------------------|
///          32 bits                32 bits              64 bits
///
/// 32 bit systems with SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION=0
///
///          Flags             TaskStatusRecord *
/// |----------------------|----------------------|
///          32 bits                32 bits
//
/// 64 bit systems with SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION=0
///
///         Flags                  Unused            TaskStatusRecord *
/// |----------------------|----------------------|---------------------|
///         32 bits                 32 bits                64 bits
///
/// Note: This layout is intentional so that we can then do 64 bit atomics on
/// just the top 64 bits of atomic state, including escalation of a thread. We
/// only need to do double wide atomics if we need to reach for the
/// StatusRecord pointers and therefore have to update the flags at the same
/// time.
///
/// Size requirements:
///     On 64 bit systems or if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION=1,
///     the field is 16 bytes long.
///
///     Otherwise, it is 8 bytes long.
///
/// Alignment requirements:
///     On 64 bit systems or if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION=1,
///     this 16-byte field needs to be 16 byte aligned to be able to do aligned
///     atomic stores field.
///
///     On all other systems, it needs to be 8 byte aligned for the atomic
///     stores.
///
///     As a result of varying alignment needs, we've marked the class as
///     needing 2-word alignment but on arm64_32 with
///     SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION=1, 16 byte alignment is
///     achieved through careful arrangement of the storage for this in the
///     AsyncTask::PrivateStorage. The additional alignment requirements are
///     enforced by static asserts below.
class alignas(2 * sizeof(void*)) ActiveTaskStatus {
  enum : uint32_t {
    /// The max priority of the task. This is always >= basePriority in the task
    PriorityMask = 0xFF,

    /// Has the task been cancelled?
    IsCancelled = 0x100,

    /// Whether the task status is "locked", meaning that further
    /// accesses need to wait on the task status record lock. This is separate
    /// from the drain lock of the task.
    IsStatusRecordLocked = 0x200,

    /// Whether the running priority has been escalated above the previous max
    /// priority in the task status
    IsEscalated = 0x400,

#if !SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    /// Whether the task is actively running. On systems which cannot track the
    /// identity of the drainer (see above), we use one bit in the flags to track
    /// whether or not task is running. Otherwise, the drain lock tells us
    /// whether or not it is running.
    IsRunning = 0x800,
#endif
    /// Task is intrusively enqueued somewhere - either in the default executor
    /// pool, or in an actor. Currently, due to lack of task stealers, this bit
    /// is cleared when a task starts running on a thread, suspends or is
    /// completed.
    ///
    /// TODO (rokhinip): Once we have task stealers, this bit refers to the
    /// enqueued-ness on the specific queue that the task is linked into and
    /// therefore, can only be cleared when the intrusively linkage is cleaned
    /// up. The enqueued-ness then becomes orthogonal to the other states of
    /// running/suspended/completed.
    IsEnqueued = 0x1000,
    /// Task has been completed.  This is purely used to enable an assertion
    /// that the task is completed when we destroy it.
    IsComplete = 0x2000,

    /// The task has a dependency and therefore, also a
    /// TaskDependencyStatusRecord in the status record list.
    HasTaskDependency = 0x4000,
  };

  // Note: this structure is mirrored by ActiveTaskStatusWithEscalation and
  // ActiveTaskStatusWithoutEscalation in
  // include/swift/RemoteInspection/RuntimeInternals.h. Any changes to the layout here
  // must also be made there.
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION && SWIFT_POINTER_IS_4_BYTES
  uint32_t Flags;
  dispatch_lock_t ExecutionLock;
  LLVM_ATTRIBUTE_UNUSED uint32_t Unused = {};
#elif SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION && SWIFT_POINTER_IS_8_BYTES
  uint32_t Flags;
  dispatch_lock_t ExecutionLock;
#elif !SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION && SWIFT_POINTER_IS_4_BYTES
  uint32_t Flags;
#else /* !SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION && SWIFT_POINTER_IS_8_BYTES */
  uint32_t Flags;
  LLVM_ATTRIBUTE_UNUSED uint32_t Unused = {};
#endif
  TaskStatusRecord *Record;

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
  ActiveTaskStatus(TaskStatusRecord *record, uintptr_t flags)
    : Flags(flags), ExecutionLock(DLOCK_OWNER_NULL), Record(record) {}

  ActiveTaskStatus(TaskStatusRecord *record, uintptr_t flags, dispatch_lock_t executionLockValue)
    : Flags(flags), ExecutionLock(executionLockValue), Record(record) {}

#else
  ActiveTaskStatus(TaskStatusRecord *record, uintptr_t flags)
    : Flags(flags), Record(record) {}
#endif

public:
#ifdef __GLIBCXX__
  /// We really don't want to provide this constructor, but in old
  /// versions of libstdc++, std::atomic<T>::load incorrectly requires
  /// the type to be default-constructible.
  ActiveTaskStatus() = default;
#endif

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
  constexpr ActiveTaskStatus(JobPriority priority)
      : Flags(uintptr_t(priority)), ExecutionLock(DLOCK_OWNER_NULL), Record(nullptr) {}
#else
  constexpr ActiveTaskStatus(JobPriority priority)
      : Flags(uintptr_t(priority)), Record(nullptr) {}
#endif

  /// Is the task currently cancelled?
  bool isCancelled() const { return Flags & IsCancelled; }
  ActiveTaskStatus withCancelled() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveTaskStatus(Record, Flags | IsCancelled, ExecutionLock);
#else
    return ActiveTaskStatus(Record, Flags | IsCancelled);
#endif
  }

  bool isEnqueued() const { return Flags & IsEnqueued; }
  ActiveTaskStatus withEnqueued() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveTaskStatus(Record, Flags | IsEnqueued, ExecutionLock);
#else
    return ActiveTaskStatus(Record, Flags | IsEnqueued);
#endif
  }
  ActiveTaskStatus withoutEnqueued() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveTaskStatus(Record, Flags & ~IsEnqueued, ExecutionLock);
#else
    return ActiveTaskStatus(Record, Flags & ~IsEnqueued);
#endif
  }

  /// Is the task currently running? Also known as whether it is drain locked.
  bool isRunning() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
  return dispatch_lock_is_locked(ExecutionLock);
#else
    return Flags & IsRunning;
#endif
  }

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
  static size_t executionLockOffset() {
    return offsetof(ActiveTaskStatus, ExecutionLock);
  }
#endif

  uint32_t currentExecutionLockOwner() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
  return dispatch_lock_owner(ExecutionLock);
#else
  return 0;
#endif
  }

  ActiveTaskStatus withRunning(bool isRunning) const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
  if (isRunning) {
    assert(!dispatch_lock_is_locked(ExecutionLock));
    return ActiveTaskStatus(Record, Flags, dispatch_lock_value_for_self());
  } else {
    return ActiveTaskStatus(Record, Flags, ExecutionLock & ~DLOCK_OWNER_MASK);
  }
#else
    return ActiveTaskStatus(Record, isRunning ? (Flags | IsRunning)
                                              : (Flags & ~IsRunning));
#endif
  }

  bool isComplete() const {
    return Flags & IsComplete;
  }

  ActiveTaskStatus withComplete() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveTaskStatus(Record, Flags | IsComplete, ExecutionLock);
#else
    return ActiveTaskStatus(Record, Flags | IsComplete);
#endif
  }

  /// Is there a lock on the linked list of status records?
  bool isStatusRecordLocked() const { return Flags & IsStatusRecordLocked; }
  ActiveTaskStatus withLockingRecord(TaskStatusRecord *lockRecord) const {
    assert(!isStatusRecordLocked());
    assert(lockRecord->Parent == Record);
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveTaskStatus(lockRecord, Flags | IsStatusRecordLocked, ExecutionLock);
#else
    return ActiveTaskStatus(lockRecord, Flags | IsStatusRecordLocked);
#endif
  }
  ActiveTaskStatus withoutLockingRecord() const {
    assert(isStatusRecordLocked());
    assert(Record->getKind() == TaskStatusRecordKind::Private_RecordLock);

    // Remove the lock record, and put the next one as the head
    auto newRecord = Record->Parent;
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveTaskStatus(newRecord, Flags & ~IsStatusRecordLocked, ExecutionLock);
#else
    return ActiveTaskStatus(newRecord, Flags & ~IsStatusRecordLocked);
#endif
  }

  /// Is there a dependencyRecord in the linked list of status records?
  bool hasTaskDependency() const { return Flags & HasTaskDependency; }
  ActiveTaskStatus withTaskDependency() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveTaskStatus(Record, Flags | HasTaskDependency, ExecutionLock);
#else
    return ActiveTaskStatus(Record, Flags | HasTaskDependency);
#endif
  }
  ActiveTaskStatus withoutTaskDependency() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveTaskStatus(Record, Flags & ~HasTaskDependency, ExecutionLock);
#else
    return ActiveTaskStatus(Record, Flags & ~HasTaskDependency);
#endif
  }

  JobPriority getStoredPriority() const {
    return JobPriority(Flags & PriorityMask);
  }
  bool isStoredPriorityEscalated() const {
    return Flags & IsEscalated;
  }

  /// Creates a new active task status for a task with the specified priority
  /// and masks away any existing priority related flags on the task status. All
  /// other flags about the task are unmodified. This is only safe to use when
  /// we know that the task we're modifying is not running.
  ActiveTaskStatus withNewPriority(JobPriority priority) const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    assert(ExecutionLock == DLOCK_OWNER_NULL);
#endif
    assert(!isEnqueued());
    return ActiveTaskStatus(Record,
                            (Flags & ~PriorityMask) | uintptr_t(priority));
  }

  ActiveTaskStatus withEscalatedPriority(JobPriority priority) const {
    assert(priority > getStoredPriority());
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveTaskStatus(Record,
                            (Flags & ~PriorityMask)
                               | IsEscalated | uintptr_t(priority), ExecutionLock);
#else
    return ActiveTaskStatus(Record, (Flags & ~PriorityMask) | IsEscalated | uintptr_t(priority));
#endif
  }

  ActiveTaskStatus withoutStoredPriorityEscalation() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveTaskStatus(Record, Flags & ~IsEscalated, ExecutionLock);
#else
    return ActiveTaskStatus(Record, Flags & ~IsEscalated);
#endif
  }

  /// Return the innermost cancellation record.  Code running
  /// asynchronously with this task should not access this record
  /// without having first locked it; see swift_taskCancel.
  TaskStatusRecord *getInnermostRecord() const {
    return Record;
  }
  ActiveTaskStatus withInnermostRecord(TaskStatusRecord *newRecord) {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveTaskStatus(newRecord, Flags, ExecutionLock);
#else
    return ActiveTaskStatus(newRecord, Flags);
#endif
  }

  static TaskStatusRecord *getStatusRecordParent(TaskStatusRecord *ptr);

  using record_iterator =
    LinkedListIterator<TaskStatusRecord, getStatusRecordParent>;
  llvm::iterator_range<record_iterator> records() const {
    return record_iterator::rangeBeginning(getInnermostRecord());
  }

  void traceStatusChanged(AsyncTask *task) {
    concurrency::trace::task_status_changed(
        task, static_cast<uint8_t>(getStoredPriority()), isCancelled(),
        isStoredPriorityEscalated(), isRunning(), isEnqueued());
  }
};

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION && SWIFT_POINTER_IS_4_BYTES
#define ACTIVE_TASK_STATUS_SIZE (4 * (sizeof(uintptr_t)))
#else
#define ACTIVE_TASK_STATUS_SIZE (2 * (sizeof(uintptr_t)))
#endif
static_assert(sizeof(ActiveTaskStatus) == ACTIVE_TASK_STATUS_SIZE,
  "ActiveTaskStatus is of incorrect size");

/// The size of an allocator slab. We want the full allocation to fit into a
/// 1024-byte malloc quantum. We subtract off the slab header size, plus a
/// little extra to stay within our limits even when there's overhead from
/// malloc stack logging.
static constexpr size_t SlabCapacity = 1024 - StackAllocator<0, nullptr>::slabHeaderSize() - 8;
extern Metadata TaskAllocatorSlabMetadata;

using TaskAllocator = StackAllocator<SlabCapacity, &TaskAllocatorSlabMetadata>;

/// Private storage in an AsyncTask object.
struct AsyncTask::PrivateStorage {
  /// State inside the AsyncTask whose state is only managed by the exclusivity
  /// runtime in stdlibCore. We zero initialize to provide a safe initial value,
  /// but actually initialize its bit state to a const global provided by
  /// libswiftCore so that libswiftCore can control the layout of our initial
  /// state.
  uintptr_t ExclusivityAccessSet[2] = {0, 0};

  /// Storage for the ActiveTaskStatus. See doc for ActiveTaskStatus for size
  /// and alignment requirements.
  alignas(ActiveTaskStatus) char StatusStorage[sizeof(ActiveTaskStatus)];

  /// The allocator for the task stack.
  /// Currently 2 words + 8 bytes.
  TaskAllocator Allocator;

  /// Storage for task-local values.
  /// Currently one word.
  TaskLocal::Storage Local;

  /// The top 32 bits of the task ID. The bottom 32 bits are in Job::Id.
  uint32_t Id;

  /// Base priority of Task - set only at creation time of task.
  /// Current max priority of task is ActiveTaskStatus.
  ///
  /// TODO (rokhinip): Only 8 bits of the full size_t are used. Change this into
  /// flagset thing so that remaining bits are available for other non-changing
  /// task status stuff
  JobPriority BasePriority;

  /// Pointer to the task status dependency record. This is allocated from the
  /// async task stack when it is needed.
  TaskDependencyStatusRecord *dependencyRecord = nullptr;

  // Always create an async task with max priority in ActiveTaskStatus = base
  // priority. It will be updated later if needed.
  PrivateStorage(JobPriority basePri)
      : Local(TaskLocal::Storage()), BasePriority(basePri) {
    _status().store(ActiveTaskStatus(basePri), std::memory_order_relaxed);
  }

  PrivateStorage(JobPriority basePri, void *slab, size_t slabCapacity)
      : Allocator(slab, slabCapacity), Local(TaskLocal::Storage()),
          BasePriority(basePri) {
    _status().store(ActiveTaskStatus(basePri), std::memory_order_relaxed);
  }

  /// Called on the thread that was previously executing the task that we are
  /// now trying to complete.
  void complete(AsyncTask *task) {
    // Drain unlock the task and remove any overrides on thread as a
    // result of the task
    auto oldStatus = task->_private()._status().load(std::memory_order_relaxed);
    while (true) {
      // Task is completing, it shouldn't have any records and therefore
      // cannot be status record locked.
      assert(oldStatus.getInnermostRecord() == NULL);
      assert(!oldStatus.isStatusRecordLocked());

      assert(oldStatus.isRunning());

      // Remove drainer, enqueued and override bit if any
      auto newStatus = oldStatus.withRunning(false);
      newStatus = newStatus.withoutStoredPriorityEscalation();
      newStatus = newStatus.withoutEnqueued();
      newStatus = newStatus.withComplete();

      // This can fail since the task can still get concurrently cancelled or
      // escalated.
      if (task->_private()._status().compare_exchange_weak(oldStatus, newStatus,
              /* success */ std::memory_order_relaxed,
              /* failure */ std::memory_order_relaxed)) {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
        if (oldStatus.isStoredPriorityEscalated()) {
          SWIFT_TASK_DEBUG_LOG("[Override] Reset override %#x on thread from task %p", oldStatus.getStoredPriority(), this);
          swift_dispatch_lock_override_end((qos_class_t) oldStatus.getStoredPriority());
        }
#endif
        break;
      }
    }

    // Destroy and deallocate any remaining task local items since the task is
    // completed. We need to do this before we destroy the task local
    // deallocator.
    Local.destroy(task);

    // Task is completed, it can no longer have a dependency.
    assert(dependencyRecord == nullptr);

    // Don't destroy the task private storage as a whole since others who have
    // a reference to the task might still need to access the ActiveTaskStatus.
    // It is destroyed when task is destroyed.
  }

  // Destroy the opaque storage of the task
  void destroy() {
#ifndef NDEBUG
    auto oldStatus = _status().load(std::memory_order_relaxed);
    assert(oldStatus.isComplete());
#endif

    this->~PrivateStorage();
  }

  swift::atomic<ActiveTaskStatus> &_status() {
    return reinterpret_cast<swift::atomic<ActiveTaskStatus>&> (this->StatusStorage);
  }

  const swift::atomic<ActiveTaskStatus> &_status() const {
    return reinterpret_cast<const swift::atomic<ActiveTaskStatus>&> (this->StatusStorage);
  }
};

// It will be aligned to 2 words on all platforms. On arm64_32, we have an
// additional requirement where it is aligned to 4 words.
static_assert(((offsetof(AsyncTask, Private) + offsetof(AsyncTask::PrivateStorage, StatusStorage)) % ACTIVE_TASK_STATUS_SIZE == 0),
   "StatusStorage is not aligned in the AsyncTask");
static_assert(sizeof(AsyncTask::PrivateStorage) <= sizeof(AsyncTask::OpaquePrivateStorage),
              "Task-private storage doesn't fit in reserved space");

inline AsyncTask::PrivateStorage &
AsyncTask::OpaquePrivateStorage::get() {
  return reinterpret_cast<PrivateStorage &>(*this);
}
inline const AsyncTask::PrivateStorage &
AsyncTask::OpaquePrivateStorage::get() const {
  return reinterpret_cast<const PrivateStorage &>(*this);
}
inline void AsyncTask::OpaquePrivateStorage::initialize(JobPriority basePri) {
  ::new (this) PrivateStorage(basePri);
}
inline void AsyncTask::OpaquePrivateStorage::initializeWithSlab(
    JobPriority basePri, void *slab, size_t slabCapacity) {
  ::new (this) PrivateStorage(basePri, slab, slabCapacity);
}

inline void AsyncTask::OpaquePrivateStorage::complete(AsyncTask *task) {
  get().complete(task);
}

inline void AsyncTask::OpaquePrivateStorage::destroy() {
  get().destroy();
}

inline AsyncTask::PrivateStorage &AsyncTask::_private() {
  return Private.get();
}
inline const AsyncTask::PrivateStorage &AsyncTask::_private() const {
  return Private.get();
}

inline bool AsyncTask::isCancelled() const {
  return _private()._status().load(std::memory_order_relaxed)
                          .isCancelled();
}

inline void AsyncTask::flagAsRunning() {

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
  dispatch_thread_override_info_s threadOverrideInfo;
  threadOverrideInfo = swift_dispatch_thread_get_current_override_qos_floor();
  qos_class_t overrideFloor = threadOverrideInfo.override_qos_floor;
#endif

  auto oldStatus = _private()._status().load(std::memory_order_relaxed);
  assert(!oldStatus.isRunning());
  assert(!oldStatus.isComplete());

  if (!oldStatus.hasTaskDependency()) {
    SWIFT_TASK_DEBUG_LOG("%p->flagAsRunning() with no task dependency", this);
    assert(_private().dependencyRecord == nullptr);

    while (true) {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
      // Task's priority is greater than the thread's - do a self escalation
      qos_class_t maxTaskPriority = (qos_class_t) oldStatus.getStoredPriority();
      if (threadOverrideInfo.can_override && (maxTaskPriority > overrideFloor)) {
        SWIFT_TASK_DEBUG_LOG("[Override] Self-override thread with oq_floor %#x to match task %p's max priority %#x",
            overrideFloor, this, maxTaskPriority);

        (void) swift_dispatch_thread_override_self(maxTaskPriority);
        overrideFloor = maxTaskPriority;
      }
#endif
      // Set self as executor and remove escalation bit if any - the task's
      // priority escalation has already been reflected on the thread.
      auto newStatus = oldStatus.withRunning(true);
      newStatus = newStatus.withoutStoredPriorityEscalation();
      newStatus = newStatus.withoutEnqueued();

      if (_private()._status().compare_exchange_weak(oldStatus, newStatus,
               /* success */ std::memory_order_relaxed,
               /* failure */ std::memory_order_relaxed)) {
        newStatus.traceStatusChanged(this);
        adoptTaskVoucher(this);
        swift_task_enterThreadLocalContext(
            (char *)&_private().ExclusivityAccessSet[0]);
        break;
      }
    }
  } else {
    auto dependencyRecord = _private().dependencyRecord;
    assert(dependencyRecord != nullptr);
    SWIFT_TASK_DEBUG_LOG("[Dependency] %p->flagAsRunning() and remove dependencyRecord %p",
                    this, dependencyRecord);

    removeStatusRecord(this, dependencyRecord, oldStatus, [&](ActiveTaskStatus unused,
                       ActiveTaskStatus& newStatus) {

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
      // Task's priority is greater than the thread's - do a self escalation
      qos_class_t maxTaskPriority = (qos_class_t) oldStatus.getStoredPriority();
      if (threadOverrideInfo.can_override && (maxTaskPriority > overrideFloor)) {
        SWIFT_TASK_DEBUG_LOG("[Override] Self-override thread with oq_floor %#x to match task %p's max priority %#x",
            overrideFloor, this, maxTaskPriority);

        (void) swift_dispatch_thread_override_self(maxTaskPriority);
        overrideFloor = maxTaskPriority;
      }
#endif
      // Set self as executor and remove escalation bit if any - the task's
      // priority escalation has already been reflected on the thread.
      newStatus = newStatus.withRunning(true);
      newStatus = newStatus.withoutStoredPriorityEscalation();
      newStatus = newStatus.withoutEnqueued();
      newStatus = newStatus.withoutTaskDependency();
    });
    this->destroyTaskDependency(dependencyRecord);

    adoptTaskVoucher(this);
    swift_task_enterThreadLocalContext(
        (char *)&_private().ExclusivityAccessSet[0]);
  }

}

/// TODO (rokhinip): We need the handoff of the thread to the next executor to
/// be done while the current thread still holds the voucher and the priority
/// from the task. Otherwise, if we reset the voucher and priority escalation
/// too early, the thread may be preempted immediately before we can finish the
/// enqueue of the high priority task to the next location. We will then have a
/// priority inversion of waiting for a low priority thread to enqueue a high
/// priority task.
///
/// In order to do this correctly, we need enqueue-ing of a task to the next
/// executor, to have a "hand-over-hand locking" type of behaviour - until the
/// enqueue completes to the new location, the original thread does not let go
/// of the task and the execution properties of the task. This involves
/// rethinking some of the enqueue logic and being able to handle races of a new
/// thread expecting to execute an enqueued task, while the task is still held
/// onto by the original enqueueing thread.
///
/// rdar://88366470 (Direct handoff behaviour when tasks switch executors)
inline void AsyncTask::flagAsAndEnqueueOnExecutor(ExecutorRef newExecutor) {
#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
  assert(false && "Should not enqueue any tasks to execute in task-to-thread model");
#else /* SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL */
  auto oldStatus = _private()._status().load(std::memory_order_relaxed);
  assert(!oldStatus.isEnqueued());

  if (!oldStatus.isRunning() && oldStatus.hasTaskDependency()) {
    // Task went from suspended --> enqueued and has a previous
    // dependency record.
    //
    // Atomically update the existing dependency record with new dependency
    // information.
    TaskDependencyStatusRecord *dependencyRecord = _private().dependencyRecord;
    assert(dependencyRecord != nullptr);

    SWIFT_TASK_DEBUG_LOG("[Dependency] %p->flagAsAndEnqueueOnExecutor() and update dependencyRecord %p",
      this, dependencyRecord);

    updateStatusRecord(this, dependencyRecord, [&] {

      // Update dependency record to the new dependency
      dependencyRecord->updateDependencyToEnqueuedOn(newExecutor);

    }, oldStatus, [&](ActiveTaskStatus unused, ActiveTaskStatus &newStatus) {

      // Remove escalation bits + set enqueued bit
      newStatus = newStatus.withoutStoredPriorityEscalation();
      newStatus = newStatus.withEnqueued();
      assert(newStatus.hasTaskDependency());
    });
  } else {
    // 2 subcases:
    // * Task went from running on this thread --> enqueued on executor
    // * Task went from suspended to enqueued on this executor and has no
    // dependency record (Eg. newly created)
    assert(_private().dependencyRecord == nullptr);

    void *allocation = _swift_task_alloc_specific(this, sizeof(class TaskDependencyStatusRecord));
    TaskDependencyStatusRecord *dependencyRecord = _private().dependencyRecord = ::new (allocation) TaskDependencyStatusRecord(this, newExecutor);
    SWIFT_TASK_DEBUG_LOG("[Dependency] %p->flagAsAndEnqueueOnExecutor() with dependencyRecord %p", this,
      dependencyRecord);

    addStatusRecord(this, dependencyRecord, oldStatus, [&](ActiveTaskStatus unused,
                    ActiveTaskStatus &newStatus) {

      newStatus = newStatus.withRunning(false);
      newStatus = newStatus.withoutStoredPriorityEscalation();
      newStatus = newStatus.withEnqueued();
      newStatus = newStatus.withTaskDependency();

      return true;
    });

    if (oldStatus.isRunning()) {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
      // The thread was previously running the task, now that we aren't and
      // we've successfully escalated the thing the task is waiting on. We need
      // to remove any task escalation on the thread as a result of the task.
      if (oldStatus.isStoredPriorityEscalated()) {
        SWIFT_TASK_DEBUG_LOG("[Override] Reset override %#x on thread from task %p",
          oldStatus.getStoredPriority(), this);
        swift_dispatch_lock_override_end((qos_class_t) oldStatus.getStoredPriority());
      }
#endif /* SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION */
      swift_task_exitThreadLocalContext((char *)&_private().ExclusivityAccessSet[0]);
      restoreTaskVoucher(this);
    }
  }

  // Set up task for enqueue to next location by setting the Job priority field
  Flags.setPriority(oldStatus.getStoredPriority());
  concurrency::trace::task_flags_changed(
      this, static_cast<uint8_t>(Flags.getPriority()), Flags.task_isChildTask(),
      Flags.task_isFuture(), Flags.task_isGroupChildTask(),
      Flags.task_isAsyncLetTask());

  swift_task_enqueue(this, newExecutor);
#endif /* SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL */
}

// Always and only called by the task on itself
inline
void AsyncTask::flagAsSuspended(TaskDependencyStatusRecord *dependencyStatusRecord) {
  SWIFT_TASK_DEBUG_LOG("[Dependency] %p->flagAsSuspended() with dependencyRecord %p", this,
                  dependencyStatusRecord);
  assert(dependencyStatusRecord != NULL);

  auto oldStatus = _private()._status().load(std::memory_order_relaxed);
  // We can only be suspended if we were previously running. See state
  // transitions listed out in Task.h
  assert(oldStatus.isRunning() && !oldStatus.isEnqueued());

  addStatusRecord(this, dependencyStatusRecord, oldStatus, [&](ActiveTaskStatus unused,
                  ActiveTaskStatus &newStatus) {
    newStatus = newStatus.withRunning(false);
    newStatus = newStatus.withoutStoredPriorityEscalation();
    newStatus = newStatus.withTaskDependency();

    // Escalate the thing we are dependent on to have the max priority that we
    // see at the time of publishing the record. If there is another concurrent
    // escalator of the task, we will still converge since escalation is a
    // maxing function and the other escalator will end up escalating the
    // dependency record we publish here.
    //
    // Note that we have to do this escalation while adding the status record
    // and not after - we are not guaranteed to be able to have a valid
    // reference to the dependencyStatusRecord or its contents, once we have
    // published it in the ActiveTaskStatus since someone else could
    // concurrently made us runnable.
    dependencyStatusRecord->performEscalationAction(newStatus.getStoredPriority());

    // Always add the dependency status record
    return true;
  });

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
  // Successfully dropped task drain lock, make sure to remove override on
  // thread due to task
  if (oldStatus.isStoredPriorityEscalated()) {
     SWIFT_TASK_DEBUG_LOG("[Override] Reset override %#x on thread from task %p",
       oldStatus.getStoredPriority(), this);
     swift_dispatch_lock_override_end((qos_class_t) oldStatus.getStoredPriority());
  }
#endif

  swift_task_exitThreadLocalContext((char *)&_private().ExclusivityAccessSet[0]);
  restoreTaskVoucher(this);
  return;
}

inline void AsyncTask::destroyTaskDependency(TaskDependencyStatusRecord *dependencyRecord) {
  assert(_private().dependencyRecord == dependencyRecord);
  _swift_task_dealloc_specific(this, dependencyRecord);

  _private().dependencyRecord = nullptr;
}

// this -> task which is suspending
// Input task -> task we are waiting on
inline void AsyncTask::flagAsSuspendedOnTask(AsyncTask *task) {
  assert(_private().dependencyRecord == nullptr);

  void *allocation = _swift_task_alloc_specific(this, sizeof(class TaskDependencyStatusRecord));
  auto record = ::new (allocation) TaskDependencyStatusRecord(this, task);
  SWIFT_TASK_DEBUG_LOG("[Dependency] Create a dependencyRecord %p for dependency on task %p", allocation, task);
  _private().dependencyRecord = record;

  this->flagAsSuspended(record);
}

inline void AsyncTask::flagAsSuspendedOnContinuation(ContinuationAsyncContext *context) {
  assert(_private().dependencyRecord == nullptr);

  void *allocation = _swift_task_alloc_specific(this, sizeof(class TaskDependencyStatusRecord));
  auto record = ::new (allocation) TaskDependencyStatusRecord(this, context);
  SWIFT_TASK_DEBUG_LOG("[Dependency] Create a dependencyRecord %p for dependency on continuation %p", allocation, context);
  _private().dependencyRecord = record;

  this->flagAsSuspended(record);
}

inline void AsyncTask::flagAsSuspendedOnTaskGroup(TaskGroup *taskGroup) {
  assert(_private().dependencyRecord == nullptr);

  void *allocation = _swift_task_alloc_specific(this, sizeof(class TaskDependencyStatusRecord));
  auto record = ::new (allocation) TaskDependencyStatusRecord(this, taskGroup);
  SWIFT_TASK_DEBUG_LOG("[Dependency] Create a dependencyRecord %p for dependency on taskGroup %p", allocation, taskGroup);
  _private().dependencyRecord = record;

  this->flagAsSuspended(record);
}

// READ ME: This is not a dead function! Do not remove it! This is a function
// that can be used when debugging locally to instrument when a task
// actually is dealloced.
inline void AsyncTask::flagAsDestroyed() {
  SWIFT_TASK_DEBUG_LOG("task destroyed %p", this);
}

inline void AsyncTask::localValuePush(const HeapObject *key,
                                      /* +1 */ OpaqueValue *value,
                                      const Metadata *valueType) {
  _private().Local.pushValue(this, key, value, valueType);
}

inline OpaqueValue *AsyncTask::localValueGet(const HeapObject *key) {
  return _private().Local.getValue(this, key);
}

/// Returns true if storage has still more bindings.
inline bool AsyncTask::localValuePop() {
  return _private().Local.popValue(this);
}

} // end namespace swift

#endif
