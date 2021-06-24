//===--- AsyncLet.h - async let object management -00------------*- C++ -*-===//
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

#include "../CompatibilityOverride/CompatibilityOverride.h"
#include "swift/Runtime/Concurrency.h"
#include "swift/ABI/AsyncLet.h"
#include "swift/ABI/Metadata.h"
#include "swift/ABI/Task.h"
#include "swift/ABI/TaskOptions.h"
#include "swift/Runtime/Mutex.h"
#include "swift/Runtime/HeapObject.h"
#include "TaskPrivate.h"
#include "AsyncCall.h"
#include "Debug.h"

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

using namespace swift;

namespace {
class AsyncLetImpl: public ChildTaskStatusRecord {
public:
  // This is where we could define a Status or other types important for async-let

private:

  /// The task that was kicked off to initialize this `async let`.
  AsyncTask *task;

  // TODO: more additional flags here, we can use them for future optimizations.
  //       e.g. "was awaited on" or "needs free"

  friend class ::swift::AsyncTask;

public:
  explicit AsyncLetImpl(AsyncTask* task)
      : ChildTaskStatusRecord(task),
        task(task) {
    assert(task->hasChildFragment() && "async let task must be a child task.");
  }

  /// Returns the task record representing this async let task.
  /// The record is stored in the parent task, and should be removed when the
  /// async let goes out of scope.
  ChildTaskStatusRecord *getTaskRecord() {
    return reinterpret_cast<ChildTaskStatusRecord *>(this);
  }

  AsyncTask *getTask() const {
    return task;
  }

}; // end AsyncLetImpl

} // end anonymous namespace


/******************************************************************************/
/************************* ASYNC LET IMPLEMENTATION ***************************/
/******************************************************************************/

static_assert(sizeof(AsyncLetImpl) <= sizeof(AsyncLet) &&
              alignof(AsyncLetImpl) <= alignof(AsyncLet),
              "AsyncLetImpl doesn't fit in AsyncLet");

static AsyncLetImpl *asImpl(AsyncLet *alet) {
  return reinterpret_cast<AsyncLetImpl*>(alet);
}

static AsyncLetImpl *asImpl(const AsyncLet *alet) {
  return reinterpret_cast<AsyncLetImpl*>(
      const_cast<AsyncLet*>(alet));
}

void swift::asyncLet_addImpl(AsyncTask *task, AsyncLet *asyncLet) {
  AsyncLetImpl *impl = new (asyncLet) AsyncLetImpl(task);

  auto record = impl->getTaskRecord();
  assert(impl == record && "the async-let IS the task record");

  // ok, now that the group actually is initialized: attach it to the task
  swift_task_addStatusRecord(record);
}

// =============================================================================
// ==== start ------------------------------------------------------------------

SWIFT_CC(swift)
void swift::swift_asyncLet_start(AsyncLet *alet,
                                 TaskOptionRecord *options,
                                 const Metadata *futureResultType,
                                 void *closureEntryPoint,
                                 HeapObject *closureContext) {
  auto flags = TaskCreateFlags();
  flags.setEnqueueJob(true);

  AsyncLetTaskOptionRecord asyncLetOptionRecord(alet);
  asyncLetOptionRecord.Parent = options;

  swift_task_create(
      flags.getOpaqueValue(),
      &asyncLetOptionRecord,
      futureResultType,
      closureEntryPoint, closureContext);
}

// =============================================================================
// ==== wait -------------------------------------------------------------------

SWIFT_CC(swiftasync)
static void swift_asyncLet_waitImpl(
    OpaqueValue *result, SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
    AsyncLet *alet, TaskContinuationFunction *resumeFunction,
    AsyncContext *callContext) {
  auto task = alet->getTask();
  swift_task_future_wait(result, callerContext, task, resumeFunction,
                         callContext);
}

SWIFT_CC(swiftasync)
static void swift_asyncLet_wait_throwingImpl(
    OpaqueValue *result, SWIFT_ASYNC_CONTEXT AsyncContext *callerContext,
    AsyncLet *alet,
    ThrowingTaskFutureWaitContinuationFunction *resumeFunction,
    AsyncContext * callContext) {
  auto task = alet->getTask();
  swift_task_future_wait_throwing(result, callerContext, task, resumeFunction,
                                  callContext);
}

// =============================================================================
// ==== end --------------------------------------------------------------------

SWIFT_CC(swift)
static void swift_asyncLet_endImpl(AsyncLet *alet) {
  auto task = alet->getTask();

  // Cancel the task as we exit the scope
  swift_task_cancel(task);

  // Remove the child record from the parent task
  auto record = asImpl(alet)->getTaskRecord();
  swift_task_removeStatusRecord(record);

  // TODO: we need to implicitly await either before the end or here somehow.

  // and finally, release the task and free the async-let
  AsyncTask *parent = swift_task_getCurrent();
  assert(parent && "async-let must have a parent task");

#if SWIFT_TASK_PRINTF_DEBUG
  fprintf(stderr, "[%lu] async let end of task %p, parent: %p\n",
          _swift_get_thread_id(), task, parent);
#endif
  _swift_task_dealloc_specific(parent, task);
}

// =============================================================================
// ==== AsyncLet Implementation ------------------------------------------------

AsyncTask* AsyncLet::getTask() const {
  return asImpl(this)->getTask();
}

// =============================================================================

#define OVERRIDE_ASYNC_LET COMPATIBILITY_OVERRIDE
#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH
