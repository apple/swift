//===--- CompatibilityOverrideConcurrency.cpp - Compatibility override tests ---===//
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

#if defined(__APPLE__) && defined(__MACH__)

#define SWIFT_TARGET_LIBRARY_NAME swift_Concurrency

#include "../../stdlib/public/CompatibilityOverride/CompatibilityOverride.h"
#include "swift/Runtime/Concurrency.h"
#include "gtest/gtest.h"

#if __has_include("pthread.h")

#define RUN_ASYNC_MAIN_DRAIN_QUEUE_TEST 1
#include <pthread.h>
#endif // HAVE_PTHREAD_H

#include <stdio.h>

using namespace swift;

static bool EnableOverride;
static bool Ran;

namespace {
template <typename T>
T getEmptyValue() {
  return T();
}
template <>
SerialExecutorRef getEmptyValue() {
  return SerialExecutorRef::generic();
}
} // namespace

#define OVERRIDE(name, ret, attrs, ccAttrs, namespace, typedArgs, namedArgs)   \
  static ccAttrs ret name##Override(COMPATIBILITY_UNPAREN_WITH_COMMA(          \
      typedArgs) Original_##name originalImpl) {                               \
    if (!EnableOverride)                                                       \
      return originalImpl COMPATIBILITY_PAREN(namedArgs);                      \
    Ran = true;                                                                \
    return getEmptyValue<ret>();                                               \
  }
#define OVERRIDE_TASK_NORETURN(name, attrs, ccAttrs, namespace, typedArgs,     \
                               namedArgs)                                      \
  static ccAttrs void name##Override(COMPATIBILITY_UNPAREN_WITH_COMMA(         \
      typedArgs) Original_##name originalImpl) {                               \
    if (!EnableOverride)                                                       \
      originalImpl COMPATIBILITY_PAREN(namedArgs);                             \
    Ran = true;                                                                \
  }

#include "../../stdlib/public/CompatibilityOverride/CompatibilityOverrideConcurrency.def"

struct OverrideSection {
  uintptr_t version;

#define OVERRIDE(name, ret, attrs, ccAttrs, namespace, typedArgs, namedArgs)   \
  Override_##name name;
#include "../../stdlib/public/CompatibilityOverride/CompatibilityOverrideConcurrency.def"
};

OverrideSection ConcurrencyOverrides
    __attribute__((section("__DATA," COMPATIBILITY_OVERRIDE_SECTION_NAME_swift_Concurrency))) = {
        0,
#define OVERRIDE(name, ret, attrs, ccAttrs, namespace, typedArgs, namedArgs)   \
  name##Override,
#include "../../stdlib/public/CompatibilityOverride/CompatibilityOverrideConcurrency.def"
};

SWIFT_CC(swift)
static void
swift_task_enqueueGlobal_override(Job *job,
                                  swift_task_enqueueGlobal_original original) {
  Ran = true;
}

SWIFT_CC(swift)
static void
swift_task_checkIsolated_override(SerialExecutorRef executor,
                                      swift_task_checkIsolated_original original) {
  Ran = true;
}

SWIFT_CC(swift)
static void swift_task_enqueueGlobalWithDelay_override(
    unsigned long long delay, Job *job,
    swift_task_enqueueGlobalWithDelay_original original) {
  Ran = true;
}

SWIFT_CC(swift)
static void swift_task_enqueueMainExecutor_override(
    Job *job, swift_task_enqueueMainExecutor_original original) {
  Ran = true;
}

SWIFT_CC(swift)
static void swift_task_startOnMainActor_override(AsyncTask* task) {
  Ran = true;
}

#ifdef RUN_ASYNC_MAIN_DRAIN_QUEUE_TEST
[[noreturn]] SWIFT_CC(swift)
static void swift_task_asyncMainDrainQueue_override_fn(
    swift_task_asyncMainDrainQueue_original original,
    swift_task_asyncMainDrainQueue_override compatOverride) {
  Ran = true;
  pthread_exit(nullptr); // noreturn function
}
#endif

class CompatibilityOverrideConcurrencyTest : public ::testing::Test {
protected:
  virtual void SetUp() {
    // This check is a bit pointless, as long as you trust the section
    // attribute to work correctly, but it ensures that the override
    // section actually gets linked into the test executable. If it's not
    // used then it disappears and the real tests begin to fail.
    ASSERT_EQ(ConcurrencyOverrides.version, static_cast<uintptr_t>(0));

    EnableOverride = true;
    Ran = false;

    // Executor hooks need to be set manually.
    swift_task_enqueueGlobal_hook = swift_task_enqueueGlobal_override;
    swift_task_enqueueGlobalWithDelay_hook =
        swift_task_enqueueGlobalWithDelay_override;
    swift_task_enqueueMainExecutor_hook =
        swift_task_enqueueMainExecutor_override;
    swift_task_checkIsolated_hook =
        swift_task_checkIsolated_override;
#ifdef RUN_ASYNC_MAIN_DRAIN_QUEUE_TEST
    swift_task_asyncMainDrainQueue_hook =
        swift_task_asyncMainDrainQueue_override_fn;
#endif
  }

  virtual void TearDown() {
    EnableOverride = false;
    ASSERT_TRUE(Ran);
  }
};

static Job fakeJob{{JobKind::DefaultActorInline},
                   static_cast<JobInvokeFunction *>(nullptr),
                   nullptr};

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_enqueue) {
  swift_task_enqueue(&fakeJob, SerialExecutorRef::generic());
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_job_run) {
  swift_job_run(&fakeJob, SerialExecutorRef::generic());
}

TEST_F(CompatibilityOverrideConcurrencyTest,
       test_swift_job_run_on_task_executor) {
  swift_job_run_on_task_executor(&fakeJob, TaskExecutorRef::undefined());
}

TEST_F(CompatibilityOverrideConcurrencyTest,
       test_swift_job_run_on_serial_and_task_executor) {
  swift_job_run_on_serial_and_task_executor(
      &fakeJob, SerialExecutorRef::generic(), TaskExecutorRef::undefined());
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_getCurrentExecutor) {
  swift_task_getCurrentExecutor();
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_switch) {
  swift_task_switch(nullptr, nullptr, SerialExecutorRef::generic());
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_enqueueGlobal) {
  swift_task_enqueueGlobal(&fakeJob);
}

TEST_F(CompatibilityOverrideConcurrencyTest,
       test_swift_task_enqueueGlobalWithDelay) {
  swift_task_enqueueGlobalWithDelay(0, &fakeJob);
}

TEST_F(CompatibilityOverrideConcurrencyTest,
       test_swift_task_checkIsolated) {
  swift_task_checkIsolated(SerialExecutorRef::generic());
}

TEST_F(CompatibilityOverrideConcurrencyTest,
       test_swift_task_enqueueMainExecutor) {
  swift_task_enqueueMainExecutor(&fakeJob);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_create_common) {
  swift_task_create_common(0, nullptr, nullptr, nullptr, nullptr, 0);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_future_wait) {
  swift_task_future_wait(nullptr, nullptr, nullptr, nullptr, nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest,
       test_swift_task_future_wait_throwing) {
  swift_task_future_wait_throwing(nullptr, nullptr, nullptr, nullptr, nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_continuation_resume) {
  swift_continuation_resume(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest,
       test_swift_continuation_throwingResume) {
  swift_continuation_throwingResume(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest,
       test_swift_continuation_throwingResumeWithError) {
  swift_continuation_throwingResumeWithError(nullptr, nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_asyncLet_wait) {
  swift_asyncLet_wait(nullptr, nullptr, nullptr, nullptr, nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_asyncLet_wait_throwing) {
  swift_asyncLet_wait(nullptr, nullptr, nullptr, nullptr, nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_asyncLet_end) {
  swift_asyncLet_end(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_taskGroup_initialize) {
  swift_taskGroup_initialize(nullptr, nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_taskGroup_initializeWithFlags) {
  swift_taskGroup_initializeWithFlags(0, nullptr, nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_taskGroup_attachChild) {
  swift_taskGroup_attachChild(nullptr, nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_taskGroup_destroy) {
  swift_taskGroup_destroy(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest,
       test_swift_taskGroup_wait_next_throwing) {
  swift_taskGroup_wait_next_throwing(nullptr, nullptr, nullptr, nullptr,
                                     nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_taskGroup_isEmpty) {
  swift_taskGroup_isEmpty(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_taskGroup_isCancelled) {
  swift_taskGroup_isCancelled(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_taskGroup_cancelAll) {
  swift_taskGroup_cancelAll(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_taskGroup_waitAll) {
  swift_taskGroup_waitAll(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_taskGroup_addPending) {
  swift_taskGroup_addPending(nullptr, true);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_localValuePush) {
  swift_task_localValuePush(nullptr, nullptr, nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_localValueGet) {
  swift_task_localValueGet(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_localValuePop) {
  swift_task_localValuePop();
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_localsCopyTo) {
  swift_task_localsCopyTo(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, task_hasTaskGroupStatusRecord) {
  swift_task_hasTaskGroupStatusRecord();
}

TEST_F(CompatibilityOverrideConcurrencyTest, task_getPreferredTaskExecutor) {
  swift_task_getPreferredTaskExecutor();
}

TEST_F(CompatibilityOverrideConcurrencyTest, task_pushTaskExecutorPreference) {
  swift_task_pushTaskExecutorPreference(TaskExecutorRef::undefined());
}

TEST_F(CompatibilityOverrideConcurrencyTest, task_popTaskExecutorPreference) {
  swift_task_popTaskExecutorPreference(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_cancel) {
  swift_task_cancel(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_cancel_group_child_tasks) {
  swift_task_cancel_group_child_tasks(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_escalate) {
  swift_task_escalate(nullptr, {});
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_startOnMainActorImpl) {
  swift_task_startOnMainActor(nullptr);
}

#if RUN_ASYNC_MAIN_DRAIN_QUEUE_TEST
TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_asyncMainDrainQueue) {

  auto runner = [](void *) -> void * {
    swift_task_asyncMainDrainQueue();
    return nullptr;
  };

  int ret = 0;
  pthread_t thread;
  pthread_attr_t attrs;
  ret = pthread_attr_init(&attrs);
  ASSERT_EQ(ret, 0);
  ret = pthread_create(&thread, &attrs, runner, nullptr);
  ASSERT_EQ(ret, 0);
  void * result = nullptr;
  ret = pthread_join(thread, &result);
  ASSERT_EQ(ret, 0);
  pthread_attr_destroy(&attrs);
  ASSERT_EQ(ret, 0);
}
#endif

#endif
