//===--- Errors.cpp - Error reporting utilities ---------------------------===//
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
// Utilities for reporting errors to stderr, system console, and crash logs.
//
//===----------------------------------------------------------------------===//

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <mutex>
#endif

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#endif
#include <stdarg.h>

#include "ImageInspection.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/Portability.h"
#include "swift/Runtime/Win32.h"
#include "swift/Threading/Errors.h"
#include "swift/Threading/Mutex.h"
#include "llvm/ADT/StringRef.h"

#if defined(_MSC_VER)
#include <DbgHelp.h>
#else
#include <cxxabi.h>
#endif

#if __has_include(<execinfo.h>)
#include <execinfo.h>
#endif

#if SWIFT_STDLIB_HAS_ASL
#include <asl.h>
#elif defined(__ANDROID__)
#include <android/log.h>
#endif

#if defined(__ELF__)
#include <unwind.h>
#endif

#include <inttypes.h>

#ifdef SWIFT_HAVE_CRASHREPORTERCLIENT
#include <atomic>
#include <malloc/malloc.h>

#include "swift/Runtime/Atomic.h"
#endif // SWIFT_HAVE_CRASHREPORTERCLIENT

namespace FatalErrorFlags {
enum: uint32_t {
  ReportBacktrace = 1 << 0
};
} // end namespace FatalErrorFlags

using namespace swift;

#if SWIFT_STDLIB_SUPPORTS_BACKTRACE_REPORTING && SWIFT_STDLIB_HAS_DLADDR
static bool getSymbolNameAddr(llvm::StringRef libraryName,
                              const SymbolInfo &syminfo,
                              std::string &symbolName, uintptr_t &addrOut) {
  // If we failed to find a symbol and thus dlinfo->dli_sname is nullptr, we
  // need to use the hex address.
  bool hasUnavailableAddress = syminfo.getSymbolName() == nullptr;

  if (hasUnavailableAddress) {
    return false;
  }

  // Ok, now we know that we have some sort of "real" name. Set the outAddr.
  addrOut = uintptr_t(syminfo.getSymbolAddress());

  // First lets try to demangle using cxxabi. If this fails, we will try to
  // demangle with swift. We are taking advantage of __cxa_demangle actually
  // providing failure status instead of just returning the original string like
  // swift demangle.
#if defined(_WIN32)
  char szUndName[1024];
  DWORD dwResult;
  dwResult = _swift_win32_withDbgHelpLibrary([&] (HANDLE hProcess) -> DWORD {
    if (!hProcess) {
      return 0;
    }

    DWORD dwFlags = UNDNAME_COMPLETE;
#if !defined(_WIN64)
    dwFlags |= UNDNAME_32_BIT_DECODE;
#endif

    return UnDecorateSymbolName(syminfo.getSymbolName(), szUndName,
                                sizeof(szUndName), dwFlags);
  });

  if (dwResult) {
    symbolName += szUndName;
    return true;
  }
#else
  int status;
  char *demangled =
      abi::__cxa_demangle(syminfo.getSymbolName(), 0, 0, &status);
  if (status == 0) {
    assert(demangled != nullptr &&
           "If __cxa_demangle succeeds, demangled should never be nullptr");
    symbolName += demangled;
    free(demangled);
    return true;
  }
  assert(demangled == nullptr &&
         "If __cxa_demangle fails, demangled should be a nullptr");
#endif

  // Otherwise, try to demangle with swift. If swift fails to demangle, it will
  // just pass through the original output.
  symbolName = demangleSymbolAsString(
      syminfo.getSymbolName(), strlen(syminfo.getSymbolName()),
      Demangle::DemangleOptions::SimplifiedUIDemangleOptions());
  return true;
}
#endif

void swift::dumpStackTraceEntry(unsigned index, void *framePC,
                                bool shortOutput) {
#if SWIFT_STDLIB_SUPPORTS_BACKTRACE_REPORTING && SWIFT_STDLIB_HAS_DLADDR
  auto syminfo = SymbolInfo::lookup(framePC);
  if (!syminfo.has_value()) {
    return;
  }

  // If SymbolInfo:lookup succeeded then fileName is non-null. Thus, we find the
  // library name here. Avoid using StringRef::rsplit because its definition
  // is not provided in the header so that it requires linking with
  // libSupport.a.
  llvm::StringRef libraryName{syminfo->getFilename()};
  libraryName = libraryName.substr(libraryName.rfind('/')).substr(1);

  // Next we get the symbol name that we are going to use in our backtrace.
  std::string symbolName;
  // We initialize symbolAddr to framePC so that if we succeed in finding the
  // symbol, we get the offset in the function and if we fail to find the symbol
  // we just get HexAddr + 0.
  uintptr_t symbolAddr = uintptr_t(framePC);
  bool foundSymbol =
      getSymbolNameAddr(libraryName, syminfo.value(), symbolName, symbolAddr);
  ptrdiff_t offset = 0;
  if (foundSymbol) {
    offset = ptrdiff_t(uintptr_t(framePC) - symbolAddr);
  } else {
    auto baseAddress = syminfo->getBaseAddress();
    offset = ptrdiff_t(uintptr_t(framePC) - uintptr_t(baseAddress));
    symbolAddr = uintptr_t(framePC);
    symbolName = "<unavailable>";
  }

  // We do not use %p here for our pointers since the format is implementation
  // defined. This makes it logically impossible to check the output. Forcing
  // hexadecimal solves this issue.
  // If the symbol is not available, we print out <unavailable> + offset
  // from the base address of where the image containing framePC is mapped.
  // This gives enough info to reconstruct identical debugging target after
  // this process terminates.
  if (shortOutput) {
    fprintf(stderr, "%s`%s + %td", libraryName.data(), symbolName.c_str(),
            offset);
  } else {
    constexpr const char *format = "%-4u %-34s 0x%0.16" PRIxPTR " %s + %td\n";
    fprintf(stderr, format, index, libraryName.data(), symbolAddr,
            symbolName.c_str(), offset);
  }
#else
  if (shortOutput) {
    fprintf(stderr, "<unavailable>");
  } else {
    constexpr const char *format = "%-4u 0x%0.16tx\n";
    fprintf(stderr, format, index, reinterpret_cast<uintptr_t>(framePC));
  }
#endif
}

#if defined(__ELF__)
struct UnwindState {
  void **current;
  void **end;
};

static _Unwind_Reason_Code SwiftUnwindFrame(struct _Unwind_Context *context, void *arg) {
  struct UnwindState *state = static_cast<struct UnwindState *>(arg);
  if (state->current == state->end) {
    return _URC_END_OF_STACK;
  }

  uintptr_t pc;
#if defined(__arm__)
  // ARM r15 is PC.  UNW_REG_PC is *not* the same value, and using that will
  // result in abnormal behaviour.
  _Unwind_VRS_Get(context, _UVRSC_CORE, 15, _UVRSD_UINT32, &pc);
  // Clear the ISA bit during the reporting.
  pc &= ~(uintptr_t)0x1;
#else
  pc = _Unwind_GetIP(context);
#endif
  if (pc) {
    *state->current++ = reinterpret_cast<void *>(pc);
  }
  return _URC_NO_REASON;
}
#endif

SWIFT_ALWAYS_INLINE
static bool withCurrentBacktraceImpl(std::function<void(void **, int)> call) {
#if SWIFT_STDLIB_SUPPORTS_BACKTRACE_REPORTING
  constexpr unsigned maxSupportedStackDepth = 128;
  void *addrs[maxSupportedStackDepth];
#if defined(_WIN32)
  int symbolCount = CaptureStackBackTrace(0, maxSupportedStackDepth, addrs, NULL);
#elif defined(__ELF__)
  struct UnwindState state = {&addrs[0], &addrs[maxSupportedStackDepth]};
  _Unwind_Backtrace(SwiftUnwindFrame, &state);
  int symbolCount = state.current - addrs;
#else
  int symbolCount = backtrace(addrs, maxSupportedStackDepth);
#endif
  call(addrs, symbolCount);
  return true;
#else
  return false;
#endif
}

SWIFT_NOINLINE
bool swift::withCurrentBacktrace(std::function<void(void **, int)> call) {
  return withCurrentBacktraceImpl(call);
}

SWIFT_NOINLINE
void swift::printCurrentBacktrace(unsigned framesToSkip) {
  bool success = withCurrentBacktraceImpl([&](void **addrs, int symbolCount) {
    for (int i = framesToSkip; i < symbolCount; ++i) {
      dumpStackTraceEntry(i - framesToSkip, addrs[i]);
    }
  });
  if (!success)
    fprintf(stderr, "<backtrace unavailable>\n");
}

// Report a message to any forthcoming crash log.
static void
reportOnCrash(uint32_t flags, const char *message)
{
#ifdef SWIFT_HAVE_CRASHREPORTERCLIENT
  char *oldMessage = nullptr;
  char *newMessage = nullptr;

  oldMessage = std::atomic_load_explicit(
    (volatile std::atomic<char *> *)&gCRAnnotations.message,
    SWIFT_MEMORY_ORDER_CONSUME);

  do {
    if (newMessage) {
      free(newMessage);
      newMessage = nullptr;
    }

    if (oldMessage) {
      swift_asprintf(&newMessage, "%s%s", oldMessage, message);
    } else {
      newMessage = strdup(message);
    }
  } while (!std::atomic_compare_exchange_strong_explicit(
             (volatile std::atomic<char *> *)&gCRAnnotations.message,
             &oldMessage, newMessage,
             std::memory_order_release,
             SWIFT_MEMORY_ORDER_CONSUME));
#else
  // empty
#endif // SWIFT_HAVE_CRASHREPORTERCLIENT
}

// Report a message to system console and stderr.
static void
reportNow(uint32_t flags, const char *message)
{
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
#if SWIFT_STDLIB_SUPPORTS_BACKTRACE_REPORTING
  if (flags & FatalErrorFlags::ReportBacktrace) {
    fputs("Current stack trace:\n", stderr);
    printCurrentBacktrace();
  }
#endif
}

SWIFT_NOINLINE SWIFT_RUNTIME_EXPORT void
_swift_runtime_on_report(uintptr_t flags, const char *message,
                         RuntimeErrorDetails *details) {
  // Do nothing. This function is meant to be used by the debugger.

  // The following is necessary to avoid calls from being optimized out.
  asm volatile("" // Do nothing.
               : // Output list, empty.
               : "r" (flags), "r" (message), "r" (details) // Input list.
               : // Clobber list, empty.
               );
}

void swift::_swift_reportToDebugger(uintptr_t flags, const char *message,
                                    RuntimeErrorDetails *details) {
  _swift_runtime_on_report(flags, message, details);
}

bool swift::_swift_reportFatalErrorsToDebugger = true;

bool swift::_swift_shouldReportFatalErrorsToDebugger() {
  return _swift_reportFatalErrorsToDebugger;
}

/// Report a fatal error to system console, stderr, and crash logs.
/// Does not crash by itself.
void swift::swift_reportError(uint32_t flags,
                              const char *message) {
#if defined(__APPLE__) && NDEBUG
  flags &= ~FatalErrorFlags::ReportBacktrace;
#endif
  reportNow(flags, message);
  reportOnCrash(flags, message);
}

// Report a fatal error to system console, stderr, and crash logs, then abort.
SWIFT_NORETURN void swift::fatalErrorv(uint32_t flags, const char *format,
                                       va_list args) {
  char *log;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
  swift_vasprintf(&log, format, args);
#pragma GCC diagnostic pop

  swift_reportError(flags, log);
  abort();
}

// Report a fatal error to system console, stderr, and crash logs, then abort.
SWIFT_NORETURN void swift::fatalError(uint32_t flags, const char *format, ...) {
  va_list args;
  va_start(args, format);

  fatalErrorv(flags, format, args);
}

// Report a warning to system console and stderr.
void
swift::warningv(uint32_t flags, const char *format, va_list args)
{
  char *log;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
  swift_vasprintf(&log, format, args);
#pragma GCC diagnostic pop
  
  reportNow(flags, log);
  
  free(log);
}

// Report a warning to system console and stderr.
void
swift::warning(uint32_t flags, const char *format, ...)
{
  va_list args;
  va_start(args, format);

  warningv(flags, format, args);
}

/// Report a warning to the system console and stderr.  This is exported,
/// unlike the swift::warning() function above.
void swift::swift_reportWarning(uint32_t flags, const char *message) {
  warning(flags, "%s", message);
}

// Crash when a deleted method is called by accident.
SWIFT_RUNTIME_EXPORT SWIFT_NORETURN void swift_deletedMethodError() {
  swift::fatalError(/* flags = */ 0,
                    "Fatal error: Call of deleted method\n");
}

// Crash due to a retain count overflow.
// FIXME: can't pass the object's address from InlineRefCounts without hacks
void swift::swift_abortRetainOverflow() {
  swift::fatalError(FatalErrorFlags::ReportBacktrace,
                    "Fatal error: Object was retained too many times");
}

// Crash due to an unowned retain count overflow.
// FIXME: can't pass the object's address from InlineRefCounts without hacks
void swift::swift_abortUnownedRetainOverflow() {
  swift::fatalError(FatalErrorFlags::ReportBacktrace,
                    "Fatal error: Object's unowned reference was retained too many times");
}

// Crash due to a weak retain count overflow.
// FIXME: can't pass the object's address from InlineRefCounts without hacks
void swift::swift_abortWeakRetainOverflow() {
  swift::fatalError(FatalErrorFlags::ReportBacktrace,
                    "Fatal error: Object's weak reference was retained too many times");
}

// Crash due to retain of a dead unowned reference.
// FIXME: can't pass the object's address from InlineRefCounts without hacks
void swift::swift_abortRetainUnowned(const void *object) {
  if (object) {
    swift::fatalError(FatalErrorFlags::ReportBacktrace,
                      "Fatal error: Attempted to read an unowned reference but "
                      "object %p was already deallocated", object);
  } else {
    swift::fatalError(FatalErrorFlags::ReportBacktrace,
                      "Fatal error: Attempted to read an unowned reference but "
                      "the object was already deallocated");
  }
}

/// Halt due to enabling an already enabled dynamic replacement().
void swift::swift_abortDynamicReplacementEnabling() {
  swift::fatalError(FatalErrorFlags::ReportBacktrace,
                    "Fatal error: trying to enable a dynamic replacement "
                    "that is already enabled");
}

/// Halt due to disabling an already disabled dynamic replacement().
void swift::swift_abortDynamicReplacementDisabling() {
  swift::fatalError(FatalErrorFlags::ReportBacktrace,
                    "Fatal error: trying to disable a dynamic replacement "
                    "that is already disabled");
}

/// Halt due to trying to use unicode data on platforms that don't have it.
void swift::swift_abortDisabledUnicodeSupport() {
  swift::fatalError(FatalErrorFlags::ReportBacktrace,
                    "Unicode normalization data is disabled on this platform");

}
