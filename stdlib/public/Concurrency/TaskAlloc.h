//===--- TaskAlloc.h - Concurrency library internal interface -*- C++ -*-===//
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
// Allocator for the concurrency library.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_CONCURRENCY_TASKALLOC_H
#define SWIFT_CONCURRENCY_TASKALLOC_H

#include "Error.h"

#define SWIFT_FATAL_ERROR swift_Concurrency_fatalError
#include "../runtime/StackAllocator.h"

namespace swift {

class AsyncTask;

/// The size of an allocator slab. We want the full allocation to fit into a
/// 1024-byte malloc quantum. We subtract off the slab header size, plus a
/// little extra to stay within our limits even when there's overhead from
/// malloc stack logging.
static constexpr size_t SlabCapacity = 1024 - StackAllocator<0, nullptr>::slabHeaderSize() - 8;
extern Metadata TaskAllocatorSlabMetadata;

using TaskAllocator = StackAllocator<SlabCapacity, &TaskAllocatorSlabMetadata>;

/// Allocate task-local memory on behalf of a specific task,
/// not necessarily the current one.  Generally this should only be
/// done on behalf of a child task.
void *_swift_task_alloc_specific(AsyncTask *task, size_t size);

/// dellocate task-local memory on behalf of a specific task,
/// not necessarily the current one.  Generally this should only be
/// done on behalf of a child task.
void _swift_task_dealloc_specific(AsyncTask *task, void *ptr);

} // end namespace swift

#endif

