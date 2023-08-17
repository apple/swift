//===--- Compiler.h - Compiler specific definitions -------------*- C++ -*-===//
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

#ifndef SWIFT_BASIC_COMPILER_H
#define SWIFT_BASIC_COMPILER_H

#include <stddef.h>

#if defined(_MSC_VER) && !defined(__clang__)
#define SWIFT_COMPILER_IS_MSVC 1
#else
#define SWIFT_COMPILER_IS_MSVC 0
#endif

// Workaround non-clang compilers
#ifndef __has_builtin
#define __has_builtin(x) 0
#endif
#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

// __builtin_assume() is an optimization hint.
#if __has_builtin(__builtin_assume)
#define SWIFT_ASSUME(x) __builtin_assume(x)
#else
#define SWIFT_ASSUME(x)
#endif

/// Attributes.

#if __has_attribute(constructor)
#define SWIFT_CONSTRUCTOR __attribute__((constructor))
#else
#define SWIFT_CONSTRUCTOR
#endif

/// \macro SWIFT_GNUC_PREREQ
/// Extend the default __GNUC_PREREQ even if glibc's features.h isn't
/// available.
#ifndef SWIFT_GNUC_PREREQ
# if defined(__GNUC__) && defined(__GNUC_MINOR__) && defined(__GNUC_PATCHLEVEL__)
#  define SWIFT_GNUC_PREREQ(maj, min, patch) \
    ((__GNUC__ << 20) + (__GNUC_MINOR__ << 10) + __GNUC_PATCHLEVEL__ >= \
     ((maj) << 20) + ((min) << 10) + (patch))
# elif defined(__GNUC__) && defined(__GNUC_MINOR__)
#  define SWIFT_GNUC_PREREQ(maj, min, patch) \
    ((__GNUC__ << 20) + (__GNUC_MINOR__ << 10) >= ((maj) << 20) + ((min) << 10))
# else
#  define SWIFT_GNUC_PREREQ(maj, min, patch) 0
# endif
#endif


/// SWIFT_ATTRIBUTE_NOINLINE - On compilers where we have a directive to do so,
/// mark a method "not for inlining".
#if __has_attribute(noinline) || SWIFT_GNUC_PREREQ(3, 4, 0)
#define SWIFT_ATTRIBUTE_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define SWIFT_ATTRIBUTE_NOINLINE __declspec(noinline)
#else
#define SWIFT_ATTRIBUTE_NOINLINE
#endif

/// SWIFT_ATTRIBUTE_ALWAYS_INLINE - On compilers where we have a directive to do
/// so, mark a method "always inline" because it is performance sensitive. GCC
/// 3.4 supported this but is buggy in various cases and produces unimplemented
/// errors, just use it in GCC 4.0 and later.
#if __has_attribute(always_inline) || SWIFT_GNUC_PREREQ(4, 0, 0)
#define SWIFT_ATTRIBUTE_ALWAYS_INLINE __attribute__((always_inline))
#elif defined(_MSC_VER)
#define SWIFT_ATTRIBUTE_ALWAYS_INLINE __forceinline
#else
#define SWIFT_ATTRIBUTE_ALWAYS_INLINE
#endif

// Needed for C++ bridging functions which return types with pointers.
#if __has_attribute(swift_attr)
#define SWIFT_IMPORT_UNSAFE __attribute__((swift_attr("import_unsafe")))
#else
#define SWIFT_IMPORT_UNSAFE
#endif

#ifdef __GNUC__
#define SWIFT_ATTRIBUTE_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define SWIFT_ATTRIBUTE_NORETURN __declspec(noreturn)
#else
#define SWIFT_ATTRIBUTE_NORETURN
#endif

#ifndef SWIFT_BUG_REPORT_URL
#define SWIFT_BUG_REPORT_URL "https://swift.org/contributing/#reporting-bugs"
#endif

#define SWIFT_BUG_REPORT_MESSAGE_BASE \
  "submit a bug report (" SWIFT_BUG_REPORT_URL ")"

#define SWIFT_BUG_REPORT_MESSAGE \
  "please " SWIFT_BUG_REPORT_MESSAGE_BASE

#define SWIFT_CRASH_BUG_REPORT_MESSAGE \
  "Please " SWIFT_BUG_REPORT_MESSAGE_BASE " and include the crash backtrace."

// Conditionally exclude declarations or statements that are only needed for
// assertions from release builds (NDEBUG) without cluttering the surrounding
// code by #ifdefs.
//
// struct DoThings  {
//   SWIFT_ASSERT_ONLY_DECL(unsigned verifyCount = 0);
//   DoThings() {
//     SWIFT_ASSERT_ONLY(verifyCount = getNumberOfThingsToDo());
//   }
//   void doThings() {
//     do {
//       // ... do each thing
//       SWIFT_ASSERT_ONLY(--verifyCount);
//     } while (!done());
//     assert(verifyCount == 0 && "did not do everything");
//   }
// };
#ifdef NDEBUG
#define SWIFT_ASSERT_ONLY_DECL(...)
#define SWIFT_ASSERT_ONLY(...) do { } while (false)
#else
#define SWIFT_ASSERT_ONLY_DECL(...) __VA_ARGS__
#define SWIFT_ASSERT_ONLY(...) do { __VA_ARGS__; } while (false)
#endif

#if defined(__LP64__) || defined(_WIN64)
#define SWIFT_POINTER_IS_8_BYTES 1
#define SWIFT_POINTER_IS_4_BYTES 0
#else
// TODO: consider supporting 16-bit targets
#define SWIFT_POINTER_IS_8_BYTES 0
#define SWIFT_POINTER_IS_4_BYTES 1
#endif

// Produce a string literal for the raw argument tokens.
#define SWIFT_STRINGIZE_RAW(TOK) #TOK

// Produce a string literal for the macro-expanded argument tokens.
#define SWIFT_STRINGIZE_EXPANDED(TOK) SWIFT_STRINGIZE_RAW(TOK)

#if defined(__USER_LABEL_PREFIX__)
#define SWIFT_SYMBOL_PREFIX_STRING \
  SWIFT_STRINGIZE_EXPANDED(__USER_LABEL_PREFIX__)
#else
// Clang and GCC always define __USER_LABEL_PREFIX__, so this should
// only come up with MSVC, and Windows doesn't use a prefix.
#define SWIFT_SYMBOL_PREFIX_STRING ""
#endif

// An attribute to override the symbol name of a declaration.
// This does not compensate for platform symbol prefixes; for that,
// use SWIFT_ASM_LABEL_WITH_PREFIX.
//
// This only actually works on Clang or GCC; MSVC does not provide
// an attribute to change the asm label.
#define SWIFT_ASM_LABEL_RAW(STRING) __asm__(STRING)
#define SWIFT_ASM_LABEL_WITH_PREFIX(STRING) \
  SWIFT_ASM_LABEL_RAW(SWIFT_SYMBOL_PREFIX_STRING STRING)

// SWIFT_FORMAT(fmt,first) marks a function as taking a format string argument
// at argument `fmt`, with the first argument for the format string as `first`.
#if __has_attribute(format)
#define SWIFT_FORMAT(fmt, first) __attribute__((format(printf, fmt, first)))
#else
#define SWIFT_FORMAT(fmt, first)
#endif

// SWIFT_VFORMAT(fmt) marks a function as taking a format string argument at
// argument `fmt`, with the arguments in a `va_list`.
#if __has_attribute(format)
#define SWIFT_VFORMAT(fmt) __attribute__((format(printf, fmt, 0)))
#else
#define SWIFT_VFORMAT(fmt)
#endif

#if __has_attribute(enum_extensibility)
#define ENUM_EXTENSIBILITY_ATTR(arg) __attribute__((enum_extensibility(arg)))
#else
#define ENUM_EXTENSIBILITY_ATTR(arg)
#endif

// The 'u8' string literal prefix creates `char` types on C++14/17 but
// `char8_t` types on C++20. To support compiling in both modes
// simultaneously, wrap Unicode literals in `SWIFT_UTF8("...")` to ensure
// that they are interpreted by the compiler as UTF-8 but always return
// `char` types.
#if defined(__cplusplus)
#if defined(__cpp_char8_t)
inline constexpr char operator""_swift_u8(char8_t c) { return c; }
inline const char *operator""_swift_u8(const char8_t *p, std::size_t) {
  return reinterpret_cast<const char *>(p);
}
#define SWIFT_UTF8(literal) u8##literal##_swift_u8
#else  // !defined(__cpp_char8_t)
#define SWIFT_UTF8(literal) u8##literal
#endif // defined(__cpp_char8_t)
#endif // defined(__cplusplus)

#endif // SWIFT_BASIC_COMPILER_H
