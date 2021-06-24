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
ExecutorRef getEmptyValue() {
  return ExecutorRef::generic();
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
#include "../../stdlib/public/CompatibilityOverride/CompatibilityOverrideConcurrency.def"

struct OverrideSection {
  uintptr_t version;

#define OVERRIDE(name, ret, attrs, ccAttrs, namespace, typedArgs, namedArgs)   \
  Override_##name name;
#include "../../stdlib/public/CompatibilityOverride/CompatibilityOverrideConcurrency.def"
};

OverrideSection ConcurrencyOverrides
    __attribute__((section("__DATA,__s_async_hook"))) = {
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
  }

  virtual void TearDown() {
    EnableOverride = false;
    ASSERT_TRUE(Ran);
  }
};

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_enqueue) {
  swift_task_enqueue(nullptr, ExecutorRef::generic());
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_job_run) {
  swift_job_run(nullptr, ExecutorRef::generic());
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_getCurrent) {
  swift_task_getCurrent();
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_getCurrentExecutor) {
  swift_task_getCurrentExecutor();
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_switch) {
  swift_task_switch(nullptr, nullptr, ExecutorRef::generic());
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_enqueueGlobal) {
  swift_task_enqueueGlobal(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest,
       test_swift_task_enqueueGlobalWithDelay) {
  swift_task_enqueueGlobalWithDelay(0, nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest,
       test_swift_task_enqueueMainExecutor) {
  swift_task_enqueueMainExecutor(nullptr);
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

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_addStatusRecord) {
  swift_task_addStatusRecord(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_tryAddStatusRecord) {
  swift_task_tryAddStatusRecord(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_removeStatusRecord) {
  swift_task_removeStatusRecord(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, task_hasTaskGroupStatusRecord) {
  swift_task_hasTaskGroupStatusRecord();
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_attachChild) {
  swift_task_attachChild(nullptr);
}

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_detachChild) {
  swift_task_detachChild(nullptr);
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

TEST_F(CompatibilityOverrideConcurrencyTest, test_swift_task_getNearestDeadline) {
  swift_task_getNearestDeadline(nullptr);
}

#endif
