//===--- LibcShims.cpp ----------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#if defined(__APPLE__)
#define _REENTRANT
#include <math.h>
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <io.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include <stdio.h>

#include "swift/shims/LibcShims.h"

#if defined(_WIN32)
static void __attribute__((__constructor__))
_swift_stdlib_configure_console_mode(void) {
  static UINT uiPrevConsoleCP = GetConsoleOutputCP();
  atexit([]() { SetConsoleOutputCP(uiPrevConsoleCP); });
  SetConsoleOutputCP(CP_UTF8);
}
#endif

SWIFT_RUNTIME_STDLIB_INTERNAL
__swift_size_t _swift_stdlib_fwrite_stdout(const void *ptr,
                                           __swift_size_t size,
                                           __swift_size_t nitems) {
  return fwrite(ptr, size, nitems, stdout);
}
