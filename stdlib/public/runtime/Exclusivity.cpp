//===--- Exclusivity.cpp - Exclusivity tracking ---------------------------===//
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
//
// This implements the runtime support for dynamically tracking exclusivity.
//
//===----------------------------------------------------------------------===//

// NOTE: This should really be applied in the CMakeLists.txt.  However, we do
// not have a way to currently specify that at the target specific level yet.

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VCEXTRALEAN
#endif

#include "swift/Runtime/Exclusivity.h"
#include "swift/shims/Visibility.h"
#include "SwiftTLSContext.h"
#include "swift/Basic/Lazy.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/EnvironmentVariables.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Threading/ThreadLocalStorage.h"
#include <cinttypes>
#include <cstdio>
#include <memory>

// Pick a return-address strategy
#if defined(__wasm__)
// Wasm can't access call frame for security purposes
#define get_return_address() ((void*) 0)
#elif __GNUC__
#define get_return_address() __builtin_return_address(0)
#elif _MSC_VER
#include <intrin.h>
#define get_return_address() _ReturnAddress()
#else
#error missing implementation for get_return_address
#define get_return_address() ((void*) 0)
#endif

using namespace swift;
using namespace swift::runtime;

bool swift::_swift_disableExclusivityChecking = false;

static const char *getAccessName(ExclusivityFlags flags) {
  switch (flags) {
  case ExclusivityFlags::Read: return "read";
  case ExclusivityFlags::Modify: return "modification";
  default: return "unknown";
  }
}

// In asserts builds if the environment variable
// SWIFT_DEBUG_RUNTIME_EXCLUSIVITY_LOGGING is set, emit logging information.
#ifndef NDEBUG

static inline bool isExclusivityLoggingEnabled() {
  return runtime::environment::SWIFT_DEBUG_RUNTIME_EXCLUSIVITY_LOGGING();
}

static inline void _flockfile_stderr() {
#if defined(_WIN32)
  _lock_file(stderr);
#elif defined(__wasi__)
  // FIXME: WebAssembly/WASI doesn't support file locking yet (https://github.com/apple/swift/issues/54533).
#else
  flockfile(stderr);
#endif
}

static inline void _funlockfile_stderr() {
#if defined(_WIN32)
  _unlock_file(stderr);
#elif defined(__wasi__)
  // FIXME: WebAssembly/WASI doesn't support file locking yet (https://github.com/apple/swift/issues/54533).
#else
  funlockfile(stderr);
#endif
}

/// Used to ensure that logging printfs are deterministic.
static inline void withLoggingLock(std::function<void()> func) {
  assert(isExclusivityLoggingEnabled() &&
         "Should only be called if exclusivity logging is enabled!");

  _flockfile_stderr();
  func();
  fflush(stderr);
  _funlockfile_stderr();
}

#endif

SWIFT_ALWAYS_INLINE
static void reportExclusivityConflict(ExclusivityFlags oldAction, void *oldPC,
                                      ExclusivityFlags newFlags, void *newPC,
                                      void *pointer) {
  constexpr unsigned maxMessageLength = 100;
  constexpr unsigned maxAccessDescriptionLength = 50;
  char message[maxMessageLength];
  snprintf(message, sizeof(message),
           "Simultaneous accesses to 0x%" PRIxPTR ", but modification requires "
           "exclusive access",
           reinterpret_cast<uintptr_t>(pointer));
  fprintf(stderr, "%s.\n", message);

  char oldAccess[maxAccessDescriptionLength];
  snprintf(oldAccess, sizeof(oldAccess),
           "Previous access (a %s) started at", getAccessName(oldAction));
  fprintf(stderr, "%s ", oldAccess);
  if (oldPC) {
    dumpStackTraceEntry(0, oldPC, /*shortOutput=*/true);
    fprintf(stderr, " (0x%" PRIxPTR ").\n", reinterpret_cast<uintptr_t>(oldPC));
  } else {
    fprintf(stderr, "<unknown>.\n");
  }

  char newAccess[maxAccessDescriptionLength];
  snprintf(newAccess, sizeof(newAccess), "Current access (a %s) started at",
           getAccessName(getAccessAction(newFlags)));
  fprintf(stderr, "%s:\n", newAccess);
  // The top frame is in swift_beginAccess, don't print it.
  constexpr unsigned framesToSkip = 1;
  printCurrentBacktrace(framesToSkip);

  RuntimeErrorDetails::Thread secondaryThread = {
    .description = oldAccess,
    .numFrames = 1,
    .frames = &oldPC
  };
  RuntimeErrorDetails details = {
    .version = RuntimeErrorDetails::currentVersion,
    .errorType = "exclusivity-violation",
    .currentStackDescription = newAccess,
    .framesToSkip = framesToSkip,
    .memoryAddress = pointer,
    .numExtraThreads = 1,
    .threads = &secondaryThread
  };
  _swift_reportToDebugger(RuntimeErrorFlagFatal, message, &details);
}

bool AccessSet::insert(Access *access, void *pc, void *pointer,
                       ExclusivityFlags flags) {
#ifndef NDEBUG
  if (isExclusivityLoggingEnabled()) {
    withLoggingLock(
        [&]() { fprintf(stderr, "Inserting new access: %p\n", access); });
  }
#endif
  auto action = getAccessAction(flags);

  for (Access *cur = Head; cur != nullptr; cur = cur->getNext()) {
    // Ignore accesses to different values.
    if (cur->Pointer != pointer)
      continue;

    // If both accesses are reads, it's not a conflict.
    if (action == ExclusivityFlags::Read && action == cur->getAccessAction())
      continue;

    // Otherwise, it's a conflict.
    reportExclusivityConflict(cur->getAccessAction(), cur->PC, flags, pc,
                              pointer);

    // 0 means no backtrace will be printed.
    fatalError(0, "Fatal access conflict detected.\n");
  }
  if (!isTracking(flags)) {
#ifndef NDEBUG
    if (isExclusivityLoggingEnabled()) {
      withLoggingLock([&]() { fprintf(stderr, "  Not tracking!\n"); });
    }
#endif
    return false;
  }

  // Insert to the front of the array so that remove tends to find it faster.
  access->initialize(pc, pointer, Head, action);
  Head = access;
#ifndef NDEBUG
  if (isExclusivityLoggingEnabled()) {
    withLoggingLock([&]() {
      fprintf(stderr, "  Tracking!\n");
      swift_dumpTrackedAccesses();
    });
  }
#endif
  return true;
}

void AccessSet::remove(Access *access) {
  assert(Head && "removal from empty AccessSet");
#ifndef NDEBUG
  if (isExclusivityLoggingEnabled()) {
    withLoggingLock(
        [&]() { fprintf(stderr, "Removing access: %p\n", access); });
  }
#endif
  auto cur = Head;
  // Fast path: stack discipline.
  if (cur == access) {
    Head = cur->getNext();
    return;
  }

  Access *last = cur;
  for (cur = cur->getNext(); cur != nullptr; last = cur, cur = cur->getNext()) {
    assert(last->getNext() == cur);
    if (cur == access) {
      last->setNext(cur->getNext());
      return;
    }
  }

  swift_unreachable("access not found in set");
}

#ifndef NDEBUG
/// Only available with asserts. Intended to be used with
/// swift_dumpTrackedAccess().
void AccessSet::forEach(std::function<void(Access *)> action) {
  for (auto *iter = Head; iter != nullptr; iter = iter->getNext()) {
    action(iter);
  }
}
#endif

// Each of these cases should define a function with this prototype:
//   AccessSets &getAllSets();

/// Begin tracking a dynamic access.
///
/// This may cause a runtime failure if an incompatible access is
/// already underway.
void swift::swift_beginAccess(void *pointer, ValueBuffer *buffer,
                              ExclusivityFlags flags, void *pc) {
  assert(pointer && "beginning an access on a null pointer?");

  Access *access = reinterpret_cast<Access*>(buffer);

  // If exclusivity checking is disabled, record in the access buffer that we
  // didn't track anything. pc is currently undefined in this case.
  if (_swift_disableExclusivityChecking) {
    access->Pointer = nullptr;
    return;
  }

  // If the provided `pc` is null, then the runtime may override it for
  // diagnostics.
  if (!pc)
    pc = get_return_address();

  if (!SwiftTLSContext::get().accessSet.insert(access, pc, pointer, flags))
    access->Pointer = nullptr;
}

/// End tracking a dynamic access.
void swift::swift_endAccess(ValueBuffer *buffer) {
  Access *access = reinterpret_cast<Access*>(buffer);
  auto pointer = access->Pointer;

  // If the pointer in the access is null, we must've declined
  // to track it because exclusivity tracking was disabled.
  if (!pointer) {
    return;
  }

  SwiftTLSContext::get().accessSet.remove(access);
}

#ifndef NDEBUG

// Dump the accesses that are currently being tracked by the runtime.
//
// This is only intended to be used in the debugger.
void swift::swift_dumpTrackedAccesses() {
  auto &accessSet = SwiftTLSContext::get().accessSet;
  if (!accessSet) {
    fprintf(stderr, "        No Accesses.\n");
    return;
  }
  accessSet.forEach([](Access *a) {
    fprintf(stderr, "        Access. Pointer: %p. PC: %p. AccessAction: %s\n",
            a->Pointer, a->PC, getAccessName(a->getAccessAction()));
  });
}

#endif

// Bring in the concurrency-specific exclusivity code.
#include "ConcurrencyExclusivity.inc"
