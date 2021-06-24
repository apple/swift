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

#include "swift/Runtime/Concurrency.h"
#include "swift/ABI/Task.h"
#include "swift/ABI/Metadata.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Error.h"
#include "Error.h"

#define SWIFT_FATAL_ERROR swift_Concurrency_fatalError
#include "../runtime/StackAllocator.h"

#if HAVE_PTHREAD_H
#include <pthread.h>
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRA_LEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace swift {

// Uncomment to enable helpful debug spew to stderr
//#define SWIFT_TASK_PRINTF_DEBUG 1

#if defined(_WIN32)
using ThreadID = decltype(GetCurrentThreadId());
#else
using ThreadID = decltype(pthread_self());
#endif

inline ThreadID _swift_get_thread_id() {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return pthread_self();
#endif
}

class AsyncTask;
class TaskGroup;

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

/// Initialize the async let storage for the given async-let child task.
void asyncLet_addImpl(AsyncTask *task, AsyncLet *asyncLet);

/// Clear the active task reference for the current thread.
AsyncTask *_swift_task_clearCurrent();

#if defined(SWIFT_STDLIB_SINGLE_THREADED_RUNTIME)
#define SWIFT_CONCURRENCY_COOPERATIVE_GLOBAL_EXECUTOR 1
#else
#define SWIFT_CONCURRENCY_COOPERATIVE_GLOBAL_EXECUTOR 0
#endif

#if SWIFT_CONCURRENCY_COOPERATIVE_GLOBAL_EXECUTOR
/// Donate this thread to the global executor until either the
/// given condition returns true or we've run out of cooperative
/// tasks to run.
void donateThreadToGlobalExecutorUntil(bool (*condition)(void*),
                                       void *context);
#endif

/// release() establishes a happens-before relation with a preceding acquire()
/// on the same address.
void _swift_tsan_acquire(void *addr);
void _swift_tsan_release(void *addr);

/// Special values used with DispatchQueueIndex to indicate the global and main
/// executors.
#define DISPATCH_QUEUE_GLOBAL_EXECUTOR (void *)1

inline SerialExecutorWitnessTable *
_swift_task_getDispatchQueueSerialExecutorWitnessTable() {
  extern SerialExecutorWitnessTable wtable
    SWIFT_ASM_LABEL_WITH_PREFIX("$ss17DispatchQueueShimCScfsWP");
  return &wtable;
}

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
class TaskFutureWaitAsyncContext : public AsyncContext {
public:
  SwiftError *errorResult;

  OpaqueValue *successResultPointer;

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
class ActiveTaskStatus {
  enum : uintptr_t {
    IsCancelled = 0x1,
    IsLocked = 0x2,
    RecordMask = ~uintptr_t(IsCancelled | IsLocked)
  };

  uintptr_t Value;

public:
  constexpr ActiveTaskStatus() : Value(0) {}
  ActiveTaskStatus(TaskStatusRecord *innermostRecord,
                   bool cancelled, bool locked)
    : Value(reinterpret_cast<uintptr_t>(innermostRecord)
                + (locked ? IsLocked : 0)
                + (cancelled ? IsCancelled : 0)) {}

  /// Is the task currently cancelled?
  bool isCancelled() const { return Value & IsCancelled; }

  /// Is there an active lock on the cancellation information?
  bool isLocked() const { return Value & IsLocked; }

  /// Return the innermost cancellation record.  Code running
  /// asynchronously with this task should not access this record
  /// without having first locked it; see swift_taskCancel.
  TaskStatusRecord *getInnermostRecord() const {
    return reinterpret_cast<TaskStatusRecord*>(Value & RecordMask);
  }

  static TaskStatusRecord *getStatusRecordParent(TaskStatusRecord *ptr);

  using record_iterator =
    LinkedListIterator<TaskStatusRecord, getStatusRecordParent>;
  llvm::iterator_range<record_iterator> records() const {
    return record_iterator::rangeBeginning(getInnermostRecord());
  }
};

/// The size of an allocator slab.
///
/// TODO: find the optimal value by experiment.
static constexpr size_t SlabCapacity = 1024;

using TaskAllocator = StackAllocator<SlabCapacity>;

/// Private storage in an AsyncTask object.
struct AsyncTask::PrivateStorage {
  /// The currently-active information about cancellation.
  /// Currently one word.
  std::atomic<ActiveTaskStatus> Status;

  /// The allocator for the task stack.
  /// Currently 2 words + 8 bytes.
  TaskAllocator Allocator;

  /// Storage for task-local values.
  /// Currently one word.
  TaskLocal::Storage Local;

  PrivateStorage()
    : Status(ActiveTaskStatus()),
      Local(TaskLocal::Storage()) {}

  PrivateStorage(void *slab, size_t slabCapacity)
    : Status(ActiveTaskStatus()),
      Allocator(slab, slabCapacity),
      Local(TaskLocal::Storage()) {}

  void complete(AsyncTask *task) {
    // Destroy and deallocate any remaining task local items.
    // We need to do this before we destroy the task local deallocator.
    Local.destroy(task);

    this->~PrivateStorage();
  }
};

static_assert(sizeof(AsyncTask::PrivateStorage)
                <= sizeof(AsyncTask::OpaquePrivateStorage) &&
              alignof(AsyncTask::PrivateStorage)
                <= alignof(AsyncTask::OpaquePrivateStorage),
              "Task-private storage doesn't fit in reserved space");

inline AsyncTask::PrivateStorage &
AsyncTask::OpaquePrivateStorage::get() {
  return reinterpret_cast<PrivateStorage &>(*this);
}
inline const AsyncTask::PrivateStorage &
AsyncTask::OpaquePrivateStorage::get() const {
  return reinterpret_cast<const PrivateStorage &>(*this);
}
inline void AsyncTask::OpaquePrivateStorage::initialize(AsyncTask *task) {
  new (this) PrivateStorage();
}
inline void
AsyncTask::OpaquePrivateStorage::initializeWithSlab(AsyncTask *task,
                                                    void *slab,
                                                    size_t slabCapacity) {
  new (this) PrivateStorage(slab, slabCapacity);
}
inline void AsyncTask::OpaquePrivateStorage::complete(AsyncTask *task) {
  get().complete(task);
}
inline void AsyncTask::OpaquePrivateStorage::destroy() {
  // nothing else to do
}

inline AsyncTask::PrivateStorage &AsyncTask::_private() {
  return Private.get();
}
inline const AsyncTask::PrivateStorage &AsyncTask::_private() const {
  return Private.get();
}

inline bool AsyncTask::isCancelled() const {
  return _private().Status.load(std::memory_order_relaxed)
                          .isCancelled();
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
