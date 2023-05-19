#include "Runtime/Concurrency.h"
#include "Concurrency/Actor.h"
#include "Concurrency/Task.h"
#include "Concurrency/TaskPrivate.h"
#include "Concurrency/VoucherSupport.h"

#include "swift/Runtime/Atomic.h"
#include "swift/Runtime/Casting.h"
#include "Runtime/Threading/ThreadLocal.h"

#include <Availability.h>
#include <TargetConditionals.h>

#include <atomic>
#include <new>

using namespace swift;

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
  static SWIFT_RUNTIME_DECLARE_THREAD_LOCAL(
      Pointer<ExecutorTrackingInfo>, ActiveInfoInThread,
      SWIFT_CONCURRENCY_EXECUTOR_TRACKING_INFO_KEY);

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
  /// leave() must be called beforet the object goes out of scope.
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
  static SWIFT_RUNTIME_DECLARE_THREAD_LOCAL(Pointer<AsyncTask>, Value,
                                            SWIFT_CONCURRENCY_TASK_KEY);

public:
  static void set(AsyncTask *task) { Value.set(task); }
  static AsyncTask *get() { return Value.get(); }
};

/// Define the thread-locals.
SWIFT_RUNTIME_DECLARE_THREAD_LOCAL(
  Pointer<AsyncTask>,
  ActiveTask::Value,
  SWIFT_CONCURRENCY_TASK_KEY);

SWIFT_RUNTIME_DECLARE_THREAD_LOCAL(
  Pointer<ExecutorTrackingInfo>,
  ExecutorTrackingInfo::ActiveInfoInThread,
  SWIFT_CONCURRENCY_EXECUTOR_TRACKING_INFO_KEY);

} // namespace

SWIFT_CC(swift)
AsyncTask *swift::swift_task_getCurrent() {
  return ActiveTask::get();
}

AsyncTask *swift::_swift_task_clearCurrent() {
  auto task = ActiveTask::get();
  ActiveTask::set(nullptr);
  return task;
}

void swift::adoptTaskVoucher(AsyncTask *task) {
  ExecutorTrackingInfo::current()->swapToJob(task);
}

void swift::restoreTaskVoucher(AsyncTask *task) {
  ExecutorTrackingInfo::current()->restoreVoucher(task);
}

static swift_once_t voucherDisableCheckOnce;
static bool vouchersDisabled;

namespace {
  struct _SwiftNSOperatingSystemVersion{
    intptr_t majorVersion;
    intptr_t minorVersion;
    intptr_t patchVersion;
  };
}

extern "C"
_SwiftNSOperatingSystemVersion
_swift_stdlib_operatingSystemVersion() __attribute__((const));

static void _initializeVouchersDisabled(void *ctxt) {
  auto osVersion = _swift_stdlib_operatingSystemVersion();
  #if TARGET_OS_WATCH
  vouchersDisabled = (
    osVersion.majorVersion == 8 &&
    osVersion.minorVersion >= 0 && osVersion.minorVersion < 3
  );
  #elif TARGET_OS_IPHONE
  vouchersDisabled = (
    osVersion.majorVersion == 15 &&
    osVersion.minorVersion >= 0 && osVersion.minorVersion < 2
  );
  #elif TARGET_OS_OSX
  vouchersDisabled = (
    osVersion.majorVersion == 12 &&
    osVersion.minorVersion >= 0 && osVersion.minorVersion < 1
  );
  #else
  vouchersDisabled = false;
  #endif
}

bool VoucherManager::vouchersAreDisabled() {
  swift_once(&voucherDisableCheckOnce,
             &_initializeVouchersDisabled,
             nullptr);
  return vouchersDisabled;
}
