//===--- Debug.h - Swift Runtime debug helpers ------------------*- C++ -*-===//
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
// Random debug support
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_RUNTIME_DEBUG_HELPERS_H
#define SWIFT_RUNTIME_DEBUG_HELPERS_H

#include "swift/Runtime/Config.h"
#include "swift/Basic/Unreachable.h"
#include <atomic>
#include <cstdarg>
#include <functional>
#include <stdint.h>

#ifdef SWIFT_HAVE_CRASHREPORTERCLIENT

#define CRASHREPORTER_ANNOTATIONS_VERSION 5
#define CRASHREPORTER_ANNOTATIONS_SECTION "__crash_info"

struct crashreporter_annotations_t {
  uint64_t version;          // unsigned long
  uint64_t message;          // char *
  uint64_t signature_string; // char *
  uint64_t backtrace;        // char *
  uint64_t message2;         // char *
  uint64_t thread;           // uint64_t
  uint64_t dialog_mode;      // unsigned int
  uint64_t abort_cause;      // unsigned int
};

extern "C" {
SWIFT_RUNTIME_LIBRARY_VISIBILITY
extern struct crashreporter_annotations_t gCRAnnotations;
}

SWIFT_RUNTIME_ATTRIBUTE_ALWAYS_INLINE
static inline void CRSetCrashLogMessage(const char *message) {
  gCRAnnotations.message = reinterpret_cast<uint64_t>(message);
}

SWIFT_RUNTIME_ATTRIBUTE_ALWAYS_INLINE
static inline const char *CRGetCrashLogMessage() {
  return reinterpret_cast<const char *>(gCRAnnotations.message);
}

#else

SWIFT_RUNTIME_ATTRIBUTE_ALWAYS_INLINE
static inline void CRSetCrashLogMessage(const char *) {}

#endif

namespace swift {

// Duplicated from Metadata.h. We want to use this header
// in places that cannot themselves include Metadata.h.
struct InProcess;
template <typename Runtime> struct TargetMetadata;
using Metadata = TargetMetadata<InProcess>;

// swift::crash() halts with a crash log message, 
// but otherwise tries not to disturb register state.

SWIFT_RUNTIME_ATTRIBUTE_NORETURN
SWIFT_RUNTIME_ATTRIBUTE_ALWAYS_INLINE // Minimize trashed registers
static inline void crash(const char *message) {
  CRSetCrashLogMessage(message);

  SWIFT_RUNTIME_BUILTIN_TRAP;
  swift_unreachable("Expected compiler to crash.");
}

// swift::fatalErrorv() halts with a crash log message,
// but makes no attempt to preserve register state.
SWIFT_RUNTIME_ATTRIBUTE_NORETURN
SWIFT_VFORMAT(2)
extern void fatalErrorv(uint32_t flags, const char *format, va_list args);

// swift::fatalError() halts with a crash log message,
// but makes no attempt to preserve register state.
SWIFT_RUNTIME_ATTRIBUTE_NORETURN
SWIFT_FORMAT(2, 3)
extern void
fatalError(uint32_t flags, const char *format, ...);

/// swift::warning() emits a warning from the runtime.
extern void
SWIFT_VFORMAT(2)
warningv(uint32_t flags, const char *format, va_list args);

/// swift::warning() emits a warning from the runtime.
extern void
SWIFT_FORMAT(2, 3)
warning(uint32_t flags, const char *format, ...);

// swift_dynamicCastFailure halts using fatalError()
// with a description of a failed cast's types.
SWIFT_RUNTIME_ATTRIBUTE_NORETURN
void
swift_dynamicCastFailure(const Metadata *sourceType,
                         const Metadata *targetType,
                         const char *message = nullptr);

// swift_dynamicCastFailure halts using fatalError()
// with a description of a failed cast's types.
SWIFT_RUNTIME_ATTRIBUTE_NORETURN
void
swift_dynamicCastFailure(const void *sourceType, const char *sourceName, 
                         const void *targetType, const char *targetName, 
                         const char *message = nullptr);

SWIFT_RUNTIME_EXPORT
void swift_reportError(uint32_t flags, const char *message);

SWIFT_RUNTIME_EXPORT
void swift_reportWarning(uint32_t flags, const char *message);

// Halt due to an overflow in swift_retain().
SWIFT_RUNTIME_ATTRIBUTE_NORETURN SWIFT_RUNTIME_ATTRIBUTE_NOINLINE
void swift_abortRetainOverflow();

// Halt due to reading an unowned reference to a dead object.
SWIFT_RUNTIME_ATTRIBUTE_NORETURN SWIFT_RUNTIME_ATTRIBUTE_NOINLINE
void swift_abortRetainUnowned(const void *object);

// Halt due to an overflow in swift_unownedRetain().
SWIFT_RUNTIME_ATTRIBUTE_NORETURN SWIFT_RUNTIME_ATTRIBUTE_NOINLINE
void swift_abortUnownedRetainOverflow();

// Halt due to an overflow in incrementWeak().
SWIFT_RUNTIME_ATTRIBUTE_NORETURN SWIFT_RUNTIME_ATTRIBUTE_NOINLINE
void swift_abortWeakRetainOverflow();

// Halt due to enabling an already enabled dynamic replacement().
SWIFT_RUNTIME_ATTRIBUTE_NORETURN SWIFT_RUNTIME_ATTRIBUTE_NOINLINE
void swift_abortDynamicReplacementEnabling();

// Halt due to disabling an already disabled dynamic replacement().
SWIFT_RUNTIME_ATTRIBUTE_NORETURN SWIFT_RUNTIME_ATTRIBUTE_NOINLINE
void swift_abortDynamicReplacementDisabling();

// Halt due to trying to use unicode data on platforms that don't have it.
SWIFT_RUNTIME_ATTRIBUTE_NORETURN SWIFT_RUNTIME_ATTRIBUTE_NOINLINE
void swift_abortDisabledUnicodeSupport();

/// This function dumps one line of a stack trace. It is assumed that \p framePC
/// is the address of the stack frame at index \p index. If \p shortOutput is
/// true, this functions prints only the name of the symbol and offset, ignores
/// \p index argument and omits the newline.
void dumpStackTraceEntry(unsigned index, void *framePC,
                         bool shortOutput = false);

SWIFT_RUNTIME_ATTRIBUTE_NOINLINE
bool withCurrentBacktrace(std::function<void(void **, int)> call);

SWIFT_RUNTIME_ATTRIBUTE_NOINLINE
void printCurrentBacktrace(unsigned framesToSkip = 1);

/// Debugger breakpoint ABI. This structure is passed to the debugger (and needs
/// to be stable) and describes extra information about a fatal error or a
/// non-fatal warning, which should be logged as a runtime issue. Please keep
/// all integer values pointer-sized.
struct RuntimeErrorDetails {
  static const uintptr_t currentVersion = 2;

  // ABI version, needs to be set to "currentVersion".
  uintptr_t version;

  // A short hyphenated string describing the type of the issue, e.g.
  // "precondition-failed" or "exclusivity-violation".
  const char *errorType;

  // Description of the current thread's stack position.
  const char *currentStackDescription;

  // Number of frames in the current stack that should be ignored when reporting
  // the issue (excluding the reportToDebugger/_swift_runtime_on_report frame).
  // The remaining top frame should point to user's code where the bug is.
  uintptr_t framesToSkip;

  // Address of some associated object (if there's any).
  const void *memoryAddress;

  // A structure describing an extra thread (and its stack) that is related.
  struct Thread {
    const char *description;
    uint64_t threadID;
    uintptr_t numFrames;
    void **frames;
  };

  // Number of extra threads (excluding the current thread) that are related,
  // and the pointer to the array of extra threads.
  uintptr_t numExtraThreads;
  Thread *threads;

  // Describes a suggested fix-it. Text in [startLine:startColumn,
  // endLine:endColumn) is to be replaced with replacementText.
  struct FixIt {
    const char *filename;
    uintptr_t startLine;
    uintptr_t startColumn;
    uintptr_t endLine;
    uintptr_t endColumn;
    const char *replacementText;
  };

  // Describes some extra information, possible with fix-its, about the current
  // runtime issue.
  struct Note {
    const char *description;
    uintptr_t numFixIts;
    FixIt *fixIts;
  };

  // Number of suggested fix-its, and the pointer to the array of them.
  uintptr_t numFixIts;
  FixIt *fixIts;

  // Number of related notes, and the pointer to the array of them.
  uintptr_t numNotes;
  Note *notes;
};

enum: uintptr_t {
  RuntimeErrorFlagNone = 0,
  RuntimeErrorFlagFatal = 1 << 0
};

/// Debugger hook. Calling this stops the debugger with a message and details
/// about the issues. Called by overlays.
SWIFT_RUNTIME_STDLIB_SPI
void _swift_reportToDebugger(uintptr_t flags, const char *message,
                             RuntimeErrorDetails *details = nullptr);

SWIFT_RUNTIME_STDLIB_SPI
bool _swift_reportFatalErrorsToDebugger;

SWIFT_RUNTIME_STDLIB_SPI
bool _swift_shouldReportFatalErrorsToDebugger();

SWIFT_RUNTIME_STDLIB_SPI
bool _swift_debug_metadataAllocationIterationEnabled;

SWIFT_RUNTIME_STDLIB_SPI
const void * const _swift_debug_allocationPoolPointer;

SWIFT_RUNTIME_STDLIB_SPI
std::atomic<const void *> _swift_debug_metadataAllocationBacktraceList;

SWIFT_RUNTIME_STDLIB_SPI
const void * const _swift_debug_protocolConformanceStatePointer;

SWIFT_RUNTIME_STDLIB_SPI
const uint64_t _swift_debug_multiPayloadEnumPointerSpareBitsMask;

// namespace swift
}

#endif // SWIFT_RUNTIME_DEBUG_HELPERS_H
