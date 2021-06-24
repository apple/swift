//===--- Actor.cpp - Unit tests for the actor API -------------------------===//
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

#include "swift/ABI/Actor.h"
#include "swift/Runtime/Concurrency.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/ADT/Optional.h"
#include "gtest/gtest.h"
#include <vector>
#include <tuple>

using namespace swift;

/// The current location.
static unsigned progressIndex = 0;
#define EXPECT_PROGRESS(NUMBER) \
  EXPECT_EQ((unsigned) (NUMBER), progressIndex++)

enum {
  FinishedIndex = 100000
};
static void finishTest() {
  progressIndex = FinishedIndex;
}

static std::vector<Job*> globalQueue;
SWIFT_CC(swift)
static void enqueueGlobal(Job *job,
                          swift_task_enqueueGlobal_original original) {
  assert(job);

  // Check that the job isn't already on the queue.
  for (auto oldJob: globalQueue) {
    EXPECT_NE(job, oldJob);
  }

  // The queue will actually be executed starting from the back.
  // Add the job after (i.e. before in execution order) all jobs
  // with lower priority.
  for (auto i = globalQueue.begin(), e = globalQueue.end(); i != e; ++i) {
    if (job->getPriority() <= (*i)->getPriority()) {
      globalQueue.insert(i, job);
      return;
    }
  }

  // If the job's priority is higher than everything in the existing
  // queue, set it as the new front of the queue.
  globalQueue.push_back(job);
}

static void run(llvm::function_ref<void()> fn) {
  swift_task_enqueueGlobal_hook = &enqueueGlobal;

  progressIndex = 0;

  // Run the setup function.
  fn();

  // The setup function needs to add something to the queue.
  EXPECT_FALSE(globalQueue.empty());

  // Insertion does a priority sort, so we can just process in-order.
  // But that order starts from the back.
  while (!globalQueue.empty()) {
    auto job = globalQueue.back();
    globalQueue.pop_back();

    swift_job_run(job, ExecutorRef::generic());
  }

  EXPECT_EQ(FinishedIndex, progressIndex);

  swift_task_enqueueGlobal_hook = nullptr;
}

namespace {

/// A simple actor.
class TestActor : public DefaultActor {
public:
  TestActor();
  ~TestActor() {
    swift_defaultActor_destroy(this);
  }
  bool HasBeenDestructed = false;
};
static SWIFT_CC(swift)
void destroyTestActor(SWIFT_CONTEXT HeapObject *_object) {
  delete static_cast<TestActor*>(_object);
}
static const FullMetadata<ClassMetadata> TestActorMetadata = {
  { { &destroyTestActor }, { &VALUE_WITNESS_SYM(Bo) } },
  { { nullptr }, ClassFlags::UsesSwiftRefcounting, 0, 0, 0, 0, 0, 0 }
};
TestActor::TestActor() : DefaultActor(&TestActorMetadata) {
  swift_defaultActor_initialize(this);
}

static TestActor *createActor() {
  return new TestActor();
}

/// A very silly template that stores the latest instance of a particular
/// lambda in global storage and then returns a function pointer that
/// matches an async task continuation function signature.
template <class Fn, class Context>
class TaskContinuationFromLambda {
  static llvm::Optional<Fn> lambdaStorage;

  SWIFT_CC(swiftasync)
  static void invoke(SWIFT_ASYNC_CONTEXT AsyncContext *context, SWIFT_CONTEXT HeapObject *) {
    return (*lambdaStorage)(static_cast<Context*>(context));
  }

public:
  static TaskContinuationFunction *get(Fn &&fn) {
    lambdaStorage.emplace(std::move(fn));
    return (TaskContinuationFunction*) &invoke;
  }
};

template <class Fn, class Context>
llvm::Optional<Fn> TaskContinuationFromLambda<Fn, Context>::lambdaStorage;

} // end anonymous namespace

template <class Context, class Fn>
static std::pair<AsyncTask*, Context*>
createTaskWithContext(JobPriority priority, Fn &&fn) {
  auto invoke =
    TaskContinuationFromLambda<Fn, Context>::get(std::move(fn));
  TaskCreateFlags flags;
  flags.setPriority(priority);
  auto pair = swift_task_create_common(flags.getOpaqueValue(),
                                       nullptr,
                                       nullptr,
                                       invoke,
                                       nullptr,
                                       sizeof(Context));
  return std::make_pair(pair.Task,
                        static_cast<Context*>(pair.InitialContext));
}

template <class Fn>
static AsyncTask *createTask(JobPriority priority, Fn &&fn) {
  return createTaskWithContext<AsyncContext, Fn>(priority, std::move(fn))
           .first;
}

template <class Context, class Fn>
static void parkTask(AsyncTask *task, Context *context, Fn &&fn) {
  auto invoke =
    TaskContinuationFromLambda<Fn, Context>::get(std::move(fn));
  task->ResumeTask = invoke;
  task->ResumeContext = context;
}

template <class Context, class Fn>
static TaskContinuationFunction *prepareContinuation(Fn &&fn) {
  return TaskContinuationFromLambda<Fn, Context>::get(std::move(fn));
}

namespace {
template <class... ValueTypes>
class TupleContext : public AsyncContext {
public:
  using TupleType = std::tuple<ValueTypes...>;
  TupleType values;

  template <unsigned N> auto get() { return std::get<N>(values); }
};

/// This extremely silly template repeatedly rotates an argument list
/// until the last argument is first, then returns a pair of that and
/// a tuple of the remaining arguments.
template <unsigned N> struct Decomposer {
  template <class FirstTy, class... OtherTys>
  static auto decompose(FirstTy &&first, OtherTys &&...others) {
    return Decomposer<N-1>::decompose(std::forward<OtherTys>(others)...,
                                      std::forward<FirstTy>(first));
  }
};
template <> struct Decomposer<0> {
  template <class FirstTy, class... OtherTys>
  static auto decompose(FirstTy &&first, OtherTys &&...others) {
    return std::make_pair(std::move(first),
             std::make_tuple(std::forward<OtherTys>(others)...));
  }
};

/// This moderately silly template forwards a template argument pack.
template <class T> struct TupleContextTypeFor;
template <class... EltTys> struct TupleContextTypeFor<std::tuple<EltTys...>> {
  using type = TupleContext<EltTys...>;
};
} // end anonymous namespace

template <class... ArgTypes>
static AsyncTask *createTaskStoring(JobPriority priority,
                                    ArgTypes... args) {
  auto fnAndTuple = Decomposer<sizeof...(args) - 1>::decompose(args...);

  using TupleType = decltype(fnAndTuple.second);
  using ContextType = typename TupleContextTypeFor<TupleType>::type;

  auto taskAndContext =
    createTaskWithContext<ContextType>(priority, std::move(fnAndTuple.first));
  auto ptr = &taskAndContext.second->values;
  new(ptr) TupleType(std::move(fnAndTuple.second));
  return taskAndContext.first;
}

TEST(ActorTest, validateTestHarness) {
  run([] {
    auto task0 = createTask(JobPriority::Background,
      [](AsyncContext *context) SWIFT_CC(swiftasync) {
        EXPECT_PROGRESS(5);
        EXPECT_PROGRESS(6);
        finishTest();
        return context->ResumeParent(context);
      });
    auto task1 = createTask(JobPriority::Default,
      [](AsyncContext *context) SWIFT_CC(swiftasync) {
        EXPECT_PROGRESS(1);
        EXPECT_PROGRESS(2);
        return context->ResumeParent(context);
      });
    auto task2 = createTask(JobPriority::Default,
      [](AsyncContext *context) SWIFT_CC(swiftasync) {
        EXPECT_PROGRESS(3);
        EXPECT_PROGRESS(4);
        return context->ResumeParent(context);
      });

    swift_task_enqueueGlobal(task0);
    swift_task_enqueueGlobal(task1);
    swift_task_enqueueGlobal(task2);
    EXPECT_PROGRESS(0);
  });
}


TEST(ActorTest, actorSwitch) {
  run([] {
    using Context = TupleContext<AsyncTask*, TestActor*>;

    auto actor = createActor();
    auto task0 = createTaskStoring(JobPriority::Default,
                                   (AsyncTask*) nullptr, actor,
      [](Context *context) SWIFT_CC(swiftasync) {
        EXPECT_PROGRESS(1);
        EXPECT_TRUE(swift_task_getCurrentExecutor().isGeneric());
        EXPECT_EQ(nullptr, context->get<0>());
        std::get<0>(context->values) = swift_task_getCurrent();

        auto continuation = prepareContinuation<Context>(
          [](Context *context) SWIFT_CC(swiftasync) {
            EXPECT_PROGRESS(2);
            auto executor = swift_task_getCurrentExecutor();
            EXPECT_FALSE(executor.isGeneric());
            EXPECT_EQ(ExecutorRef::forDefaultActor(context->get<1>()),
                      executor);
            EXPECT_EQ(swift_task_getCurrent(), context->get<0>());
            auto continuation = prepareContinuation<Context>(
              [](Context *context) SWIFT_CC(swiftasync) {
                EXPECT_PROGRESS(3);
                EXPECT_TRUE(swift_task_getCurrentExecutor().isGeneric());
                EXPECT_EQ(swift_task_getCurrent(), context->get<0>());
                finishTest();
                return context->ResumeParent(context);
              });
            return swift_task_switch(context, continuation,
                                     ExecutorRef::generic());
          });
        return swift_task_switch(context, continuation,
                 ExecutorRef::forDefaultActor(context->get<1>()));
      });
    swift_task_enqueueGlobal(task0);
    EXPECT_PROGRESS(0);
  });
}

TEST(ActorTest, actorContention) {
  run([] {
    using Context = TupleContext<AsyncTask*, TestActor*>;
    auto actor = createActor();

    // This test only really works because actors are FIFO.

    auto task0 = createTaskStoring(JobPriority::Default,
                                   (AsyncTask*) nullptr, actor,
      [](Context *context) SWIFT_CC(swiftasync) {
        EXPECT_PROGRESS(1);
        EXPECT_TRUE(swift_task_getCurrentExecutor().isGeneric());
        EXPECT_EQ(nullptr, context->get<0>());
        auto task = swift_task_getCurrent();
        EXPECT_FALSE(task == nullptr);
        std::get<0>(context->values) = task;

        parkTask(task, context,
          [](Context *context) SWIFT_CC(swiftasync) {
            EXPECT_PROGRESS(3);
            auto executor = swift_task_getCurrentExecutor();
            EXPECT_FALSE(executor.isGeneric());
            EXPECT_EQ(ExecutorRef::forDefaultActor(context->get<1>()),
                      executor);
            auto task = swift_task_getCurrent();
            EXPECT_EQ(task, context->get<0>());
            parkTask(task, context,
              [](Context *context) SWIFT_CC(swiftasync) {
                EXPECT_PROGRESS(4);
                EXPECT_TRUE(swift_task_getCurrentExecutor().isGeneric());
                EXPECT_EQ(swift_task_getCurrent(), context->get<0>());
                return context->ResumeParent(context);
              });
            swift_task_enqueue(task, ExecutorRef::generic());
          });

        swift_task_enqueue(task, ExecutorRef::forDefaultActor(context->get<1>()));
      });
    swift_task_enqueueGlobal(task0);

    auto task1 = createTaskStoring(JobPriority::Background,
                                   (AsyncTask*) nullptr, actor,
      [](Context *context) SWIFT_CC(swiftasync) {
        EXPECT_PROGRESS(2);
        auto executor = swift_task_getCurrentExecutor();
        EXPECT_FALSE(executor.isGeneric());
        EXPECT_EQ(ExecutorRef::forDefaultActor(context->get<1>()),
                  executor);
        EXPECT_EQ(nullptr, context->get<0>());
        auto task = swift_task_getCurrent();
        std::get<0>(context->values) = task;

        parkTask(task, context,
          [](Context *context) SWIFT_CC(swiftasync) {
            EXPECT_PROGRESS(5);
            EXPECT_TRUE(swift_task_getCurrentExecutor().isGeneric());
            EXPECT_EQ(swift_task_getCurrent(), context->get<0>());
            finishTest();
            return context->ResumeParent(context);
          });

        swift_task_enqueue(task, ExecutorRef::generic());
      });
    swift_task_enqueue(task1, ExecutorRef::forDefaultActor(actor));

    EXPECT_PROGRESS(0);
  });
}
