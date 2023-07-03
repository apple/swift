//===--- TaskQueue.inc - Unix-specific TaskQueue ----------------*- C++ -*-===//
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

#include "swift/Basic/TaskQueue.h"
#include "swift/Basic/Statistic.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/ErrorHandling.h"

#include <string>
#include <cerrno>

#if HAVE_POSIX_SPAWN
#include <spawn.h>
#endif

#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#if defined(HAVE_GETRUSAGE) && !defined(__HAIKU__)
#include <sys/resource.h>
#endif

#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>

#if !defined(__APPLE__)
extern char **environ;
#else
extern "C" {
  // _NSGetEnviron is from crt_externs.h which is missing in the iOS SDK.
  extern char ***_NSGetEnviron(void);
}
#endif

namespace swift {
namespace sys {

#if defined(HAVE_GETRUSAGE) && !defined(__HAIKU__)
TaskProcessInformation::TaskProcessInformation(ProcessId Pid, struct rusage Usage)
    : TaskProcessInformation(Pid,
        uint64_t(Usage.ru_utime.tv_sec) * 1000000 +
        uint64_t(Usage.ru_utime.tv_usec),
        uint64_t(Usage.ru_stime.tv_sec) * 1000000 +
        uint64_t(Usage.ru_stime.tv_usec),
        Usage.ru_maxrss) {
#ifndef __APPLE__
            // Apple systems report bytes; everything else appears to report KB.
            this->ProcessUsage.value().Maxrss <<= 10;
#endif // __APPLE__
        }
#endif // defined(HAVE_GETRUSAGE) && !defined(__HAIKU__)

class Task {
  /// The path to the executable which this Task will execute.
  const char *ExecPath;

  /// Any arguments which should be passed during execution.
  ArrayRef<const char *> Args;

  /// The environment which will be used during execution. If empty, uses
  /// this process's environment.
  ArrayRef<const char *> Env;

  /// Context which should be associated with this task.
  void *Context;

  /// True if the errors of the Task should be stored in Errors instead of Output.
  bool SeparateErrors;

  /// The pid of this Task when executing.
  pid_t Pid;

  /// A pipe for reading output from the child process.
  int Pipe;

  /// A pipe for reading errors from the child process, if SeparateErrors is true.
  int ErrorPipe;

  /// The current state of the Task.
  enum class TaskState { Preparing, Executing, Finished } State;

  /// Once the Task has finished, this contains the buffered output of the Task.
  std::string Output;

  /// Once the Task has finished, if SeparateErrors is true, this contains the
  /// errors from the Task.
  std::string Errors;

  /// Optional place to count I/O and subprocess events.
  UnifiedStatsReporter *Stats;

public:
  Task(const char *ExecPath, ArrayRef<const char *> Args,
       ArrayRef<const char *> Env, void *Context, bool SeparateErrors,
       UnifiedStatsReporter *USR)
      : ExecPath(ExecPath), Args(Args), Env(Env), Context(Context),
        SeparateErrors(SeparateErrors), Pid(-1), Pipe(-1), ErrorPipe(-1),
        State(TaskState::Preparing), Stats(USR) {
    assert((Env.empty() || Env.back() == nullptr) &&
           "Env must either be empty or null-terminated!");
  }

  const char *getExecPath() const { return ExecPath; }
  ArrayRef<const char *> getArgs() const { return Args; }
  StringRef getOutput() const { return Output; }
  StringRef getErrors() const { return Errors; }
  void *getContext() const { return Context; }
  pid_t getPid() const { return Pid; }
  int getPipe() const { return Pipe; }
  int getErrorPipe() const { return ErrorPipe; }

  /// Begins execution of this Task.
  /// \returns true on error.
  bool execute();

  /// Reads data from the pipes, if any is available.
  ///
  /// If \p UntilEnd is true, reads until the end of the stream; otherwise reads
  /// once (possibly with a retry on EINTR), and returns.
  /// \returns true on error.
  bool readFromPipes(bool UntilEnd);

  /// Performs any post-execution work for this Task, such as reading
  /// piped output and closing the pipe.
  void finishExecution();
};

} // end namespace sys
} // end namespace swift

bool Task::execute() {
  assert(State < TaskState::Executing && "This Task cannot be executed twice!");
  State = TaskState::Executing;

  // Construct argv.
  SmallVector<const char *, 128> Argv;
  Argv.push_back(ExecPath);
  Argv.append(Args.begin(), Args.end());
  Argv.push_back(0); // argv is expected to be null-terminated.

  // Set up the pipe.
  int FullPipe[2];
  pipe(FullPipe);
  Pipe = FullPipe[0];

  int FullErrorPipe[2];
  if (SeparateErrors) {
    pipe(FullErrorPipe);
    ErrorPipe = FullErrorPipe[0];
  }

  // Get the environment to pass down to the subtask.
  const char *const *envp = Env.empty() ? nullptr : Env.data();
  if (!envp) {
#if __APPLE__
    envp = *_NSGetEnviron();
#else
    envp = environ;
#endif
  }

  const char **argvp = Argv.data();

#if HAVE_POSIX_SPAWN
  posix_spawn_file_actions_t FileActions;
  posix_spawn_file_actions_init(&FileActions);

  posix_spawn_file_actions_adddup2(&FileActions, FullPipe[1], STDOUT_FILENO);

  if (SeparateErrors) {
    posix_spawn_file_actions_adddup2(&FileActions, FullErrorPipe[1],
                                     STDERR_FILENO);
  } else {
    posix_spawn_file_actions_adddup2(&FileActions, STDOUT_FILENO,
                                     STDERR_FILENO);
  }

  posix_spawn_file_actions_addclose(&FileActions, FullPipe[0]);
  if (SeparateErrors) {
    posix_spawn_file_actions_addclose(&FileActions, FullErrorPipe[0]);
  }

  // Spawn the subtask.
  int spawnErr =
      posix_spawn(&Pid, ExecPath, &FileActions, nullptr,
                  const_cast<char **>(argvp), const_cast<char **>(envp));

  posix_spawn_file_actions_destroy(&FileActions);
  close(FullPipe[1]);
  if (SeparateErrors) {
    close(FullErrorPipe[1]);
  }

  if (spawnErr != 0 || Pid == 0) {
    close(FullPipe[0]);
    if (SeparateErrors) {
      close(FullErrorPipe[0]);
    }
    State = TaskState::Finished;
    return true;
  }
#else
  Pid = fork();
  switch (Pid) {
  case -1: {
    close(FullPipe[0]);
    if (SeparateErrors) {
      close(FullErrorPipe[0]);
    }
    State = TaskState::Finished;
    Pid = 0;
    break;
  }
  case 0: {
    // Child process: Execute the program.
    dup2(FullPipe[1], STDOUT_FILENO);
    if (SeparateErrors) {
      dup2(FullErrorPipe[1], STDERR_FILENO);
    } else {
      dup2(STDOUT_FILENO, STDERR_FILENO);
    }
    close(FullPipe[0]);
    if (SeparateErrors) {
      close(FullErrorPipe[0]);
    }
    execve(ExecPath, const_cast<char **>(argvp), const_cast<char **>(envp));

    // If the execve() failed, we should exit. Follow Unix protocol and
    // return 127 if the executable was not found, and 126 otherwise.
    // Use _exit rather than exit so that atexit functions and static
    // object destructors cloned from the parent process aren't
    // redundantly run, and so that any data buffered in stdio buffers
    // cloned from the parent aren't redundantly written out.
    _exit(errno == ENOENT ? 127 : 126);
  }
  default:
    // Parent process: Break out of the switch to do our processing.
    break;
  }

  close(FullPipe[1]);
  if (SeparateErrors) {
    close(FullErrorPipe[1]);
  }

  if (Pid == 0)
    return true;
#endif

  return false;
}

/// Read the data in \p Pipe, and append it to \p Output.
/// \p Pipe must be in blocking mode, and must contain unread data.
/// If \p UntilEnd is true, keep reading, and possibly blocking, till the pipe
/// is closed. If \p UntilEnd is false, just read once. Return true if error
static bool readFromAPipe(std::string &Output, int Pipe,
                          UnifiedStatsReporter *Stats, bool UntilEnd) {
  char outputBuffer[1024];
  ssize_t readBytes = 0;
  while ((readBytes = read(Pipe, outputBuffer, sizeof(outputBuffer))) != 0) {
    if (readBytes < 0) {
      if (errno == EINTR)
        // read() was interrupted, so try again.
        // Q: Why isn't there a counter to break out of this loop if there are
        //    more than some number of EINTRs?
        // A: EINTR on a blocking read means only one thing: the syscall was
        //    interrupted and the program should retry. So there is no need to
        //    stop retrying after any particular number of interruptions (any
        //    more than the program would stop reading after a particular number
        //    of bytes or whatever).
        continue;
      return true;
    }
    Output.append(outputBuffer, readBytes);
    if (Stats)
      Stats->getDriverCounters().NumDriverPipeReads++;
    if (!UntilEnd)
      break;
  }
  return false;
}

bool Task::readFromPipes(bool UntilEnd) {
  bool Ret = readFromAPipe(Output, Pipe, Stats, UntilEnd);
  if (SeparateErrors) {
    Ret |= readFromAPipe(Errors, ErrorPipe, Stats, UntilEnd);
  }
  return Ret;
}

void Task::finishExecution() {
  assert(State == TaskState::Executing &&
         "This Task must be executing to finish execution!");

  State = TaskState::Finished;

  // Read the output of the command, so we can use it later.
  readFromPipes(/*UntilEnd*/ false);

  close(Pipe);
  if (SeparateErrors) {
    close(ErrorPipe);
  }
}

bool TaskQueue::supportsBufferingOutput() {
  // The Unix implementation supports buffering output.
  return true;
}

bool TaskQueue::supportsParallelExecution() {
  // The Unix implementation supports parallel execution.
  return true;
}

unsigned TaskQueue::getNumberOfParallelTasks() const {
  // TODO: add support for choosing a better default value for
  // MaxNumberOfParallelTasks if NumberOfParallelTasks is 0. (Optimally, this
  // should choose a value > 1 tailored to the current system.)
  return NumberOfParallelTasks > 0 ? NumberOfParallelTasks : 1;
}

void TaskQueue::addTask(const char *ExecPath, ArrayRef<const char *> Args,
                        ArrayRef<const char *> Env, void *Context,
                        bool SeparateErrors) {
  std::unique_ptr<Task> T(
      new Task(ExecPath, Args, Env, Context, SeparateErrors, Stats));
  QueuedTasks.push(std::move(T));
}

/// Owns Tasks, handles correspondence between Tasks, file descriptors, and
/// process IDs.
/// FIXME: only handles stdout pipes, ignores stderr pipes.
class TaskMap {
  using PidToTaskMap = llvm::DenseMap<pid_t, std::unique_ptr<Task>>;
  PidToTaskMap TasksByPid;

public:
  TaskMap() = default;

  bool empty() const { return TasksByPid.empty(); }
  unsigned size() const { return TasksByPid.size(); }

  void add(std::unique_ptr<Task> T) { TasksByPid[T->getPid()] = std::move(T); }

  Task &findTaskForFd(const int fd) {
    auto predicate = [&fd](PidToTaskMap::value_type &value) -> bool {
      return value.second->getPipe() == fd;
    };
    auto iter = std::find_if(TasksByPid.begin(), TasksByPid.end(), predicate);
    assert(iter != TasksByPid.end() &&
           "All outstanding fds must be associated with a  Task");
    return *iter->second;
  }

  void destroyTask(Task &T) { TasksByPid.erase(T.getPid()); }
};

/// Concurrently execute the tasks in the TaskQueue, collecting the outputs from
/// each task.
/// Maintain invariants connecting tasks to execute, tasks currently executing,
/// and fds being polled. These invariants include:
/// A task is not in both TasksToBeExecuted and TasksBeingExecuted,
/// A task is executing iff it is in TasksBeingExecuted,
/// A task is executing iff any of its fds being polled are in FdsBeingPolled
/// (These should be all of its output fds, but today is only stdout.)
/// When a task has finished executing, wait for it to die, takes
/// action appropriate to the cause of death, then reclaim its
/// storage.
class TaskMonitor {
  std::queue<std::unique_ptr<Task>> &TasksToBeExecuted;
  TaskMap TasksBeingExecuted;

  std::vector<struct pollfd> FdsBeingPolled;

  const unsigned MaxNumberOfParallelTasks;

public:
  struct Callbacks {
    const TaskQueue::TaskBeganCallback TaskBegan;
    const TaskQueue::TaskFinishedCallback TaskFinished;
    const TaskQueue::TaskSignalledCallback TaskSignalled;
    const std::function<void()> PolledAnFd;
  };

private:
  Callbacks callbacks;

public:
  TaskMonitor(std::queue<std::unique_ptr<Task>> &TasksToBeExecuted,
              const unsigned NumberOfParallelTasks, const Callbacks &callbacks)
      : TasksToBeExecuted(TasksToBeExecuted),
        MaxNumberOfParallelTasks(
            NumberOfParallelTasks == 0 ? 1 : NumberOfParallelTasks),
        callbacks(callbacks) {}

  /// Run the tasks to be executed.
  /// \return true on error.
  bool executeTasks();

private:
  bool isFinishedExecutingTasks() const {
    return TasksBeingExecuted.empty() && TasksToBeExecuted.empty();
  }

  /// Start up tasks if we aren't already at the parallel limit, and no earlier
  /// subtasks have failed.
  /// \return true on error.
  bool startUpSomeTasks();

  /// \return true on error.
  bool beginExecutingATask(Task &T);

  /// Enter the task and its outputs in this TaskMonitor's data structures so
  /// it can be polled.
  void startPollingFdsOfTask(const Task &T);

  void stopPolling(ArrayRef<int> FinishedFds);

  enum class PollResult { HardError, SoftError, NoError };
  PollResult pollTheFds();

  /// \return None on error.
  llvm::Optional<std::vector<int>> readFromReadyFdsReturningFinishedOnes();

  /// Ensure that events bits returned from polling are what's expected.
  void verifyEvents(short events) const;

  void readDataIfAvailable(short events, int fd, Task &T) const;

  bool didTaskHangup(short events) const;
};

bool TaskMonitor::executeTasks() {
  while (!isFinishedExecutingTasks()) {
    if (startUpSomeTasks())
      return true;

    switch (pollTheFds()) {
    case PollResult::HardError:
      return true;
    case PollResult::SoftError:
      continue;
    case PollResult::NoError:
      break;
    }
    llvm::Optional<std::vector<int>> FinishedFds =
        readFromReadyFdsReturningFinishedOnes();
    if (!FinishedFds)
      return true;

    stopPolling(*FinishedFds);
  }
  return false;
}

bool TaskMonitor::startUpSomeTasks() {
  while (!TasksToBeExecuted.empty() &&
         TasksBeingExecuted.size() < MaxNumberOfParallelTasks) {
    std::unique_ptr<Task> T(TasksToBeExecuted.front().release());
    TasksToBeExecuted.pop();
    if (beginExecutingATask(*T))
      return true;
    startPollingFdsOfTask(*T);
    TasksBeingExecuted.add(std::move(T));
  }
  return false;
}

void TaskMonitor::startPollingFdsOfTask(const Task &T) {
  FdsBeingPolled.push_back({T.getPipe(), POLLIN | POLLPRI | POLLHUP, 0});
  // We should also poll T->getErrorPipe(), but this introduces timing
  // issues with shutting down the task after reading getPipe().
}

TaskMonitor::PollResult TaskMonitor::pollTheFds() {
  assert(!FdsBeingPolled.empty() &&
         "We should only call poll() if we have fds to watch!");
  int ReadyFdCount = poll(FdsBeingPolled.data(), FdsBeingPolled.size(), -1);
  if (callbacks.PolledAnFd)
    callbacks.PolledAnFd();
  if (ReadyFdCount != -1)
    return PollResult::NoError;
  return errno == EAGAIN || errno == EINTR ? PollResult::SoftError
                                           : PollResult::HardError;
}

bool TaskMonitor::beginExecutingATask(Task &T) {
  if (T.execute())
    return true;
  if (callbacks.TaskBegan)
    callbacks.TaskBegan(T.getPid(), T.getContext());
  return false;
}

static bool
cleanUpAHungUpTask(Task &T,
                   const TaskQueue::TaskFinishedCallback FinishedCallback,
                   TaskQueue::TaskSignalledCallback SignalledCallback);

/**
 Wait for the process with a given pid to finish.

 @param pidToWaitFor the pid of the process to wait for
 @return Status information of the wait call and information about process
 */
static std::pair<llvm::Optional<int>, TaskProcessInformation> waitForPid(const pid_t pidToWaitFor);
static bool
cleanUpAfterSignal(int Status, const Task &T, TaskProcessInformation ProcInfo,
                   const TaskQueue::TaskSignalledCallback SignalledCallback);
static bool
cleanUpAfterExit(int Status, const Task &T, TaskProcessInformation ProcInfo,
                 const TaskQueue::TaskFinishedCallback FinishedCallback);

llvm::Optional<std::vector<int>>
TaskMonitor::readFromReadyFdsReturningFinishedOnes() {
  std::vector<int> finishedFds;
  for (struct pollfd &fd : FdsBeingPolled) {
    const int fileDes = fd.fd;
    const short receivedEvents = fd.revents;
    fd.revents = 0;
    verifyEvents(receivedEvents);
    Task &T = TasksBeingExecuted.findTaskForFd(fileDes);
    readDataIfAvailable(receivedEvents, fileDes, T);
    if (!didTaskHangup(receivedEvents))
      continue;
    finishedFds.push_back(fileDes);
    const bool hadError =
        cleanUpAHungUpTask(T, callbacks.TaskFinished, callbacks.TaskSignalled);
    TasksBeingExecuted.destroyTask(T);
    if (hadError)
      return llvm::None;
  }
  return finishedFds;
}

void TaskMonitor::verifyEvents(const short events) const {
  // We passed an invalid fd; this should never happen,
  // since we always mark fds as finished after calling
  // Task::finishExecution() (which closes the Task's fd).
  assert((events & POLLNVAL) == 0 && "Asked poll() to watch a closed fd");
  const short expectedEvents = POLLIN | POLLPRI | POLLHUP | POLLERR;
  assert((events & ~expectedEvents) == 0 && "Received unexpected event");
  (void)expectedEvents;
}

void TaskMonitor::readDataIfAvailable(const short events, const int fd,
                                      Task &T) const {
  if (events & (POLLIN | POLLPRI)) {
    // There's data available to read. Read _some_ of it here, but not
    // necessarily _all_, since the pipe is in blocking mode and we might
    // have other input pending (or soon -- before this subprocess is done
    // writing) from other subprocesses.
    //
    // FIXME: longer term, this should probably either be restructured to
    // use O_NONBLOCK, or at very least poll the stderr file descriptor as
    // well; the whole loop here is a bit of a mess.
    T.readFromPipes(/*UntilEnd*/ false);
  }
}

bool TaskMonitor::didTaskHangup(const short events) const {
  return (events & (POLLHUP | POLLERR)) != 0;
}

static bool
cleanUpAHungUpTask(Task &T,
                   const TaskQueue::TaskFinishedCallback FinishedCallback,
                   const TaskQueue::TaskSignalledCallback SignalledCallback) {
  const auto StatusAndProcessInformation = waitForPid(T.getPid());
  if (!StatusAndProcessInformation.first)
    return true;

  T.finishExecution();
  int Status = *(StatusAndProcessInformation.first);
  TaskProcessInformation ProcInfo = StatusAndProcessInformation.second;
  return WIFEXITED(Status)
             ? cleanUpAfterExit(Status, T, ProcInfo, FinishedCallback)
             : WIFSIGNALED(Status)
                   ? cleanUpAfterSignal(Status, T, ProcInfo, SignalledCallback)
                   : false /* Can this case ever happen? */;
}

static std::pair<llvm::Optional<int>, TaskProcessInformation> waitForPid(const pid_t pidToWaitFor) {
  for (;;) {
    int Status = 0;

#if defined(HAVE_GETRUSAGE) && !defined(__HAIKU__) && defined(HAVE_WAIT4)
    struct rusage Usage;
    const pid_t pidFromWait = wait4(pidToWaitFor, &Status, 0, &Usage);
    TaskProcessInformation ProcInfo(pidToWaitFor, Usage);
#else
    const pid_t pidFromWait = waitpid(pidToWaitFor, &Status, 0);
    TaskProcessInformation ProcInfo(pidToWaitFor);
#endif

    if (pidFromWait == pidToWaitFor)
      return std::make_pair(Status, ProcInfo);
    assert(pidFromWait == -1 &&
           "Did not pass WNOHANG, should only get pidToWaitFor or -1");
    if (errno == ECHILD || errno == EINVAL)
      return std::make_pair(llvm::None, TaskProcessInformation(pidToWaitFor));
  }
}

static bool
cleanUpAfterExit(int Status, const Task &T, TaskProcessInformation ProcInfo,
                 const TaskQueue::TaskFinishedCallback FinishedCallback) {
  const int Result = WEXITSTATUS(Status);
  if (!FinishedCallback) {
    // Since we don't have a TaskFinishedCallback, treat a subtask
    // which returned a nonzero exit code as having failed.
    return Result != 0;
  }
  // If we have a TaskFinishedCallback, only have an error if the callback
  // returns StopExecution.
  return TaskFinishedResponse::StopExecution ==
         FinishedCallback(T.getPid(), Result, T.getOutput(), T.getErrors(), ProcInfo,
                          T.getContext());
}

static bool
cleanUpAfterSignal(int Status, const Task &T, TaskProcessInformation ProcInfo,
                   const TaskQueue::TaskSignalledCallback SignalledCallback) {
  // The process exited due to a signal.
  const int Signal = WTERMSIG(Status);
  StringRef ErrorMsg = strsignal(Signal);

  if (!SignalledCallback) {
    // Since we don't have a TaskCrashedCallback, treat a crashing
    // subtask as having failed.
    return true;
  }
  // If we have a TaskCrashedCallback, only return an error if the callback
  // returns StopExecution.
  return TaskFinishedResponse::StopExecution ==
         SignalledCallback(T.getPid(), ErrorMsg, T.getOutput(), T.getErrors(),
                           T.getContext(), Signal, ProcInfo);
}

void TaskMonitor::stopPolling(ArrayRef<int> FinishedFds) {
  // Remove any fds which we've closed from FdsBeingPolled.
  for (int fd : FinishedFds) {
    auto predicate = [&fd](struct pollfd &i) { return i.fd == fd; };
    auto iter =
        std::find_if(FdsBeingPolled.begin(), FdsBeingPolled.end(), predicate);
    assert(iter != FdsBeingPolled.end() &&
           "The finished fd must be in FdsBeingPolled!");
    FdsBeingPolled.erase(iter);
  }
}

bool TaskQueue::execute(TaskBeganCallback BeganCallback,
                        TaskFinishedCallback FinishedCallback,
                        TaskSignalledCallback SignalledCallback) {
  TaskMonitor::Callbacks callbacks{
      BeganCallback, FinishedCallback, SignalledCallback, [&] {
        if (Stats)
          ++Stats->getDriverCounters().NumDriverPipePolls;
      }};

  TaskMonitor TE(QueuedTasks, getNumberOfParallelTasks(), callbacks);
  return TE.executeTasks();
}
