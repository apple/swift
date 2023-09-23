//===--- LibcShims.h - Access to POSIX for Swift's core stdlib --*- C++ -*-===//
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
//  Using the Darwin (or Glibc) module in the core stdlib would create a
//  circular dependency, so instead we import these declarations as part of
//  SwiftShims.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_STDLIB_SHIMS_LIBCSHIMS_H
#define SWIFT_STDLIB_SHIMS_LIBCSHIMS_H

#include "SwiftStdbool.h"
#include "SwiftStdint.h"
#include "SwiftStddef.h"
#include "Visibility.h"

#if __has_feature(nullability)
#pragma clang assume_nonnull begin
#endif

#ifdef __cplusplus
extern "C" {
#endif


// Input/output <stdio.h>
SWIFT_RUNTIME_STDLIB_INTERNAL
__swift_size_t _swift_stdlib_fwrite_stdout(const void *ptr, __swift_size_t size,
                                           __swift_size_t nitems);

// General utilities <stdlib.h>
// Memory management functions
static inline void _swift_stdlib_free(void *_Nullable ptr) {
  extern void free(void *_Nullable);
  free(ptr);
}

// String handling <string.h>
SWIFT_READONLY
static inline __swift_size_t _swift_stdlib_strlen(const char *s) {
  extern __swift_size_t strlen(const char *);
  return strlen(s);
}

SWIFT_READONLY
static inline __swift_size_t _swift_stdlib_strlen_unsigned(const unsigned char *s) {
  return _swift_stdlib_strlen((const char *)s);
}

SWIFT_READONLY
static inline void * _Nullable _swift_stdlib_memchr(const void *s, int c,
                                                    __swift_size_t n) {
  extern void * _Nullable memchr(const void * _Nonnull, int, __swift_size_t);
  return memchr(s, c, n);
}

SWIFT_READONLY
static inline int _swift_stdlib_memcmp(const void *s1, const void *s2,
                                       __swift_size_t n) {
// FIXME: Is there a way to identify Glibc specifically?
#if defined(__gnu_linux__)
  extern int memcmp(const void * _Nonnull, const void * _Nonnull, __swift_size_t);
#else
  extern int memcmp(const void * _Null_unspecified, const void * _Null_unspecified, __swift_size_t);
#endif
  return memcmp(s1, s2, n);
}

// Casting helper. This code needs to work when included from C or C++.
// Casting away const with a C-style cast warns in C++. Use a const_cast
// there.
#ifdef __cplusplus
#define CONST_CAST(type, value) const_cast<type>(value)
#else
#define CONST_CAST(type, value) (type)value
#endif

#ifndef _VA_LIST
typedef __builtin_va_list va_list;
#define _VA_LIST
#endif

static inline int _swift_stdlib_vprintf(const char * __restrict fmt, va_list args) {
    extern int vprintf(const char * __restrict, va_list);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    return vprintf(fmt, args);
#pragma clang diagnostic pop
}

// Non-standard extensions
#if defined(__APPLE__)
#define HAS_MALLOC_SIZE 1
static inline __swift_size_t _swift_stdlib_malloc_size(const void *ptr) {
  extern __swift_size_t malloc_size(const void *);
  return malloc_size(ptr);
}
#elif defined(__linux__) || defined(__CYGWIN__) || defined(__ANDROID__) \
   || defined(__HAIKU__) || defined(__FreeBSD__) || defined(__wasi__)
#define HAS_MALLOC_SIZE 1
static inline __swift_size_t _swift_stdlib_malloc_size(const void *ptr) {
#if defined(__ANDROID__)
#if !defined(__ANDROID_API__) || __ANDROID_API__ >= 17
  extern __swift_size_t malloc_usable_size(const void *ptr);
#endif
#else
  extern __swift_size_t malloc_usable_size(void *ptr);
#endif
  return malloc_usable_size(CONST_CAST(void *, ptr));
}
#elif defined(_WIN32)
#define HAS_MALLOC_SIZE 1
static inline __swift_size_t _swift_stdlib_malloc_size(const void *ptr) {
  extern __swift_size_t _msize(void *ptr);
  return _msize(CONST_CAST(void *, ptr));
}
#else
#define HAS_MALLOC_SIZE 0

static inline __swift_size_t _swift_stdlib_malloc_size(const void *ptr) {
  return 0;
}
#endif

static inline __swift_bool _swift_stdlib_has_malloc_size() {
  return HAS_MALLOC_SIZE != 0;
}

// Math library functions
static inline SWIFT_ALWAYS_INLINE
float _stdlib_remainderf(float _self, float _other) {
  return __builtin_remainderf(_self, _other);
}
  
static inline SWIFT_ALWAYS_INLINE
float _stdlib_squareRootf(float _self) {
#if defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))
  typedef float __m128 __attribute__((__vector_size__(16), __aligned__(16)));
  return __builtin_ia32_sqrtss(__extension__ (__m128){ _self, 0, 0, 0 })[0];
#else
  return __builtin_sqrtf(_self);
#endif
}

static inline SWIFT_ALWAYS_INLINE
double _stdlib_remainder(double _self, double _other) {
  return __builtin_remainder(_self, _other);
}

static inline SWIFT_ALWAYS_INLINE
double _stdlib_squareRoot(double _self) {
  return __builtin_sqrt(_self);
}

#if !defined _WIN32 && (defined __i386__ || defined __x86_64__)
static inline SWIFT_ALWAYS_INLINE
long double _stdlib_remainderl(long double _self, long double _other) {
  return __builtin_remainderl(_self, _other);
}
  
static inline SWIFT_ALWAYS_INLINE
long double _stdlib_squareRootl(long double _self) {
  return __builtin_sqrtl(_self);
}
#endif

// Apple's math.h does not declare lgamma_r() etc by default, but they're
// unconditionally exported by libsystem_m.dylib in all OS versions that
// support Swift development; we simply need to provide declarations here.
#if defined(__APPLE__)
float lgammaf_r(float x, int *psigngam);
double lgamma_r(double x, int *psigngam);
long double lgammal_r(long double x, int *psigngam);
#endif // defined(__APPLE__)

#ifdef __cplusplus
} // extern "C"
#endif

#if __has_feature(nullability)
#pragma clang assume_nonnull end
#endif

#endif // SWIFT_STDLIB_SHIMS_LIBCSHIMS_H
