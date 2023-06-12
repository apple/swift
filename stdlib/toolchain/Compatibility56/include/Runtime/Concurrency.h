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

#ifndef SWIFT_RUNTIME_CONCURRENCY_BACKDEPLOY56_H
#define SWIFT_RUNTIME_CONCURRENCY_BACKDEPLOY56_H

#include "Concurrency/Task.h"
#include "Concurrency/TaskStatus.h"
#include "Concurrency/AsyncLet.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"

// Does the runtime use a cooperative global executor?
#if defined(SWIFT_STDLIB_SINGLE_THREADED_RUNTIME)
#define SWIFT_CONCURRENCY_COOPERATIVE_GLOBAL_EXECUTOR 1
#else
#define SWIFT_CONCURRENCY_COOPERATIVE_GLOBAL_EXECUTOR 0
#endif

// Does the runtime integrate with libdispatch?
#ifndef SWIFT_CONCURRENCY_ENABLE_DISPATCH
#if SWIFT_CONCURRENCY_COOPERATIVE_GLOBAL_EXECUTOR
#define SWIFT_CONCURRENCY_ENABLE_DISPATCH 0
#else
#define SWIFT_CONCURRENCY_ENABLE_DISPATCH 1
#endif
#endif

namespace swift {
class DefaultActor;
class TaskOptionRecord;

struct SwiftError;

struct AsyncTaskAndContext {
  AsyncTask *Task;
  AsyncContext *InitialContext;
};

/// Caution: not all future-initializing functions actually throw, so
/// this signature may be incorrect.
using FutureAsyncSignature =
  AsyncSignature<void(void*), /*throws*/ true>;

/// Escalate the priority of a task and all of its child tasks.
///
/// This can be called from any thread.
///
/// This has no effect if the task already has at least the given priority.
/// Returns the priority of the task.
SWIFT_CC(swift)
__attribute__((visibility("hidden")))
JobPriority swift_task_escalateBackdeploy56(AsyncTask *task,
                                            JobPriority newPriority);
} // namespace swift

#pragma clang diagnostic pop

#endif // SWIFT_RUNTIME_CONCURRENCY_BACKDEPLOY56_H
