//===--- TaskStatus.cpp - Unit tests for the task-status API --------------===//
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

#include "swift/Runtime/Concurrency.h"
#include "swift/Basic/STLExtras.h"
#include "gtest/gtest.h"

using namespace swift;

namespace {
template <class T> struct ValueContext;

template <class T>
using InvokeFunctionRef =
  llvm::function_ref<void(ValueContext<T> *context)>;
using BodyFunctionRef =
  llvm::function_ref<void(AsyncTask *task)>;

template <class Storage> struct ValueContext : AsyncContext {
  Storage Value;
  InvokeFunctionRef<Storage> StoredInvokeFn;
};

// Disable template argument deduction.
template <class T>
using undeduced =
  typename std::enable_if<std::is_same<T, T>::value, T>::type;

template <class T>
SWIFT_CC(swiftasync)
static void simpleTaskInvokeFunction(SWIFT_ASYNC_CONTEXT AsyncContext *context,
                                     SWIFT_CONTEXT HeapObject *) {
  auto valueContext = static_cast<ValueContext<T>*>(context);
  valueContext->StoredInvokeFn(valueContext);

  // Destroy the stored value.
  valueContext->Value.T::~T();

  // Return to finish off the task.
  // In a normal situation we'd need to free the context, but here
  // we know we're at the top level.
  return valueContext->ResumeParent(valueContext);
}

template <class T>
static void withSimpleTask(TaskCreateFlags flags, T &&value,
                           undeduced<InvokeFunctionRef<T>> invokeFn,
                           BodyFunctionRef body) {
  auto taskAndContext =
      swift_task_create_common(flags.getOpaqueValue(),
                               nullptr,
                               nullptr,
                               reinterpret_cast<TaskContinuationFunction *>(
                                 &simpleTaskInvokeFunction<T>),
                               nullptr,
                               sizeof(ValueContext<T>));

  auto valueContext =
    static_cast<ValueContext<T>*>(taskAndContext.InitialContext);
  new (&valueContext->Value) T(std::forward<T>(value));
  valueContext->StoredInvokeFn = invokeFn;

  // Forward our owning reference to the task into its execution,
  // causing it to be destroyed when it completes.
  body(taskAndContext.Task);
}

template <class T>
static void withSimpleTask(T &&value,
                           undeduced<InvokeFunctionRef<T>> invokeFn,
                           BodyFunctionRef bodyFn) {
  withSimpleTask(TaskCreateFlags(), std::forward<T>(value), invokeFn, bodyFn);
}

static ExecutorRef createFakeExecutor(uintptr_t value) {
  return ExecutorRef::forDefaultActor(reinterpret_cast<DefaultActor*>(value));
}

} // end anonymous namespace

TEST(TaskStatusTest, basicTasks) {
  AsyncTask *createdTask = nullptr;
  auto createdExecutor = createFakeExecutor(1234);
  bool hasRun = false;

  struct Storage { int value; };
  withSimpleTask(Storage{47},
    [&](ValueContext<Storage> *context) {
    // The task passed in should be the task we created.
    EXPECT_EQ(createdTask, swift_task_getCurrent());

    // The executor passed in should be the executor we created.
    //EXPECT_EQ(createdExecutor, executor);

    // We shouldn't have run yet.
    EXPECT_FALSE(hasRun);

    // The context should've been initialized correctly.
    // (This is really just testing our harness, not the runtime.)
    EXPECT_EQ(47, context->Value.value);

    hasRun = true;
  }, [&](AsyncTask *task) {
    createdTask = task;

    EXPECT_FALSE(hasRun);
    swift_job_run(task, createdExecutor);
    EXPECT_TRUE(hasRun);

    createdTask = nullptr;
  });

  EXPECT_TRUE(hasRun);
  EXPECT_EQ(nullptr, createdTask);
}

TEST(TaskStatusTest, cancellation_simple) {
  struct Storage { int value; };
  withSimpleTask(Storage{47},
    [&](ValueContext<Storage> *context) {
    auto task = swift_task_getCurrent();
    EXPECT_FALSE(swift_task_isCancelled(task));
    swift_task_cancel(task);
    EXPECT_TRUE(swift_task_isCancelled(task));
    swift_task_cancel(task);
    EXPECT_TRUE(swift_task_isCancelled(task));
  }, [&](AsyncTask *task) {
    swift_job_run(task, createFakeExecutor(1234));
  });
}

// Test basic deadline mechanics (other than actually setting up
// something to cancel the task).  Also tests adding and removing
// records quite a bit.
TEST(TaskStatusTest, deadline) {
  struct Storage { int value; };
  withSimpleTask(Storage{47},
    [&](ValueContext<Storage> *context) {
    auto task = swift_task_getCurrent();
    EXPECT_FALSE(swift_task_isCancelled(task));

    TaskDeadline deadlineOne = { 1234 };
    TaskDeadline deadlineTwo = { 2345 };
    DeadlineStatusRecord recordOne(deadlineOne);
    DeadlineStatusRecord recordTwo(deadlineTwo);
    bool result;

    NearestTaskDeadline nearest = swift_task_getNearestDeadline(task);
    EXPECT_EQ(NearestTaskDeadline::None, nearest.ValueKind);

    // Add deadline 1.  Check that we haven't been cancelled yet.
    result = swift_task_addStatusRecord(&recordOne);
    EXPECT_TRUE(result);

    // There should now be an active deadline.
    nearest = swift_task_getNearestDeadline(task);
    EXPECT_EQ(NearestTaskDeadline::Active, nearest.ValueKind);
    EXPECT_EQ(deadlineOne, nearest.Value);

    // Remove deadline 1.  Check that we haven't been cancelled yet.
    result = swift_task_removeStatusRecord(&recordOne);
    EXPECT_TRUE(result);

    // There shouldn't be an active deadline anymore.
    nearest = swift_task_getNearestDeadline(task);
    EXPECT_EQ(NearestTaskDeadline::None, nearest.ValueKind);

    // Add deadline 1, then 2.
    result = swift_task_addStatusRecord(&recordOne);
    EXPECT_TRUE(result);
    result = swift_task_addStatusRecord(&recordTwo);
    EXPECT_TRUE(result);

    // The nearest deadline should be deadline 1.
    nearest = swift_task_getNearestDeadline(task);
    EXPECT_EQ(NearestTaskDeadline::Active, nearest.ValueKind);
    EXPECT_EQ(deadlineOne, nearest.Value);

    // Remove the deadlines.
    result = swift_task_removeStatusRecord(&recordTwo);
    EXPECT_TRUE(result);
    result = swift_task_removeStatusRecord(&recordOne);
    EXPECT_TRUE(result);

    // Add deadline 2, then 1s.
    result = swift_task_addStatusRecord(&recordTwo);
    EXPECT_TRUE(result);

    // In the middle, the nearest deadline should be deadline 2.
    nearest = swift_task_getNearestDeadline(task);
    EXPECT_EQ(NearestTaskDeadline::Active, nearest.ValueKind);
    EXPECT_EQ(deadlineTwo, nearest.Value);

    result = swift_task_addStatusRecord(&recordOne);
    EXPECT_TRUE(result);

    // The nearest deadline should be deadline 1.
    nearest = swift_task_getNearestDeadline(task);
    EXPECT_EQ(NearestTaskDeadline::Active, nearest.ValueKind);
    EXPECT_EQ(deadlineOne, nearest.Value);

    // Remove the deadlines.
    result = swift_task_removeStatusRecord(&recordOne);
    EXPECT_TRUE(result);
    result = swift_task_removeStatusRecord(&recordTwo);
    EXPECT_TRUE(result);

    // Do the same thing with tryAddStatus.
    result = swift_task_tryAddStatusRecord(&recordTwo);
    EXPECT_TRUE(result);
    result = swift_task_tryAddStatusRecord(&recordOne);
    EXPECT_TRUE(result);
    // The nearest deadline should be deadline 1.
    nearest = swift_task_getNearestDeadline(task);
    EXPECT_EQ(NearestTaskDeadline::Active, nearest.ValueKind);
    EXPECT_EQ(deadlineOne, nearest.Value);
    result = swift_task_removeStatusRecord(&recordOne);
    EXPECT_TRUE(result);
    result = swift_task_removeStatusRecord(&recordTwo);
    EXPECT_TRUE(result);

    // Remove out of order.
    result = swift_task_addStatusRecord(&recordTwo);
    EXPECT_TRUE(result);
    result = swift_task_addStatusRecord(&recordOne);
    EXPECT_TRUE(result);
    // The nearest deadline should be deadline 1.
    nearest = swift_task_getNearestDeadline(task);
    EXPECT_EQ(NearestTaskDeadline::Active, nearest.ValueKind);
    EXPECT_EQ(deadlineOne, nearest.Value);
    result = swift_task_removeStatusRecord(&recordTwo);
    EXPECT_TRUE(result);
    result = swift_task_removeStatusRecord(&recordOne);
    EXPECT_TRUE(result);

    // Add deadline 2, then cancel.
    result = swift_task_addStatusRecord(&recordTwo);
    EXPECT_TRUE(result);

    // The nearest deadline should be deadline 2.
    nearest = swift_task_getNearestDeadline(task);
    EXPECT_EQ(NearestTaskDeadline::Active, nearest.ValueKind);
    EXPECT_EQ(deadlineTwo, nearest.Value);

    // Cancel.
    swift_task_cancel(task);
    EXPECT_TRUE(swift_task_isCancelled(task));

    // We should report already cancelled now.
    nearest = swift_task_getNearestDeadline(task);
    EXPECT_EQ(NearestTaskDeadline::AlreadyCancelled, nearest.ValueKind);

    // Add deadline 1.
    result = swift_task_addStatusRecord(&recordOne);
    EXPECT_FALSE(result);

    nearest = swift_task_getNearestDeadline(task);
    EXPECT_EQ(NearestTaskDeadline::AlreadyCancelled, nearest.ValueKind);

    result = swift_task_removeStatusRecord(&recordOne);
    EXPECT_FALSE(result);

    result = swift_task_tryAddStatusRecord(&recordOne);
    EXPECT_FALSE(result);

    result = swift_task_removeStatusRecord(&recordTwo);
    EXPECT_FALSE(result);

    nearest = swift_task_getNearestDeadline(task);
    EXPECT_EQ(NearestTaskDeadline::AlreadyCancelled, nearest.ValueKind);

    EXPECT_TRUE(swift_task_isCancelled(task));
  }, [&](AsyncTask *task) {
    swift_job_run(task, createFakeExecutor(1234));
  });
}
