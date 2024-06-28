//===--- TracingStubs.h - Default stub implementation of tracing. --*- C++ -*-//
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
// Concurrency tracing stubs for OSes without tracing support.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_CONCURRENCY_TRACINGSIGNPOST_H
#define SWIFT_CONCURRENCY_TRACINGSIGNPOST_H

#include "Tracing.h"

namespace swift {
namespace concurrency {
namespace trace {

inline void actor_create(HeapObject *actor) {}

inline void actor_destroy(HeapObject *actor) {}

inline void actor_deallocate(HeapObject *actor) {}

inline void actor_enqueue(HeapObject *actor, Job *job) {}

inline void actor_dequeue(HeapObject *actor, Job *job) {}

inline void actor_state_changed(HeapObject *actor, Job *firstJob, uint8_t state,
                                bool isDistributedRemote,
                                bool isPriorityEscalated, uint8_t maxPriority) {
}

inline void actor_note_job_queue(HeapObject *actor, Job *first,
                                 Job *(*getNext)(Job *)) {}

inline void task_create(AsyncTask *task, AsyncTask *parent, TaskGroup *group,
                        AsyncLet *asyncLet, uint8_t jobPriority,
                        bool isChildTask, bool isFuture, bool isGroupChildTask,
                        bool isAsyncLetTask) {}

inline void task_destroy(AsyncTask *task) {}

inline void task_wait(AsyncTask *task, AsyncTask *waitingOn, uintptr_t status) {
}

inline void task_resume(AsyncTask *task) {}

inline void task_status_changed(AsyncTask *task, uint8_t maxPriority,
                                bool isCancelled, bool isEscalated,
                                bool isStarting, bool isRunning, bool isEnqueued) {}

inline void task_flags_changed(AsyncTask *task, uint8_t jobPriority,
                               bool isChildTask, bool isFuture,
                               bool isGroupChildTask, bool isAsyncLetTask) {}

inline void task_continuation_init(AsyncTask *task,
                                   ContinuationAsyncContext *context) {}

inline void task_continuation_await(ContinuationAsyncContext *context) {}

inline void task_continuation_resume(ContinuationAsyncContext *context,
                                     bool error) {}

inline void job_enqueue_global(Job *job) {}

inline void job_enqueue_global_with_delay(unsigned long long delay, Job *job) {}

inline void job_enqueue_main_executor(Job *job) {}

inline job_run_info job_run_begin(Job *job) { return {}; }

inline void job_run_end(job_run_info info) {}

} // namespace trace
} // namespace concurrency
} // namespace swift

#endif
