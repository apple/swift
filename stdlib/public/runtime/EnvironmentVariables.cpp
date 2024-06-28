//===--- EnvironmentVariables.h - Debug variables. --------------*- C++ -*-===//
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
//
// Debug behavior conditionally enabled using environment variables.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Debug.h"
#include "swift/Runtime/Paths.h"
#include "swift/Runtime/EnvironmentVariables.h"

#include <string.h>
#include <inttypes.h>

using namespace swift;

namespace {

// This is required to make the macro machinery work correctly; we can't
// declare a VARIABLE(..., const char *, ...) because then the token-pasted
// names won't work properly.  It *does* mean that if you want to use std::string
// somewhere in this file, you'll have to fully qualify the name.
typedef const char *string;

// Require all environment variable names to start with SWIFT_
static constexpr bool hasSwiftPrefix(const char *str) {
  const char prefix[] = "SWIFT_";
  for (unsigned i = 0; i < sizeof(prefix) - 1; i++)
    if (str[i] != prefix[i])
      return false;
  return true;
}
#define VARIABLE(name, type, defaultValue, help) \
  static_assert(hasSwiftPrefix(#name), "Names must start with SWIFT");
#include "EnvironmentVariables.def"

#if SWIFT_STDLIB_HAS_ENVIRON

// Value parsers. Add new functions named parse_<type> to accommodate more
// debug variable types.
static bool parse_bool(const char *name, const char *value, bool defaultValue) {
  if (!value)
    return defaultValue;
  switch (value[0]) {
  case 'Y':
  case 'y':
  case 'T':
  case 't':
  case '1':
    return true;
  case 'N':
  case 'n':
  case 'F':
  case 'f':
  case '0':
    return false;
  default:
    swift::warning(RuntimeErrorFlagNone,
                  "Warning: cannot parse value %s=%s, defaulting to %s.\n",
                  name, value, defaultValue ? "true" : "false");
    return defaultValue;
  }
}

static uint8_t parse_uint8_t(const char *name,
                             const char *value,
                             uint8_t defaultValue) {
  if (!value)
    return defaultValue;
  char *end;
  long n = strtol(value, &end, 0);
  if (*end != '\0') {
    swift::warning(RuntimeErrorFlagNone,
                   "Warning: cannot parse value %s=%s, defaulting to %u.\n",
                   name, value, defaultValue);
    return defaultValue;
  }

  if (n < 0) {
    swift::warning(RuntimeErrorFlagNone,
                   "Warning: %s=%s out of bounds, clamping to 0.\n",
                   name, value);
    return 0;
  }
  if (n > UINT8_MAX) {
    swift::warning(RuntimeErrorFlagNone,
                   "Warning: %s=%s out of bounds, clamping to %d.\n",
                   name, value, UINT8_MAX);
    return UINT8_MAX;
  }

  return n;
}

static uint32_t parse_uint32_t(const char *name,
                               const char *value,
                               uint32_t defaultValue) {
  if (!value)
    return defaultValue;
  char *end;
  long long n = strtoll(value, &end, 0);
  if (*end != '\0') {
    swift::warning(RuntimeErrorFlagNone,
                   "Warning: cannot parse value %s=%s, defaulting to %u.\n",
                   name, value, defaultValue);
    return defaultValue;
  }

  if (n < 0) {
    swift::warning(RuntimeErrorFlagNone,
                   "Warning: %s=%s out of bounds, clamping to 0.\n",
                   name, value);
    return 0;
  }
  if (n > UINT32_MAX) {
    swift::warning(RuntimeErrorFlagNone,
                   "Warning: %s=%s out of bounds, clamping to %" PRIu32 ".\n",
                   name, value, UINT32_MAX);
    return UINT32_MAX;
  }

  return n;
}

static string parse_string(const char *name,
                           const char *value,
                           string defaultValue) {
  if (!value || value[0] == 0)
    return strdup(defaultValue);
  return strdup(value);
}

// Print a list of all the environment variables. Lazy initialization makes
// this a bit odd, but the use of these variables in the metadata system means
// it's almost certain to run early.
//
// The "extra" parameter is printed after the header and before the list of
// variables.
void printHelp(const char *extra) {
  swift::warning(RuntimeErrorFlagNone, "Swift runtime debugging:\n");
  if (extra)
    swift::warning(RuntimeErrorFlagNone, "%s\n", extra);
#define VARIABLE(name, type, defaultValue, help) \
  swift::warning(RuntimeErrorFlagNone, "%7s %s [default: %s] - %s\n", \
                 #type, #name, #defaultValue, help);
#include "EnvironmentVariables.def"
  swift::warning(RuntimeErrorFlagNone, "SWIFT_DEBUG_HELP=YES - Print this help.");
}

#endif  // SWIFT_STDLIB_HAS_ENVIRON

} // end anonymous namespace

// Define backing variables.
#define VARIABLE(name, type, defaultValue, help) \
  type swift::runtime::environment::name ## _variable = defaultValue;
#include "EnvironmentVariables.def"

// Initialization code.
swift::once_t swift::runtime::environment::initializeToken;

#if SWIFT_STDLIB_HAS_ENVIRON && (defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__linux__))
extern "C" char **environ;
#define ENVIRON environ
#elif defined(_WIN32)
// `_environ` is DLL-imported unless we are linking against the static C runtime
// (via `/MT` or `/MTd`).
#if defined(_DLL)
extern "C" __declspec(dllimport) char **_environ;
#else
extern "C" char **_environ;
#endif
// `_environ` is unavailable in the Windows Runtime environment.
// https://docs.microsoft.com/en-us/cpp/c-runtime-library/environ-wenviron?view=msvc-160
#if !defined(_WINRT_DLL)
#define ENVIRON _environ
#endif
#endif

#ifdef ENVIRON
void swift::runtime::environment::initialize(void *context) {
  // On platforms where we have an environment variable array available, scan it
  // directly. This optimizes for the common case where no variables are set,
  // since we only need to perform one scan to set all variables. It also allows
  // us to detect some spelling mistakes by warning on unknown SWIFT_ variables.

  bool SWIFT_DEBUG_HELP_variable = false;
  for (char **var = ENVIRON; *var; var++) {
    // Immediately skip anything without a SWIFT_ prefix.
    if (strncmp(*var, "SWIFT_", 6) != 0)
      continue;

    bool foundVariable = false;
    // Check each defined variable in turn, plus SWIFT_DEBUG_HELP. Variables are
    // parsed by functions named parse_<type> above. An unknown type will
    // produce an error that parse_<unknown-type> doesn't exist. Add new parsers
    // above.
#define VARIABLE(name, type, defaultValue, help)                       \
    if (strncmp(*var, #name "=", strlen(#name "=")) == 0) {            \
      name ## _variable =                                              \
        parse_ ## type(#name, *var + strlen(#name "="), defaultValue); \
      foundVariable = true;                                            \
    }
    // SWIFT_DEBUG_HELP is not in the variables list. Parse it like the other
    // variables.
    VARIABLE(SWIFT_DEBUG_HELP, bool, false, )
#include "EnvironmentVariables.def"

    // Flag unknown SWIFT_DEBUG_ variables to catch misspellings. We don't flag
    // all unknown SWIFT_ variables, because there are a bunch of other SWIFT_
    // variables used for other purposes, such as SWIFT_SOURCE_ROOT and
    // SWIFT_INSTALL_DIR, and we don't want to warn for all of those.
    const char *swiftDebugPrefix = "SWIFT_DEBUG_";
    if (!foundVariable &&
        strncmp(*var, swiftDebugPrefix, strlen(swiftDebugPrefix)) == 0) {
      const char *equals = strchr(*var, '=');
      if (!equals)
        equals = *var + strlen(*var);
      swift::warning(RuntimeErrorFlagNone,
                     "Warning: unknown environment variable %.*s\n",
                     (int)(equals - *var), *var);
    }
  }

  if (SWIFT_DEBUG_HELP_variable)
    printHelp(nullptr);
}
#elif SWIFT_STDLIB_HAS_ENVIRON
void swift::runtime::environment::initialize(void *context) {
  // Emit a getenv call for each variable. This is less efficient but works
  // everywhere.
#define VARIABLE(name, type, defaultValue, help) \
  name ## _variable = parse_ ## type(#name, getenv(#name), defaultValue);
#include "EnvironmentVariables.def"

  // Print help if requested.
  if (parse_bool("SWIFT_DEBUG_HELP", getenv("SWIFT_DEBUG_HELP"), false))
    printHelp("Using getenv to read variables. Unknown SWIFT_DEBUG_ variables "
              "will not be flagged.");
}
#else
void swift::runtime::environment::initialize(void *context) {
  (void)context;
}
#endif

SWIFT_RUNTIME_EXPORT
bool swift_COWChecksEnabled() {
  return runtime::environment::SWIFT_DEBUG_ENABLE_COW_CHECKS();
}

SWIFT_RUNTIME_STDLIB_SPI bool concurrencyEnableCooperativeQueues() {
  return runtime::environment::
      SWIFT_DEBUG_CONCURRENCY_ENABLE_COOPERATIVE_QUEUES();
}

SWIFT_RUNTIME_STDLIB_SPI bool concurrencyValidateUncheckedContinuations() {
  return runtime::environment::SWIFT_DEBUG_VALIDATE_UNCHECKED_CONTINUATIONS();
}

SWIFT_RUNTIME_STDLIB_SPI const char *concurrencyIsCurrentExecutorLegacyModeOverride() {
  return runtime::environment::SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE();
}
