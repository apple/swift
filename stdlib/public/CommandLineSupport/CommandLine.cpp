//===--- CommandLine.cpp - OS-specific command line arguments -------------===//
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
// OS-specific command line argument handling is defined here.
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if __has_include(<sys/errno.h>)
#include <sys/errno.h>
#else
#include <errno.h>
#endif

#include "swift/Runtime/Debug.h"
#include "swift/Runtime/Win32.h"

#include "swift/shims/GlobalObjects.h"
#include "swift/shims/RuntimeStubs.h"
#include "swift/shims/Visibility.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#endif

// Backing storage for overrides of `Swift.CommandLine.arguments`.
static char **_swift_stdlib_ProcessOverrideUnsafeArgv = nullptr;
static int _swift_stdlib_ProcessOverrideUnsafeArgc = 0;

// This needs to findable by dlopen() for JIT purposes (see Immediate.cpp).
SWIFT_CC(swift) extern "C" SWIFT_ATTRIBUTE_FOR_EXPORTS
void _swift_stdlib_overrideUnsafeArgvArgc(char **argv, int argc) {
  _swift_stdlib_ProcessOverrideUnsafeArgv = argv;
  _swift_stdlib_ProcessOverrideUnsafeArgc = argc;
}

namespace swift {
  /// A platform-specific implementation of @c _swift_stdlib_getUnsafeArgvArgc.
  /// 
  /// This function should return @c argc and @c argv cheaply (ideally in
  /// constant time and without needing to allocate.) If it cannot do so,
  /// it should return @c nullptr, at which point the caller can call
  /// @c enumerateUnsafeArgv() in order to reconstruct @c argv locally.
  /// 
  /// The result of this function is @em not owned by the caller and should
  /// persist for the lifetime of the process.
  static char **getUnsafeArgvArgc(int *outArgLen);

  /// A platform-specific function that enumerates the contents of @c argv
  /// one argument at a time.
  /// 
  /// @a body is a function that takes two arguments:
  /// 
  /// - The first argument is the value of @c argc if it can be readily
  ///   computed, or @c -1 otherwise.
  /// - The second argument is the element of @c argv being enumerated. The
  ///   caller makes a copy of this string. The implementation should not
  ///   enumerate the trailing @c nullptr required by the C standard.
  /// 
  /// Callers should call @c getUnsafeArgvArgc() before calling this function
  /// in case a fast path is available. If that function is implemented on this
  /// platform, then this function does not need to be implemented.
  template <typename F>
  static void enumerateUnsafeArgv(const F& body);
}

SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
char **_swift_stdlib_getUnsafeArgvArgc(int *outArgLen) {
  assert(outArgLen != nullptr);

  // Check the override before doing any platform-specific work.
  if (SWIFT_UNLIKELY(_swift_stdlib_ProcessOverrideUnsafeArgv)) {
    *outArgLen = _swift_stdlib_ProcessOverrideUnsafeArgc;
    return _swift_stdlib_ProcessOverrideUnsafeArgv;
  }

  // Try the platform-specific fast path that avoids heap (re)allocation. Not
  // all platforms implement this function.
  if (auto argv = swift::getUnsafeArgvArgc(outArgLen)) {
    return argv;
  }

  // Take a slower path wherein we construct the argv array one element at a
  // time. If the enumeration function can provide us with a hint for argc, we
  // can avoid calling realloc() more than once.
  int maxArgc = 0;
  int argc = 0;
  char **argv = nullptr;
  swift::enumerateUnsafeArgv([&] (int maxArgcHint, const char *arg) {
    if (argc >= maxArgc) {
      if (maxArgcHint > maxArgc) {
        // The platform was able to cheaply get argc, so use the
        // provided value.
        maxArgc = maxArgcHint;
      } else {
        // The platform could not tell us argc, so grow the argument array by
        // small amounts. We assume here that the command line doesn't usually
        // have many arguments, so a small/linear growth factor should be safe.
        //
        // If we ask for space for 15 arguments on the first allocation, then
        // when we call realloc (asking for additional space for the trailing
        // nullptr), we'll avoid wasting memory due to malloc()'s granularity.
        maxArgc = (maxArgc == 0) ? 15 : (maxArgc + 16);
      }

      // Reallocate (or initially allocate) the argv buffer. Overallocate by
      // one element to allow for a trailing nullptr.
      // 
      // NOTE: It is intentional that we do not simply use std::vector here.
      // STL collections may call operator new() which can be overridden by 
      // client code and that client code could call back into Swift.
      size_t argvSize = sizeof(char *) * (maxArgc + 1);
      argv = reinterpret_cast<char **>(realloc(argv, argvSize));
      if (!argv) {
        swift::fatalError(0,
          "Fatal error: Could not allocate space for %d commandline "
          " arguments: %d\n",
          argc, errno);
      }
    }

    argv[argc] = strdup(arg);
    argc += 1;
  });

  if (argv) {
    // Ensure the arguments array has a trailing nullptr, per the C standard.
    argv[argc] = nullptr;

  } else {
    // We didn't get any arguments and never ended up allocating an array.
    // Return an empty array (save for the trailing nullptr) instead.
    static char *emptyArgv[] = { nullptr };
    argc = 0;
    argv = emptyArgv;
  }

  *outArgLen = argc;
  return argv;
}

#if defined(__APPLE__)
// NOTE: forward declare this rather than including crt_externs.h as not all
// SDKs provide it
extern "C" char ***_NSGetArgv(void);
extern "C" int *_NSGetArgc(void);

static char **swift::getUnsafeArgvArgc(int *outArgLen) {
  *outArgLen = *_NSGetArgc();
  return *_NSGetArgv();
}

template <typename F>
static void swift::enumerateUnsafeArgv(const F& body) { }
#elif defined(__linux__) || defined(__CYGWIN__)
static char **swift::getUnsafeArgvArgc(int *outArgLen) {
  return nullptr;
}

template <typename F>
static void swift::enumerateUnsafeArgv(const F& body) {
  FILE *cmdline = fopen("/proc/self/cmdline", "rb");
  if (!cmdline) {
    swift::fatalError(0,
      "Fatal error: Unable to open interface to '/proc/self/cmdline': %d.\n",
      errno);
  }

  char *arg = nullptr;
  size_t size = 0;
  while (getdelim(&arg, &size, '\0', cmdline) != -1) {
    body(-1, arg);
  }
  if (arg) {
    free(arg);
  }

  fclose(cmdline);
}
#elif defined(_WIN32)
#include <stdlib.h>

static char **swift::getUnsafeArgvArgc(int *outArgLen) {
  return nullptr;
}

template <typename F>
static void swift::enumerateUnsafeArgv(const F& body) {
  int argc = 0;
  if (LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    std::for_each(wargv, wargv + argc, [=] (wchar_t *warg) {
      if (char *arg = _swift_win32_copyUTF8FromWide(warg)) {
        body(argc, arg);
        free(arg);
      } else {
        // Note that GetLastError() and errno may not be so useful here,
        // as in the error case we may have called free(), which might reset
        // either or both of them.
        swift::fatalError(0,
                          "Fatal error: Unable to convert argument '%ls' to "
                          "UTF-8: %lx, %d.\n",
                          warg, ::GetLastError(), errno);
      }
    });

    LocalFree(wargv);
  }
}
#elif defined(__FreeBSD__)
#include <errno.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

static char **swift::getUnsafeArgvArgc(int *outArgLen) {
  return nullptr;
}

template <typename F>
static void swift::enumerateUnsafeArgv(const F& body) {
  char *argPtr = nullptr; // or use ARG_MAX? 8192 is used in LLDB though..
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ARGS, getpid()};
  size_t argPtrSize = 0;
  for (int i = 0; i < 3 && !argPtr; ++i) { // give up after 3 tries
    if (sysctl(mib, 4, nullptr, &argPtrSize, nullptr, 0) != -1) {
      argPtr = static_cast<char *>(malloc(argPtrSize));
      if (sysctl(mib, 4, argPtr, &argPtrSize, nullptr, 0) == -1) {
        free(argPtr);
        argPtr = nullptr;
        if (errno != ENOMEM)
          break;
      }
    } else {
      break;
    }
  }
  if (!argPtr)
    swift::fatalError(0,
                      "Fatal error: Could not retrieve commandline "
                      "arguments: sysctl: %s.\n",
                      strerror(errno));

  char *curPtr = argPtr;
  char *endPtr = argPtr + argPtrSize;

  for (; curPtr < endPtr; curPtr += strlen(curPtr) + 1) {
    body(-1, curPtr);
  }
  free(argPtr);
}
#elif defined(__wasi__)
#include <stdlib.h>
#include <wasi/api.h>
#include <wasi/libc.h>

static char **swift::getUnsafeArgvArgc(int *outArgLen) {
  __wasi_errno_t err;

  size_t argv_buf_size = 0;
  size_t argc = 0;
  err = __wasi_args_sizes_get(&argc, &argv_buf_size);
  if (err != __WASI_ERRNO_SUCCESS) {
    swift::fatalError(0,
                      "Fatal error: Could not retrieve commandline "
                      "arguments: %d.\n", static_cast<int>(err));
    return nullptr;
  }

  char *argv_buf = static_cast<char *>(malloc(argv_buf_size));
  char **argv = static_cast<char **>(calloc(argc + 1, sizeof(char *)));

  err = __wasi_args_get((uint8_t **)argv, (uint8_t *)argv_buf);
  if (err != __WASI_ERRNO_SUCCESS) {
    swift::fatalError(0,
                      "Fatal error: Could not retrieve commandline "
                      "arguments: %d.\n", static_cast<int>(err));
    return nullptr;
  }

  *outArgLen = static_cast<int>(argc);

  return argv;
}

template <typename F>
static void swift::enumerateUnsafeArgv(const F& body) { }
#elif defined(__OpenBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/exec.h>

static char **swift::getUnsafeArgvArgc(int *outArgLen) {
  int mib[2] = {CTL_VM, VM_PSSTRINGS};
  struct _ps_strings _ps;
  size_t len = sizeof(_ps);

  if (sysctl(mib, 2, &_ps, &len, NULL, 0) == -1) {
    return nullptr;
  }

  struct ps_strings *ps = static_cast<struct ps_strings *>(_ps.val);
  *outArgLen = ps->ps_nargvstr;
  return ps->ps_argvstr;
}

template <typename F>
static void swift::enumerateUnsafeArgv(const F& body) { }
#else // Add your favorite OS's command line arg grabber here.
static char **swift::getUnsafeArgvArgc(int *outArgLen) {
  swift::fatalError(
      0,
      "Fatal error: Command line arguments not supported on this platform.\n");
}

template <typename F>
static void swift::enumerateUnsafeArgv(const F& body) { }
#endif
