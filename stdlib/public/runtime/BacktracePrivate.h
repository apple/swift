//===--- BacktraceUtils.h - Private backtracing utilities -------*- C++ -*-===//
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
// Private declarations of the Swift runtime's backtracing code.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_RUNTIME_BACKTRACE_UTILS_H
#define SWIFT_RUNTIME_BACKTRACE_UTILS_H

#include "swift/Runtime/Config.h"
#include "swift/shims/Visibility.h"

#include <inttypes.h>

#ifdef _WIN32
// For DWORD
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// For wchar_t
#include <cwchar>
#endif

namespace swift {
namespace runtime {
namespace backtrace {

#ifdef _WIN32
typedef wchar_t ArgChar;
typedef DWORD ErrorCode;
#else
typedef char ArgChar;
typedef int ErrorCode;
#endif

enum class UnwindAlgorithm {
  Auto = 0,
  Fast = 1,
  Precise = 2
};

enum class OnOffTty {
  Default = -1,
  Off = 0,
  On = 1,
  TTY = 2
};

enum class Preset {
  Auto = -1,
  Friendly = 0,
  Medium = 1,
  Full = 2
};

enum class ThreadsToShow {
  Preset = -1,
  All = 0,
  Crashed = 1
};

enum class RegistersToShow {
  Preset = -1,
  None = 0,
  All = 1,
  Crashed = 2
};

enum class ImagesToShow {
  Preset = -1,
  None = 0,
  All = 1,
  Mentioned = 2
};

enum class SanitizePaths {
  Preset = -1,
  Off = 0,
  On = 1
};

enum class OutputTo {
  Auto = -1,
  Stdout = 0,
  Stderr = 2,
};

enum class Symbolication {
  Off = 0,
  Fast = 1,
  Full = 2,
};

struct BacktraceSettings {
  UnwindAlgorithm  algorithm;
  OnOffTty         enabled;
  bool             demangle;
  OnOffTty         interactive;
  OnOffTty         color;
  unsigned         timeout;
  ThreadsToShow    threads;
  RegistersToShow  registers;
  ImagesToShow     images;
  unsigned         limit;
  unsigned         top;
  SanitizePaths    sanitize;
  Preset           preset;
  bool             cache;
  OutputTo         outputTo;
  Symbolication    symbolicate;
  const char      *swiftBacktracePath;
};

SWIFT_RUNTIME_STDLIB_INTERNAL BacktraceSettings _swift_backtraceSettings;

inline bool _swift_backtrace_isEnabled() {
  return _swift_backtraceSettings.enabled == OnOffTty::On;
}

SWIFT_RUNTIME_STDLIB_INTERNAL ErrorCode _swift_installCrashHandler();

#ifdef __linux__
SWIFT_RUNTIME_STDLIB_INTERNAL bool _swift_spawnBacktracer(const ArgChar * const *argv, int memserver_fd);
#else
SWIFT_RUNTIME_STDLIB_INTERNAL bool _swift_spawnBacktracer(const ArgChar * const *argv);
#endif

SWIFT_RUNTIME_STDLIB_INTERNAL void _swift_displayCrashMessage(int signum, const void *pc);

SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_formatAddress(uintptr_t addr, char buffer[18]);

inline void _swift_formatAddress(const void *ptr, char buffer[18]) {
  _swift_formatAddress(reinterpret_cast<uintptr_t>(ptr), buffer);
}

SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_formatUnsigned(unsigned u, char buffer[22]);

} // namespace backtrace
} // namespace runtime
} // namespace swift

#endif // SWIFT_RUNTIME_BACKTRACE_UTILS_H
