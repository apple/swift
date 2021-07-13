//===--- TaskOptions.h - ABI structures for task options --------*- C++ -*-===//
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
// Swift ABI describing task options.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ABI_TASK_OPTIONS_H
#define SWIFT_ABI_TASK_OPTIONS_H

#include "swift/ABI/TaskLocal.h"
#include "swift/ABI/Executor.h"
#include "swift/ABI/HeapObject.h"
#include "swift/ABI/Metadata.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/Runtime/Config.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/Support/Casting.h"

namespace swift {

// ==== ------------------------------------------------------------------------
// ==== Task Options, for creating and waiting on tasks

/// The abstract base class for all options that may be used
/// to configure a newly spawned task.
class TaskOptionRecord {
public:
  const TaskOptionRecordFlags Flags;
  TaskOptionRecord *Parent;

  TaskOptionRecord(TaskOptionRecordKind kind,
                   TaskOptionRecord *parent = nullptr)
    : Flags(kind), Parent(parent) { }

  TaskOptionRecord(const TaskOptionRecord &) = delete;
  TaskOptionRecord &operator=(const TaskOptionRecord &) = delete;

  TaskOptionRecordKind getKind() const {
    return Flags.getKind();
  }

  TaskOptionRecord *getParent() const {
    return Parent;
  }
};

/******************************************************************************/
/****************************** TASK OPTIONS **********************************/
/******************************************************************************/

class TaskGroupTaskOptionRecord : public TaskOptionRecord {
  TaskGroup * const Group;

  public:
    TaskGroupTaskOptionRecord(TaskGroup *group)
    : TaskOptionRecord(TaskOptionRecordKind::TaskGroup),
      Group(group) {}

  TaskGroup *getGroup() const {
    return Group;
  }

  static bool classof(const TaskOptionRecord *record) {
    return record->getKind() == TaskOptionRecordKind::TaskGroup;
  }
};


/// Task option to specify on what executor the task should be executed.
///
/// Not passing this option implies that that a "best guess" or good default
/// executor should be used instead, most often this may mean the global
/// concurrent executor, or the enclosing actor's executor.
class ExecutorTaskOptionRecord : public TaskOptionRecord {
  const ExecutorRef Executor;

public:
  ExecutorTaskOptionRecord(ExecutorRef executor)
    : TaskOptionRecord(TaskOptionRecordKind::Executor),
      Executor(executor) {}

  ExecutorRef getExecutor() const {
    return Executor;
  }

  static bool classof(const TaskOptionRecord *record) {
    return record->getKind() == TaskOptionRecordKind::Executor;
  }
};

/// DEPRECATED. AsyncLetWithBufferTaskOptionRecord is used instead.
/// Task option to specify that the created task is for an 'async let'.
class AsyncLetTaskOptionRecord : public TaskOptionRecord {
  AsyncLet *asyncLet;

public:
  AsyncLetTaskOptionRecord(AsyncLet *asyncLet)
    : TaskOptionRecord(TaskOptionRecordKind::AsyncLet),
      asyncLet(asyncLet) {}

  AsyncLet *getAsyncLet() const {
    return asyncLet;
  }

  static bool classof(const TaskOptionRecord *record) {
    return record->getKind() == TaskOptionRecordKind::AsyncLet;
  }
};

class AsyncLetWithBufferTaskOptionRecord : public TaskOptionRecord {
  AsyncLet *asyncLet;
  void *resultBuffer;

public:
  AsyncLetWithBufferTaskOptionRecord(AsyncLet *asyncLet,
                                     void *resultBuffer)
    : TaskOptionRecord(TaskOptionRecordKind::AsyncLetWithBuffer),
      asyncLet(asyncLet),
      resultBuffer(resultBuffer) {}

  AsyncLet *getAsyncLet() const {
    return asyncLet;
  }
  
  void *getResultBuffer() const {
    return resultBuffer;
  }

  static bool classof(const TaskOptionRecord *record) {
    return record->getKind() == TaskOptionRecordKind::AsyncLetWithBuffer;
  }
};

} // end namespace swift

#endif
