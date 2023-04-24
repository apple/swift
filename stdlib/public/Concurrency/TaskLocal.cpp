//===--- TaskLocal.cpp - Task Local Values --------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/ABI/TaskLocal.h"
#include "../CompatibilityOverride/CompatibilityOverride.h"
#include "TaskPrivate.h"
#include "swift/ABI/Actor.h"
#include "swift/ABI/Metadata.h"
#include "swift/ABI/Task.h"
#include "swift/Runtime/Atomic.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/Concurrency.h"
#include "swift/Threading/ThreadLocalStorage.h"
#include "llvm/ADT/PointerIntPair.h"
#include <new>
#include <set>

#if SWIFT_STDLIB_HAS_ASL
#include <asl.h>
#elif defined(__ANDROID__)
#include <android/log.h>
#endif

#if defined(_WIN32)
#include <io.h>
#endif

using namespace swift;

// =============================================================================

/// An extremely silly class which exists to make pointer
/// default-initialization constexpr.
template <class T> struct Pointer {
  T *Value;
  constexpr Pointer() : Value(nullptr) {}
  constexpr Pointer(T *value) : Value(value) {}
  operator T *() const { return Value; }
  T *operator->() const { return Value; }
};

/// THIS IS RUNTIME INTERNAL AND NOT ABI.
class FallbackTaskLocalStorage {
  static SWIFT_THREAD_LOCAL_TYPE(Pointer<TaskLocal::Storage>,
                                 tls_key::concurrency_fallback) Value;

public:
  static void set(TaskLocal::Storage *task) { Value.set(task); }
  static TaskLocal::Storage *get() { return Value.get(); }

  static TaskLocal::Storage *getOrCreate() {
    if (auto storage = FallbackTaskLocalStorage::get()) {
      return storage;
    }
    void *allocation = malloc(sizeof(TaskLocal::Storage));
    auto *freshStorage = new (allocation) TaskLocal::Storage();

    FallbackTaskLocalStorage::set(freshStorage);
    return freshStorage;
  }
};

/// Define the thread-locals.
SWIFT_THREAD_LOCAL_TYPE(Pointer<TaskLocal::Storage>,
                        tls_key::concurrency_fallback)
FallbackTaskLocalStorage::Value;

// ==== ABI --------------------------------------------------------------------

SWIFT_CC(swift)
static void swift_task_localValuePushImpl(const HeapObject *key,
                                              /* +1 */ OpaqueValue *value,
                                              const Metadata *valueType) {
  if (AsyncTask *task = swift_task_getCurrent()) {
    task->localPushValue(key, value, valueType);
    return;
  }

  // no AsyncTask available so we must check the fallback
  TaskLocal::Storage *Local = FallbackTaskLocalStorage::getOrCreate();
  Local->pushValue(/*task=*/nullptr, key, value, valueType);
}

SWIFT_CC(swift)
static OpaqueValue* swift_task_localValueGetImpl(const HeapObject *key) {
  if (AsyncTask *task = swift_task_getCurrent()) {
    // we're in the context of a task and can use the task's storage
    return task->localGetValue(key);
  }

  // no AsyncTask available so we must check the fallback
  if (auto Local = FallbackTaskLocalStorage::get()) {
    return Local->getValue(/*task*/nullptr, key);
  }

  // no value found in task-local or fallback thread-local storage.
  return nullptr;
}

SWIFT_CC(swift)
static void swift_task_localValuePopImpl() {
  if (AsyncTask *task = swift_task_getCurrent()) {
    task->localPop();
    return;
  }

  if (TaskLocal::Storage *Local = FallbackTaskLocalStorage::get()) {
    bool hasRemainingBindings = Local->pop(nullptr);
    if (!hasRemainingBindings) {
      // We clean up eagerly, it may be that this non-swift-concurrency thread
      // never again will use task-locals, and as such we better remove the storage.
      FallbackTaskLocalStorage::set(nullptr);
      free(Local);
    }
    return;
  }

  assert(false && "Attempted to pop value but no task or thread-local storage available!");
}

SWIFT_CC(swift)
static bool swift_task_localStopPushImpl() {
  if (AsyncTask *task = swift_task_getCurrent()) {
    task->localPushStop();
    return true;
  }

  // no AsyncTask available so we must check the fallback
  if (TaskLocal::Storage *Local = FallbackTaskLocalStorage::get()) {
    Local->pushStop(/*task=*/nullptr);
    return true;
  }

  // We are outside of the task, and fallback storage does not exist
  // Don't push anything for performance reasons, but return an indicator
  // to validate stack consistency in swift_task_localStopPopImpl().
  return false;
}

SWIFT_CC(swift)
static void swift_task_localStopPopImpl(bool didPush) {
  if (didPush) {
    return swift_task_localValuePopImpl();
  } else {
    assert(swift_task_getCurrent() == nullptr &&
           FallbackTaskLocalStorage::get() == nullptr);
  }
}

SWIFT_CC(swift)
static void swift_task_localsCopyToImpl(AsyncTask *task) {
  assert(task && "TaskLocal item attempt to copy to null target task!");
  TaskLocal::copyTo(&task->_private().Local, task);
}

void TaskLocal::copyTo(Storage *target, AsyncTask *task) {
  TaskLocal::Storage *Local = nullptr;

  if (AsyncTask *task = swift_task_getCurrent()) {
    Local = &task->_private().Local;
  } else if (auto *storage = FallbackTaskLocalStorage::get()) {
    Local = storage;
  } else {
    // bail out, there are no values to copy
    return;
  }

  Local->copyTo(target, task);
}

// =============================================================================
// ==== Initialization ---------------------------------------------------------

void TaskLocal::Storage::initializeLinkParent(AsyncTask* task,
                                              AsyncTask* parent) {
  assert(!head && "initial task local storage was already initialized");
  assert(parent && "parent must be provided to link to it");
  head = TaskLocal::Item::createParentLink(task, parent);
}

void *TaskLocal::Item::allocate(size_t amountToAllocate, AsyncTask *task) {
  return task ? _swift_task_alloc_specific(task, amountToAllocate)
              : malloc(amountToAllocate);
}

TaskLocal::Item *TaskLocal::Item::createParentLink(AsyncTask *task,
                                                   AsyncTask *parent) {
  auto parentHead = parent->_private().Local.head;
  if (!parentHead) {
    return nullptr;
  }

  if (parentHead->getKind() == ItemKind::ParentLink) {
    // it has no values, and just points to its parent,
    // therefore skip also skip pointing to that parent and point
    // to whichever parent it was pointing to as well, it may be its
    // immediate parent, or some super-parent.
    parentHead = parentHead->getNext();
  }

  size_t amountToAllocate = Item::itemSize(/*valueType*/ nullptr);
  void *allocation = _swift_task_alloc_specific(task, amountToAllocate);
  return ::new (allocation) Item(parentHead, ItemKind::ParentLink);
}

TaskLocal::Item *TaskLocal::Item::createValue(TaskLocal::Item *next,
                                              AsyncTask *task,
                                              const HeapObject *key,
                                              const Metadata *valueType) {
  size_t amountToAllocate = Item::itemSize(valueType);
  void *allocation = allocate(amountToAllocate, task);
  return ::new (allocation) Item(next, key, valueType);
}

TaskLocal::Item *TaskLocal::Item::createStop(TaskLocal::Item *next,
                                             AsyncTask *task) {
  size_t amountToAllocate = Item::itemSize(/*valueType*/ nullptr);
  void *allocation = allocate(amountToAllocate, task);
  return ::new (allocation) Item(next, ItemKind::Stop);
}

void TaskLocal::Item::copyTo(TaskLocal::Storage *target, AsyncTask *task) {
  assert(getKind() == ItemKind::Value);
  auto item = Item::createValue(target->head, task, keyOrKind.key, valueType);
  valueType->vw_initializeWithCopy(item->getStoragePtr(), getStoragePtr());

  /// A `copyTo` may ONLY be invoked BEFORE the task is actually scheduled,
  /// so right now we can safely copy the value into the task without additional
  /// synchronization.
  target->head = item;
}

// =============================================================================
// ==== checks -----------------------------------------------------------------

SWIFT_CC(swift)
static void swift_task_reportIllegalTaskLocalBindingWithinWithTaskGroupImpl(
    const unsigned char *file, uintptr_t fileLength,
    bool fileIsASCII, uintptr_t line) {

  char *message;
  swift_asprintf(
      &message,
      "error: task-local: detected illegal task-local value binding at %.*s:%d.\n"
      "Task-local values must only be set in a structured-context, such as: "
      "around any (synchronous or asynchronous function invocation), "
      "around an 'async let' declaration, or around a 'with(Throwing)TaskGroup(...){ ... }' "
      "invocation. Notably, binding a task-local value is illegal *within the body* "
      "of a withTaskGroup invocation.\n"
      "\n"
      "The following example is illegal:\n\n"
      "    await withTaskGroup(...) { group in \n"
      "        await <task-local>.withValue(1234) {\n"
      "            group.addTask { ... }\n"
      "        }\n"
      "    }\n"
      "\n"
      "And should be replaced by, either: setting the value for the entire group:\n"
      "\n"
      "    // bind task-local for all tasks spawned within the group\n"
      "    await <task-local>.withValue(1234) {\n"
      "        await withTaskGroup(...) { group in\n"
      "            group.addTask { ... }\n"
      "        }\n"
      "    }\n"
      "\n"
      "or, inside the specific task-group child task:\n"
      "\n"
      "    // bind-task-local for only specific child-task\n"
      "    await withTaskGroup(...) { group in\n"
      "        group.addTask {\n"
      "            await <task-local>.withValue(1234) {\n"
      "                ... \n"
      "            }\n"
      "        }\n"
      "\n"
      "        group.addTask { ... }\n"
      "    }\n",
      (int)fileLength, file,
      (int)line);

  if (_swift_shouldReportFatalErrorsToDebugger()) {
    RuntimeErrorDetails details = {
        .version = RuntimeErrorDetails::currentVersion,
        .errorType = "task-local-violation",
        .currentStackDescription = "Task-local bound in illegal context",
        .framesToSkip = 1,
    };
    _swift_reportToDebugger(RuntimeErrorFlagFatal, message, &details);
  }

#if defined(_WIN32)
  #define STDERR_FILENO 2
  _write(STDERR_FILENO, message, strlen(message));
#else
  fputs(message, stderr);
  fflush(stderr);
#endif
#if SWIFT_STDLIB_HAS_ASL
  asl_log(nullptr, nullptr, ASL_LEVEL_ERR, "%s", message);
#elif defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_FATAL, "SwiftRuntime", "%s", message);
#endif

  free(message);
  abort();
}

// =============================================================================
// ==== destroy ----------------------------------------------------------------

void TaskLocal::Item::destroy(AsyncTask *task) {
  // otherwise it was task-local allocated, so we can safely destroy it right away
  if (valueType) {
    valueType->vw_destroy(getStoragePtr());
  }

  // if task is available, we must have used the task allocator to allocate this item,
  // so we must deallocate it using the same. Otherwise, we must have used malloc.
  if (task) _swift_task_dealloc_specific(task, this);
  else free(this);
}

void TaskLocal::Storage::destroy(AsyncTask *task) {
  auto item = head;
  head = nullptr;
  while (item) {
    TaskLocal::Item *next = item->getNext();
    auto kind = item->getKind();
    item->destroy(task);
    if (kind == ItemKind::ParentLink) {
      // we're done here; as we must not proceed into the parent owned values.
      break;
    }
    item = next;
  }
}

// =============================================================================
// ==== Task Local Storage: operations -----------------------------------------

void TaskLocal::Storage::pushValue(AsyncTask *task,
                                   const HeapObject *key,
                                   /* +1 */ OpaqueValue *value,
                                   const Metadata *valueType) {
  assert(value && "Task local value must not be nil");

  auto item = Item::createValue(head, task, key, valueType);
  valueType->vw_initializeWithTake(item->getStoragePtr(), value);
  head = item;
}

void TaskLocal::Storage::pushStop(AsyncTask *task) {
  head = Item::createStop(head, task);
}

bool TaskLocal::Storage::pop(AsyncTask *task) {
  assert(head && "attempted to pop item off empty task-local stack");
  auto old = head;
  head = head->getNext();
  old->destroy(task);

  /// if pointing at not-null next item, there are remaining bindings.
  return head != nullptr;
}

OpaqueValue* TaskLocal::Storage::getValue(AsyncTask *task,
                                          const HeapObject *key) {
  assert(key && "TaskLocal key must not be null.");

  for (auto item = head; item; item = item->getNext()) {
    switch (item->getKind()) {
    case ItemKind::Value:
      if (item->getKey() == key) {
        return item->getStoragePtr();
      }
      break;
    case ItemKind::ParentLink:
      continue;
    case ItemKind::Stop:
      return nullptr;
    }
  }

  return nullptr;
}

void TaskLocal::Storage::copyTo(TaskLocal::Storage *target, AsyncTask *task) {
  assert(target &&
         "Task-local storage must not be null when copying values into it");
  assert(!(target->head) &&
         "Cannot copy to task-local storage when it is already in use");

  // Set of keys for which we already have copied to the new task.
  // We only ever need to copy the *first* encounter of any given key,
  // because it is the most "specific"/"recent" binding and any other binding
  // of a key does not matter for the target task as it will never be able to
  // observe it.
  std::set<const HeapObject*> copied = {};

  for (auto item = head; item; item = item->getNext()) {
    switch (item->getKind()) {
    case ItemKind::Value:
      // we only have to copy an item if it is the most recent binding of a key.
      // i.e. if we've already seen an item for this key, we can skip it.
      if (copied.emplace(item->getKey()).second) {
        item->copyTo(target, task);
      }
      break;
    case ItemKind::ParentLink:
      // Parent links are not re-created when copying
      // Just skip to the next item;
      continue;
    case ItemKind::Stop:
      return;
    }
  }
}

TaskLocal::AdHocScope::AdHocScope(Storage *storage) {
  assert(swift_task_getCurrent() == nullptr &&
         "Cannot use ad-hoc scope with a task");
  oldStorage = FallbackTaskLocalStorage::get();
  FallbackTaskLocalStorage::set(storage);
}

TaskLocal::AdHocScope::~AdHocScope() {
  FallbackTaskLocalStorage::set(oldStorage);
}

TaskLocal::WithResetValuesScope::WithResetValuesScope() {
  didPush = swift_task_localStopPush();
}

TaskLocal::WithResetValuesScope::~WithResetValuesScope() {
  swift_task_localStopPop(didPush);
}

#define OVERRIDE_TASK_LOCAL COMPATIBILITY_OVERRIDE
#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH
