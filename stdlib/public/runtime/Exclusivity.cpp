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
#include "../SwiftShims/Visibility.h"
#include "swift/Basic/Lazy.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/ThreadLocalStorage.h"
#include <memory>
#include <inttypes.h>
#include <stdio.h>

// Pick a return-address strategy
#if __GNUC__
#define get_return_address() __builtin_return_address(0)
#elif _MSC_VER
#include <intrin.h>
#define get_return_address() _ReturnAddress()
#else
#error missing implementation for get_return_address
#define get_return_address() ((void*) 0)
#endif

using namespace swift;

bool swift::_swift_disableExclusivityChecking = false;

static const char *getAccessName(ExclusivityFlags flags) {
  switch (flags) {
  case ExclusivityFlags::Read: return "read";
  case ExclusivityFlags::Modify: return "modification";
  default: return "unknown";
  }
}

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

namespace {

/// A single access that we're tracking.
///
/// The following inputs are accepted by the begin_access runtime entry
/// point. This table show the action performed by the current runtime to
/// convert those inputs into stored fields in the Access scratch buffer.
///
/// Pointer | Runtime     | Access | PC    | Reported| Access
/// Argument| Behavior    | Pointer| Arg   | PC      | PC
/// -------- ------------- -------- ------- --------- ----------
/// null    | [trap or missing enforcement]
/// nonnull | [nontracked]| null   | null  | caller  | [discard]
/// nonnull | [nontracked]| null   | valid | <same>  | [discard]
/// nonnull | [tracked]   | <same> | null  | caller  | caller
/// nonnull | [tracked]   | <same> | valid | <same>  | <same>
///
/// [nontracked] means that the Access scratch buffer will not be added to the
/// runtime's list of tracked accesses. However, it may be passed to a
/// subsequent call to end_unpaired_access. The null Pointer field then
/// identifies the Access record as nontracked.
///
/// The runtime owns the contents of the scratch buffer, which is allocated by
/// the compiler but otherwise opaque. The runtime may later reuse the Pointer
/// or PC fields or any spare bits for additional flags, and/or a pointer to
/// out-of-line storage.
struct Access {
  void *Pointer;
  void *PC;
  uintptr_t NextAndAction;

  enum : uintptr_t {
    ActionMask = (uintptr_t)ExclusivityFlags::ActionMask,
    NextMask = ~ActionMask
  };

  Access *getNext() const {
    return reinterpret_cast<Access*>(NextAndAction & NextMask);
  }

  void setNext(Access *next) {
    NextAndAction =
      reinterpret_cast<uintptr_t>(next) | (NextAndAction & ActionMask);
  }

  ExclusivityFlags getAccessAction() const {
    return ExclusivityFlags(NextAndAction & ActionMask);
  }

  void initialize(void *pc, void *pointer, Access *next,
                  ExclusivityFlags action) {
    Pointer = pointer;
    PC = pc;
    NextAndAction = reinterpret_cast<uintptr_t>(next) | uintptr_t(action);
  }
};

static_assert(sizeof(Access) <= sizeof(ValueBuffer) &&
              alignof(Access) <= alignof(ValueBuffer),
              "Access doesn't fit in a value buffer!");

/// A set of accesses that we're tracking.  Just a singly-linked list.
class AccessSet {
  Access *Head = nullptr;
public:
  constexpr AccessSet() {}

  bool insert(Access *access, void *pc, void *pointer, ExclusivityFlags flags) {
    auto action = getAccessAction(flags);

    for (Access *cur = Head; cur != nullptr; cur = cur->getNext()) {
      // Ignore accesses to different values.
      if (cur->Pointer != pointer)
        continue;

      // If both accesses are reads, it's not a conflict.
      if (action == ExclusivityFlags::Read &&
          action == cur->getAccessAction())
        continue;

      // Otherwise, it's a conflict.
      reportExclusivityConflict(cur->getAccessAction(), cur->PC,
                                flags, pc, pointer);

      // 0 means no backtrace will be printed.
      fatalError(0, "Fatal access conflict detected.\n");
    }
    if (!isTracking(flags))
      return false;

    // Insert to the front of the array so that remove tends to find it faster.
    access->initialize(pc, pointer, Head, action);
    Head = access;
    return true;
  }

  void remove(Access *access) {
    assert(Head && "removal from empty AccessSet");
    auto cur = Head;
    // Fast path: stack discipline.
    if (cur == access) {
      Head = cur->getNext();
      return;
    }

    Access *last = cur;
    for (cur = cur->getNext(); cur != nullptr;
         last = cur, cur = cur->getNext()) {
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
  void forEach(std::function<void (Access *)> action) {
    for (auto *iter = Head; iter != nullptr; iter = iter->getNext()) {
      action(iter);
    }
  }
#endif
};

class SwiftTLSContext {
public:
  /// The set of tracked accesses.
  AccessSet accessSet;

  // The "implicit" boolean parameter which is passed to a dynamically
  // replaceable function.
  // If true, the original function should be executed instead of the
  // replacement function.
  bool CallOriginalOfReplacedFunction = false;
};

} // end anonymous namespace

// Each of these cases should define a function with this prototype:
//   AccessSets &getAllSets();

#ifdef SWIFT_STDLIB_SINGLE_THREADED_RUNTIME

static SwiftTLSContext &getTLSContext() {
  static SwiftTLSContext TLSContext;
  return TLSContext;
}

#elif SWIFT_TLS_HAS_RESERVED_PTHREAD_SPECIFIC
// Use the reserved TSD key if possible.

static SwiftTLSContext &getTLSContext() {
  SwiftTLSContext *ctx = static_cast<SwiftTLSContext*>(
    SWIFT_THREAD_GETSPECIFIC(SWIFT_RUNTIME_TLS_KEY));
  if (ctx)
    return *ctx;
  
  static OnceToken_t setupToken;
  SWIFT_ONCE_F(setupToken, [](void *) {
    pthread_key_init_np(SWIFT_RUNTIME_TLS_KEY, [](void *pointer) {
      delete static_cast<SwiftTLSContext*>(pointer);
    });
  }, nullptr);
  
  ctx = new SwiftTLSContext();
  SWIFT_THREAD_SETSPECIFIC(SWIFT_RUNTIME_TLS_KEY, ctx);
  return *ctx;
}

#elif __has_feature(cxx_thread_local)
// Second choice is direct language support for thread-locals.

static thread_local SwiftTLSContext TLSContext;

static SwiftTLSContext &getTLSContext() {
  return TLSContext;
}

#else
// Use the platform thread-local data API.

static __swift_thread_key_t createSwiftThreadKey() {
  __swift_thread_key_t key;
  int result = SWIFT_THREAD_KEY_CREATE(&key, [](void *pointer) {
    delete static_cast<SwiftTLSContext*>(pointer);
  });

  if (result != 0) {
    fatalError(0, "couldn't create thread key for exclusivity: %s\n",
               strerror(result));
  }
  return key;
}

static SwiftTLSContext &getTLSContext() {
  static __swift_thread_key_t key = createSwiftThreadKey();

  SwiftTLSContext *ctx = static_cast<SwiftTLSContext*>(SWIFT_THREAD_GETSPECIFIC(key));
  if (!ctx) {
    ctx = new SwiftTLSContext();
    SWIFT_THREAD_SETSPECIFIC(key, ctx);
  }
  return *ctx;
}

#endif

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

  if (!getTLSContext().accessSet.insert(access, pc, pointer, flags))
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

  getTLSContext().accessSet.remove(access);
}

char *swift::swift_getFunctionReplacement(char **ReplFnPtr, char *CurrFn) {
  char *ReplFn = *ReplFnPtr;
  char *RawReplFn = ReplFn;

#if SWIFT_PTRAUTH
  RawReplFn = ptrauth_strip(RawReplFn, ptrauth_key_function_pointer);
#endif
  if (RawReplFn == CurrFn)
    return nullptr;

  SwiftTLSContext &ctx = getTLSContext();
  if (ctx.CallOriginalOfReplacedFunction) {
    ctx.CallOriginalOfReplacedFunction = false;
    return nullptr;
  }
  return ReplFn;
}

char *swift::swift_getOrigOfReplaceable(char **OrigFnPtr) {
  char *OrigFn = *OrigFnPtr;
  getTLSContext().CallOriginalOfReplacedFunction = true;
  return OrigFn;
}

#ifndef NDEBUG

// Dump the accesses that are currently being tracked by the runtime.
//
// This is only intended to be used in the debugger.
void swift::swift_dumpTrackedAccesses() {
  getTLSContext().accessSet.forEach([](Access *a) {
      fprintf(stderr, "Access. Pointer: %p. PC: %p. AccessAction: %s\n",
              a->Pointer, a->PC, getAccessName(a->getAccessAction()));
  });
}

#endif
