//===--- FoundationHelpers.mm - Cocoa framework helper shims --------------===//
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
// This file contains shims to refer to framework functions required by the
// standard library. The stdlib cannot directly import these modules without
// introducing circular dependencies.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Config.h"

#if SWIFT_OBJC_INTEROP
#import <CoreFoundation/CoreFoundation.h>
#include "swift/shims/CoreFoundationShims.h"
#import <objc/runtime.h>
#include "swift/Runtime/Once.h"
#include <dlfcn.h>

typedef enum {
    dyld_objc_string_kind
} DyldObjCConstantKind;

using namespace swift;

static CFHashCode(*_CFStringHashCString)(const uint8_t *bytes, CFIndex len);
static CFHashCode(*_CFStringHashNSString)(id str);
static id(*_CFStringCreateTaggedPointerString)(const uint8_t *bytes, CFIndex numBytes);
static __swift_uint8_t(*_NSIsNSString)(id arg);
static once_t initializeBridgingFuncsOnce;

static void _initializeBridgingFunctionsImpl(void *ctxt) {
  void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY | RTLD_NOLOAD | RTLD_LOCAL | RTLD_FIRST);
  _NSIsNSString = (__swift_uint8_t(*)(id))dlsym(cf ? cf : RTLD_DEFAULT, "_NSIsNSString");
  _CFStringHashNSString = (CFHashCode(*)(id))dlsym(cf ? cf : RTLD_DEFAULT, "CFStringHashNSString");
  _CFStringHashCString = (CFHashCode(*)(const uint8_t *, CFIndex))dlsym(cf ? cf : RTLD_DEFAULT, "CFStringHashCString");
  _CFStringCreateTaggedPointerString = (id(*)(const uint8_t *, CFIndex))dlsym(cf ? cf : RTLD_DEFAULT, "_CFStringCreateTaggedPointerString");
  if (cf) {
    dlclose(cf);
  }
}

static inline void initializeBridgingFunctions() {
  once(initializeBridgingFuncsOnce,
       _initializeBridgingFunctionsImpl,
       nullptr);
}

void *
_swift_stdlib_createTaggedPointerString(const _swift_shims_UInt8 * _Nonnull bytes,
                                        _swift_shims_CFIndex length) {
  initializeBridgingFunctions();
 // if (_CFStringCreateTaggedPointerString != NULL) {
    return (void *)_CFStringCreateTaggedPointerString(bytes, length);
 // }
 // return nil;
}

__swift_uint8_t
_swift_stdlib_isNSString(id obj) {
  initializeBridgingFunctions();
  if (_NSIsNSString != NULL) {
    return _NSIsNSString(obj);
  }
  return [obj isKindOfClass: objc_lookUpClass("NSString")];
}

_swift_shims_CFHashCode
_swift_stdlib_CFStringHashNSString(id _Nonnull obj) {
  initializeBridgingFunctions();
  return _CFStringHashNSString(obj);
}

_swift_shims_CFHashCode
_swift_stdlib_CFStringHashCString(const _swift_shims_UInt8 * _Nonnull bytes,
                                  _swift_shims_CFIndex length) {
  initializeBridgingFunctions();
  return _CFStringHashCString(bytes, length);
}

const __swift_uint8_t *
_swift_stdlib_NSStringCStringUsingEncodingTrampoline(id _Nonnull obj,
                                                  unsigned long encoding) {
  typedef __swift_uint8_t * _Nullable (*cStrImplPtr)(id, SEL, unsigned long);
  cStrImplPtr imp = (cStrImplPtr)class_getMethodImplementation([obj superclass],
                                                               @selector(cStringUsingEncoding:));
  return imp(obj, @selector(cStringUsingEncoding:), encoding);
}

__swift_uint8_t
_swift_stdlib_NSStringGetCStringTrampoline(id _Nonnull obj,
                                         _swift_shims_UInt8 *buffer,
                                         _swift_shims_CFIndex maxLength,
                                         unsigned long encoding) {
  typedef __swift_uint8_t (*getCStringImplPtr)(id,
                                             SEL,
                                             _swift_shims_UInt8 *,
                                             _swift_shims_CFIndex,
                                             unsigned long);
  SEL sel = @selector(getCString:maxLength:encoding:);
  getCStringImplPtr imp = (getCStringImplPtr)class_getMethodImplementation([obj superclass], sel);
  
  return imp(obj, sel, buffer, maxLength, encoding);

}

//extern "C" bool _dyld_is_objc_constant(DyldObjCConstantKind kind,
//                                       const void *addr) SWIFT_RUNTIME_WEAK_IMPORT;

__swift_uint8_t
_swift_stdlib_dyld_is_objc_constant_string(const void *addr) {
  return false;
  //This currently always return false
//  return (SWIFT_RUNTIME_WEAK_CHECK(_dyld_is_objc_constant)
//          && SWIFT_RUNTIME_WEAK_USE(_dyld_is_objc_constant(dyld_objc_string_kind, addr))) ? 1 : 0;
}

#endif

