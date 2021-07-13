//===--- UnicodeNormalization.cpp - Unicode Normalization Helpers ---------===//
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
// Functions that use ICU to do unicode normalization and collation.
//
//===----------------------------------------------------------------------===//

#include "../SwiftShims/UnicodeShims.h"

#include <stdint.h>

#if defined(__APPLE__)

// Declare a few external functions to avoid a dependency on ICU headers.
extern "C" {

// Types
typedef struct UBreakIterator UNormalizer2;
typedef enum UErrorCode {} UErrorCode;
typedef enum UCharNameChoice {} UCharNameChoice;
typedef uint16_t UChar;
typedef int32_t UChar32;
typedef int8_t UBool;
typedef __swift_stdlib_UProperty UProperty;

#define U_MAX_VERSION_LENGTH 4
typedef uint8_t UVersionInfo[U_MAX_VERSION_LENGTH];

// Comparison, normalization, and character property APIs
int32_t unorm2_spanQuickCheckYes(const UNormalizer2 *, const UChar *, int32_t,
                                 UErrorCode *);
int32_t unorm2_normalize(const UNormalizer2 *, const UChar *, int32_t, UChar *,
                         int32_t, UErrorCode *);
const UNormalizer2 *unorm2_getNFCInstance(UErrorCode *);
UBool unorm2_hasBoundaryBefore(const UNormalizer2 *norm2, UChar32 c);
UBool u_hasBinaryProperty(UChar32, UProperty);
void u_charAge(UChar32, UVersionInfo);
int32_t u_getIntPropertyValue(UChar32, UProperty);
int32_t u_charName(UChar32, UCharNameChoice, char *, int32_t, UErrorCode *);
int32_t u_strToLower(UChar *, int32_t, const UChar *, int32_t, const char *,
                     UErrorCode *);
int32_t u_strToTitle(UChar *, int32_t, const UChar *, int32_t,
                     UBreakIterator *, const char *, UErrorCode *);
int32_t u_strToUpper(UChar *, int32_t, const UChar *, int32_t, const char *,
                     UErrorCode *);
double u_getNumericValue(UChar32);
}

#else

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation"

#include <unicode/ustring.h>
#include <unicode/ucol.h>
#include <unicode/ucoleitr.h>
#include <unicode/uiter.h>
#include <unicode/uchar.h>
#include <unicode/ustring.h>
#include <unicode/uvernum.h>
#include <unicode/uversion.h>

#pragma clang diagnostic pop

#endif

#if !defined(__APPLE__)
#include "swift/Basic/Lazy.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/Debug.h"

#include <algorithm>
#include <mutex>
#include <assert.h>

/// Convert the unicode string to uppercase. This function will return the
/// required buffer length as a result. If this length does not match the
/// 'DestinationCapacity' this function must be called again with a buffer of
/// the required length to get an uppercase version of the string.
int32_t
_swift_stdlib_unicode_strToUpper(uint16_t *Destination,
                                        int32_t DestinationCapacity,
                                        const uint16_t *Source,
                                        int32_t SourceLength) {
  UErrorCode ErrorCode = U_ZERO_ERROR;
  uint32_t OutputLength = u_strToUpper(reinterpret_cast<UChar *>(Destination),
                                       DestinationCapacity,
                                       reinterpret_cast<const UChar *>(Source),
                                       SourceLength,
                                       "", &ErrorCode);
  if (U_FAILURE(ErrorCode) && ErrorCode != U_BUFFER_OVERFLOW_ERROR) {
    swift::crash("u_strToUpper: Unexpected error uppercasing unicode string.");
  }
  return OutputLength;
}

/// Convert the unicode string to lowercase. This function will return the
/// required buffer length as a result. If this length does not match the
/// 'DestinationCapacity' this function must be called again with a buffer of
/// the required length to get a lowercase version of the string.
int32_t
_swift_stdlib_unicode_strToLower(uint16_t *Destination,
                                        int32_t DestinationCapacity,
                                        const uint16_t *Source,
                                        int32_t SourceLength) {
  UErrorCode ErrorCode = U_ZERO_ERROR;
  uint32_t OutputLength = u_strToLower(reinterpret_cast<UChar *>(Destination),
                                       DestinationCapacity,
                                       reinterpret_cast<const UChar *>(Source),
                                       SourceLength,
                                       "", &ErrorCode);
  if (U_FAILURE(ErrorCode) && ErrorCode != U_BUFFER_OVERFLOW_ERROR) {
    swift::crash("u_strToLower: Unexpected error lowercasing unicode string.");
  }
  return OutputLength;
}
#endif

namespace {
template <typename T, typename U> T *ptr_cast(U *p) {
  return static_cast<T *>(static_cast<void *>(p));
}
template <typename T, typename U> const T *ptr_cast(const U *p) {
  return static_cast<const T *>(static_cast<const void *>(p));
}
}

__swift_stdlib_UBool __swift_stdlib_unorm2_hasBoundaryBefore(
    const __swift_stdlib_UNormalizer2 *ptr, __swift_stdlib_UChar32 char32) {
  return unorm2_hasBoundaryBefore(ptr_cast<UNormalizer2>(ptr), char32);
}
const __swift_stdlib_UNormalizer2 *
__swift_stdlib_unorm2_getNFCInstance(__swift_stdlib_UErrorCode *err) {
  return ptr_cast<__swift_stdlib_UNormalizer2>(
      unorm2_getNFCInstance(ptr_cast<UErrorCode>(err)));
}

int32_t __swift_stdlib_unorm2_normalize(
    const __swift_stdlib_UNormalizer2 *norm, const __swift_stdlib_UChar *src,
    __swift_int32_t len, __swift_stdlib_UChar *dst, __swift_int32_t capacity,
    __swift_stdlib_UErrorCode *err) {
  // TODO remove this compatibility when we require ICU >= 59 on Linux
#if defined(__APPLE__) || U_ICU_VERSION_MAJOR_NUM >= 59
  return unorm2_normalize(ptr_cast<UNormalizer2>(norm), src, len, dst, capacity,
                          ptr_cast<UErrorCode>(err));
#else
  return unorm2_normalize(ptr_cast<UNormalizer2>(norm),
                          reinterpret_cast<const UChar *>(src), len,
                          reinterpret_cast<UChar *>(dst), capacity,
                          ptr_cast<UErrorCode>(err));
#endif
}

__swift_int32_t __swift_stdlib_unorm2_spanQuickCheckYes(
    const __swift_stdlib_UNormalizer2 *norm, const __swift_stdlib_UChar *ptr,
    __swift_int32_t len, __swift_stdlib_UErrorCode *err) {
  return unorm2_spanQuickCheckYes(ptr_cast<UNormalizer2>(norm),
                                  ptr_cast<UChar>(ptr), len,
                                  ptr_cast<UErrorCode>(err));
}

__swift_stdlib_UBool
__swift_stdlib_u_hasBinaryProperty(__swift_stdlib_UChar32 c,
                                          __swift_stdlib_UProperty p) {
  return u_hasBinaryProperty(c, static_cast<UProperty>(p));
}

void
__swift_stdlib_u_charAge(__swift_stdlib_UChar32 c,
                                __swift_stdlib_UVersionInfo versionInfo) {
  return u_charAge(c, versionInfo);
}

__swift_int32_t
__swift_stdlib_u_getIntPropertyValue(__swift_stdlib_UChar32 c,
                                            __swift_stdlib_UProperty p) {
  return u_getIntPropertyValue(c, static_cast<UProperty>(p));
}

__swift_int32_t __swift_stdlib_u_charName(
    __swift_stdlib_UChar32 code, __swift_stdlib_UCharNameChoice nameChoice,
    char *buffer, __swift_int32_t bufferLength,
    __swift_stdlib_UErrorCode *pErrorCode) {
  return u_charName(code, static_cast<UCharNameChoice>(nameChoice),
                    buffer, bufferLength,
                    ptr_cast<UErrorCode>(pErrorCode));
}

__swift_int32_t __swift_stdlib_u_strToLower(
    __swift_stdlib_UChar *dest, __swift_int32_t destCapacity,
    const __swift_stdlib_UChar *src, __swift_int32_t srcLength,
    const char *locale, __swift_stdlib_UErrorCode *pErrorCode) {
  return u_strToLower(ptr_cast<UChar>(dest), destCapacity,
                      ptr_cast<UChar>(src), srcLength,
                      locale, ptr_cast<UErrorCode>(pErrorCode));
}

__swift_int32_t __swift_stdlib_u_strToTitle(
    __swift_stdlib_UChar *dest, __swift_int32_t destCapacity,
    const __swift_stdlib_UChar *src, __swift_int32_t srcLength,
    __swift_stdlib_UBreakIterator *titleIter, const char *locale,
    __swift_stdlib_UErrorCode *pErrorCode) {
  return u_strToTitle(ptr_cast<UChar>(dest), destCapacity,
                      ptr_cast<UChar>(src), srcLength,
                      ptr_cast<UBreakIterator>(titleIter), locale,
                      ptr_cast<UErrorCode>(pErrorCode));
}

__swift_int32_t __swift_stdlib_u_strToUpper(
    __swift_stdlib_UChar *dest, __swift_int32_t destCapacity,
    const __swift_stdlib_UChar *src, __swift_int32_t srcLength,
    const char *locale, __swift_stdlib_UErrorCode *pErrorCode) {
  return u_strToUpper(ptr_cast<UChar>(dest), destCapacity,
                      ptr_cast<UChar>(src), srcLength,
                      locale, ptr_cast<UErrorCode>(pErrorCode));
}

double __swift_stdlib_u_getNumericValue(__swift_stdlib_UChar32 c) {
  return u_getNumericValue(c);
}


// Force an autolink with ICU
#if defined(__MACH__)
asm(".linker_option \"-licucore\"\n");
#endif // defined(__MACH__)

