//===--- CoreFoundationShims.h - Access to CF for the core stdlib ---------===//
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
//  Using the CoreFoundation module in the core stdlib would create a
//  circular dependency, so instead we import these declarations as
//  part of SwiftShims.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_STDLIB_SHIMS_COREFOUNDATIONSHIMS_H
#define SWIFT_STDLIB_SHIMS_COREFOUNDATIONSHIMS_H

#include "SwiftStdint.h"
#include "Visibility.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __OBJC2__
#if __LLP64__
typedef unsigned long long _swift_shims_CFHashCode;
typedef signed long long _swift_shims_CFIndex;
#else
typedef unsigned long _swift_shims_CFHashCode;
typedef signed long _swift_shims_CFIndex;
#endif

// Consider creating SwiftMacTypes.h for these
typedef unsigned char _swift_shims_Boolean;
typedef __swift_uint8_t _swift_shims_UInt8;
typedef __swift_uint32_t _swift_shims_CFStringEncoding;

/* This is layout-compatible with constant CFStringRefs on Darwin */
typedef struct __swift_shims_builtin_CFString {
  const void * _Nonnull isa; // point to __CFConstantStringClassReference
  unsigned long flags;
  const __swift_uint8_t * _Nonnull str;
  unsigned long length;
} _swift_shims_builtin_CFString;

SWIFT_RUNTIME_STDLIB_API
void * _Nullable
_swift_stdlib_createTaggedPointerString(const _swift_shims_UInt8 * _Nonnull bytes,
                                        _swift_shims_CFIndex length);

SWIFT_RUNTIME_STDLIB_API
__swift_uint8_t _swift_stdlib_isNSString(id _Nonnull obj);

SWIFT_RUNTIME_STDLIB_API
_swift_shims_CFHashCode _swift_stdlib_CFStringHashNSString(id _Nonnull obj);

SWIFT_RUNTIME_STDLIB_API
_swift_shims_CFHashCode
_swift_stdlib_CFStringHashCString(const _swift_shims_UInt8 * _Nonnull bytes,
                                  _swift_shims_CFIndex length);

SWIFT_RUNTIME_STDLIB_API
const __swift_uint8_t * _Nullable
_swift_stdlib_NSStringCStringUsingEncodingTrampoline(id _Nonnull obj,
                                                     unsigned long encoding);

SWIFT_RUNTIME_STDLIB_API
__swift_uint8_t
_swift_stdlib_NSStringGetCStringTrampoline(id _Nonnull obj,
                                           _swift_shims_UInt8 *_Nonnull buffer,
                                           _swift_shims_CFIndex maxLength,
                                           unsigned long encoding);

SWIFT_RUNTIME_STDLIB_API
__swift_uint8_t
_swift_stdlib_dyld_is_objc_constant_string(const void * _Nonnull addr);
  
#endif // __OBJC2__

#ifdef __cplusplus
} // extern "C"
#endif

#endif // SWIFT_STDLIB_SHIMS_COREFOUNDATIONSHIMS_H

