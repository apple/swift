//===--- Assertions.h - Assertion macros                               ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file provides three alternatives to the C/C++ standard `assert()` macro
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_ASSERTIONS_H
#define SWIFT_BASIC_ASSERTIONS_H

// Only for use in this header
#if __has_builtin(__builtin_expect)
#define ASSERT_UNLIKELY(expression) (__builtin_expect(!!(expression), 0))
#else
#define ASSERT_UNLIKELY(expression) ((expression))
#endif

// ================================ Mandatory Asserts ================================

// `ASSERT(expr)`:
// * is always compiled into the executable and
// * always checks the condition, regardless of build or runtime settings.
// This should be used for most assertions, which is why it
// deserves the short name.  In particular, for simple checks
// (e.g., validating that something is non-null), this is just as
// fast as a disabled `CONDITIONAL_ASSERT`, so there's no point in
// using the conditional form.
//
// You can globally replace `assert` with `ASSERT` in a piece of code
// to have all your assertions enabled in all builds.  If you do this,
// please do a little profiling first, just in case you have some checks
// that are more expensive than you think.  You can switch those to
// `CONDITIONAL_ASSERT` or `DEBUG_ASSERT` as needed.

// Visual Studio doesn't have __FILE_NAME__
#ifdef __FILE_NAME__

#define ASSERT(expr) \
  do { \
    if (ASSERT_UNLIKELY(!(expr))) {			   \
      ASSERT_failure(#expr, __FILE_NAME__, __LINE__, __func__); \
    } \
  } while (0)

#else

#define ASSERT(expr) \
  do { \
    if (ASSERT_UNLIKELY(!(expr))) {			   \
      ASSERT_failure(#expr, __FILE__, __LINE__, __func__); \
    } \
  } while (0)

#endif

// Function that reports the actual failure when it occurs.
void ASSERT_failure(const char *expr, const char *file, int line, const char *func);

// ================================ Conditional Asserts ================================

// `CONDITIONAL_ASSERT(expr)`:
// * is always compiled into the executable, but
// * only checks the condition if a runtime flag is defined.
// That runtime flag is disabled by default in release builds
// but can be enabled with the command-line flag `-compiler-assertions`
//
// Use this for asserts that are comparatively expensive to check.
//
// You can globally change `assert` to `CONDITIONAL_ASSERT` to make all your
// assertions _optionally_ available in release builds.  Anyone can then add
// `-compiler-assertions` to their build flags to get more information about a
// compiler problem.  Before you check it in, do a little checking for
// assertions that might be checked huge numbers of times (e.g., invariants
// for inner loops or core utilities); those may need to become `DEBUG_ASSERT`
// or else refactored to be checked more selectively.
//
// Over time, plan to change most of the resulting `CONDITIONAL_ASSERT` into
// plain `ASSERT` to enable them by default.

#define CONDITIONAL_ASSERT(expr) \
  do { \
    if (ASSERT_UNLIKELY(CONDITIONAL_ASSERT_enabled())) {	\
      ASSERT(expr); \
    } \
  } while (0)

// Use `CONDITIONAL_ASSERT_enabled()` to guard complex, expensive code that
// should only run when assertions are enabled.  This is exactly the
// same check that's used to enable `CONDITIONAL_ASSERT()` at runtime.
// This is not often used -- if you are just setting a flag or updating
// a counter, it's likely cheaper just to do it than to test whether or not
// to do it. Only use this for relatively complex operations.
//
// if (CONDITIONAL_ASSERT_enabled()) {
//    ... stuff ...
// }

// Declare a callable function version of this runtime test.
int CONDITIONAL_ASSERT_enabled();

// Define a macro version of this test
extern int CONDITIONAL_ASSERT_Global_enable_flag;
#define CONDITIONAL_ASSERT_enabled() \
  (CONDITIONAL_ASSERT_Global_enable_flag != 0)

// Profiling note: If you uncomment the line below to #undef the macro, then
// we'll always call the function, which lets you profile assertions by
// counting calls to this function.

// #undef CONDITIONAL_ASSERT_enabled

// ================================ Debug Asserts ================================

// `DEBUG_ASSERT(expr)`:
// * is only compiled into the executable in debug or "asserts enabled" builds, and
// * always performs the check (whenever it is compiled in).
//
// This basically makes it a synonym for the Standard C `assert(expr)`.
//
// You should mostly avoid this except for occasional experiments in your
// local tree.  It can be useful in two situations:
//
// * Assertions that are known to mis-fire.
//   Such assertions should not be `ASSERT` (since that will cause unnecessary
//   broken compiles) and `CONDITIONAL_ASSERT` gets enabled a lot by people who
//   are not compiler experts.  So `DEBUG_ASSERT` is appropriate there until the
//   check can be fixed so it doesn't mis-fire.
//
// * Inner loops that can run billions of times.
//   For these, even the cost of testing whether `CONDITIONAL_ASSERT` is enabled
//   can be prohibitive.  Use `DEBUG_ASSERT`, but also look for ways to refactor
//   so you can verify correctness without having an assertion in an inner loop.
//
// P.S.  Please do not bulk replace `assert` with `DEBUG_ASSERT`.  The whole
// point of this package is to move us toward having assertions always compiled
// in and always enabled.
#ifdef NDEBUG
  #define DEBUG_ASSERT(expr) do { } while (0)
  #undef DEBUG_ASSERT_enabled
#else
  #define DEBUG_ASSERT(expr) ASSERT(expr)
  #define DEBUG_ASSERT_enabled 1
#endif

// Code that's only needed within `DEBUG_ASSERT` can be guarded as follows:
//
// #ifndef NDEBUG
//    ... code that's only needed for DEBUG_ASSERT ...
// #endif
//
// or with the equivalent
//
// #ifdef DEBUG_ASSERT_enabled
//    ... code that's only needed for DEBUG_ASSERT ...
// #endif
//
// For example, you may need this for variables or functions that
// are only used within DEBUG_ASSERT statements.

// A common case is to declare a variable or perform a simple
// expression.  These can be used to avoid some boilerplate:
//
// void doThings()  {
//   DEBUG_ASSERT_DECL(std::vector<Things> thingsToVerify;);
//   while (!done) {
//     // ... do each thing ...
//     DEBUG_ASSERT_EXPR(thingsToVerify.append(item));
//   }
//   DEBUG_ASSERT(verifyAllThe(thingsToVerify));
// }

#ifdef DEBUG_ASSERT_enabled
  #define DEBUG_ASSERT_DECL(...) __VA_ARGS__
  #define DEBUG_ASSERT_EXPR(...) do { __VA_ARGS__; } while (false)
#else
  #define DEBUG_ASSERT_DECL(...)
  #define DEBUG_ASSERT_EXPR(...) do { } while (false)
#endif

// Older version of the same idea:
#define SWIFT_ASSERT_ONLY_DECL DEBUG_ASSERT_DECL
#define SWIFT_ASSERT_ONLY DEBUG_ASSERT_EXPR

// ================================ Utility and Helper Functions ================================

// Utility function to print out help information for
// various command-line options that affect the assertion
// behavior.
void ASSERT_help();

#endif // SWIFT_BASIC_ASSERTIONS_H
