//===--- Debug.h - Swift Concurrency debug helpers --------------*- C++ -*-===//
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
// Debugging and inspection support.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_CONCURRENCY_DEBUG_H
#define SWIFT_CONCURRENCY_DEBUG_H

#include "swift/Runtime/Config.h"

namespace swift {

// Dispatch knows about these symbol names. Don't change them without consulting
// dispatch.

/// The metadata pointer used for job objects.
SWIFT_EXPORT_FROM(swift_Concurrency)
const void *const _swift_concurrency_debug_jobMetadata;

/// The metadata pointer used for async task objects.
SWIFT_EXPORT_FROM(swift_Concurrency)
const void *const _swift_concurrency_debug_asyncTaskMetadata;

/// A fake metadata pointer placed at the start of async task slab allocations.
SWIFT_EXPORT_FROM(swift_Concurrency)
const void *const _swift_concurrency_debug_asyncTaskSlabMetadata;

SWIFT_EXPORT_FROM(swift_Concurrency)
const void *const _swift_concurrency_debug_non_future_adapter;
SWIFT_EXPORT_FROM(swift_Concurrency)
const void *const _swift_concurrency_debug_non_context_adapter;
SWIFT_EXPORT_FROM(swift_Concurrency)
const void *const _swift_concurrency_debug_future_adapter;
SWIFT_EXPORT_FROM(swift_Concurrency)
const void *const _swift_concurrency_debug_task_wait_throwing_resume_adapter;
SWIFT_EXPORT_FROM(swift_Concurrency)
const void *const _swift_concurrency_debug_task_future_wait_resume_adapter;

/// Whether the runtime we are inspecting supports priority escalation
SWIFT_EXPORT_FROM(swift_Concurrency)
bool _swift_concurrency_debug_supportsPriorityEscalation;

} // namespace swift

#endif
