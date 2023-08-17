///===--- Actor.cpp - Standard actor implementation ------------------------===///
///
/// This source file is part of the Swift.org open source project
///
/// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
/// Licensed under Apache License v2.0 with Runtime Library Exception
///
/// See https:///swift.org/LICENSE.txt for license information
/// See https:///swift.org/CONTRIBUTORS.txt for the list of Swift project authors
///
///===----------------------------------------------------------------------===///
///
/// The default actor implementation for Swift actors, plus related
/// routines such as generic executor enqueuing and switching.
///
///===----------------------------------------------------------------------===///

#include "ConcurrencyRuntime.h"

#include "CompatibilityOverride.h"
#include "swift/Basic/ListMerger.h"
#include "swift/Runtime/Atomic.h"
#include "swift/Runtime/Casting.h"
#include "swift/Threading/Once.h"
#include "swift/Threading/Mutex.h"
#include "swift/Threading/Thread.h"
#include "swift/Threading/ThreadLocalStorage.h"
#ifndef SWIFT_CONCURRENCY_BACK_DEPLOYMENT
#include "llvm/Config/config.h"
#else
// All platforms where we care about back deployment have a known
// configurations.
#define HAVE_PTHREAD_H 1
#define SWIFT_OBJC_INTEROP 1
#endif
#include "llvm/ADT/PointerIntPair.h"
#include "Actor.h"
#include "Task.h"
#include "TaskPrivate.h"
#include "VoucherSupport.h"

#if SWIFT_CONCURRENCY_ENABLE_DISPATCH
#include <dispatch/dispatch.h>
#endif

#if defined(__APPLE__)
#include <asl.h>
#elif defined(__ANDROID__)
#include <android/log.h>
#endif

#if defined(__ELF__)
#include <unwind.h>
#endif

#if defined(__APPLE__)
#include <asl.h>
#elif defined(__ANDROID__)
#include <android/log.h>
#endif

#if defined(__ELF__)
#include <sys/syscall.h>
#endif

#if defined(_WIN32)
#include <io.h>
#endif

#if SWIFT_OBJC_INTEROP
extern "C" void *objc_autoreleasePoolPush();
extern "C" void objc_autoreleasePoolPop(void *);
#endif

using namespace swift;

/// Should we yield the thread?
static bool shouldYieldThread() {
  // FIXME: system scheduler integration
  return false;
}

/*****************************************************************************/
/******************************* TASK TRACKING ******************************/
/*****************************************************************************/

namespace {

/// An extremely silly class which exists to make pointer
/// default-initialization constexpr.
template <class T> struct Pointer {
  T *Value;
  constexpr Pointer() : Value(nullptr) {}
  constexpr Pointer(T *value) : Value(value) {}
  operator T *() const { return Value; }
  T *operator->() const { return Value; }
};

/// A class which encapsulates the information we track about
/// the current thread and active executor.
class ExecutorTrackingInfo {
  /// A thread-local variable pointing to the active tracking
  /// information about the current thread, if any.
  ///
  /// TODO: this is obviously runtime-internal and therefore not
  /// reasonable to make ABI. We might want to also provide a way
  /// for generated code to efficiently query the identity of the
  /// current executor, in order to do a cheap comparison to avoid
  /// doing all the work to suspend the task when we're already on
  /// the right executor. It would make sense for that to be a
  /// separate thread-local variable (or whatever is most efficient
  /// on the target platform).
  static SWIFT_THREAD_LOCAL_TYPE(Pointer<ExecutorTrackingInfo>,
                                 tls_key::concurrency_executor_tracking_info)
      ActiveInfoInThread;

  /// The active executor.
  ExecutorRef ActiveExecutor = ExecutorRef::generic();

  /// Whether this context allows switching.  Some contexts do not;
  /// for example, we do not allow switching from swift_job_run
  /// unless the passed-in executor is generic.
  bool AllowsSwitching = true;

  VoucherManager voucherManager;

  /// The tracking info that was active when this one was entered.
  ExecutorTrackingInfo *SavedInfo;

public:
  ExecutorTrackingInfo() = default;

  ExecutorTrackingInfo(const ExecutorTrackingInfo &) = delete;
  ExecutorTrackingInfo &operator=(const ExecutorTrackingInfo &) = delete;

  /// Unconditionally initialize a fresh tracking state on the
  /// current state, shadowing any previous tracking state.
  /// leave() must be called before the object goes out of scope.
  void enterAndShadow(ExecutorRef currentExecutor) {
    ActiveExecutor = currentExecutor;
    SavedInfo = ActiveInfoInThread.get();
    ActiveInfoInThread.set(this);
  }

  void swapToJob(Job *job) { voucherManager.swapToJob(job); }

  void restoreVoucher(AsyncTask *task) { voucherManager.restoreVoucher(task); }

  ExecutorRef getActiveExecutor() const {
    return ActiveExecutor;
  }

  void setActiveExecutor(ExecutorRef newExecutor) {
    ActiveExecutor = newExecutor;
  }


  bool allowsSwitching() const {
    return AllowsSwitching;
  }

  /// Disallow switching in this tracking context.  This should only
  /// be set on a new tracking info, before any jobs are run in it.
  void disallowSwitching() {
    AllowsSwitching = false;
  }

  static ExecutorTrackingInfo *current() {
    return ActiveInfoInThread.get();
  }

  void leave() {
    voucherManager.leave();
    ActiveInfoInThread.set(SavedInfo);
  }
};

class ActiveTask {
  /// A thread-local variable pointing to the active tracking
  /// information about the current thread, if any.
  static SWIFT_THREAD_LOCAL_TYPE(Pointer<AsyncTask>,
                                 tls_key::concurrency_task) Value;

public:
  static void set(AsyncTask *task) { Value.set(task); }
  static AsyncTask *get() { return Value.get(); }
};

/// Define the thread-locals.
SWIFT_THREAD_LOCAL_TYPE(Pointer<AsyncTask>, tls_key::concurrency_task)
ActiveTask::Value;

SWIFT_THREAD_LOCAL_TYPE(Pointer<ExecutorTrackingInfo>,
                        tls_key::concurrency_executor_tracking_info)
ExecutorTrackingInfo::ActiveInfoInThread;

} // end anonymous namespace

void swift::runJobInEstablishedExecutorContext(Job *job) {
  _swift_tsan_acquire(job);

#if SWIFT_OBJC_INTEROP
  auto pool = objc_autoreleasePoolPush();
#endif

  if (auto task = dyn_cast<AsyncTask>(job)) {
    // Update the active task in the current thread.
    ActiveTask::set(task);

    // Update the task status to say that it's running on the
    // current thread.  If the task suspends somewhere, it should
    // update the task status appropriately; we don't need to update
    // it afterwards.
    task->flagAsRunning();

    task->runInFullyEstablishedContext();

    assert(ActiveTask::get() == nullptr &&
           "active task wasn't cleared before suspending?");
  } else {
    // There's no extra bookkeeping to do for simple jobs besides swapping in
    // the voucher.
    ExecutorTrackingInfo::current()->swapToJob(job);
    job->runSimpleInFullyEstablishedContext();
  }

#if SWIFT_OBJC_INTEROP
  objc_autoreleasePoolPop(pool);
#endif

  _swift_tsan_release(job);
}

void swift::adoptTaskVoucher(AsyncTask *task) {
  ExecutorTrackingInfo::current()->swapToJob(task);
}

void swift::restoreTaskVoucher(AsyncTask *task) {
  ExecutorTrackingInfo::current()->restoreVoucher(task);
}

SWIFT_CC(swift)
AsyncTask *swift::swift_task_getCurrent() {
  return ActiveTask::get();
}

AsyncTask *swift::_swift_task_clearCurrent() {
  auto task = ActiveTask::get();
  ActiveTask::set(nullptr);
  return task;
}

SWIFT_CC(swift)
static ExecutorRef swift_task_getCurrentExecutorImpl() {
  auto currentTracking = ExecutorTrackingInfo::current();
  auto result = (currentTracking ? currentTracking->getActiveExecutor()
                                 : ExecutorRef::generic());
  SWIFT_TASK_DEBUG_LOG("getting current executor %p", result.getIdentity());
  return result;
}

/// Determine whether we are currently executing on the main thread
/// independently of whether we know that we are on the main actor.
static bool isExecutingOnMainThread() {
#if SWIFT_STDLIB_SINGLE_THREADED_CONCURRENCY
  return true;
#else
  return Thread::onMainThread();
#endif
}

JobPriority swift::swift_task_getCurrentThreadPriority() {
#if SWIFT_STDLIB_SINGLE_THREADED_CONCURRENCY
  return JobPriority::UserInitiated;
#elif defined(__APPLE__)
  return static_cast<JobPriority>(qos_class_self());
#else
  if (isExecutingOnMainThread())
    return JobPriority::UserInitiated;

  return JobPriority::Unspecified;
#endif
}

SWIFT_CC(swift)
static bool swift_task_isCurrentExecutorImpl(ExecutorRef executor) {
  if (auto currentTracking = ExecutorTrackingInfo::current()) {
    return currentTracking->getActiveExecutor() == executor;
  }

  return executor.isMainExecutor() && isExecutingOnMainThread();
}

/// Logging level for unexpected executors:
/// 0 - no logging
/// 1 - warn on each instance
/// 2 - fatal error
static unsigned unexpectedExecutorLogLevel = 1;

static void checkUnexpectedExecutorLogLevel(void *context) {
  const char *levelStr = getenv("SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL");
  if (!levelStr)
    return;

  long level = strtol(levelStr, nullptr, 0);
  if (level >= 0 && level < 3)
    unexpectedExecutorLogLevel = level;
}

SWIFT_CC(swift)
void swift::swift_task_reportUnexpectedExecutor(
    const unsigned char *file, uintptr_t fileLength, bool fileIsASCII,
    uintptr_t line, ExecutorRef executor) {
  // Make sure we have an appropriate log level.
  static swift::once_t logLevelToken;
  swift::once(logLevelToken, checkUnexpectedExecutorLogLevel, nullptr);

  bool isFatalError = false;
  switch (unexpectedExecutorLogLevel) {
  case 0:
    return;

  case 1:
    isFatalError = false;
    break;

  case 2:
    isFatalError = true;
    break;
  }

  const char *functionIsolation;
  const char *whereExpected;
  if (executor.isMainExecutor()) {
    functionIsolation = "@MainActor function";
    whereExpected = "the main thread";
  } else {
    functionIsolation = "actor-isolated function";
    whereExpected = "the same actor";
  }

  char *message;
  swift_asprintf(
      &message,
      "%s: data race detected: %s at %.*s:%d was not called on %s\n",
      isFatalError ? "error" : "warning", functionIsolation,
      (int)fileLength, file, (int)line, whereExpected);

  if (_swift_shouldReportFatalErrorsToDebugger()) {
    RuntimeErrorDetails details = {
        .version = RuntimeErrorDetails::currentVersion,
        .errorType = "actor-isolation-violation",
        .currentStackDescription = "Actor-isolated function called from another thread",
        .framesToSkip = 1,
    };
    _swift_reportToDebugger(
        isFatalError ? RuntimeErrorFlagFatal : RuntimeErrorFlagNone, message,
        &details);
  }

#if defined(_WIN32)
#define STDERR_FILENO 2
  _write(STDERR_FILENO, message, strlen(message));
#else
  write(STDERR_FILENO, message, strlen(message));
#endif
#if defined(__APPLE__)
  asl_log(nullptr, nullptr, ASL_LEVEL_ERR, "%s", message);
#elif defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_FATAL, "SwiftRuntime", "%s", message);
#endif

  free(message);

  if (isFatalError)
    abort();
}

/*****************************************************************************/
/*********************** DEFAULT ACTOR IMPLEMENTATION ************************/
/*****************************************************************************/

namespace {

class DefaultActorImpl;

/// A job to process a default actor.  Allocated inline in the actor.
class ProcessInlineJob : public Job {
public:
  ProcessInlineJob(JobPriority priority)
    : Job({JobKind::DefaultActorInline, priority}, &process) {}

  SWIFT_CC(swiftasync)
  static void process(Job *job);

  static bool classof(const Job *job) {
    return job->Flags.getKind() == JobKind::DefaultActorInline;
  }
};

/// A job to process a default actor that's allocated separately from
/// the actor but doesn't need the override mechanics.
class ProcessOutOfLineJob : public Job {
  DefaultActorImpl *Actor;
public:
  ProcessOutOfLineJob(DefaultActorImpl *actor, JobPriority priority)
    : Job({JobKind::DefaultActorSeparate, priority}, &process),
      Actor(actor) {}

  SWIFT_CC(swiftasync)
  static void process(Job *job);

  static bool classof(const Job *job) {
    return job->Flags.getKind() == JobKind::DefaultActorSeparate;
  }
};

/// A job to process a default actor with a new priority; allocated
/// separately from the actor.
class ProcessOverrideJob;

/// Information about the currently-running processing job.
struct RunningJobInfo {
  enum KindType : uint8_t {
    Inline, Override, Other
  };
  KindType Kind;
  JobPriority Priority;
  ProcessOverrideJob *OverrideJob;

  bool wasInlineJob() const {
    return Kind == Inline;
  }

  static RunningJobInfo forOther(JobPriority priority) {
    return {Other, priority, nullptr};
  }
  static RunningJobInfo forInline(JobPriority priority) {
    return {Inline, priority, nullptr};
  }
  static RunningJobInfo forOverride(ProcessOverrideJob *job);

  void setAbandoned();
  void setRunning();
  bool waitForActivation();
};

class JobRef {
  enum : uintptr_t {
    NeedsPreprocessing = 0x1,
    IsOverride = 0x2,
    JobMask = ~uintptr_t(NeedsPreprocessing | IsOverride)
  };

  /// A Job* that may have one of the two bits above mangled into it.
  uintptr_t Value;

  JobRef(Job *job, unsigned flags)
    : Value(reinterpret_cast<uintptr_t>(job) | flags) {}
public:
  constexpr JobRef() : Value(0) {}

  /// Return a reference to a job that's been properly preprocessed.
  static JobRef getPreprocessed(Job *job) {
    /// We allow null pointers here.
    return { job, 0 };
  }

  /// Return a reference to a job that hasn't been preprocesssed yet.
  static JobRef getUnpreprocessed(Job *job) {
    assert(job && "passing a null job");
    return { job, NeedsPreprocessing };
  }

  /// Return a reference to an override job, which needs special
  /// treatment during preprocessing.
  static JobRef getOverride(ProcessOverrideJob *job);

  /// Is this a null reference?
  operator bool() const { return Value != 0; }

  /// Does this job need to be pre-processed before we can treat
  /// the job queue as a proper queue?
  bool needsPreprocessing() const {
    return Value & NeedsPreprocessing;
  }

  /// Is this an unpreprocessed override job?
  bool isOverride() const {
    return Value & IsOverride;
  }

  /// Given that this is an override job, return it.
  ProcessOverrideJob *getAsOverride() const {
    assert(isOverride());
    return reinterpret_cast<ProcessOverrideJob*>(Value & JobMask);
  }
  ProcessOverrideJob *getAsPreprocessedOverride() const;

  Job *getAsJob() const {
    assert(!isOverride());
    return reinterpret_cast<Job*>(Value & JobMask);
  }
  Job *getAsPreprocessedJob() const {
    assert(!isOverride() && !needsPreprocessing());
    return reinterpret_cast<Job*>(Value);
  }

  bool operator==(JobRef other) const {
    return Value == other.Value;
  }
  bool operator!=(JobRef other) const {
    return Value != other.Value;
  }
};

/// The default actor implementation.
///
/// Ownership of the actor is subtle.  Jobs are assumed to keep the actor
/// alive as long as they're executing on it; this allows us to avoid
/// retaining and releasing whenever threads are scheduled to run a job.
/// While jobs are enqueued on the actor, there is a conceptual shared
/// ownership of the currently-enqueued jobs which is passed around
/// between threads and processing jobs and managed using extra retains
/// and releases of the actor.  The basic invariant is as follows:
///
/// - Let R be 1 if there are jobs enqueued on the actor or if a job
///   is currently running on the actor; otherwise let R be 0.
/// - Let N be the number of active processing jobs for the actor.
/// - N >= R
/// - There are N - R extra retains of the actor.
///
/// We can think of this as there being one "owning" processing job
/// and K "extra" jobs.  If there is a processing job that is actively
/// running the actor, it is always the owning job; otherwise, any of
/// the N jobs may win the race to become the owning job.
///
/// We then have the following ownership rules:
///
/// - When we enqueue the first job on an actor, then R becomes 1, and
///   we must create a processing job so that N >= R.  We do not need to
///   retain the actor.
/// - When we create an extra job to process an actor (e.g. because of
///   priority overrides), N increases but R remains the same.  We must
///   retain the actor.
/// - When we start running an actor, our job definitively becomes the
///   owning job, but neither N nor R changes.  We do not need to retain
///   the actor.
/// - When we go to start running an actor and for whatever reason we
///   don't actually do so, we are eliminating an extra processing job,
///   and so N decreases but R remains the same.  We must release the
///   actor.
/// - When we are running an actor and give it up, and there are no
///   remaining jobs on it, then R becomes 0 and N decreases by 1.
///   We do not need to release the actor.
/// - When we are running an actor and give it up, and there are jobs
///   remaining on it, then R remains 1 but N is decreasing by 1.
///   We must either release the actor or create a new processing job
///   for it to maintain the balance.
class DefaultActorImpl : public HeapObject {
  enum class Status {
    /// The actor is not currently scheduled.  Completely redundant
    /// with the job list being empty.
    Idle,

    /// There is currently a job scheduled to process the actor at the
    /// stored max priority.
    Scheduled,

    /// There is currently a thread processing the actor at the stored
    /// max priority.
    Running,

    /// The actor is a zombie that's been fully released but is still
    /// running.  We delay deallocation until its running thread gives
    /// it up, which fortunately doesn't touch anything in the
    /// actor except for the DefaultActorImpl.
    ///
    /// To coordinate between the releasing thread and the running
    /// thread, we have a two-stage "latch".  This is because the
    /// releasing thread does not atomically decide to not deallocate.
    /// Fortunately almost all of the overhead here is only incurred
    /// when we actually do end up in the zombie state.
    Zombie_Latching,
    Zombie_ReadyForDeallocation
  };

  struct Flags : public FlagSet<size_t> {
    enum : size_t {
      Status_offset = 0,
      Status_width = 3,

      HasActiveInlineJob = 3,

      IsDistributedRemote = 4,

      MaxPriority = 8,
      MaxPriority_width = JobFlags::Priority_width,

      // FIXME: add a reference to the running thread ID so that we
      // can boost priorities.
    };

    /// What is the current high-level status of this actor?
    FLAGSET_DEFINE_FIELD_ACCESSORS(Status_offset, Status_width, Status,
                                   getStatus, setStatus)

    bool isAnyRunningStatus() const {
      auto status = getStatus();
      return status == Status::Running ||
             status == Status::Zombie_Latching ||
             status == Status::Zombie_ReadyForDeallocation;
    }

    bool isAnyZombieStatus() const {
      auto status = getStatus();
      return status == Status::Zombie_Latching ||
             status == Status::Zombie_ReadyForDeallocation;
    }

    /// Is there currently an active processing job allocated inline
    /// in the actor?
    FLAGSET_DEFINE_FLAG_ACCESSORS(HasActiveInlineJob,
                                  hasActiveInlineJob, setHasActiveInlineJob)

    /// Is the actor a distributed 'remote' actor?
    /// I.e. it does not have storage for user-defined properties and all
    /// function call must be transformed into $distributed_ function calls.
    FLAGSET_DEFINE_FLAG_ACCESSORS(IsDistributedRemote,
                                  isDistributedRemote, setIsDistributedRemote)

    /// What is the maximum priority of jobs that are currently running
    /// or enqueued on this actor?
    ///
    /// Note that the above isn't quite correct: we don't actually
    /// lower this after we finish processing higher-priority tasks.
    /// (Doing so introduces some subtleties around kicking off
    /// lower-priority processing jobs.)
    FLAGSET_DEFINE_FIELD_ACCESSORS(MaxPriority, MaxPriority_width,
                                   JobPriority,
                                   getMaxPriority, setMaxPriority)
  };

  /// This is designed to fit into two words, which can generally be
  /// done lock-free on all our supported platforms.
  struct alignas(2 * sizeof(void*)) State {
    JobRef FirstJob;
    struct Flags Flags;
  };

  swift::atomic<State> CurrentState;

  friend class ProcessInlineJob;
  union {
    // When the ProcessInlineJob storage is initialized, its metadata pointer
    // will point to Job's metadata. When it isn't, the metadata pointer is
    // NULL. Use HeapObject to initialize the metadata pointer to NULL and allow
    // it to be checked without fully initializing the ProcessInlineJob.
    HeapObject JobStorageHeapObject{nullptr};

    ProcessInlineJob JobStorage;
  };

public:
  /// Properly construct an actor, except for the heap header.
  void initialize(bool isDistributedRemote = false) {
    auto flags = Flags();
    flags.setIsDistributedRemote(isDistributedRemote);
    new (&CurrentState) swift::atomic<State>(State{JobRef(), flags});
    JobStorageHeapObject.metadata = nullptr;
  }

  /// Properly destruct an actor, except for the heap header.
  void destroy();

  /// Properly respond to the last release of a default actor.  Note
  /// that the actor will have been completely torn down by the time
  /// we reach this point.
  void deallocate();

  /// Add a job to this actor.
  void enqueue(Job *job);

  /// Take over running this actor in the current thread, if possible.
  bool tryAssumeThread(RunningJobInfo runner);

  /// Give up running this actor in the current thread.
  void giveUpThread(RunningJobInfo runner);

  /// Claim the next job off the actor or give it up.
  Job *claimNextJobOrGiveUp(bool actorIsOwned, RunningJobInfo runner);

  /// Check if the actor is actually a distributed *remote* actor.
  ///
  /// Note that a distributed *local* actor instance is the same as any other
  /// ordinary default (local) actor, and no special handling is needed for them.
  bool isDistributedRemote();

private:
  void deallocateUnconditional();

  /// Schedule an inline processing job.  This can generally only be
  /// done if we know nobody else is trying to do it at the same time,
  /// e.g. if this thread just successfully transitioned the actor from
  /// Idle to Scheduled.
  void scheduleNonOverrideProcessJob(JobPriority priority,
                                     bool hasActiveInlineJob);

  static DefaultActorImpl *fromInlineJob(Job *job) {
    assert(isa<ProcessInlineJob>(job));
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
    return reinterpret_cast<DefaultActorImpl*>(
      reinterpret_cast<char*>(job) - offsetof(DefaultActorImpl, JobStorage));
#pragma clang diagnostic pop
  }

  class OverrideJobCache {
    ProcessOverrideJob *Job = nullptr;
    bool IsNeeded = false;
#ifndef NDEBUG
    bool WasCommitted = false;
#endif
  public:
    OverrideJobCache() = default;
    OverrideJobCache(const OverrideJobCache &) = delete;
    OverrideJobCache &operator=(const OverrideJobCache &) = delete;
    ~OverrideJobCache() {
      assert(WasCommitted && "didn't commit override job!");
    }

    void addToState(DefaultActorImpl *actor, State &newState);
    void setNotNeeded() { IsNeeded = false; }
    void commit();
  };
};

} /// end anonymous namespace

static_assert(sizeof(DefaultActorImpl) <= sizeof(DefaultActor) &&
              alignof(DefaultActorImpl) <= alignof(DefaultActor),
              "DefaultActorImpl doesn't fit in DefaultActor");

static DefaultActorImpl *asImpl(DefaultActor *actor) {
  return reinterpret_cast<DefaultActorImpl*>(actor);
}

static DefaultActor *asAbstract(DefaultActorImpl *actor) {
  return reinterpret_cast<DefaultActor*>(actor);
}

/*****************************************************************************/
/*********************** DEFAULT ACTOR IMPLEMENTATION ************************/
/*****************************************************************************/

void DefaultActorImpl::destroy() {
  auto oldState = CurrentState.load(std::memory_order_relaxed);
  while (true) {
    assert(!oldState.FirstJob && "actor has queued jobs at destruction");
    if (oldState.Flags.getStatus() == Status::Idle) return;

    assert(oldState.Flags.getStatus() == Status::Running &&
           "actor scheduled but not running at destruction");

    // If the actor is currently running, set it to zombie status
    // so that we know to deallocate it when it's given up.
    auto newState = oldState;
    newState.Flags.setStatus(Status::Zombie_Latching);
    if (CurrentState.compare_exchange_weak(oldState, newState,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed))
      return;
  }
}

void DefaultActorImpl::deallocate() {
  // If we're in a zombie state waiting to latch, put the actor in the
  // ready-for-deallocation state, but don't actually deallocate yet.
  // Note that we should never see the actor already in the
  // ready-for-deallocation state; giving up the actor while in the
  // latching state will always put it in Idle state.
  auto oldState = CurrentState.load(std::memory_order_relaxed);
  while (oldState.Flags.getStatus() == Status::Zombie_Latching) {
    auto newState = oldState;
    newState.Flags.setStatus(Status::Zombie_ReadyForDeallocation);
    if (CurrentState.compare_exchange_weak(oldState, newState,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed))
      return;
  }

  assert(oldState.Flags.getStatus() == Status::Idle);

  deallocateUnconditional();
}

void DefaultActorImpl::deallocateUnconditional() {
  if (JobStorageHeapObject.metadata != nullptr)
    JobStorage.~ProcessInlineJob();
  auto metadata = cast<ClassMetadata>(this->metadata);
  swift_deallocObject(this, metadata->getInstanceSize(),
                      metadata->getInstanceAlignMask());
}

/// Given that a job is enqueued normally on a default actor, get/set
/// the next job in the actor's queue.
///
/// Note that this must not be used on the override jobs that can appear
/// in the queue; those jobs are not actually in the actor's queue (they're
/// on the global execution queues).  So the actor's actual queue flows
/// through the NextJob field on those objects rather than through
/// the SchedulerPrivate fields.
static JobRef getNextJobInQueue(Job *job) {
  return *reinterpret_cast<JobRef*>(job->SchedulerPrivate);
}
static void setNextJobInQueue(Job *job, JobRef next) {
  *reinterpret_cast<JobRef*>(job->SchedulerPrivate) = next;
}

/// Schedule a processing job that doesn't have to be an override job.
///
/// We can either do this with inline storage or heap-allocated.
/// To ues inline storage, we need to verify that the hasActiveInlineJob
/// flag is not set in the state and then successfully set it.  The
/// argument reports that this has happened correctly.
///
/// We should only schedule a non-override processing job at all if
/// we're transferring ownership of the jobs in it; see the ownership
/// comment on DefaultActorImpl.
void DefaultActorImpl::scheduleNonOverrideProcessJob(JobPriority priority,
                                                     bool hasActiveInlineJob) {
  Job *job;
  if (hasActiveInlineJob) {
    job = new ProcessOutOfLineJob(this, priority);
  } else {
    if (JobStorageHeapObject.metadata != nullptr)
      JobStorage.~ProcessInlineJob();
    job = new (&JobStorage) ProcessInlineJob(priority);
  }
  swift_task_enqueueGlobal(job);
}


namespace {

/// A job to process a specific default actor at a higher priority than
/// it was previously running at.
///
/// When an override job is successfully registered with an actor
/// (not enqueued there), the thread processing the actor and the
/// thread processing the override job coordinate by each calling
/// one of a set of methods on the object.
class ProcessOverrideJob : public Job {
  DefaultActorImpl *Actor;

  ConditionVariable::Mutex Lock;
  ConditionVariable Queue;

  /// Has the actor made a decision about this job yet?
  bool IsResolvedByActor = false;

  /// Has the job made a decision about itself yet?
  bool IsResolvedByJob = false;

  /// Has this job been abandoned?
  bool IsAbandoned = false;

public:
  /// SchedulerPrivate in an override job is used for actually scheduling
  /// the job, so the actor queue goes through this instead.
  ///
  /// We also use this temporarily for the list of override jobs on
  /// the actor that we need to wake up.
  JobRef NextJob;

public:
  ProcessOverrideJob(DefaultActorImpl *actor, JobPriority priority,
                     JobRef nextJob)
    : Job({JobKind::DefaultActorOverride, priority}, &process),
      Actor(actor), NextJob(nextJob) {}

  DefaultActorImpl *getActor() const { return Actor; }

  /// Called by the job to notify the actor that the job has chosen
  /// to abandon its work.  This is irrevocable: the job is not going
  /// to have a thread behind it.
  ///
  /// This may delete the job or cause it to be deleted on another thread.
  void setAbandoned() {
    bool shouldDelete = false;
    Lock.withLock([&] {
      assert(!IsResolvedByJob && "job already resolved itself");
      IsResolvedByJob = true;
      IsAbandoned = true;
      shouldDelete = IsResolvedByJob && IsResolvedByActor;
    });
    if (shouldDelete) delete this;
  }

  /// Called by the job to notify the actor that the job has successfully
  /// taken over the actor and is now running it.
  ///
  /// This may delete the job object or cause it to be deleted on
  /// another thread.
  void setRunning() {
    bool shouldDelete = false;
    Lock.withLock([&] {
      assert(!IsResolvedByJob && "job already resolved itself");
      IsResolvedByJob = true;
      shouldDelete = IsResolvedByJob && IsResolvedByActor;
    });
    if (shouldDelete) delete this;
  }

  /// Called by the job to wait for the actor to resolve what the job
  /// should do.
  bool waitForActivation() {
    bool isActivated = false;
    Lock.withLockOrWait(Queue, [&] {
      assert(!IsResolvedByJob && "job already resolved itself");
      if (IsResolvedByActor) {
        isActivated = !IsAbandoned;
        IsResolvedByJob = true;
        return true;
      }
      return false;
    });
    delete this;
    return isActivated;
  }

  /// Called by the actor to notify this job that the actor thinks it
  /// should try to take over the actor.  It's okay if that doesn't
  /// succeed (as long as that's because some other job is going to
  /// take over).
  ///
  /// This may delete the job or cause it to be deleted on another
  /// thread.
  bool wakeAndActivate() {
    bool shouldDelete = false;
    bool mayHaveBeenActivated = false;
    Lock.withLockThenNotifyAll(Queue, [&] {
      assert(!IsResolvedByActor && "actor already resolved this sjob");
      IsResolvedByActor = true;
      mayHaveBeenActivated = IsResolvedByJob && !IsAbandoned;
      shouldDelete = IsResolvedByJob && IsResolvedByActor;
    });
    if (shouldDelete) delete this;
    return mayHaveBeenActivated;
  }

  /// Called by the actor to notify this job that the actor does not
  /// think it should try to take over the actor.  It's okay if the
  /// job successfully takes over the actor anyway.
  ///
  /// This may delete the job or cause it to be deleted on another
  /// thread.
  void wakeAndAbandon() {
    bool shouldDelete = false;
    Lock.withLockThenNotifyAll(Queue, [&] {
      assert(!IsResolvedByActor && "actor already resolved this job");
      IsResolvedByActor = true;
      IsAbandoned = true;
      shouldDelete = IsResolvedByJob && IsResolvedByActor;
    });
    if (shouldDelete) delete this;
  }

  SWIFT_CC(swiftasync)
  static void process(Job *job);

  static bool classof(const Job *job) {
    return job->Flags.getKind() == JobKind::DefaultActorOverride;
  }
};

} /// end anonymous namespace

JobRef JobRef::getOverride(ProcessOverrideJob *job) {
  return JobRef(job, NeedsPreprocessing | IsOverride);
}
ProcessOverrideJob *JobRef::getAsPreprocessedOverride() const {
  return cast_or_null<ProcessOverrideJob>(getAsPreprocessedJob());
}
RunningJobInfo RunningJobInfo::forOverride(ProcessOverrideJob *job) {
  return {Override, job->getPriority(), job};
}

/// Flag that the current processing job has been abandoned
/// and will not be running the actor.
void RunningJobInfo::setAbandoned() {
  if (OverrideJob) {
    OverrideJob->setAbandoned();
    OverrideJob = nullptr;
  }
}

/// Flag that the current processing job is now running the actor.
void RunningJobInfo::setRunning() {
  if (OverrideJob) {
    OverrideJob->setRunning();
    OverrideJob = nullptr;
  }
}

/// Try to wait for the current processing job to be activated,
/// if that's possible.  It's okay to call this multiple times
/// (or to call setAbandoned/setRunning after it) as long as
/// it's all on a single value.
bool RunningJobInfo::waitForActivation() {
  if (Kind == Override) {
    // If we don't have an override job, it's because we've already
    // waited for activation successfully.
    if (!OverrideJob) return true;

    bool result = OverrideJob->waitForActivation();
    OverrideJob = nullptr;
    return result;
  }
  return false;
}

/// Wake all the overrides in the given list, activating the first
/// that exactly matches the target priority, if any.
static void wakeOverrides(ProcessOverrideJob *nextOverride,
                          Optional<JobPriority> targetPriority) {
  bool hasAlreadyActivated = false;
  while (nextOverride) {
    // We have to advance to the next override before we call one of
    // the wake methods because they can delete the job immediately
    // (and even if they don't, we'd still be racing with deletion).
    auto cur = nextOverride;
    nextOverride = cur->NextJob.getAsPreprocessedOverride();

    if (hasAlreadyActivated ||
        !targetPriority)
      cur->wakeAndAbandon();
    else
      hasAlreadyActivated = cur->wakeAndActivate();
  }
}

/// Flag that an override job is needed and create it.
void DefaultActorImpl::OverrideJobCache::addToState(DefaultActorImpl *actor,
                                                    State &newState) {
  IsNeeded = true;
  auto newPriority = newState.Flags.getMaxPriority();
  auto nextJob = newState.FirstJob;
  if (Job) {
    Job->Flags.setPriority(newPriority);
    Job->NextJob = nextJob;
  } else {
    // Override jobs are always "extra" from the perspective of our
    // ownership rules and so require a retain of the actor.  We must
    // do this before changing the actor state because other jobs may
    // race to release the actor as soon as we change the actor state.
    swift_retain(actor);
    Job = new ProcessOverrideJob(actor, newPriority, nextJob);
  }
  newState.FirstJob = JobRef::getOverride(Job);
}

/// Schedule the override job if we created one and still need it.
/// If we created one but didn't end up needing it (which can happen
/// with a race to override), destroy it.
void DefaultActorImpl::OverrideJobCache::commit() {
#ifndef NDEBUG
  assert(!WasCommitted && "committing override job multiple timee");
  WasCommitted = true;
#endif

  if (Job) {
    if (IsNeeded) {
      swift_task_enqueueGlobal(Job);
    } else {
      swift_release(Job->getActor());
      delete Job;
    }
  }
}

namespace {

struct JobQueueTraits {
  static Job *getNext(Job *job) {
    return getNextJobInQueue(job).getAsPreprocessedJob();
  }
  static void setNext(Job *job, Job *next) {
    setNextJobInQueue(job, JobRef::getPreprocessed(next));
  }
  static int compare(Job *lhs, Job *rhs) {
    return descendingPriorityOrder(lhs->getPriority(), rhs->getPriority());
  }
};

} // end anonymous namespace

/// Preprocess the prefix of the actor's queue that hasn't already
/// been preprocessed:
///
/// - Split the jobs into registered overrides and actual jobs.
/// - Append the actual jobs to any already-preprocessed job list.
///
/// The returned job should become the new root of the job queue
/// (or may be immediately dequeued, in which its successor should).
/// All of the jobs in this list are guaranteed to be non-override jobs.
static Job *preprocessQueue(JobRef first,
                            JobRef previousFirst,
                            Job *previousFirstNewJob,
                            ProcessOverrideJob *&overridesToWake) {
  assert(previousFirst || previousFirstNewJob == nullptr);

  if (!first.needsPreprocessing())
    return first.getAsPreprocessedJob();

  using ListMerger = swift::ListMerger<Job*, JobQueueTraits>;
  ListMerger newJobs;

  while (first != previousFirst) {
    // If we find something that doesn't need preprocessing, it must've
    // been left by a previous queue-processing, which means that
    // this must be our first attempt to preprocess in this processing.
    // Just treat the queue from this point as a well-formed whole
    // to which we need to add any new items we might've just found.
    if (!first.needsPreprocessing()) {
      assert(!previousFirst && !previousFirstNewJob);
      previousFirstNewJob = first.getAsPreprocessedJob();
      break;
    }

    // If the job is an override, add it to the list of override jobs
    // that we need to wake up.  Note that the list of override jobs
    // flows through NextJob; we must not use getNextJobInQueue because
    // that touches queue-private state, and the override job is
    // not enqueued on the actor, merely registered with it.
    if (first.isOverride()) {
      auto overrideJob = first.getAsOverride();
      first = overrideJob->NextJob;
      overrideJob->NextJob = JobRef::getPreprocessed(overridesToWake);
      overridesToWake = overrideJob;
      continue;
    }

    // If the job isn't an override, add it to the front of the list of
    // jobs we're building up.  Note that this reverses the order of
    // jobs; since enqueue() always adds jobs to the front, reversing
    // the order effectively makes the actor queue FIFO, which is what
    // we want.
    auto job = first.getAsJob();
    first = getNextJobInQueue(job);
    newJobs.insertAtFront(job);
  }

  // If there are jobs already in the queue, put the new jobs at the end.
  auto firstNewJob = newJobs.release();
  if (!firstNewJob) {
    firstNewJob = previousFirstNewJob;
  } else if (previousFirstNewJob) {
    // Merge the jobs we just processed into the existing job list.
    ListMerger merge(previousFirstNewJob);
    merge.merge(firstNewJob);
    firstNewJob = merge.release();
  }

  return firstNewJob;
}

void DefaultActorImpl::giveUpThread(RunningJobInfo runner) {
  SWIFT_TASK_DEBUG_LOG("giving up thread for actor %p", this);
  auto oldState = CurrentState.load(std::memory_order_acquire);
  assert(oldState.Flags.isAnyRunningStatus());

  ProcessOverrideJob *overridesToWake = nullptr;
  auto firstNewJob = preprocessQueue(oldState.FirstJob, JobRef(), nullptr,
                                     overridesToWake);

  _swift_tsan_release(this);
  while (true) {
    // In Zombie_ReadyForDeallocation state, nothing else should
    // be touching the atomic, and there's no point updating it.
    if (oldState.Flags.getStatus() == Status::Zombie_ReadyForDeallocation) {
      wakeOverrides(overridesToWake, oldState.Flags.getMaxPriority());
      deallocateUnconditional();
      return;
    }

    // In Zombie_Latching state, we should try to update to Idle;
    // if we beat the releasing thread, it'll deallocate.
    // In Running state, we may need to schedule a processing job.

    State newState = oldState;
    newState.FirstJob = JobRef::getPreprocessed(firstNewJob);
    if (firstNewJob) {
      assert(oldState.Flags.getStatus() == Status::Running);
      newState.Flags.setStatus(Status::Scheduled);
    } else {
      newState.Flags.setStatus(Status::Idle);
    }

    // If the runner was an inline job, it's no longer active.
    if (runner.wasInlineJob()) {
      newState.Flags.setHasActiveInlineJob(false);
    }

    bool hasMoreJobs = (bool) newState.FirstJob;
    bool hasOverrideAtNewPriority = false;
    bool hasActiveInlineJob = newState.Flags.hasActiveInlineJob();
    bool needsNewProcessJob = hasMoreJobs && !hasOverrideAtNewPriority;

    // If we want to create a new inline job below, be sure to claim that
    // in the new state.
    if (needsNewProcessJob && !hasActiveInlineJob) {
      newState.Flags.setHasActiveInlineJob(true);
    }

    auto firstPreprocessed = oldState.FirstJob;
    if (!CurrentState.compare_exchange_weak(oldState, newState,
                    /*success*/ std::memory_order_release,
                    /*failure*/ std::memory_order_acquire)) {
      // Preprocess any new queue items.
      firstNewJob = preprocessQueue(oldState.FirstJob,
                                    firstPreprocessed,
                                    firstNewJob,
                                    overridesToWake);

      // Try again.
      continue;
    }

#define LOG_STATE_TRANSITION                                                   \
  SWIFT_TASK_DEBUG_LOG("actor %p transitioned from %zx to %zx (%s)", this,     \
                       oldState.Flags.getOpaqueValue(),                        \
                       newState.Flags.getOpaqueValue(), __FUNCTION__)
    LOG_STATE_TRANSITION;

    // The priority of the remaining work.
    auto newPriority = newState.Flags.getMaxPriority();

    // Process the override commands we found.
    wakeOverrides(overridesToWake, newPriority);

    // This is the actor's owning job; per the ownership rules (see
    // the comment on DefaultActorImpl), if there are remaining
    // jobs, we need to balance out our ownership one way or another.
    // We also, of course, need to ensure that there's a thread that's
    // actually going to process the actor.
    if (hasMoreJobs) {
      // If we know that there's an override job at the new priority,
      // we can let it become the owning job.  We just need to release.
      if (hasOverrideAtNewPriority) {
        swift_release(this);

      // Otherwies, enqueue a job that will try to take over running
      // with the new priority.  This also ensures that there's a job
      // at that priority which will actually take over the actor.
      } else {
        scheduleNonOverrideProcessJob(newPriority, hasActiveInlineJob);
      }
    }

    return;
  }
}

/// Claim the next job on the actor or give it up forever.
///
/// The running thread doesn't need to already own the actor to do this.
/// It does need to be participating correctly in the ownership
/// scheme as a "processing job"; see the comment on DefaultActorImpl.
Job *DefaultActorImpl::claimNextJobOrGiveUp(bool actorIsOwned,
                                            RunningJobInfo runner) {
  auto oldState = CurrentState.load(std::memory_order_acquire);

  // The status had better be Running unless we're trying to acquire
  // our first job.
  assert(oldState.Flags.isAnyRunningStatus() || !actorIsOwned);

  // If we don't yet own the actor, we need to try to claim the actor
  // first; we cannot safely access the queue memory yet because other
  // threads may concurrently be trying to do this.
  if (!actorIsOwned) {
    // We really shouldn't ever be in a state where we're trying to take
    // over a non-running actor if the actor is in a zombie state.
    assert(!oldState.Flags.isAnyZombieStatus());

    while (true) {
      // A helper function when the only change we need to try is to
      // update for an inline runner.
      auto tryUpdateForInlineRunner = [&]{
        if (!runner.wasInlineJob()) return true;

        auto newState = oldState;
        newState.Flags.setHasActiveInlineJob(false);
        auto success = CurrentState.compare_exchange_weak(oldState, newState,
                            /*success*/ std::memory_order_relaxed,
                            /*failure*/ std::memory_order_acquire);
        if (success) LOG_STATE_TRANSITION;
        return success;
      };

      // If the actor is out of work, or its priority doesn't match our
      // priority, don't try to take over the actor.
      if (!oldState.FirstJob) {

        // The only change we need here is inline-runner bookkeeping.
        if (!tryUpdateForInlineRunner())
          continue;

        // We're eliminating a processing thread; balance ownership.
        swift_release(this);
        runner.setAbandoned();
        return nullptr;
      }

      // If the actor is currently running, we'd need to wait for
      // it to stop.  We can do this if we're an override job;
      // otherwise we need to exit.
      if (oldState.Flags.getStatus() == Status::Running) {
        if (!runner.waitForActivation()) {
          // The only change we need here is inline-runner bookkeeping.
          if (!tryUpdateForInlineRunner())
            continue;

          swift_release(this);
          return nullptr;
        }

        // Fall through into the compare-exchange below, but anticipate
        // that the actor is now Scheduled instead of Running.
        oldState.Flags.setStatus(Status::Scheduled);
      }

      // Try to set the state as Running.
      assert(oldState.Flags.getStatus() == Status::Scheduled);
      auto newState = oldState;
      newState.Flags.setStatus(Status::Running);

      // Also do our inline-runner bookkeeping.
      if (runner.wasInlineJob())
        newState.Flags.setHasActiveInlineJob(false);

      if (!CurrentState.compare_exchange_weak(oldState, newState,
                              /*success*/ std::memory_order_relaxed,
                              /*failure*/ std::memory_order_acquire))
        continue;
      LOG_STATE_TRANSITION;
      _swift_tsan_acquire(this);

      // If that succeeded, we can proceed to the main body.
      oldState = newState;
      runner.setRunning();
      break;
    }
  }

  assert(oldState.Flags.isAnyRunningStatus());

  // We should have taken care of the inline-job bookkeeping now.
  assert(!oldState.Flags.hasActiveInlineJob() || !runner.wasInlineJob());

  // Okay, now it's safe to look at queue state.
  // Preprocess any queue items at the front of the queue.
  ProcessOverrideJob *overridesToWake = nullptr;
  auto newFirstJob = preprocessQueue(oldState.FirstJob, JobRef(),
                                     nullptr, overridesToWake);

  Optional<JobPriority> remainingJobPriority;
  _swift_tsan_release(this);
  while (true) {
    // In Zombie_ReadyForDeallocation state, nothing else should
    // be touching the atomic, and there's no point updating it.
    if (oldState.Flags.getStatus() == Status::Zombie_ReadyForDeallocation) {
      wakeOverrides(overridesToWake, oldState.Flags.getMaxPriority());
      deallocateUnconditional();
      return nullptr;
    }

    State newState = oldState;

    // If the priority we're currently running with is adqeuate for
    // all the remaining jobs, try to dequeue something.
    // FIXME: should this be an exact match in priority instead of
    // potentially running jobs with too high a priority?
    Job *jobToRun;
    if (newFirstJob) {
      jobToRun = newFirstJob;
      newState.FirstJob = getNextJobInQueue(newFirstJob);
      newState.Flags.setStatus(Status::Running);

    // Otherwise, we should give up the thread.
    } else {
      jobToRun = nullptr;
      newState.FirstJob = JobRef::getPreprocessed(newFirstJob);
      newState.Flags.setStatus(newFirstJob ? Status::Scheduled
                                           : Status::Idle);
    }

    // Try to update the queue.  The changes we've made to the queue
    // structure need to be made visible even if we aren't dequeuing
    // anything.
    auto firstPreprocessed = oldState.FirstJob;
    if (!CurrentState.compare_exchange_weak(oldState, newState,
                     /*success*/ std::memory_order_release,
                     /*failure*/ std::memory_order_acquire)) {
      // Preprocess any new queue items, which will have been formed
      // into a linked list leading to the last head we observed.
      // (The fact that that job may not be the head anymore doesn't
      // matter; we're looking for an exact match with that.)
      newFirstJob = preprocessQueue(oldState.FirstJob,
                                    firstPreprocessed,
                                    newFirstJob,
                                    overridesToWake);

      // Loop to retry updating the state.
      continue;
    }
    LOG_STATE_TRANSITION;

    // We successfully updated the state.

    // If we're giving up the thread with jobs remaining, we need
    // to release the actor, and we should wake overrides with the
    // right priority.
    Optional<JobPriority> remainingJobPriority;
    if (!jobToRun && newFirstJob) {
      remainingJobPriority = newState.Flags.getMaxPriority();
    }

    // Wake the overrides.
    wakeOverrides(overridesToWake, remainingJobPriority);

    // Per the ownership rules (see the comment on DefaultActorImpl),
    // release the actor if we're giving up the thread with jobs
    // remaining.  We intentionally do this after wakeOverrides to
    // try to get the overrides running a little faster.
    if (remainingJobPriority)
      swift_release(this);

    return jobToRun;
  }
}

SWIFT_CC(swift)
static void swift_job_runImpl(Job *job, ExecutorRef executor) {
  ExecutorTrackingInfo trackingInfo;

  // swift_job_run is a primary entrypoint for executors telling us to
  // run jobs.  Actor executors won't expect us to switch off them
  // during this operation.  But do allow switching if the executor
  // is generic.
  if (!executor.isGeneric()) trackingInfo.disallowSwitching();

  trackingInfo.enterAndShadow(executor);

  SWIFT_TASK_DEBUG_LOG("%s(%p)", __func__, job);
  runJobInEstablishedExecutorContext(job);

  trackingInfo.leave();

  // Give up the current executor if this is a switching context
  // (which, remember, only happens if we started out on a generic
  // executor) and we've switched to a default actor.
  auto currentExecutor = trackingInfo.getActiveExecutor();
  if (trackingInfo.allowsSwitching() && currentExecutor.isDefaultActor()) {
    // Use an underestimated priority; if this means we create an
    // extra processing job in some cases, that's probably okay.
    auto runner = RunningJobInfo::forOther(JobPriority(0));
    asImpl(currentExecutor.getDefaultActor())->giveUpThread(runner);
  }
}

/// The primary function for processing an actor on a thread.  Start
/// processing the given default actor as the active default actor on
/// the current thread, and keep processing whatever actor we're
/// running when code returns back to us until we're not processing
/// any actors anymore.
///
/// \param currentActor is expected to be passed in as retained to ensure that
///                     the actor lives for the duration of job execution.
///                     Note that this may conflict with the retain/release
///                     design in the DefaultActorImpl, but it does fix bugs!
SWIFT_CC(swiftasync)
static void processDefaultActor(DefaultActorImpl *currentActor,
                                RunningJobInfo runner) {
  SWIFT_TASK_DEBUG_LOG("processDefaultActor %p", currentActor);
  DefaultActorImpl *actor = currentActor;

  // If we actually have work to do, we'll need to set up tracking info.
  // Optimistically assume that we will; the alternative (an override job
  // took over the actor first) is rare.
  ExecutorTrackingInfo trackingInfo;
  trackingInfo.enterAndShadow(
    ExecutorRef::forDefaultActor(asAbstract(currentActor)));

  // Remember whether we've already taken over the actor.
  bool threadIsRunningActor = false;
  while (true) {
    assert(currentActor);

    // Immediately check if we've been asked to yield the thread.
    if (shouldYieldThread())
      break;

    // Try to claim another job from the current actor, taking it over
    // if we haven't already.
    auto job = currentActor->claimNextJobOrGiveUp(threadIsRunningActor,
                                                  runner);

    SWIFT_TASK_DEBUG_LOG("processDefaultActor %p claimed job %p", currentActor,
                         job);

    // If we failed to claim a job, we have nothing to do.
    if (!job) {
      // We also gave up the actor as part of failing to claim it.
      // Make sure we don't try to give up the actor again.
      currentActor = nullptr;
      break;
    }

    // This thread now owns the current actor.
    threadIsRunningActor = true;

    // Run the job.
    runJobInEstablishedExecutorContext(job);

    // The current actor may have changed after the job.
    // If it's become nil, or not a default actor, we have nothing to do.
    auto currentExecutor = trackingInfo.getActiveExecutor();

    SWIFT_TASK_DEBUG_LOG("processDefaultActor %p current executor now %p",
                         currentActor, currentExecutor.getIdentity());

    if (!currentExecutor.isDefaultActor()) {
      // The job already gave up the thread for us.
      // Make sure we don't try to give up the actor again.
      currentActor = nullptr;
      break;
    }
    currentActor = asImpl(currentExecutor.getDefaultActor());
  }

  // Leave the tracking info.
  trackingInfo.leave();

  // If we still have an active actor, we should give it up.
  if (threadIsRunningActor && currentActor) {
    currentActor->giveUpThread(runner);
  }

  swift_release(actor);
}

SWIFT_CC(swiftasync)
void ProcessInlineJob::process(Job *job) {
  DefaultActorImpl *actor = DefaultActorImpl::fromInlineJob(job);

  // Pull the priority out of the job before we do anything that might
  // invalidate it.
  auto targetPriority = job->getPriority();
  auto runner = RunningJobInfo::forInline(targetPriority);

  swift_retain(actor);
  return processDefaultActor(actor, runner); // 'return' forces tail call
}

SWIFT_CC(swiftasync)
void ProcessOutOfLineJob::process(Job *job) {
  auto self = cast<ProcessOutOfLineJob>(job);
  DefaultActorImpl *actor = self->Actor;

  // Pull the priority out of the job before we do anything that might
  // invalidate it.
  auto targetPriority = job->getPriority();
  auto runner = RunningJobInfo::forOther(targetPriority);

  delete self;

  swift_retain(actor);
  return processDefaultActor(actor, runner); // 'return' forces tail call
}

SWIFT_CC(swiftasync)
void ProcessOverrideJob::process(Job *job) {
  auto self = cast<ProcessOverrideJob>(job);

  // Pull the actor and priority out of the job.
  auto actor = self->Actor;
  auto runner = RunningJobInfo::forOverride(self);

  swift_retain(actor);
  return processDefaultActor(actor, runner); // 'return' forces tail call
}

void DefaultActorImpl::enqueue(Job *job) {
  auto oldState = CurrentState.load(std::memory_order_relaxed);

  OverrideJobCache overrideJob;

  while (true) {
    assert(!oldState.Flags.isAnyZombieStatus() &&
           "enqueuing work on a zombie actor");
    auto newState = oldState;

    // Put the job at the front of the job list (which will get
    // reversed during preprocessing).
    setNextJobInQueue(job, oldState.FirstJob);
    newState.FirstJob = JobRef::getUnpreprocessed(job);

    auto oldStatus = oldState.Flags.getStatus();
    bool wasIdle = oldStatus == Status::Idle;

    // Update the priority: the priority of the job we're adding
    // if the actor was idle, or the max if not.  Only the running
    // thread can decrease the actor's priority once it's non-idle.
    // (But note that the job we enqueue can still observe a
    // lowered priority.)
    auto oldPriority = oldState.Flags.getMaxPriority();
    auto newPriority =
      wasIdle ? job->getPriority()
              : std::max(oldPriority, job->getPriority());
    newState.Flags.setMaxPriority(newPriority);

    // If we need an override job, create it (if necessary) and
    // register it with the queue.
    bool needsOverride = false;
    if (needsOverride) {
      overrideJob.addToState(this, newState);
    } else {
      overrideJob.setNotNeeded();
    }

    // If we don't need an override job, then we might be able to
    // create an inline job; flag that.
    bool hasActiveInlineJob = newState.Flags.hasActiveInlineJob();
    if (wasIdle && !hasActiveInlineJob)
      newState.Flags.setHasActiveInlineJob(true);

    // Make sure the status is at least Scheduled.  We'll actually
    // schedule the job below, if we succeed at this.
    if (wasIdle) {
      newState.Flags.setStatus(Status::Scheduled);
    }

    // Try the compare-exchange, and try again if it fails.
    if (!CurrentState.compare_exchange_weak(oldState, newState,
          /*success*/ std::memory_order_release,
          /*failure*/ std::memory_order_relaxed))
      continue;
    LOG_STATE_TRANSITION;

    // Okay, we successfully updated the status.  Schedule a job to
    // process the actor if necessary.

    // Commit the override job if we created one.
    overrideJob.commit();

    // If the actor is currently idle, schedule it using the
    // invasive job.
    if (wasIdle) {
      assert(!needsOverride);
      scheduleNonOverrideProcessJob(newPriority, hasActiveInlineJob);
    }

    return;
  }
}

bool DefaultActorImpl::tryAssumeThread(RunningJobInfo runner) {
  // We have to load-acquire in order to properly order accesses to
  // the actor's state for the new task.
  auto oldState = CurrentState.load(std::memory_order_acquire);

  // If the actor is currently idle, try to mark it as running.
  while (oldState.Flags.getStatus() == Status::Idle) {
    assert(!oldState.FirstJob);
    auto newState = oldState;
    newState.Flags.setStatus(Status::Running);
    newState.Flags.setMaxPriority(runner.Priority);

    if (CurrentState.compare_exchange_weak(oldState, newState,
                              /*success*/ std::memory_order_relaxed,
                              /*failure*/ std::memory_order_acquire)) {
      LOG_STATE_TRANSITION;
      _swift_tsan_acquire(this);
      return true;
    }
  }

  assert(!oldState.Flags.isAnyZombieStatus() &&
         "trying to assume a zombie actor");

  return false;
}

void swift::swift_defaultActor_initialize(DefaultActor *_actor) {
  asImpl(_actor)->initialize();
}

void swift::swift_defaultActor_destroy(DefaultActor *_actor) {
  asImpl(_actor)->destroy();
}

void swift::swift_defaultActor_enqueue(Job *job, DefaultActor *_actor) {
  asImpl(_actor)->enqueue(job);
}

void swift::swift_defaultActor_deallocate(DefaultActor *_actor) {
  asImpl(_actor)->deallocate();
}

static bool isDefaultActorClass(const ClassMetadata *metadata) {
  assert(metadata->isTypeMetadata());
  while (true) {
    // Trust the class descriptor if it says it's a default actor.
    if (!metadata->isArtificialSubclass() &&
        metadata->getDescription()->isDefaultActor()) {
      return true;
    }

    // Go to the superclass.
    metadata = metadata->Superclass;

    // If we run out of Swift classes, it's not a default actor.
    if (!metadata || !metadata->isTypeMetadata()) return false;
  }
}

void swift::swift_defaultActor_deallocateResilient(HeapObject *actor) {
  auto metadata = cast<ClassMetadata>(actor->metadata);
  if (isDefaultActorClass(metadata))
    return swift_defaultActor_deallocate(static_cast<DefaultActor*>(actor));

  swift_deallocObject(actor, metadata->getInstanceSize(),
                      metadata->getInstanceAlignMask());
}

/// FIXME: only exists for the quick-and-dirty MainActor implementation.
namespace swift {
  Metadata* MainActorMetadata = nullptr;
}

/*****************************************************************************/
/****************************** ACTOR SWITCHING ******************************/
/*****************************************************************************/

/// Can the current executor give up its thread?
static bool canGiveUpThreadForSwitch(ExecutorTrackingInfo *trackingInfo,
                                     ExecutorRef currentExecutor) {
  assert(trackingInfo || currentExecutor.isGeneric());

  // Some contexts don't allow switching at all.
  if (trackingInfo && !trackingInfo->allowsSwitching())
    return false;

  // We can certainly "give up" a generic executor to try to run
  // a task for an actor.
  if (currentExecutor.isGeneric())
    return true;

  // If the current executor is a default actor, we know how to make
  // it give up its thread.
  if (currentExecutor.isDefaultActor())
    return true;

  return false;
}

/// Tell the current executor to give up its thread, given that it
/// returned true from canGiveUpThreadForSwitch().
///
/// Note that we don't update DefaultActorProcessingFrame here; we'll
/// do that in runOnAssumedThread.
static void giveUpThreadForSwitch(ExecutorRef currentExecutor,
                                  RunningJobInfo runner) {
  if (currentExecutor.isGeneric())
    return;

  asImpl(currentExecutor.getDefaultActor())->giveUpThread(runner);
}

/// Try to assume control of the current thread for the given executor
/// in order to run the given job.
///
/// This doesn't actually run the job yet.
///
/// Note that we don't update DefaultActorProcessingFrame here; we'll
/// do that in runOnAssumedThread.
static bool tryAssumeThreadForSwitch(ExecutorRef newExecutor,
                                     RunningJobInfo runner) {
  // If the new executor is generic, we don't need to do anything.
  if (newExecutor.isGeneric()) {
    return true;
  }

  // If the new executor is a default actor, ask it to assume the thread.
  if (newExecutor.isDefaultActor()) {
    return asImpl(newExecutor.getDefaultActor())->tryAssumeThread(runner);
  }

  return false;
}

/// Given that we've assumed control of an executor on this thread,
/// continue to run the given task on it.
SWIFT_CC(swiftasync)
static void runOnAssumedThread(AsyncTask *task, ExecutorRef executor,
                               ExecutorTrackingInfo *oldTracking,
                               RunningJobInfo runner) {
  // Note that this doesn't change the active task and so doesn't
  // need to either update ActiveTask or flagAsRunning/flagAsSuspended.

  // If there's already tracking info set up, just change the executor
  // there and tail-call the task.  We don't want these frames to
  // potentially accumulate linearly.
  if (oldTracking) {
    oldTracking->setActiveExecutor(executor);

    return task->runInFullyEstablishedContext(); // 'return' forces tail call
  }

  // Otherwise, set up tracking info.
  ExecutorTrackingInfo trackingInfo;
  trackingInfo.enterAndShadow(executor);

  // Run the new task.
  task->runInFullyEstablishedContext();

  // Leave the tracking frame, and give up the current actor if
  // we have one.
  //
  // In principle, we could execute more tasks from the actor here, but
  // that's probably not a reasonable thing to do in an assumed context
  // rather than a dedicated actor-processing job.
  executor = trackingInfo.getActiveExecutor();
  trackingInfo.leave();

  SWIFT_TASK_DEBUG_LOG("leaving assumed thread, current executor is %p",
                       executor.getIdentity());

  if (executor.isDefaultActor())
    asImpl(executor.getDefaultActor())->giveUpThread(runner);
}

SWIFT_CC(swiftasync)
static void swift_task_switchImpl(SWIFT_ASYNC_CONTEXT AsyncContext *resumeContext,
                                  TaskContinuationFunction *resumeFunction,
                                  ExecutorRef newExecutor) {
  auto trackingInfo = ExecutorTrackingInfo::current();
  auto currentExecutor =
    (trackingInfo ? trackingInfo->getActiveExecutor()
                  : ExecutorRef::generic());
  SWIFT_TASK_DEBUG_LOG("trying to switch from executor %p to %p",
                       currentExecutor.getIdentity(),
                       newExecutor.getIdentity());

  // If the current executor is compatible with running the new executor,
  // we can just immediately continue running with the resume function
  // we were passed in.
  if (!currentExecutor.mustSwitchToRun(newExecutor)) {
    return resumeFunction(resumeContext); // 'return' forces tail call
  }

  auto task = swift_task_getCurrent();
  assert(task && "no current task!");

  // Park the task for simplicity instead of trying to thread the
  // initial resumption information into everything below.
  task->ResumeContext = resumeContext;
  task->ResumeTask = resumeFunction;

  // Okay, we semantically need to switch.
  auto runner = RunningJobInfo::forOther(task->getPriority());

  // If the current executor can give up its thread, and the new executor
  // can take over a thread, try to do so; but don't do this if we've
  // been asked to yield the thread.
  if (canGiveUpThreadForSwitch(trackingInfo, currentExecutor) &&
      !shouldYieldThread() &&
      tryAssumeThreadForSwitch(newExecutor, runner)) {
    SWIFT_TASK_DEBUG_LOG(
        "switch succeeded, task %p assumed thread for executor %p", task,
        newExecutor.getIdentity());
    giveUpThreadForSwitch(currentExecutor, runner);
    // 'return' forces tail call
    return runOnAssumedThread(task, newExecutor, trackingInfo, runner);
  }

  // Otherwise, just asynchronously enqueue the task on the given
  // executor.
  SWIFT_TASK_DEBUG_LOG("switch failed, task %p enqueued on executor %p", task,
                       newExecutor.getIdentity());
  task->flagAsSuspended();
  _swift_task_clearCurrent();
  swift_task_enqueue(task, newExecutor);
}

/*****************************************************************************/
/************************* GENERIC ACTOR INTERFACES **************************/
/*****************************************************************************/

// Implemented in Swift to avoid some annoying hard-coding about
// SerialExecutor's protocol witness table.  We could inline this
// with effort, though.
extern "C" SWIFT_CC(swift)
void _swift_task_enqueueOnExecutor(Job *job, HeapObject *executor,
                                   const Metadata *selfType,
                                   const SerialExecutorWitnessTable *wtable);

SWIFT_CC(swift)
static void swift_task_enqueueImpl(Job *job, ExecutorRef executor) {
  SWIFT_TASK_DEBUG_LOG("enqueue job %p on executor %p", job,
                       executor.getIdentity());

  assert(job && "no job provided");

  _swift_tsan_release(job);

  if (executor.isGeneric())
    return swift_task_enqueueGlobal(job);

  if (executor.isDefaultActor())
    return asImpl(executor.getDefaultActor())->enqueue(job);

  auto wtable = executor.getSerialExecutorWitnessTable();
  auto executorObject = executor.getIdentity();
  auto executorType = swift_getObjectType(executorObject);
  _swift_task_enqueueOnExecutor(job, executorObject, executorType, wtable);
}

#define OVERRIDE_ACTOR COMPATIBILITY_OVERRIDE
#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH


/*****************************************************************************/
/***************************** DISTRIBUTED ACTOR *****************************/
/*****************************************************************************/

OpaqueValue*
swift::swift_distributedActor_remote_initialize(const Metadata *actorType) {
  auto *classMetadata = actorType->getClassObject();

  // TODO(distributed): make this allocation smaller
  // ==== Allocate the memory for the remote instance
  HeapObject *alloc = swift_allocObject(classMetadata,
                                        classMetadata->getInstanceSize(),
                                        classMetadata->getInstanceAlignMask());

  // TODO: remove this memset eventually, today we only do this to not have
  //       to modify the destructor logic, as releasing zeroes is no-op
  memset(alloc + 1, 0, classMetadata->getInstanceSize() - sizeof(HeapObject));

  // TODO(distributed): a remote one does not have to have the "real"
  //  default actor body, e.g. we don't need an executor at all; so
  //  we can allocate more efficiently and only share the flags/status field
  //  between the both memory representations
  // --- Currently we ride on the DefaultActorImpl to reuse the memory layout
  // of the flags etc. So initialize the default actor into the allocation.
  auto actor = asImpl(reinterpret_cast<DefaultActor*>(alloc));
  actor->initialize(/*remote*/true);
  assert(actor->isDistributedRemote());

  return reinterpret_cast<OpaqueValue*>(actor);
}

bool swift::swift_distributed_actor_is_remote(DefaultActor *_actor) {
  return asImpl(_actor)->isDistributedRemote();
}

bool DefaultActorImpl::isDistributedRemote() {
  auto state = CurrentState.load(std::memory_order_relaxed);
  return state.Flags.isDistributedRemote();
}
