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

#include "swift/Basic/STLExtras.h"
#include "swift/Runtime/Concurrency.h"
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

static SerialExecutorRef createFakeExecutor(uintptr_t value) {
  return SerialExecutorRef::forDefaultActor(reinterpret_cast<DefaultActor*>(value));
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

TEST(TaskStatusTest, main_actor_simple) {
    auto mainExecutor = swift_task_getMainExecutor();
    bool hasRun = false;
    
    struct Storage { int value; };
    withSimpleTask(Storage{47}, [&](ValueContext<Storage> *context) {
        EXPECT_TRUE(swift_task_isCurrentExecutor(mainExecutor));
    }, [&](AsyncTask *task) {
        swift_job_run(task, mainExecutor);
    });
}
