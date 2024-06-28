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

#if 0
#define SWIFT_TASK_LOCAL_DEBUG_LOG_ENABLED 1
#define SWIFT_TASK_LOCAL_DEBUG_LOG(key, fmt, ...)                       \
fprintf(stderr, "[%s:%d][task:%p key:%p] (%s) " fmt "\n",               \
      __FILE__, __LINE__,                                               \
      swift_task_getCurrent(),                                          \
      key,                                                              \
      __FUNCTION__,                                                     \
      __VA_ARGS__)

#else
#define SWIFT_TASK_LOCAL_DEBUG_LOG_ENABLED 0
#define SWIFT_TASK_LOCAL_DEBUG_LOG(key, fmt, ...) (void)0
#endif

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
  SWIFT_TASK_LOCAL_DEBUG_LOG(key, "push value: %p", value);
  if (AsyncTask *task = swift_task_getCurrent()) {
    task->localValuePush(key, value, valueType);
    return;
  }

  // no AsyncTask available so we must check the fallback
  TaskLocal::Storage *Local = nullptr;
  if (auto storage = FallbackTaskLocalStorage::get()) {
    Local = storage;
  } else {
    void *allocation = malloc(sizeof(TaskLocal::Storage));
    auto *freshStorage = new(allocation) TaskLocal::Storage();

    FallbackTaskLocalStorage::set(freshStorage);
    Local = freshStorage;
  }

  Local->pushValue(/*task=*/nullptr, key, value, valueType);
}

SWIFT_CC(swift)
static OpaqueValue* swift_task_localValueGetImpl(const HeapObject *key) {
  if (AsyncTask *task = swift_task_getCurrent()) {
    // we're in the context of a task and can use the task's storage
    auto value = task->localValueGet(key);
    SWIFT_TASK_LOCAL_DEBUG_LOG(key, "got value: %p", value);
    return value;
  }

  // no AsyncTask available so we must check the fallback
  if (auto Local = FallbackTaskLocalStorage::get()) {
    auto value = Local->getValue(/*task*/nullptr, key);
    SWIFT_TASK_LOCAL_DEBUG_LOG(key, "got value: %p", value);
    return value;
  }

  // no value found in task-local or fallback thread-local storage.
  SWIFT_TASK_LOCAL_DEBUG_LOG(key, "got no value: %d", 0);
  return nullptr;
}

SWIFT_CC(swift)
static void swift_task_localValuePopImpl() {
  if (AsyncTask *task = swift_task_getCurrent()) {
    task->localValuePop();
    return;
  }

  if (TaskLocal::Storage *Local = FallbackTaskLocalStorage::get()) {
    bool hasRemainingBindings = Local->popValue(nullptr);
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
static void swift_task_localsCopyToImpl(AsyncTask *target) {
  if (auto Current = TaskLocal::Storage::getCurrent(swift_task_getCurrent())) {
    Current->copyTo(target);
  }
}

// =============================================================================
// ==== Initialization ---------------------------------------------------------

TaskLocal::Storage*
TaskLocal::Storage::getCurrent(AsyncTask *current) {
  if (current) {
    return &current->_private().Local;
  } else if (auto *storage = FallbackTaskLocalStorage::get()) {
    return storage;
  }

  return nullptr;
}

void TaskLocal::Storage::initializeLinkParent(AsyncTask* task,
                                              AsyncTask* parent) {
  assert(!head && "initial task local storage was already initialized");
  assert(parent && "parent must be provided to link to it");
  head = TaskLocal::Item::createParentLink(task, parent);
}

TaskLocal::Item*
TaskLocal::Item::createParentLink(AsyncTask *task, AsyncTask *parent) {
  size_t amountToAllocate = Item::itemSize(/*valueType*/nullptr);
  void *allocation = _swift_task_alloc_specific(task, amountToAllocate);
  Item *item = new(allocation) Item();

  auto parentHead = parent->_private().Local.head;
  if (parentHead) {
    if (parentHead->isEmpty()) {
      switch (parentHead->getNextLinkType()) {
        case NextLinkType::IsParent:
          // it has no values, and just points to its parent,
          // therefore skip also skip pointing to that parent and point
          // to whichever parent it was pointing to as well, it may be its
          // immediate parent, or some super-parent.
          item->next = reinterpret_cast<uintptr_t>(parentHead->getNext()) |
                       static_cast<uintptr_t>(NextLinkType::IsParent);
          break;
        case NextLinkType::IsNext:
        case NextLinkType::IsNextCreatedInTaskGroupBody:
          if (parentHead->getNext()) {
            assert(false && "empty taskValue head in parent task, yet parent's 'head' is `IsNext`, "
                            "this should not happen, as it implies the parent must have stored some value.");
          } else {
            // is terminal pointer
            item->next = reinterpret_cast<uintptr_t>(parentHead->getNext());
          }
          break;
      }
    } else {
      item->next = reinterpret_cast<uintptr_t>(parentHead) |
                   static_cast<uintptr_t>(NextLinkType::IsParent);
    }
  } else {
    item->next = reinterpret_cast<uintptr_t>(parentHead);
  }

  return item;
}

TaskLocal::Item*
TaskLocal::Item::createLink(AsyncTask *task,
                            const HeapObject *key,
                            const Metadata *valueType,
                            bool inTaskGroupBody) {
  size_t amountToAllocate = Item::itemSize(valueType);
  void *allocation = task ? _swift_task_alloc_specific(task, amountToAllocate)
                          : malloc(amountToAllocate);
  Item *item = ::new (allocation) Item(key, valueType);

  auto next = task ? task->_private().Local.head
                   : FallbackTaskLocalStorage::get()->head;
  item->next = reinterpret_cast<uintptr_t>(next) |
               static_cast<uintptr_t>(
                   inTaskGroupBody ? NextLinkType::IsNextCreatedInTaskGroupBody
                                   : NextLinkType::IsNext);

  return item;
}

TaskLocal::Item*
TaskLocal::Item::createLink(AsyncTask *task,
                            const HeapObject *key,
                            const Metadata *valueType) {
  return createLink(task, key, valueType, /*=inTaskGroupBody=*/false);
}


TaskLocal::Item*
TaskLocal::Item::createLinkInTaskGroup(AsyncTask *task,
                                       const HeapObject *key,
                                       const Metadata *valueType) {
  return createLink(task, key, valueType, /*=inTaskGroupBody=*/true);
}


TaskLocal::Item*
TaskLocal::Item::copyTo(AsyncTask *target) {
  assert(target && "TaskLocal item attempt to copy to null target task!");

  // 'parent' pointers are signified by null valueType.
  // We must not copy parent pointers, but rather perform a deep copy of all values,
  // as such, we skip parent pointers here entirely.
  if (isParentPointer())
    return nullptr;

  auto item = Item::createLink(target, key, valueType);
  valueType->vw_initializeWithCopy(item->getStoragePtr(), getStoragePtr());

  /// A `copyTo` may ONLY be invoked BEFORE the task is actually scheduled,
  /// so right now we can safely copy the value into the task without additional
  /// synchronization.
  target->_private().Local.head = item;

  return item;
}

// =============================================================================
// ==== checks -----------------------------------------------------------------

/// UNUSED: This is effectively not used anymore by new runtimes because we will
/// defensively copy in this situation since Swift 6, rather than crash as a
/// means of defence.
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
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  asl_log(nullptr, nullptr, ASL_LEVEL_ERR, "%s", message);
#pragma clang diagnostic pop
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
  TaskLocal::Item *next;
  while (item) {
    auto linkType = item->getNextLinkType();
    switch (linkType) {
    case TaskLocal::NextLinkType::IsNext: {
      next = item->getNext();
      item->destroy(task);
      item = next;
      break;
    }
    case TaskLocal::NextLinkType::IsNextCreatedInTaskGroupBody:
    case TaskLocal::NextLinkType::IsParent: {
      // we're done here; as we must not proceed into the parent owned values.
      // we do have to destroy the item pointing at the parent/edge itself though.
      item->destroy(task);
      return;
    }
    }
  }
}

// =============================================================================
// ==== Task Local Storage: operations -----------------------------------------

void TaskLocal::Storage::pushValue(AsyncTask *task,
                                   const HeapObject *key,
                                   /* +1 */ OpaqueValue *value,
                                   const Metadata *valueType) {
  assert(value && "Task local value must not be nil");
  assert(swift_task_getCurrent() == task &&
         "must only be pushing task locals onto current task");

  // Why it matters to detect if we're pushing a value in a task group body:
  // We specifically need to prevent this pattern:
  //
  //    $number.withValue(0xBAADF00D) { // push
  //      group.addTask { ... }
  //    } // pop! BOOM!
  //
  // because the end of the withValue scope would pop the value,
  // and thus if the child task didn't copy the value, it'd refer to a bad
  // memory location at this point.
  bool inTaskGroupBody = swift_task_hasTaskGroupStatusRecord();

  TaskLocal::Item* item = Item::createLink(
      task, key, valueType,
      inTaskGroupBody);

  valueType->vw_initializeWithTake(item->getStoragePtr(), value);
  head = item;
  SWIFT_TASK_LOCAL_DEBUG_LOG(item->key, "Created link item:%p, in group body:%d",
                             item, inTaskGroupBody);
}

bool TaskLocal::Storage::popValue(AsyncTask *task) {
  assert(head && "attempted to pop value off empty task-local stack");
  SWIFT_TASK_LOCAL_DEBUG_LOG(head->key, "pop local item:%p, value:%p", head, head->getStoragePtr());

  auto old = head;
  head = head->getNext();
  old->destroy(task);

  /// if pointing at not-null next item, there are remaining bindings.
  return head != nullptr;
}

std::optional<TaskLocal::NextLinkType>
TaskLocal::Storage::peekHeadLinkType() const {
  if (!head)
    return {};

  return head->getNextLinkType();
}

OpaqueValue* TaskLocal::Storage::getValue(AsyncTask *task,
                                          const HeapObject *key) {
  assert(key && "TaskLocal key must not be null.");

  auto item = head;
  while (item) {
    if (item->key == key) {
      return item->getStoragePtr();
    }

    item = item->getNext();
  }

  return nullptr;
}


void TaskLocal::Storage::copyTo(AsyncTask *target) {
  assert(target && "task must not be null when copying values into it");
  assert(!(target->_private().Local.head) &&
      "Task must not have any task-local values bound before copying into it");

  // Set of keys for which we already have copied to the new task.
  // We only ever need to copy the *first* encounter of any given key,
  // because it is the most "specific"/"recent" binding and any other binding
  // of a key does not matter for the target task as it will never be able to
  // observe it.
  std::set<const HeapObject*> copied = {};

  auto item = head;
  while (item) {
    // we only have to copy an item if it is the most recent binding of a key.
    // i.e. if we've already seen an item for this key, we can skip it.
    if (copied.emplace(item->key).second) {
      item->copyTo(target);
    }

    item = item->getNext();
  }
}

// TODO(concurrency): This can be optimized to copy only from the CURRENT group,
//  but we need to detect this, e.g. by more flags in the items made from a group?
void TaskLocal::Storage::copyToOnlyOnlyFromCurrentGroup(AsyncTask *target) {
  assert(target && "task must not be null when copying values into it");
  assert(!(target->_private().Local.head) &&
      "Task must not have any task-local values bound before copying into it");

  // Set of keys for which we already have copied to the new task.
  // We only ever need to copy the *first* encounter of any given key,
  // because it is the most "specific"/"recent" binding and any other binding
  // of a key does not matter for the target task as it will never be able to
  // observe it.
  std::set<const HeapObject*> copied = {};

  auto item = head;
  TaskLocal::Item *copiedHead = nullptr;
  while (item) {
    // we only have to copy an item if it is the most recent binding of a key.
    // i.e. if we've already seen an item for this key, we can skip it.
    if (copied.emplace(item->key).second) {

      if (!item->isNextLinkPointerCreatedInTaskGroupBody() && copiedHead) {
        // The next item is not the "risky one" so we can directly link to it,
        // as we would have within normal child task relationships. E.g. this is
        // a parent or next pointer to a "safe" (withValue { withTaskGroup { ... } })
        // binding, so we re-link our current head to point at this item.
        copiedHead->relinkTaskGroupLocalHeadToSafeNext(item);
        break;
      }

      auto copy = item->copyTo(target);
      if (!copiedHead) {
        copiedHead = copy;
      }

      // If we didn't copy an item, e.g. because it was a pointer to parent,
      // break out of the loop and keep pointing at parent still.
      if (!copy) {
        break;
      }
    } else {
      SWIFT_TASK_LOCAL_DEBUG_LOG(item->key, "skip copy, already copied most recent value, value was [%p]", item->getStoragePtr());
    }

    item = item->getNext();
  }
}

#define OVERRIDE_TASK_LOCAL COMPATIBILITY_OVERRIDE
#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH
