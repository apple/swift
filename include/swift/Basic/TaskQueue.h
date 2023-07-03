//===--- TaskQueue.h - Task Execution Work Queue ----------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_TASKQUEUE_H
#define SWIFT_BASIC_TASKQUEUE_H

#include "swift/Basic/JSONSerialization.h"
#include "swift/Basic/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Config/config.h"
#include "llvm/Support/Program.h"

#include <functional>
#include <memory>
#include <queue>

#if defined(HAVE_GETRUSAGE) && !defined(__HAIKU__)
struct rusage;
#endif

namespace swift {
class UnifiedStatsReporter;
namespace sys {

class Task; // forward declared to allow for platform-specific implementations

using ProcessId = llvm::sys::procid_t;

/// Indicates how a TaskQueue should respond to the task finished event.
enum class TaskFinishedResponse {
  /// Indicates that execution should continue.
  ContinueExecution,
  /// Indicates that execution should stop (no new tasks will begin execution,
  /// but tasks which are currently executing will be allowed to finish).
  StopExecution,
};

/// TaskProcessInformation is bound to a task and contains information about the
/// process that ran this task. This is especially useful to find out which
/// tasks ran in the same process (in multifile-mode or when WMO is activated
/// e.g.). If available, it also contains information about the usage of
/// resources like CPU time or memory the process used in the system. How ever,
/// this could differ from platform to platform and is therefore optional.

/// One process could handle multiple tasks in some modes of the Swift compiler
/// (multifile, WMO). To not break existing tools, the driver does use unique
/// identifiers for the tasks that are not the process identifier. To still be
/// able to reason about tasks that ran in the same process the
/// TaskProcessInformation struct contains information about the actual process
/// of the operating system. The OSPid is the actual process identifier and is
/// therefore not guaranteed to be unique over all tasks. The ProcessUsage
/// contains optional usage information about the operating system process. It
/// could be used by tools that take those information as input for analyzing
/// the Swift compiler on a process-level. It will be `None` if the execution
/// has been skipped or one of the following symbols are not available on the
/// system: `rusage`, `wait4`.
struct TaskProcessInformation {

  struct ResourceUsage {
    // user time in µs
    uint64_t Utime;
    // system time in µs
    uint64_t Stime;
    // maximum resident set size in Bytes
    uint64_t Maxrss;

    ResourceUsage(uint64_t Utime, uint64_t Stime, uint64_t Maxrss)
        : Utime(Utime), Stime(Stime), Maxrss(Maxrss) {}

    virtual ~ResourceUsage() = default;
    virtual void provideMapping(json::Output &out);
  };

private:
  // the process identifier of the operating system
  ProcessId OSPid;
  // usage information about the process, if available
  llvm::Optional<ResourceUsage> ProcessUsage;

public:
  TaskProcessInformation(ProcessId Pid, uint64_t utime, uint64_t stime,
                         uint64_t maxrss)
      : OSPid(Pid), ProcessUsage(ResourceUsage(utime, stime, maxrss)) {}

  TaskProcessInformation(ProcessId Pid)
      : OSPid(Pid), ProcessUsage(llvm::None) {}

#if defined(HAVE_GETRUSAGE) && !defined(__HAIKU__)
  TaskProcessInformation(ProcessId Pid, struct rusage Usage);
#endif // defined(HAVE_GETRUSAGE) && !defined(__HAIKU__)
  llvm::Optional<ResourceUsage> getResourceUsage() { return ProcessUsage; }
  virtual ~TaskProcessInformation() = default;
  virtual void provideMapping(json::Output &out);
};

/// A class encapsulating the execution of multiple tasks in parallel.
class TaskQueue {
  /// Tasks which have not begun execution.
  std::queue<std::unique_ptr<Task>> QueuedTasks;

  /// The number of tasks to execute in parallel.
  unsigned NumberOfParallelTasks;

  /// Optional place to count I/O and subprocess events.
  UnifiedStatsReporter *Stats;

public:
  /// Create a new TaskQueue instance.
  ///
  /// \param NumberOfParallelTasks indicates the number of tasks which should
  /// be run in parallel. If 0, the TaskQueue will choose the most appropriate
  /// number of parallel tasks for the current system.
  /// \param USR Optional stats reporter to count I/O and subprocess events.
  TaskQueue(unsigned NumberOfParallelTasks = 0,
            UnifiedStatsReporter *USR = nullptr);
  virtual ~TaskQueue();

  // TODO: remove once -Wdocumentation stops warning for \param, \returns on
  // std::function (<rdar://problem/15665132>).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation"
  /// A callback which will be executed when each task begins execution
  ///
  /// \param Pid the ProcessId of the task which just began execution.
  /// \param Context the context which was passed when the task was added
  using TaskBeganCallback = std::function<void(ProcessId Pid, void *Context)>;

  /// A callback which will be executed after each task finishes
  /// execution.
  ///
  /// \param Pid the ProcessId of the task which finished execution.
  /// \param ReturnCode the return code of the task which finished execution.
  /// \param Output the output from the task which finished execution,
  /// if available. (This may not be available on all platforms.)
  /// \param Errors the errors from the task which finished execution, if
  /// available and SeparateErrors was true.  (This may not be available on all
  /// platforms.)
  /// \param ProcInfo contains information like the operating process identifier
  /// and resource usage if available
  /// \param Context the context which was passed when the task was added
  ///
  /// \returns true if further execution of tasks should stop,
  /// false if execution should continue
  using TaskFinishedCallback = std::function<TaskFinishedResponse(
      ProcessId Pid, int ReturnCode, StringRef Output, StringRef Errors,
      TaskProcessInformation ProcInfo, void *Context)>;

  /// A callback which will be executed if a task exited abnormally due
  /// to a signal.
  ///
  /// \param Pid the ProcessId of the task which exited abnormally.
  /// \param ErrorMsg a string describing why the task exited abnormally. If
  /// no reason could be deduced, this may be empty.
  /// \param Output the output from the task which exited abnormally, if
  /// available. (This may not be available on all platforms.)
  /// \param Errors the errors from the task which exited abnormally, if
  /// available and SeparateErrors was true.  (This may not be available on all
  /// platforms.)
  /// \param ProcInfo contains information like the operating process identifier
  /// and resource usage if available
  /// \param Context the context which was passed when the task was added
  /// \param Signal the terminating signal number, if available. This may not be
  /// available on all platforms. If it is ever provided, it should not be
  /// removed in future versions of the compiler.
  ///
  /// \returns a TaskFinishedResponse indicating whether or not execution
  /// should proceed
  using TaskSignalledCallback = std::function<TaskFinishedResponse(
      ProcessId Pid, StringRef ErrorMsg, StringRef Output, StringRef Errors,
      void *Context, llvm::Optional<int> Signal,
      TaskProcessInformation ProcInfo)>;
#pragma clang diagnostic pop

  /// Indicates whether TaskQueue supports buffering output on the
  /// current system.
  ///
  /// \note If this returns false, the TaskFinishedCallback passed
  /// to \ref execute will always receive an empty StringRef for output, even
  /// if the task actually generated output.
  static bool supportsBufferingOutput();

  /// Indicates whether TaskQueue supports parallel execution on the
  /// current system.
  static bool supportsParallelExecution();

  /// \returns the maximum number of tasks which this TaskQueue will execute in
  /// parallel
  unsigned getNumberOfParallelTasks() const;

  /// Adds a task to the TaskQueue.
  ///
  /// \param ExecPath the path to the executable which the task should execute
  /// \param Args the arguments which should be passed to the task
  /// \param Env the environment which should be used for the task;
  /// must be null-terminated. If empty, inherits the parent's environment.
  /// \param Context an optional context which will be associated with the task
  /// \param SeparateErrors Controls whether error output is reported separately
  virtual void addTask(const char *ExecPath, ArrayRef<const char *> Args,
                       ArrayRef<const char *> Env = llvm::None,
                       void *Context = nullptr, bool SeparateErrors = false);

  /// Synchronously executes the tasks in the TaskQueue.
  ///
  /// \param Began a callback which will be called when a task begins
  /// \param Finished a callback which will be called when a task finishes
  /// \param Signalled a callback which will be called if a task exited
  /// abnormally due to a signal
  ///
  /// \returns true if all tasks did not execute successfully
  virtual bool
  execute(TaskBeganCallback Began = TaskBeganCallback(),
          TaskFinishedCallback Finished = TaskFinishedCallback(),
          TaskSignalledCallback Signalled = TaskSignalledCallback());

  /// Returns true if there are any tasks that have been queued but have not
  /// yet been executed.
  virtual bool hasRemainingTasks() {
    return !QueuedTasks.empty();
  }
};

/// A class which simulates execution of tasks with behavior similar to
/// TaskQueue.
class DummyTaskQueue : public TaskQueue {
  class DummyTask {
  public:
    const char *ExecPath;
    ArrayRef<const char *> Args;
    ArrayRef<const char *> Env;
    void *Context;
    bool SeparateErrors;

    DummyTask(const char *ExecPath, ArrayRef<const char *> Args,
              ArrayRef<const char *> Env = llvm::None, void *Context = nullptr,
              bool SeparateErrors = false)
        : ExecPath(ExecPath), Args(Args), Env(Env), Context(Context),
          SeparateErrors(SeparateErrors) {}
  };

  std::queue<std::unique_ptr<DummyTask>> QueuedTasks;

public:
  /// Create a new DummyTaskQueue instance.
  DummyTaskQueue(unsigned NumberOfParallelTasks = 0);
  virtual ~DummyTaskQueue();

  void addTask(const char *ExecPath, ArrayRef<const char *> Args,
               ArrayRef<const char *> Env = llvm::None,
               void *Context = nullptr, bool SeparateErrors = false) override;

  bool
  execute(TaskBeganCallback Began = TaskBeganCallback(),
          TaskFinishedCallback Finished = TaskFinishedCallback(),
          TaskSignalledCallback Signalled = TaskSignalledCallback()) override;

  bool hasRemainingTasks() override {
    // Need to override here because QueuedTasks is redeclared.
    return !QueuedTasks.empty();
  }

};

} // end namespace sys

namespace json {
template <> struct ObjectTraits<sys::TaskProcessInformation> {
  static void mapping(Output &out, sys::TaskProcessInformation &value) {
    value.provideMapping(out);
  }
};

template <> struct ObjectTraits<sys::TaskProcessInformation::ResourceUsage> {
  static void mapping(Output &out,
                      sys::TaskProcessInformation::ResourceUsage &value) {
    value.provideMapping(out);
  }
};
} // end namespace json

} // end namespace swift

#endif // SWIFT_BASIC_TASKQUEUE_H
