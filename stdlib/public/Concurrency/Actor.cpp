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

#include "swift/Runtime/Concurrency.h"
#include <atomic>
#include <new>

#include "../CompatibilityOverride/CompatibilityOverride.h"
#include "swift/ABI/Actor.h"
#include "swift/ABI/Task.h"
#include "TaskPrivate.h"
#include "swift/Basic/HeaderFooterLayout.h"
#include "swift/Basic/PriorityQueue.h"
#include "swift/Concurrency/Actor.h"
#include "swift/Runtime/AccessibleFunction.h"
#include "swift/Runtime/Atomic.h"
#include "swift/Runtime/Bincompat.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/DispatchShims.h"
#include "swift/Runtime/EnvironmentVariables.h"
#include "swift/Threading/Mutex.h"
#include "swift/Threading/Once.h"
#include "swift/Threading/Thread.h"
#include "swift/Threading/ThreadLocalStorage.h"
#ifdef SWIFT_CONCURRENCY_BACK_DEPLOYMENT
// All platforms where we care about back deployment have a known
// configurations.
#define HAVE_PTHREAD_H 1
#define SWIFT_OBJC_INTEROP 1
#endif
#include "llvm/ADT/PointerIntPair.h"
#include "TaskPrivate.h"
#include "VoucherSupport.h"

#if SWIFT_CONCURRENCY_ENABLE_DISPATCH
#include <dispatch/dispatch.h>
#endif

#if SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
#define SWIFT_CONCURRENCY_ACTORS_AS_LOCKS 1
#else
#define SWIFT_CONCURRENCY_ACTORS_AS_LOCKS 0
#endif

#if SWIFT_STDLIB_HAS_ASL
#include <asl.h>
#elif defined(__ANDROID__)
#include <android/log.h>
#endif

#if defined(__ELF__)
#include <unwind.h>
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
  // return dispatch_swift_job_should_yield();
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
  SerialExecutorRef ActiveExecutor = SerialExecutorRef::generic();

  /// The current task executor, if present, otherwise `undefined`.
  /// The task executor should be used to execute code when the active executor
  /// is `generic`.
  TaskExecutorRef TaskExecutor = TaskExecutorRef::undefined();

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
  void enterAndShadow(SerialExecutorRef currentExecutor,
                      TaskExecutorRef taskExecutor) {
    ActiveExecutor = currentExecutor;
    TaskExecutor = taskExecutor;
    SavedInfo = ActiveInfoInThread.get();
    ActiveInfoInThread.set(this);
  }

  void swapToJob(Job *job) { voucherManager.swapToJob(job); }

  void restoreVoucher(AsyncTask *task) { voucherManager.restoreVoucher(task); }

  SerialExecutorRef getActiveExecutor() const { return ActiveExecutor; }
  void setActiveExecutor(SerialExecutorRef newExecutor) {
    ActiveExecutor = newExecutor;
  }

  TaskExecutorRef getTaskExecutor() const { return TaskExecutor; }

  void setTaskExecutor(TaskExecutorRef newExecutor) {
    TaskExecutor = newExecutor;
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
  static AsyncTask *swap(AsyncTask *newTask) {
    return Value.swap(newTask);
  }
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
    auto oldTask = ActiveTask::swap(task);

    // Update the task status to say that it's running on the
    // current thread.  If the task suspends somewhere, it should
    // update the task status appropriately; we don't need to update
    // it afterwards.
    task->flagAsRunning();

    auto traceHandle = concurrency::trace::job_run_begin(job);
    task->runInFullyEstablishedContext();
    concurrency::trace::job_run_end(traceHandle);

    assert(ActiveTask::get() == nullptr &&
           "active task wasn't cleared before suspending?");
    if (oldTask) ActiveTask::set(oldTask);
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
  return ActiveTask::swap(nullptr);
}

AsyncTask *swift::_swift_task_setCurrent(AsyncTask *new_task) {
  return ActiveTask::swap(new_task);
}

SWIFT_CC(swift)
static SerialExecutorRef swift_task_getCurrentExecutorImpl() {
  auto currentTracking = ExecutorTrackingInfo::current();
  auto result = (currentTracking ? currentTracking->getActiveExecutor()
                                 : SerialExecutorRef::generic());
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
#elif SWIFT_CONCURRENCY_TASK_TO_THREAD_MODEL
  return JobPriority::Unspecified;
#elif defined(__APPLE__) && SWIFT_CONCURRENCY_ENABLE_DISPATCH
  return static_cast<JobPriority>(qos_class_self());
#else
  if (isExecutingOnMainThread())
    return JobPriority::UserInitiated;

  return JobPriority::Unspecified;
#endif
}

// Implemented in Swift to avoid some annoying hard-coding about
// SerialExecutor's protocol witness table.  We could inline this
// with effort, though.
extern "C" SWIFT_CC(swift)
bool _task_serialExecutor_isSameExclusiveExecutionContext(
    HeapObject *currentExecutor, HeapObject *executor,
    const Metadata *selfType,
    const SerialExecutorWitnessTable *wtable);

// We currently still support "legacy mode" in which isCurrentExecutor is NOT
// allowed to crash, because it is used to power "log warnings" data race
// detector. This mode is going away in Swift 6, but until then we allow this.
// This override exists primarily to be able to test both code-paths.
enum IsCurrentExecutorCheckMode: unsigned {
  /// The default mode when an app was compiled against "new" enough SDK.
  /// It allows crashing in isCurrentExecutor, and calls into `checkIsolated`.
  Swift6_UseCheckIsolated_AllowCrash,
  /// Legacy mode; Primarily to support old applications which used data race
  /// detector with "warning" mode, which is no longer supported. When such app
  /// is re-compiled against a new SDK, it will see crashes in what was
  /// previously warnings; however, until until recompiled, warnings will be
  /// used, and `checkIsolated` cannot be invoked.
  Legacy_NoCheckIsolated_NonCrashing,
};
static IsCurrentExecutorCheckMode isCurrentExecutorMode =
    Swift6_UseCheckIsolated_AllowCrash;

// Shimming call to Swift runtime because Swift Embedded does not have
// these symbols defined.
bool __swift_bincompat_useLegacyNonCrashingExecutorChecks() {
#if !SWIFT_CONCURRENCY_EMBEDDED
  return swift::runtime::bincompat::
      swift_bincompat_useLegacyNonCrashingExecutorChecks();
#else
  return false;
#endif
}

// Shimming call to Swift runtime because Swift Embedded does not have
// these symbols defined.
const char *__swift_runtime_env_useLegacyNonCrashingExecutorChecks() {
  // Potentially, override the platform detected mode, primarily used in tests.
#if SWIFT_STDLIB_HAS_ENVIRON && !SWIFT_CONCURRENCY_EMBEDDED
  return swift::runtime::environment::
      concurrencyIsCurrentExecutorLegacyModeOverride();
#else
  return nullptr;
#endif
}

// Done this way because of the interaction with the initial value of
// 'unexpectedExecutorLogLevel'
bool swift_bincompat_useLegacyNonCrashingExecutorChecks() {
  bool legacyMode = __swift_bincompat_useLegacyNonCrashingExecutorChecks();

  // Potentially, override the platform detected mode, primarily used in tests.
  if (const char *modeStr =
          __swift_runtime_env_useLegacyNonCrashingExecutorChecks()) {
    if (strcmp(modeStr, "nocrash") == 0 ||
        strcmp(modeStr, "legacy") == 0) {
      return true;
    } else if (strcmp(modeStr, "crash") == 0 ||
               strcmp(modeStr, "swift6") == 0) {
      return false; // don't use the legacy mode
    } // else, just use the platform detected mode
  } // no override, use the default mode

  return legacyMode;
}

// Check override of executor checking mode.
static void checkIsCurrentExecutorMode(void *context) {
  bool useLegacyMode =
      swift_bincompat_useLegacyNonCrashingExecutorChecks();
  isCurrentExecutorMode = useLegacyMode ? Legacy_NoCheckIsolated_NonCrashing
                                        : Swift6_UseCheckIsolated_AllowCrash;
}

SWIFT_CC(swift)
static bool swift_task_isCurrentExecutorImpl(SerialExecutorRef expectedExecutor) {
  auto current = ExecutorTrackingInfo::current();

  // To support old applications on apple platforms which assumed this call
  // does not crash, try to use a more compatible mode for those apps.
  //
  // We only allow returning `false` directly from this function when operating
  // in 'Legacy_NoCheckIsolated_NonCrashing' mode. If allowing crashes, we
  // instead must call into 'checkIsolated' or crash directly.
  //
  // Whenever we confirm an executor equality, we can return true, in any mode.
  static swift::once_t checkModeToken;
  swift::once(checkModeToken, checkIsCurrentExecutorMode, nullptr);

  if (!current) {
    // We have no current executor, i.e. we are running "outside" of Swift
    // Concurrency. We could still be running on a thread/queue owned by
    // the expected executor however, so we need to try a bit harder before
    // we fail.

    // Special handling the main executor by detecting the main thread.
    if (expectedExecutor.isMainExecutor() && isExecutingOnMainThread()) {
      return true;
    }

    // We cannot use 'complexEquality' as it requires two executor instances,
    // and we do not have a 'current' executor here.

    // Otherwise, as last resort, let the expected executor check using
    // external means, as it may "know" this thread is managed by it etc.
    if (isCurrentExecutorMode == Swift6_UseCheckIsolated_AllowCrash) {
      swift_task_checkIsolated(expectedExecutor); // will crash if not same context

      // checkIsolated did not crash, so we are on the right executor, after all!
      return true;
    }

    assert(isCurrentExecutorMode == Legacy_NoCheckIsolated_NonCrashing);
    return false;
  }

  SerialExecutorRef currentExecutor = current->getActiveExecutor();

  // Fast-path: the executor is exactly the same memory address;
  // We assume executors do not come-and-go appearing under the same address,
  // and treat pointer equality of executors as good enough to assume the executor.
  if (currentExecutor == expectedExecutor) {
    return true;
  }

  // Fast-path, specialize the common case of comparing two main executors.
  if (currentExecutor.isMainExecutor() && expectedExecutor.isMainExecutor()) {
    return true;
  }

  // Only in legacy mode:
  // We check if the current xor expected executor are the main executor.
  // If so only one of them is, we know that WITHOUT 'checkIsolated' or invoking
  // 'dispatch_assert_queue' we cannot be truly sure the expected/current truly
  // are "on the same queue". There exists no non-crashing API to check this,
  // so we PESSIMISTICALLY return false here.
  //
  // In Swift6 mode:
  // We don't do this naive check, because we'll fall back to
  // `expected.checkIsolated()` which, if it is the main executor, will invoke
  // the crashing 'dispatch_assert_queue(main queue)' which will either crash
  // or confirm we actually are on the main queue; or the custom expected
  // executor has a chance to implement a similar queue check.
  if (isCurrentExecutorMode == Legacy_NoCheckIsolated_NonCrashing) {
    if ((expectedExecutor.isMainExecutor() && !currentExecutor.isMainExecutor()) ||
        (!expectedExecutor.isMainExecutor() && currentExecutor.isMainExecutor())) {
      return false;
    }
  }

  // Complex equality means that if two executors of the same type have some
  // special logic to check if they are "actually the same".
  if (expectedExecutor.isComplexEquality()) {
    if (currentExecutor.getIdentity() &&
        expectedExecutor.getIdentity() &&
        swift_compareWitnessTables(
            reinterpret_cast<const WitnessTable *>(
                currentExecutor.getSerialExecutorWitnessTable()),
            reinterpret_cast<const WitnessTable *>(
                expectedExecutor.getSerialExecutorWitnessTable()))) {

      auto isSameExclusiveExecutionContextResult =
          _task_serialExecutor_isSameExclusiveExecutionContext(
              currentExecutor.getIdentity(), expectedExecutor.getIdentity(),
              swift_getObjectType(currentExecutor.getIdentity()),
              expectedExecutor.getSerialExecutorWitnessTable());

      // if the 'isSameExclusiveExecutionContext' returned true we trust
      // it and return; if it was false, we need to give checkIsolated another
      // chance to check.
      if (isSameExclusiveExecutionContextResult) {
        return true;
      } // else, we must give 'checkIsolated' a last chance to verify isolation
    }
  }

  // This provides a last-resort check by giving the expected SerialExecutor the
  // chance to perform a check using some external knowledge if perhaps we are,
  // after all, on this executor, but the Swift concurrency runtime was just not
  // aware.
  //
  // Unless handled in `swift_task_checkIsolated` directly, this should call
  // through to the executor's `SerialExecutor.checkIsolated`.
  //
  // This call is expected to CRASH, unless it has some way of proving that
  // we're actually indeed running on this executor.
  //
  // For example, when running outside of Swift concurrency tasks, but trying to
  // `MainActor.assumeIsolated` while executing DIRECTLY on the main dispatch
  // queue, this allows Dispatch to check for this using its own tracking
  // mechanism, and thus allow the assumeIsolated to work correctly, even though
  // the code executing is not even running inside a Task.
  //
  // Note that this only works because the closure in assumeIsolated is
  // synchronous, and will not cause suspensions, as that would require the
  // presence of a Task.
  if (isCurrentExecutorMode == Swift6_UseCheckIsolated_AllowCrash) {
    swift_task_checkIsolated(expectedExecutor); // will crash if not same context

    // The checkIsolated call did not crash, so we are on the right executor.
    return true;
  }

  // In the end, since 'checkIsolated' could not be used, so we must assume
  // that the executors are not the same context.
  assert(isCurrentExecutorMode == Legacy_NoCheckIsolated_NonCrashing);
  return false;
}

/// Logging level for unexpected executors:
/// 0 - no logging -- will be IGNORED when Swift6 mode of isCurrentExecutor is used
/// 1 - warn on each instance -- will be IGNORED when Swift6 mode of isCurrentExecutor is used
/// 2 - fatal error
///
/// NOTE: The default behavior on Apple platforms depends on the SDK version
/// an application was linked to. Since Swift 6 the default is to crash,
/// and the logging behavior is no longer available.
static unsigned unexpectedExecutorLogLevel =
    swift_bincompat_useLegacyNonCrashingExecutorChecks()
        ? 1 // legacy apps default to the logging mode, and cannot use `checkIsolated`
        : 2; // new apps will only crash upon concurrency violations, and will call into `checkIsolated`

static void checkUnexpectedExecutorLogLevel(void *context) {
#if SWIFT_STDLIB_HAS_ENVIRON
  const char *levelStr = getenv("SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL");
  if (!levelStr)
    return;

  long level = strtol(levelStr, nullptr, 0);
  if (level >= 0 && level < 3) {
    if (swift_bincompat_useLegacyNonCrashingExecutorChecks()) {
      // legacy mode permits doing nothing or just logging, since the method
      // used to perform the check itself is not going to crash:
      unexpectedExecutorLogLevel = level;
    } else {
      // We are in swift6/crash mode of isCurrentExecutor which means that
      // rather than returning false, that method will always CRASH when an
      // executor mismatch is discovered.
      //
      // Thus, for clarity, we set this mode also to crashing, as runtime should
      // not expect to be able to get any logging or ignoring done. In practice,
      // the crash would happen before logging or "ignoring", but this should
      // help avoid confusing situations like "I thought it should log" when
      // debugging the runtime.
      unexpectedExecutorLogLevel = 2;
    }
  }
#endif // SWIFT_STDLIB_HAS_ENVIRON
}

SWIFT_CC(swift)
void swift::swift_task_reportUnexpectedExecutor(
    const unsigned char *file, uintptr_t fileLength, bool fileIsASCII,
    uintptr_t line, SerialExecutorRef executor) {
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
  fputs(message, stderr);
  fflush(stderr);
#endif
#if SWIFT_STDLIB_HAS_ASL
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  asl_log(nullptr, nullptr, ASL_LEVEL_ERR, "%s", message);
#pragma clang diagnostic pop
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

#if !SWIFT_CONCURRENCY_ACTORS_AS_LOCKS
/// A job to process a default actor that's allocated separately from
/// the actor.
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

/// Similar to the ActiveTaskStatus, this denotes the ActiveActorState for
/// tracking the atomic state of the actor
///
/// The runtime needs to track the following state about the actor within the
/// same atomic:
///
/// * The current status of the actor - scheduled, running, idle, etc
/// * The current maximum priority of the jobs enqueued in the actor
/// * The identity of the thread currently holding the actor lock
/// * Pointer to list of jobs enqueued in actor
///
/// It is important for all of this information to be in the same atomic so that
/// when the actor's state changes, the information is visible to all threads
/// that may be modifying the actor, allowing the algorithm to eventually
/// converge.
///
/// In order to provide priority escalation support with actors, deeper
/// integration is required with the OS in order to have the intended side
/// effects. On Darwin, Swift Concurrency Tasks runs on dispatch's queues. As
/// such, we need to use an encoding of thread identity vended by libdispatch
/// called dispatch_lock_t, and a futex-style dispatch API in order to escalate
/// the priority of a thread. Henceforth, the dispatch_lock_t tracked in the
/// ActiveActorStatus will be called the DrainLock.
///
/// When a thread starts running on an actor, it's identity is recorded in the
/// ActiveActorStatus. This way, if a higher priority job is enqueued behind the
/// thread executing the actor, we can escalate the thread holding the actor
/// lock, thereby resolving the priority inversion. When a thread hops off of
/// the actor, any priority boosts it may have gotten as a result of contention
/// on the actor, is removed as well.
///
/// In order to keep the entire ActiveActorStatus size to 2 words, the thread
/// identity is only tracked on platforms which can support 128 bit atomic
/// operations. The ActiveActorStatus's layout has thus been changed to have the
/// following layout depending on the system configuration supported:
///
/// 32 bit systems with SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION=1
///
///          Flags               Drain Lock               Unused Job*
/// |----------------------|----------------------|----------------------|-------------------|
///          32 bits                32 bits                32 bits              32 bits
///
/// 64 bit systems with SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION=1
///
///         Flags                Drain Lock             Job*
/// |----------------------|-------------------|----------------------|
///          32 bits                32 bits             64 bits
///
/// 32 bit systems with SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION=0
///
///          Flags                  Job*
/// |----------------------|----------------------|
///          32 bits                32 bits
//
/// 64 bit systems with SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION=0
///
///         Flags                  Unused                 Job*
/// |----------------------|----------------------|---------------------|
///         32 bits                 32 bits               64 bits
///
/// Size requirements:
///     On 64 bit systems or if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION=1,
///     the field is 16 bytes long.
///
///     Otherwise, it is 8 bytes long.
///
/// Alignment requirements:
///     On 64 bit systems or if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION=1,
///     this 16-byte field needs to be 16 byte aligned to be able to do aligned
///     atomic stores field.
///
///     On all other systems, it needs to be 8 byte aligned for the atomic
///     stores.
///
///     As a result of varying alignment needs, we've marked the class as
///     needing 2-word alignment but on arm64_32 with
///     SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION=1, 16 byte alignment is
///     achieved through careful arrangement of the storage for this in the
///     DefaultActorImpl. The additional alignment requirements are
///     enforced by static asserts below.
class alignas(sizeof(void *) * 2) ActiveActorStatus {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION && SWIFT_POINTER_IS_4_BYTES
  uint32_t Flags;
  dispatch_lock_t DrainLock;
  LLVM_ATTRIBUTE_UNUSED uint32_t Unused = {};
#elif SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION && SWIFT_POINTER_IS_8_BYTES
  uint32_t Flags;
  dispatch_lock_t DrainLock;
#elif !SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION && SWIFT_POINTER_IS_4_BYTES
  uint32_t Flags;
#else /* !SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION && SWIFT_POINTER_IS_8_BYTES */
  uint32_t Flags;
  LLVM_ATTRIBUTE_UNUSED uint32_t Unused = {};
#endif
  Job *FirstJob;

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
  ActiveActorStatus(uint32_t flags, dispatch_lock_t drainLockValue, Job *job)
      : Flags(flags), DrainLock(drainLockValue), FirstJob(job) {}
#else
  ActiveActorStatus(uint32_t flags, Job *job) : Flags(flags), FirstJob(job) {}
#endif

  uint32_t getActorState() const {
    return Flags & concurrency::ActorFlagConstants::ActorStateMask;
  }
  uint32_t setActorState(uint32_t state) const {
    return (Flags & ~concurrency::ActorFlagConstants::ActorStateMask) | state;
  }

public:
  bool operator==(ActiveActorStatus other) const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return (Flags == other.Flags) && (DrainLock == other.DrainLock) && (FirstJob == other.FirstJob);
#else
    return (Flags == other.Flags) && (FirstJob == other.FirstJob);
#endif
  }

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
  constexpr ActiveActorStatus()
      : Flags(), DrainLock(DLOCK_OWNER_NULL), FirstJob(nullptr) {}
#else
  constexpr ActiveActorStatus() : Flags(), FirstJob(nullptr) {}
#endif

  bool isIdle() const {
    bool isIdle = (getActorState() == concurrency::ActorFlagConstants::Idle);
    if (isIdle) {
      assert(!FirstJob);
    }
    return isIdle;
  }
  ActiveActorStatus withIdle() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveActorStatus(
        setActorState(concurrency::ActorFlagConstants::Idle), DLOCK_OWNER_NULL,
        FirstJob);
#else
    return ActiveActorStatus(
        setActorState(concurrency::ActorFlagConstants::Idle), FirstJob);
#endif
  }

  bool isAnyRunning() const {
    uint32_t state = getActorState();
    return (state == concurrency::ActorFlagConstants::Running) ||
           (state ==
            concurrency::ActorFlagConstants::Zombie_ReadyForDeallocation);
  }
  bool isRunning() const {
    return getActorState() == concurrency::ActorFlagConstants::Running;
  }
  ActiveActorStatus withRunning() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveActorStatus(
        setActorState(concurrency::ActorFlagConstants::Running),
        dispatch_lock_value_for_self(), FirstJob);
#else
    return ActiveActorStatus(
        setActorState(concurrency::ActorFlagConstants::Running), FirstJob);
#endif
  }

  bool isScheduled() const {
    return getActorState() == concurrency::ActorFlagConstants::Scheduled;
  }

  ActiveActorStatus withScheduled() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveActorStatus(
        setActorState(concurrency::ActorFlagConstants::Scheduled),
        DLOCK_OWNER_NULL, FirstJob);
#else
    return ActiveActorStatus(
        setActorState(concurrency::ActorFlagConstants::Scheduled), FirstJob);
#endif
  }

  bool isZombie_ReadyForDeallocation() const {
    return getActorState() ==
           concurrency::ActorFlagConstants::Zombie_ReadyForDeallocation;
  }
  ActiveActorStatus withZombie_ReadyForDeallocation() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    assert(dispatch_lock_owner(DrainLock) != DLOCK_OWNER_NULL);
    return ActiveActorStatus(
        setActorState(
            concurrency::ActorFlagConstants::Zombie_ReadyForDeallocation),
        DrainLock, FirstJob);
#else
    return ActiveActorStatus(
        setActorState(
            concurrency::ActorFlagConstants::Zombie_ReadyForDeallocation),
        FirstJob);
#endif
  }

  JobPriority getMaxPriority() const {
    return (
        JobPriority)((Flags & concurrency::ActorFlagConstants::PriorityMask) >>
                     concurrency::ActorFlagConstants::PriorityShift);
  }
  ActiveActorStatus withNewPriority(JobPriority priority) const {
    uint32_t flags =
        Flags & ~concurrency::ActorFlagConstants::PriorityAndOverrideMask;
    flags |=
        (uint32_t(priority) << concurrency::ActorFlagConstants::PriorityShift);
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveActorStatus(flags, DrainLock, FirstJob);
#else
    return ActiveActorStatus(flags, FirstJob);
#endif
  }
  ActiveActorStatus resetPriority() const {
    return withNewPriority(JobPriority::Unspecified);
  }

  bool isMaxPriorityEscalated() const {
    return Flags & concurrency::ActorFlagConstants::IsPriorityEscalated;
  }
  ActiveActorStatus withEscalatedPriority(JobPriority priority) const {
    JobPriority currentPriority =
        JobPriority((Flags & concurrency::ActorFlagConstants::PriorityMask) >>
                    concurrency::ActorFlagConstants::PriorityShift);
    (void)currentPriority;
    assert(priority > currentPriority);

    uint32_t flags =
        (Flags & ~concurrency::ActorFlagConstants::PriorityMask) |
        (uint32_t(priority) << concurrency::ActorFlagConstants::PriorityShift);
    flags |= concurrency::ActorFlagConstants::IsPriorityEscalated;
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveActorStatus(flags, DrainLock, FirstJob);
#else
    return ActiveActorStatus(flags, FirstJob);
#endif
  }
  ActiveActorStatus withoutEscalatedPriority() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveActorStatus(
        Flags & ~concurrency::ActorFlagConstants::IsPriorityEscalated,
        DrainLock, FirstJob);
#else
    return ActiveActorStatus(
        Flags & ~concurrency::ActorFlagConstants::IsPriorityEscalated,
        FirstJob);
#endif
  }

  Job *getFirstUnprioritisedJob() const { return FirstJob; }
  ActiveActorStatus withFirstUnprioritisedJob(Job *firstJob) const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return ActiveActorStatus(Flags, DrainLock, firstJob);
#else
    return ActiveActorStatus(Flags, firstJob);
#endif
  }

  uint32_t getOpaqueFlags() const {
    return Flags;
  }

  uint32_t currentDrainer() const {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    return dispatch_lock_owner(DrainLock);
#else
    return 0;
#endif
  }

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
  static size_t drainLockOffset() {
    return offsetof(ActiveActorStatus, DrainLock);
  }
#endif

  void traceStateChanged(HeapObject *actor, bool distributedActorIsRemote) {
    // Convert our state to a consistent raw value. These values currently match
    // the enum values, but this explicit conversion provides room for change.
    uint8_t traceState = 255;
    switch (getActorState()) {
    case concurrency::ActorFlagConstants::Idle:
      traceState = 0;
      break;
    case concurrency::ActorFlagConstants::Scheduled:
      traceState = 1;
      break;
    case concurrency::ActorFlagConstants::Running:
      traceState = 2;
      break;
    case concurrency::ActorFlagConstants::Zombie_ReadyForDeallocation:
      traceState = 3;
      break;
    }
    concurrency::trace::actor_state_changed(
        actor, getFirstUnprioritisedJob(), traceState, distributedActorIsRemote,
        isMaxPriorityEscalated(), static_cast<uint8_t>(getMaxPriority()));
  }
};

#if !SWIFT_CONCURRENCY_ACTORS_AS_LOCKS

/// Given that a job is enqueued normally on a default actor, get/set
/// the next job in the actor's queue.
static Job *getNextJob(Job *job) {
  return *reinterpret_cast<Job **>(job->SchedulerPrivate);
}
static void setNextJob(Job *job, Job *next) {
  *reinterpret_cast<Job **>(job->SchedulerPrivate) = next;
}

struct JobQueueTraits {
  static Job *getNext(Job *job) { return getNextJob(job); }
  static void setNext(Job *job, Job *next) { setNextJob(job, next); }

  enum { prioritiesCount = PriorityBucketCount };
  static int getPriorityIndex(Job *job) {
    return getPriorityBucketIndex(job->getPriority());
  }
};

#endif

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION && SWIFT_POINTER_IS_4_BYTES
#define ACTIVE_ACTOR_STATUS_SIZE (4 * (sizeof(uintptr_t)))
#else
#define ACTIVE_ACTOR_STATUS_SIZE (2 * (sizeof(uintptr_t)))
#endif
static_assert(sizeof(ActiveActorStatus) == ACTIVE_ACTOR_STATUS_SIZE,
  "ActiveActorStatus is of incorrect size");
#endif /* !SWIFT_CONCURRENCY_ACTORS_AS_LOCKS */

class DefaultActorImplHeader : public HeapObject {
protected:
  // TODO (rokhinip): Make this a flagset
  bool isDistributedRemoteActor;
#if SWIFT_CONCURRENCY_ACTORS_AS_LOCKS
  // If actors are locks, we don't need to maintain any extra bookkeeping in the
  // ActiveActorStatus since all threads which are contending will block
  // synchronously, no job queue is needed and the lock will handle all priority
  // escalation logic
  Mutex drainLock;
#else
  // Note: There is some padding that is added here by the compiler in order to
  // enforce alignment. This is space that is available for us to use in
  // the future
  alignas(sizeof(ActiveActorStatus)) char StatusStorage[sizeof(ActiveActorStatus)];
#endif
};

// All the fields accessed under the actor's lock should be moved
// to the end of the default-actor reservation to minimize false sharing.
// The memory following the DefaultActorImpl object are the stored properties of
// the actor, which are all accessed only by the current processing thread.
class DefaultActorImplFooter {
protected:
#if !SWIFT_CONCURRENCY_ACTORS_AS_LOCKS
  using PriorityQueue = swift::PriorityQueue<Job *, JobQueueTraits>;

  // When enqueued, jobs are atomically added to a linked list with the head
  // stored inside ActiveActorStatus. This list contains jobs in the LIFO order
  // regardless of their priorities.
  //
  // When the processing thread sees new incoming jobs in
  // ActiveActorStatus, it reverses them and inserts them into
  // prioritizedJobs in the appropriate priority bucket.
  //
  PriorityQueue prioritizedJobs;
#endif
};

// We can't use sizeof(DefaultActor) since the alignment requirement on the
// default actor means that we have some padding added when calculating
// sizeof(DefaultActor). However that padding isn't available for us to use
// in DefaultActorImpl.
enum {
  DefaultActorSize = sizeof(void *) * NumWords_DefaultActor + sizeof(HeapObject)
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
/// - Let N be the number of active processing jobs for the actor. N may be > 1
///   because we may have stealers for actors if we have to escalate the max
///   priority of the actor
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
/// (1) When we enqueue the first job on an actor, then R becomes 1, and
///     we must create a processing job so that N >= R.  We do not need to
///     retain the actor.
/// (2) When we create an extra job to process an actor (e.g. because of
///     priority overrides), N increases but R remains the same.  We must
///     retain the actor - ie stealer for actor has a reference on the actor
/// (3) When we start running an actor, our job definitively becomes the
///     owning job, but neither N nor R changes.  We do not need to retain
///     the actor.
/// (4) When we go to start running an actor and for whatever reason we
///     don't actually do so, we are eliminating an extra processing job,
///     and so N decreases but R remains the same.  We must release the
///     actor.
/// (5) When we are running an actor and give it up, and there are no
///     remaining jobs on it, then R becomes 0 and N decreases by 1.
///     We do not need to release the actor.
/// (6) When we are running an actor and give it up, and there are jobs
///     remaining on it, then R remains 1 but N is decreasing by 1.
///     We must either release the actor or create a new processing job
///     for it to maintain the balance.
///
/// The current behaviour of actors is such that we only have a single
/// processing job for an actor at a given time. Stealers jobs support does not
/// exist yet. As a result, the subset of rules that currently apply
/// are (1), (3), (5), (6).
class DefaultActorImpl
    : public HeaderFooterLayout<DefaultActorImplHeader, DefaultActorImplFooter,
                                DefaultActorSize> {
public:
  /// Properly construct an actor, except for the heap header.
  void initialize(bool isDistributedRemote = false) {
    this->isDistributedRemoteActor = isDistributedRemote;
#if SWIFT_CONCURRENCY_ACTORS_AS_LOCKS
    new (&this->drainLock) Mutex();
#else
   _status().store(ActiveActorStatus(), std::memory_order_relaxed);
   new (&this->prioritizedJobs) PriorityQueue();
#endif
    SWIFT_TASK_DEBUG_LOG("Creating default actor %p", this);
    concurrency::trace::actor_create(this);
  }

  /// Properly destruct an actor, except for the heap header.
  void destroy();

  /// Properly respond to the last release of a default actor.  Note
  /// that the actor will have been completely torn down by the time
  /// we reach this point.
  void deallocate();

  /// Try to lock the actor, if it is already running or scheduled, fail
  bool tryLock(bool asDrainer);

  /// Unlock an actor
  bool unlock(bool forceUnlock);

#if !SWIFT_CONCURRENCY_ACTORS_AS_LOCKS
  /// Enqueue a job onto the actor.
  void enqueue(Job *job, JobPriority priority);

  /// Enqueue a stealer for the given task since it has been escalated to the
  /// new priority
  void enqueueStealer(Job *job, JobPriority priority);

  /// Dequeues one job from `prioritisedJobs`.
  /// The calling thread must be holding the actor lock while calling this
  Job *drainOne();

  /// Atomically claims incoming jobs from ActiveActorStatus, and calls `handleUnprioritizedJobs()`.
  /// Called with actor lock held on current thread.
  void processIncomingQueue();
#endif

  /// Check if the actor is actually a distributed *remote* actor.
  ///
  /// Note that a distributed *local* actor instance is the same as any other
  /// ordinary default (local) actor, and no special handling is needed for them.
  bool isDistributedRemote();

#if !SWIFT_CONCURRENCY_ACTORS_AS_LOCKS
  swift::atomic<ActiveActorStatus> &_status() {
    return reinterpret_cast<swift::atomic<ActiveActorStatus> &>(this->StatusStorage);
  }

  const swift::atomic<ActiveActorStatus> &_status() const {
    return reinterpret_cast<const swift::atomic<ActiveActorStatus> &>(this->StatusStorage);
  }

  // Only for static assert use below, not for actual use otherwise
  static constexpr size_t offsetOfActiveActorStatus() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
    return offsetof(DefaultActorImpl, StatusStorage);
#pragma clang diagnostic pop
  }
#endif /* !SWIFT_CONCURRENCY_ACTORS_AS_LOCKS */

private:
#if !SWIFT_CONCURRENCY_ACTORS_AS_LOCKS
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
  dispatch_lock_t *drainLockAddr();
#endif
  /// Schedule a processing job.
  /// It can be done when actor transitions from Idle to Scheduled or
  /// when actor gets a priority override and we schedule a stealer.
  void scheduleActorProcessJob(JobPriority priority);

  /// Processes claimed incoming jobs into `prioritizedJobs`.
  /// Incoming jobs are of mixed priorities and in LIFO order.
  /// Called with actor lock held on current thread.
  void handleUnprioritizedJobs(Job *head);
#endif /* !SWIFT_CONCURRENCY_ACTORS_AS_LOCKS */

  void deallocateUnconditional();
};

class NonDefaultDistributedActorImpl : public HeapObject {
  // TODO (rokhinip): Make this a flagset
  bool isDistributedRemoteActor;

public:
  /// Properly construct an actor, except for the heap header.
  void initialize(bool isDistributedRemote = false) {
    this->isDistributedRemoteActor = isDistributedRemote;
    SWIFT_TASK_DEBUG_LOG("Creating non-default distributed actor %p", this);
    concurrency::trace::actor_create(this);
  }

  /// Properly destruct an actor, except for the heap header.
  void destroy() {
    // empty
  }

  /// Properly respond to the last release of a default actor.  Note
  /// that the actor will have been completely torn down by the time
  /// we reach this point.
  void deallocate() {
    // empty
  }

  /// Check if the actor is actually a distributed *remote* actor.
  ///
  /// Note that a distributed *local* actor instance is the same as any other
  /// ordinary default (local) actor, and no special handling is needed for them.
  bool isDistributedRemote() {
    return isDistributedRemoteActor;
  }
};

} /// end anonymous namespace

static_assert(size_without_trailing_padding<DefaultActorImpl>::value <=
                      DefaultActorSize &&
                  alignof(DefaultActorImpl) <= alignof(DefaultActor),
              "DefaultActorImpl doesn't fit in DefaultActor");
#if !SWIFT_CONCURRENCY_ACTORS_AS_LOCKS
static_assert(DefaultActorImpl::offsetOfActiveActorStatus() % ACTIVE_ACTOR_STATUS_SIZE == 0,
              "ActiveActorStatus is aligned to the right size");
#endif

static_assert(sizeof(DefaultActor) == sizeof(NonDefaultDistributedActor),
              "NonDefaultDistributedActor size should be the same as DefaultActor");
static_assert(sizeof(NonDefaultDistributedActorImpl) <= ((sizeof(void *) * NumWords_NonDefaultDistributedActor) + sizeof(HeapObject)) &&
              alignof(NonDefaultDistributedActorImpl) <= alignof(NonDefaultDistributedActor),
              "NonDefaultDistributedActorImpl doesn't fit in NonDefaultDistributedActor");

static DefaultActorImpl *asImpl(DefaultActor *actor) {
  return reinterpret_cast<DefaultActorImpl*>(actor);
}

static DefaultActor *asAbstract(DefaultActorImpl *actor) {
  return reinterpret_cast<DefaultActor*>(actor);
}

static NonDefaultDistributedActorImpl *asImpl(NonDefaultDistributedActor *actor) {
  return reinterpret_cast<NonDefaultDistributedActorImpl*>(actor);
}

/*****************************************************************************/
/******************** NEW DEFAULT ACTOR IMPLEMENTATION ***********************/
/*****************************************************************************/

#if !SWIFT_CONCURRENCY_ACTORS_AS_LOCKS

static void traceJobQueue(DefaultActorImpl *actor, Job *first) {
  concurrency::trace::actor_note_job_queue(
      actor, first, [](Job *job) { return getNextJob(job); });
}

static SWIFT_ATTRIBUTE_ALWAYS_INLINE void traceActorStateTransition(DefaultActorImpl *actor,
    ActiveActorStatus oldState, ActiveActorStatus newState, bool distributedActorIsRemote) {

  SWIFT_TASK_DEBUG_LOG("Actor %p transitioned from %#x to %#x (%s)", actor,
                       oldState.getOpaqueFlags(), newState.getOpaqueFlags(),
                       __FUNCTION__);
  newState.traceStateChanged(actor, distributedActorIsRemote);
}

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
dispatch_lock_t *DefaultActorImpl::drainLockAddr() {
  ActiveActorStatus *actorStatus = (ActiveActorStatus *) &this->StatusStorage;
  return (dispatch_lock_t *) (((char *) actorStatus) + ActiveActorStatus::drainLockOffset());
}
#endif /* SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION */

void DefaultActorImpl::scheduleActorProcessJob(JobPriority priority) {
  Job *job = new ProcessOutOfLineJob(this, priority);
  SWIFT_TASK_DEBUG_LOG(
      "Scheduling processing job %p for actor %p at priority %#zx", job, this,
      priority);
  swift_task_enqueueGlobal(job);
}

void DefaultActorImpl::enqueue(Job *job, JobPriority priority) {
  // We can do relaxed loads here, we are just using the current head in the
  // atomic state and linking that into the new job we are inserting, we don't
  // need acquires
  SWIFT_TASK_DEBUG_LOG("Enqueueing job %p onto actor %p at priority %#zx", job,
                       this, priority);
  concurrency::trace::actor_enqueue(this, job);
  bool distributedActorIsRemote = swift_distributed_actor_is_remote(this);
  auto oldState = _status().load(std::memory_order_relaxed);
  while (true) {
    auto newState = oldState;

    // Link this into the queue in the atomic state
    Job *currentHead = oldState.getFirstUnprioritisedJob();
    setNextJob(job, currentHead);
    newState = newState.withFirstUnprioritisedJob(job);

    if (oldState.isIdle()) {
      // Schedule the actor
      newState = newState.withScheduled();
      newState = newState.withNewPriority(priority);
    } else {
      if (priority > oldState.getMaxPriority()) {
        newState = newState.withEscalatedPriority(priority);
      }
    }

    // This needs to be a store release so that we also publish the contents of
    // the new Job we are adding to the atomic job queue. Pairs with consume
    // in drainOne.
    if (_status().compare_exchange_weak(oldState, newState,
                   /* success */ std::memory_order_release,
                   /* failure */ std::memory_order_relaxed)) {
      traceActorStateTransition(this, oldState, newState, distributedActorIsRemote);

      if (!oldState.isScheduled() && newState.isScheduled()) {
        // We took responsibility to schedule the actor for the first time. See
        // also ownership rule (1)
        return scheduleActorProcessJob(newState.getMaxPriority());
      }

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
      if (oldState.getMaxPriority() != newState.getMaxPriority()) {
        if (newState.isRunning()) {
          // Actor is running on a thread, escalate the thread running it
          SWIFT_TASK_DEBUG_LOG("[Override] Escalating actor %p which is running on %#x to %#x priority", this, newState.currentDrainer(), priority);
          dispatch_lock_t *lockAddr = this->drainLockAddr();
          swift_dispatch_lock_override_start_with_debounce(lockAddr, newState.currentDrainer(),
                         (qos_class_t) priority);
        } else {
          // We are scheduling a stealer for an actor due to priority override.
          // This extra processing job has a reference on the actor. See
          // ownership rule (2).
          SWIFT_TASK_DEBUG_LOG(
              "[Override] Scheduling a stealer for actor %p at %#x priority",
              this, newState.getMaxPriority());
          swift_retain(this);
          scheduleActorProcessJob(newState.getMaxPriority());
        }
      }
#endif
      return;
    }
  }
}

// The input job is already escalated to the new priority and has already been
// enqueued into the actor. Push a stealer job for it on the actor.
//
// The caller of this function is escalating the input task and holding its
// TaskStatusRecordLock and escalating this executor via the
// TaskDependencyStatusRecord.
void DefaultActorImpl::enqueueStealer(Job *job, JobPriority priority) {

  SWIFT_TASK_DEBUG_LOG("[Override] Escalating an actor %p due to job that is enqueued being escalated", this);

  bool distributedActorIsRemote = swift_distributed_actor_is_remote(this);
  auto oldState = _status().load(std::memory_order_relaxed);
  while (true) {
    // Until we figure out how to safely enqueue a stealer and rendevouz with
    // the original job so that we don't double-invoke the job, we shall simply
    // escalate the actor's max priority to match the new one.
    //
    // Ideally, we'd also re-sort the job queue so that the escalated job gets
    // to the front of the queue but since the actor's max QoS is a saturating
    // function, this still handles the priority inversion correctly but with
    // priority overhang instead.

    if (oldState.isIdle()) {
      // We are observing a race. Possible scenarios:
      //
      //  1. Escalator is racing with the drain of the actor/task. The task has
      //  just been popped off the actor and is about to run. The thread running
      //  the task will readjust its own priority once it runs since it should
      //  see the escalation in the ActiveTaskStatus and we don't need to
      //  escalate the actor as it will be spurious.
      //
      //  2. Escalator is racing with the enqueue of the task. The task marks
      //  the place it will enqueue in the dependency record before it enqueues
      //  itself. Escalator raced in between these two operations and escalated the
      //  task. Pushing a stealer job for the task onto the actor should fix it.
      return;
    }
    auto newState = oldState;

    if (priority > oldState.getMaxPriority()) {
      newState = newState.withEscalatedPriority(priority);
    }

    if (oldState == newState)
      return;

    if (_status().compare_exchange_weak(oldState, newState,
                   /* success */ std::memory_order_relaxed,
                   /* failure */ std::memory_order_relaxed)) {
      traceActorStateTransition(this, oldState, newState, distributedActorIsRemote);
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
      if (newState.isRunning()) {
        // Actor is running on a thread, escalate the thread running it
        SWIFT_TASK_DEBUG_LOG("[Override] Escalating actor %p which is running on %#x to %#x priority", this, newState.currentDrainer(), priority);
        dispatch_lock_t *lockAddr = this->drainLockAddr();
        swift_dispatch_lock_override_start_with_debounce(lockAddr, newState.currentDrainer(),
                       (qos_class_t) priority);

      } else if (newState.isScheduled()) {
        // We are scheduling a stealer for an actor due to priority override.
        // This extra processing job has a reference on the actor. See
        // ownership rule (2).
        SWIFT_TASK_DEBUG_LOG(
            "[Override] Scheduling a stealer for actor %p at %#x priority",
            this, newState.getMaxPriority());
        swift_retain(this);
        scheduleActorProcessJob(newState.getMaxPriority());
      }
#endif
    }
  }

}

void DefaultActorImpl::processIncomingQueue() {
  // Pairs with the store release in DefaultActorImpl::enqueue
  bool distributedActorIsRemote = swift_distributed_actor_is_remote(this);
  auto oldState = _status().load(SWIFT_MEMORY_ORDER_CONSUME);
  _swift_tsan_consume(this);

  // We must ensure that any jobs not seen by collectJobs() don't have any
  // dangling references to the jobs that have been collected. For that we must
  // atomically set head pointer to NULL. If it fails because more jobs have
  // been added in the meantime, we have to re-read the head pointer.
  while (true) {
    // If there aren't any new jobs in the incoming queue, we can return
    // immediately without updating the status.
    if (!oldState.getFirstUnprioritisedJob()) {
      return;
    }
    assert(oldState.isAnyRunning());

    auto newState = oldState;
    newState = newState.withFirstUnprioritisedJob(nullptr);

    if (_status().compare_exchange_weak(
            oldState, newState,
            /* success */ std::memory_order_relaxed,
            /* failure */ std::memory_order_relaxed)) {
      SWIFT_TASK_DEBUG_LOG("Collected some jobs from actor %p", this);
      traceActorStateTransition(this, oldState, newState,
                                distributedActorIsRemote);
      break;
    }
  }

  handleUnprioritizedJobs(oldState.getFirstUnprioritisedJob());
}

// Called with actor lock held on current thread
void DefaultActorImpl::handleUnprioritizedJobs(Job *head) {
  // Reverse jobs from LIFO to FIFO order
  Job *reversed = nullptr;
  while (head) {
    auto next = getNextJob(head);
    setNextJob(head, reversed);
    reversed = head;
    head = next;
  }
  prioritizedJobs.enqueueContentsOf(reversed);
}

// Called with actor lock held on current thread
Job *DefaultActorImpl::drainOne() {
  SWIFT_TASK_DEBUG_LOG("Draining one job from default actor %p", this);

  traceJobQueue(this, prioritizedJobs.peek());
  auto firstJob = prioritizedJobs.dequeue();
  if (!firstJob) {
    SWIFT_TASK_DEBUG_LOG("No jobs to drain on actor %p", this);
  } else {
    SWIFT_TASK_DEBUG_LOG("Drained first job %p from actor %p", firstJob, this);
    concurrency::trace::actor_dequeue(this, firstJob);
  }
  return firstJob;
}

// Called from processing jobs which are created to drain an actor. We need to
// reason about this function together with swift_task_switch because threads
// can switch from "following actors" to "following tasks" and vice versa.
//
// The way this function works is as following:
//
// 1. We grab the actor lock for the actor we were scheduled to drain.
// 2. Drain the first task out of the actor and run it. Henceforth, the thread
// starts executing the task and following it around. It is no longer a
// processing job for the actor. Note that it will hold the actor lock until it
// needs to hop off the actor but conceptually, it is now a task executor as well.
// 3. When the thread needs to hop off the actor, it is done deep in the
// callstack of this function with an indirection through user code:
// defaultActorDrain -> runJobInEstablishedExecutorContext ->  user
// code -> a call to swift_task_switch to hop out of actor
// 4. This call to swift_task_switch() will attempt to hop off the existing
// actor and jump to the new one. There are 2 possible outcomes at that point:
//   (a) We were able to hop to the new actor and so thread continues executing
//   task. We then schedule a job for the old actor to continue if it has
//   pending work
//   (b) We were not able to take the fast path to the new actor, the task gets
//   enqueued onto the new actor it is going to, and the thread now follows the
//   actor we were trying to hop off. At this point, note that we don't give up
//   the actor lock in `swift_task_switchImpl` so we will come back to
//   defaultActorDrain with the actor lock still held.
//
// At the point of return from the job execution, we may not be holding the lock
// of the same actor that we had started off with, so we need to reevaluate what
// the current actor is
static void defaultActorDrain(DefaultActorImpl *actor) {
  SWIFT_TASK_DEBUG_LOG("Draining default actor %p", actor);
  DefaultActorImpl *currentActor = actor;

  bool actorLockAcquired = actor->tryLock(true);
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
  if (!actorLockAcquired) {
    // tryLock may fail when we compete with other stealers for the actor.
    goto done;
  }
#endif

  (void)actorLockAcquired;
  assert(actorLockAcquired);

  // Setup a TSD for tracking current execution info
  ExecutorTrackingInfo trackingInfo;
  trackingInfo.enterAndShadow(
      SerialExecutorRef::forDefaultActor(asAbstract(currentActor)),
      /*taskExecutor, will be replaced per each job. */
      TaskExecutorRef::undefined());

  while (true) {
    Job *job = currentActor->drainOne();
    if (job == NULL) {
      // No work left to do, try unlocking the actor. This may fail if there is
      // work concurrently enqueued in which case, we'd try again in the loop
      if (currentActor->unlock(false)) {
        break;
      }
    } else {
      if (AsyncTask *task = dyn_cast<AsyncTask>(job)) {
        auto taskExecutor = task->getPreferredTaskExecutor();
        trackingInfo.setTaskExecutor(taskExecutor);
      }

      // This thread is now going to follow the task on this actor. It may hop off
      // the actor
      runJobInEstablishedExecutorContext(job);

      // We could have come back from the job on a generic executor and not as
      // part of a default actor. If so, there is no more work left for us to do
      // here.
      auto currentExecutor = trackingInfo.getActiveExecutor();
      if (!currentExecutor.isDefaultActor()) {
        currentActor = nullptr;
        break;
      }
      currentActor = asImpl(currentExecutor.getDefaultActor());
    }

    if (shouldYieldThread()) {
      currentActor->unlock(true);
      break;
    }

    currentActor->processIncomingQueue();
  }

  // Leave the tracking info.
  trackingInfo.leave();

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
done:
#endif
  // Balances with the retain taken in ProcessOutOfLineJob::process
  swift_release(actor);
}

SWIFT_CC(swiftasync)
void ProcessOutOfLineJob::process(Job *job) {
  auto self = cast<ProcessOutOfLineJob>(job);
  DefaultActorImpl *actor = self->Actor;

  delete self;

  // Balances with the swift_release in defaultActorDrain()
  swift_retain(actor);
  return defaultActorDrain(actor); // 'return' forces tail call
}

#endif /* !SWIFT_CONCURRENCY_ACTORS_AS_LOCKS */

void DefaultActorImpl::destroy() {
#if SWIFT_CONCURRENCY_ACTORS_AS_LOCKS
  // TODO (rokhinip): Do something to assert that the lock is unowned
#else
  auto oldState = _status().load(std::memory_order_acquire);
  // Tasks on an actor are supposed to keep the actor alive until they start
  // running and we can only get here if ref count of the object = 0 which means
  // there should be no more tasks enqueued on the actor.
  assert(!oldState.getFirstUnprioritisedJob() && "actor has queued jobs at destruction");

  if (oldState.isIdle()) {
    assert(prioritizedJobs.empty() && "actor has queued jobs at destruction");
    return;
  }
  assert(oldState.isRunning() && "actor scheduled but not running at destruction");
  // In running state we cannot safely access prioritizedJobs to assert that it is empty.
#endif
}

void DefaultActorImpl::deallocate() {
#if !SWIFT_CONCURRENCY_ACTORS_AS_LOCKS
  // If we're running, mark ourselves as ready for deallocation but don't
  // deallocate yet. When we stop running the actor - at unlock() time - we'll
  // do the actual deallocation.
  //
  // If we're idle, just deallocate immediately
  auto oldState = _status().load(std::memory_order_relaxed);
  while (oldState.isRunning()) {
    auto newState = oldState.withZombie_ReadyForDeallocation();
    if (_status().compare_exchange_weak(oldState, newState,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed))
      return;
  }

  assert(oldState.isIdle());
#endif
  deallocateUnconditional();
}

void DefaultActorImpl::deallocateUnconditional() {
  concurrency::trace::actor_deallocate(this);

  auto metadata = cast<ClassMetadata>(this->metadata);
  swift_deallocClassInstance(this, metadata->getInstanceSize(),
                             metadata->getInstanceAlignMask());
}

bool DefaultActorImpl::tryLock(bool asDrainer) {
#if SWIFT_CONCURRENCY_ACTORS_AS_LOCKS
  this->drainLock.lock();
  return true;
#else /* SWIFT_CONCURRENCY_ACTORS_AS_LOCKS */

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
  SWIFT_TASK_DEBUG_LOG("Thread %#x attempting to jump onto %p, as drainer = %d", dispatch_lock_value_for_self(), this, asDrainer);
  dispatch_thread_override_info_s threadOverrideInfo;
  threadOverrideInfo = swift_dispatch_thread_get_current_override_qos_floor();
  qos_class_t overrideFloor = threadOverrideInfo.override_qos_floor;
retry:;
#else
  SWIFT_TASK_DEBUG_LOG("Thread attempting to jump onto %p, as drainer = %d", this, asDrainer);
#endif

  bool distributedActorIsRemote = swift_distributed_actor_is_remote(this);
  auto oldState = _status().load(std::memory_order_relaxed);
  while (true) {
    bool assertNoJobs = false;
    if (asDrainer) {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
      if (!oldState.isScheduled()) {
        // Some other actor stealer won the race and started running the actor
        // and potentially be done with it if state is observed as idle here.

        // This extra processing jobs releases its reference. See ownership rule
        // (4).
        swift_release(this);

        return false;
      }
#endif

      // We are still in the race with other stealers to take over the actor.
      assert(oldState.isScheduled());

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
      // We only want to self override a thread if we are taking the actor lock
      // as a drainer because there might have been higher priority work
      // enqueued that might have escalated the max priority of the actor to be
      // higher than the original thread request.
      qos_class_t maxActorPriority = (qos_class_t) oldState.getMaxPriority();

      if (threadOverrideInfo.can_override && (maxActorPriority > overrideFloor)) {
        SWIFT_TASK_DEBUG_LOG("[Override] Self-override thread with oq_floor %#x to match max actor %p's priority %#x", overrideFloor, this, maxActorPriority);

        (void) swift_dispatch_thread_override_self(maxActorPriority);
        overrideFloor = maxActorPriority;
        goto retry;
      }
#endif /* SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION */
    } else {
      // We're trying to take the lock in an uncontended manner
      if (oldState.isRunning() || oldState.isScheduled()) {
        SWIFT_TASK_DEBUG_LOG("Failed to jump to %p in fast path", this);
        return false;
      }

      assert(oldState.getMaxPriority() == JobPriority::Unspecified);
      assert(!oldState.getFirstUnprioritisedJob());
      // We cannot assert here that prioritizedJobs is empty,
      // because lock is not held yet. Raise a flag to assert after getting the lock.
      assertNoJobs = true;
    }

    // Taking the drain lock clears the max priority escalated bit because we've
    // already represented the current max priority of the actor on the thread.
    auto newState = oldState.withRunning();
    newState = newState.withoutEscalatedPriority();

    // Claim incoming jobs when obtaining lock as a drainer, to save one
    // round of atomic load and compare-exchange.
    // This is not useful when obtaining lock for assuming thread during actor
    // switching, because arbitrary use code can run between locking and
    // draining the next job. So we still need to call processIncomingQueue() to
    // check for higher priority jobs that could have been scheduled in the
    // meantime. And processing is more efficient when done in larger batches.
    if (asDrainer) {
      newState = newState.withFirstUnprioritisedJob(nullptr);
    }

    // This needs an acquire since we are taking a lock
    if (_status().compare_exchange_weak(oldState, newState,
                                 std::memory_order_acquire,
                                 std::memory_order_relaxed)) {
      _swift_tsan_acquire(this);
      if (assertNoJobs) {
        assert(prioritizedJobs.empty());
      }
      traceActorStateTransition(this, oldState, newState, distributedActorIsRemote);
      if (asDrainer) {
        handleUnprioritizedJobs(oldState.getFirstUnprioritisedJob());
      }
      return true;
    }
  }
#endif /* SWIFT_CONCURRENCY_ACTORS_AS_LOCKS */
}

/* This can be called in 2 ways:
 *    * force_unlock = true: Thread is following task and therefore wants to
 *    give up the current actor since task is switching away.
 *    * force_unlock = false: Thread is not necessarily following the task, it
 *    may want to follow actor if there is more work on it.
 *
 *    Returns true if we managed to unlock the actor, false if we didn't. If the
 *    actor has more jobs remaining on it during unlock, this method will
 *    schedule the actor
 */
bool DefaultActorImpl::unlock(bool forceUnlock)
{
#if SWIFT_CONCURRENCY_ACTORS_AS_LOCKS
  this->drainLock.unlock();
  return true;
#else
  bool distributedActorIsRemote = swift_distributed_actor_is_remote(this);
  auto oldState = _status().load(std::memory_order_relaxed);
  SWIFT_TASK_DEBUG_LOG("Try unlock-ing actor %p with forceUnlock = %d", this, forceUnlock);

  _swift_tsan_release(this);
  while (true) {
    assert(oldState.isAnyRunning());
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
    assert(dispatch_lock_is_locked_by_self(*(this->drainLockAddr())));
#endif

    if (oldState.isZombie_ReadyForDeallocation()) {
#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
      // Reset any override on this thread as a result of this thread running
      // the actor
      if (oldState.isMaxPriorityEscalated()) {
        swift_dispatch_lock_override_end((qos_class_t)oldState.getMaxPriority());
      }
#endif
      deallocateUnconditional();
      SWIFT_TASK_DEBUG_LOG("Unlock-ing actor %p succeeded with full deallocation", this);
      return true;
    }

    auto newState = oldState;
    // Lock is still held at this point, so it is safe to access prioritizedJobs
    if (!prioritizedJobs.empty() || oldState.getFirstUnprioritisedJob()) {
      // There is work left to do, don't unlock the actor
      if (!forceUnlock) {
        SWIFT_TASK_DEBUG_LOG("Unlock-ing actor %p failed", this);
        return false;
      }
      // We need to schedule the actor - remove any escalation bits since we'll
      // schedule the actor at the max priority currently on it

      // N decreases by 1 as this processing job is going away; but R is
      // still 1. We schedule a new processing job to maintain N >= R.

      // It is possible that there are stealers scheduled for the actor already;
      // but, we still schedule one anyway. This is because it is possible that
      // those stealers got scheduled when we were running the actor and gone
      // away. (See tryLock function.)
      newState = newState.withScheduled();
      newState = newState.withoutEscalatedPriority();
    } else {
      // There is no work left to do - actor goes idle

      // R becomes 0 and N descreases by 1.
      // But, we may still have stealers scheduled so N could be > 0. This is
      // fine since N >= R. Every such stealer, once scheduled, will observe
      // actor as idle, will release its ref and return. (See tryLock function.)
      newState = newState.withIdle();
      newState = newState.resetPriority();
    }

    // This needs to be a release since we are unlocking a lock
    if (_status().compare_exchange_weak(oldState, newState,
                      /* success */ std::memory_order_release,
                      /* failure */ std::memory_order_relaxed)) {
      _swift_tsan_release(this);
      traceActorStateTransition(this, oldState, newState, distributedActorIsRemote);

      if (newState.isScheduled()) {
        // See ownership rule (6) in DefaultActorImpl
        scheduleActorProcessJob(newState.getMaxPriority());
      } else {
        // See ownership rule (5) in DefaultActorImpl
        SWIFT_TASK_DEBUG_LOG("Actor %p is idle now", this);
      }

#if SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION
      // Reset any asynchronous escalations we may have gotten on this thread
      // after taking the drain lock.
      //
      // Only do this after we have reenqueued the actor so that we don't lose
      // any "mojo" prior to the enqueue.
      if (oldState.isMaxPriorityEscalated()) {
        swift_dispatch_lock_override_end((qos_class_t) oldState.getMaxPriority());
      }
#endif
      return true;
    }
  }
#endif
}

SWIFT_CC(swift)
static void swift_job_runImpl(Job *job, SerialExecutorRef executor) {
  ExecutorTrackingInfo trackingInfo;

  // swift_job_run is a primary entrypoint for executors telling us to
  // run jobs.  Actor executors won't expect us to switch off them
  // during this operation.  But do allow switching if the executor
  // is generic.
  if (!executor.isGeneric()) trackingInfo.disallowSwitching();

  trackingInfo.enterAndShadow(executor, TaskExecutorRef::undefined());

  SWIFT_TASK_DEBUG_LOG("job %p", job);
  runJobInEstablishedExecutorContext(job);

  trackingInfo.leave();

  // Give up the current executor if this is a switching context
  // (which, remember, only happens if we started out on a generic
  // executor) and we've switched to a default actor.
  auto currentExecutor = trackingInfo.getActiveExecutor();
  if (trackingInfo.allowsSwitching() && currentExecutor.isDefaultActor()) {
    asImpl(currentExecutor.getDefaultActor())->unlock(true);
  }
}

SWIFT_CC(swift)
static void swift_job_run_on_serial_and_task_executorImpl(Job *job,
                                               SerialExecutorRef serialExecutor,
                                               TaskExecutorRef taskExecutor) {
  ExecutorTrackingInfo trackingInfo;

  // TODO: we don't allow switching
  trackingInfo.disallowSwitching();
  trackingInfo.enterAndShadow(serialExecutor, taskExecutor);

  SWIFT_TASK_DEBUG_LOG("job %p", job);
  runJobInEstablishedExecutorContext(job);

  trackingInfo.leave();

  // Give up the current executor if this is a switching context
  // (which, remember, only happens if we started out on a generic
  // executor) and we've switched to a default actor.
  auto currentExecutor = trackingInfo.getActiveExecutor();
  if (trackingInfo.allowsSwitching() && currentExecutor.isDefaultActor()) {
    asImpl(currentExecutor.getDefaultActor())->unlock(true);
  }
}

SWIFT_CC(swift)
static void swift_job_run_on_task_executorImpl(Job *job,
                                               TaskExecutorRef taskExecutor) {
  swift_job_run_on_serial_and_task_executor(
      job, SerialExecutorRef::generic(), taskExecutor);
}

void swift::swift_defaultActor_initialize(DefaultActor *_actor) {
  asImpl(_actor)->initialize();
}

void swift::swift_defaultActor_destroy(DefaultActor *_actor) {
  asImpl(_actor)->destroy();
}

void swift::swift_defaultActor_enqueue(Job *job, DefaultActor *_actor) {
#if SWIFT_CONCURRENCY_ACTORS_AS_LOCKS
  assert(false && "Should not enqueue onto default actor in actor as locks model");
#else
  asImpl(_actor)->enqueue(job, job->getPriority());
#endif
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
    if (!metadata || !metadata->isTypeMetadata()) {
      return false;
    }
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
                                     SerialExecutorRef currentExecutor) {
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
static void giveUpThreadForSwitch(SerialExecutorRef currentExecutor) {
  if (currentExecutor.isGeneric()) {
    SWIFT_TASK_DEBUG_LOG("Giving up current generic executor %p",
                         currentExecutor.getIdentity());
    return;
  }

  asImpl(currentExecutor.getDefaultActor())->unlock(true);
}

/// Try to assume control of the current thread for the given executor
/// in order to run the given job.
///
/// This doesn't actually run the job yet.
///
/// Note that we don't update DefaultActorProcessingFrame here; we'll
/// do that in runOnAssumedThread.
static bool tryAssumeThreadForSwitch(SerialExecutorRef newExecutor,
                                     TaskExecutorRef newTaskExecutor) {
  // If the new executor is generic, we don't need to do anything.
  if (newExecutor.isGeneric() && newTaskExecutor.isUndefined()) {
    return true;
  }

  // If the new executor is a default actor, ask it to assume the thread.
  if (newExecutor.isDefaultActor()) {
    return asImpl(newExecutor.getDefaultActor())->tryLock(false);
  }

  return false;
}

static bool mustSwitchToRun(SerialExecutorRef currentSerialExecutor,
                            SerialExecutorRef newSerialExecutor,
                            TaskExecutorRef currentTaskExecutor,
                            TaskExecutorRef newTaskExecutor) {
  if (currentSerialExecutor.getIdentity() != newSerialExecutor.getIdentity()) {
    return true; // must switch, new isolation context
  }

  // else, we may have to switch if the preferred task executor is different
  auto differentTaskExecutor =
      currentTaskExecutor.getIdentity() != newTaskExecutor.getIdentity();
  if (differentTaskExecutor) {
    return true;
  }

  return false;
}

/// Given that we've assumed control of an executor on this thread,
/// continue to run the given task on it.
SWIFT_CC(swiftasync)
static void runOnAssumedThread(AsyncTask *task, SerialExecutorRef executor,
                               ExecutorTrackingInfo *oldTracking) {
  // Note that this doesn't change the active task and so doesn't
  // need to either update ActiveTask or flagAsRunning/flagAsSuspended.

  // If there's already tracking info set up, just change the executor
  // there and tail-call the task.  We don't want these frames to
  // potentially accumulate linearly.
  if (oldTracking) {
    oldTracking->setActiveExecutor(executor);
    oldTracking->setTaskExecutor(task->getPreferredTaskExecutor());

    return task->runInFullyEstablishedContext(); // 'return' forces tail call
  }

  // Otherwise, set up tracking info.
  ExecutorTrackingInfo trackingInfo;
  trackingInfo.enterAndShadow(executor, task->getPreferredTaskExecutor());

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
    asImpl(executor.getDefaultActor())->unlock(true);
}

SWIFT_CC(swiftasync)
static void swift_task_switchImpl(SWIFT_ASYNC_CONTEXT AsyncContext *resumeContext,
                                  TaskContinuationFunction *resumeFunction,
                                  SerialExecutorRef newExecutor) {
  auto task = swift_task_getCurrent();
  assert(task && "no current task!");

  auto trackingInfo = ExecutorTrackingInfo::current();
  auto currentExecutor =
    (trackingInfo ? trackingInfo->getActiveExecutor()
                  : SerialExecutorRef::generic());
  auto currentTaskExecutor = (trackingInfo ? trackingInfo->getTaskExecutor()
                                           : TaskExecutorRef::undefined());
  auto newTaskExecutor = task->getPreferredTaskExecutor();
  SWIFT_TASK_DEBUG_LOG("Task %p trying to switch executors: executor %p to "
                       "%p%s; task executor: from %p%s to %p%s",
                       task, currentExecutor.getIdentity(),
                       currentExecutor.isMainExecutor() ? " (MainActorExecutor)"
                       : currentExecutor.isGeneric()    ? " (GenericExecutor)"
                                                        : "",
                       newExecutor.getIdentity(),
                       newExecutor.isMainExecutor() ? " (MainActorExecutor)"
                       : newExecutor.isGeneric()    ? " (GenericExecutor)"
                                                    : "",
                       currentTaskExecutor.getIdentity(),
                       currentTaskExecutor.isDefined() ? "" : " (undefined)",
                       newTaskExecutor.getIdentity(),
                       newTaskExecutor.isDefined() ? "" : " (undefined)");

  // If the current executor is compatible with running the new executor,
  // we can just immediately continue running with the resume function
  // we were passed in.
  if (!mustSwitchToRun(currentExecutor, newExecutor, currentTaskExecutor,
                       newTaskExecutor)) {
    return resumeFunction(resumeContext); // 'return' forces tail call
  }

  // Park the task for simplicity instead of trying to thread the
  // initial resumption information into everything below.
  task->ResumeContext = resumeContext;
  task->ResumeTask = resumeFunction;

  // If the current executor can give up its thread, and the new executor
  // can take over a thread, try to do so; but don't do this if we've
  // been asked to yield the thread.
  if (currentTaskExecutor.isUndefined() &&
      canGiveUpThreadForSwitch(trackingInfo, currentExecutor) &&
      !shouldYieldThread() &&
      tryAssumeThreadForSwitch(newExecutor, newTaskExecutor)) {
    SWIFT_TASK_DEBUG_LOG(
        "switch succeeded, task %p assumed thread for executor %p", task,
        newExecutor.getIdentity());
    giveUpThreadForSwitch(currentExecutor);
    // 'return' forces tail call
    return runOnAssumedThread(task, newExecutor, trackingInfo);
  }

  // Otherwise, just asynchronously enqueue the task on the given
  // executor.
  SWIFT_TASK_DEBUG_LOG(
      "switch failed, task %p enqueued on executor %p (task executor: %p)",
      task, newExecutor.getIdentity(), currentTaskExecutor.getIdentity());

  task->flagAsAndEnqueueOnExecutor(newExecutor);
  _swift_task_clearCurrent();
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
extern "C" SWIFT_CC(swift) void _swift_task_enqueueOnTaskExecutor(
    Job *job, HeapObject *executor, const Metadata *selfType,
    const TaskExecutorWitnessTable *wtable);

extern "C" SWIFT_CC(swift) void _swift_task_makeAnyTaskExecutor(
    HeapObject *executor, const Metadata *selfType,
    const TaskExecutorWitnessTable *wtable);

SWIFT_CC(swift)
static void swift_task_enqueueImpl(Job *job, SerialExecutorRef executor) {
  SWIFT_TASK_DEBUG_LOG("enqueue job %p on serial executor %p", job,
                       executor.getIdentity());

  assert(job && "no job provided");
  job->SchedulerPrivate[0] = NULL;
  job->SchedulerPrivate[1] = NULL;

  _swift_tsan_release(job);

  if (executor.isGeneric()) {
    // TODO: check the task for a flag if we need to look for task executor
    if (auto task = dyn_cast<AsyncTask>(job)) {
      auto taskExecutor = task->getPreferredTaskExecutor();
      if (taskExecutor.isDefined()) {
#if SWIFT_CONCURRENCY_EMBEDDED
        swift_unreachable("task executors not supported in embedded Swift");
#else
        auto wtable = taskExecutor.getTaskExecutorWitnessTable();
        auto taskExecutorObject = taskExecutor.getIdentity();
        auto taskExecutorType = swift_getObjectType(taskExecutorObject);
        return _swift_task_enqueueOnTaskExecutor(job, taskExecutorObject,
                                                 taskExecutorType, wtable);
#endif // SWIFT_CONCURRENCY_EMBEDDED
      } // else, fall-through to the default global enqueue
    }
    return swift_task_enqueueGlobal(job);
  }

  if (executor.isDefaultActor()) {
    auto taskExecutor = TaskExecutorRef::undefined();
    if (auto task = dyn_cast<AsyncTask>(job)) {
      taskExecutor = task->getPreferredTaskExecutor();
    }

#if SWIFT_CONCURRENCY_EMBEDDED
    swift_unreachable("task executors not supported in embedded Swift");
#else
    if (taskExecutor.isDefined()) {
      auto wtable = taskExecutor.getTaskExecutorWitnessTable();
      auto executorObject = taskExecutor.getIdentity();
      auto executorType = swift_getObjectType(executorObject);
      return _swift_task_enqueueOnTaskExecutor(job, executorObject,
                                               executorType, wtable);
    } else {
      return swift_defaultActor_enqueue(job, executor.getDefaultActor());
    }
#endif // SWIFT_CONCURRENCY_EMBEDDED
  }

#if SWIFT_CONCURRENCY_EMBEDDED
  swift_unreachable("custom executors not supported in embedded Swift");
#else
  // For main actor or actors with custom executors
  auto wtable = executor.getSerialExecutorWitnessTable();
  auto executorObject = executor.getIdentity();
  auto executorType = swift_getObjectType(executorObject);
  _swift_task_enqueueOnExecutor(job, executorObject, executorType, wtable);
#endif // SWIFT_CONCURRENCY_EMBEDDED
}

static void
swift_actor_escalate(DefaultActorImpl *actor, AsyncTask *task, JobPriority newPriority)
{
#if !SWIFT_CONCURRENCY_ACTORS_AS_LOCKS
  return actor->enqueueStealer(task, newPriority);
#endif
}

SWIFT_CC(swift)
void swift::swift_executor_escalate(SerialExecutorRef executor, AsyncTask *task,
  JobPriority newPriority) {
  if (executor.isGeneric()) {
    // TODO (rokhinip): We'd push a stealer job for the task on the executor.
    return;
  }

  if (executor.isDefaultActor()) {
    return swift_actor_escalate(asImpl(executor.getDefaultActor()), task, newPriority);
  }

  // TODO (rokhinip): This is either the main actor or an actor with a custom
  // executor. We need to let the executor know that the job has been escalated.
  // For now, do nothing
  return;
}

#define OVERRIDE_ACTOR COMPATIBILITY_OVERRIDE
#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH


/*****************************************************************************/
/***************************** DISTRIBUTED ACTOR *****************************/
/*****************************************************************************/

void swift::swift_nonDefaultDistributedActor_initialize(NonDefaultDistributedActor *_actor) {
  asImpl(_actor)->initialize();
}

OpaqueValue*
swift::swift_distributedActor_remote_initialize(const Metadata *actorType) {
  const ClassMetadata *metadata = actorType->getClassObject();

  // TODO(distributed): make this allocation smaller
  // ==== Allocate the memory for the remote instance
  HeapObject *alloc = swift_allocObject(metadata,
                                        metadata->getInstanceSize(),
                                        metadata->getInstanceAlignMask());

  // TODO: remove this memset eventually, today we only do this to not have
  //       to modify the destructor logic, as releasing zeroes is no-op
  memset(alloc + 1, 0, metadata->getInstanceSize() - sizeof(HeapObject));

  // TODO(distributed): a remote one does not have to have the "real"
  //  default actor body, e.g. we don't need an executor at all; so
  //  we can allocate more efficiently and only share the flags/status field
  //  between the both memory representations
  // If it is a default actor, we reuse the same layout as DefaultActorImpl,
  // and store flags in the allocation directly as we initialize it.
  if (isDefaultActorClass(metadata)) {
    auto actor = asImpl(reinterpret_cast<DefaultActor *>(alloc));
    actor->initialize(/*remote*/true);
    assert(swift_distributed_actor_is_remote(alloc));
    return reinterpret_cast<OpaqueValue*>(actor);
  } else {
    auto actor = asImpl(reinterpret_cast<NonDefaultDistributedActor *>(alloc));
    actor->initialize(/*remote*/true);
    assert(swift_distributed_actor_is_remote(alloc));
    return reinterpret_cast<OpaqueValue*>(actor);
  }
}

bool swift::swift_distributed_actor_is_remote(HeapObject *_actor) {
  const ClassMetadata *metadata = cast<ClassMetadata>(_actor->metadata);
  if (isDefaultActorClass(metadata)) {
    return asImpl(reinterpret_cast<DefaultActor *>(_actor))->isDistributedRemote();
  } else {
    return asImpl(reinterpret_cast<NonDefaultDistributedActor *>(_actor))->isDistributedRemote();
  }
}

bool DefaultActorImpl::isDistributedRemote() {
  return this->isDistributedRemoteActor;
}
