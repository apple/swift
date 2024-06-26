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
#include <dlfcn.h>

typedef enum {
    dyld_objc_string_kind
} DyldObjCConstantKind;

using namespace swift;

typedef struct _CFBridgingState {
  long version;
  CFHashCode(*_CFStringHashCString)(const uint8_t *bytes, CFIndex len);
  CFHashCode(*_CFStringHashNSString)(id str);
  CFTypeID(*_CFGetTypeID)(CFTypeRef obj);
  CFTypeID _CFStringTypeID = 0;
  Class NSErrorClass;
  Class NSStringClass;
  Class NSArrayClass;
  Class NSMutableArrayClass;
  Class NSSetClass;
  Class NSDictionaryClass;
  Class NSEnumeratorClass;
  //version 0 ends here
} CFBridgingState;

static std::atomic<CFBridgingState const *> bridgingState;

static CFBridgingState const *getBridgingState() {
  return bridgingState.load(SWIFT_MEMORY_ORDER_CONSUME);
}

static inline bool initializeBridgingFunctions();
static CFBridgingState const *requireBridgingState() {
  assert(initializeBridgingFunctions());
  return getBridgingState();
}

extern "C" bool _dyld_is_objc_constant(DyldObjCConstantKind kind,
                                       const void *addr) SWIFT_RUNTIME_WEAK_IMPORT;

@class __SwiftNativeNSStringBase, __SwiftNativeNSError, __SwiftNativeNSArrayBase, __SwiftNativeNSMutableArrayBase, __SwiftNativeNSDictionaryBase, __SwiftNativeNSSetBase, __SwiftNativeNSEnumeratorBase;

static void _reparentClasses() {
  auto state = getBridgingState();
  if (state == nullptr) {
    return;
  }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  if (state->NSStringClass) {
    [state->NSStringClass class]; //make sure the class is realized
    class_setSuperclass([__SwiftNativeNSStringBase class],  state->NSStringClass);
  }
  if (state->NSErrorClass) {
    [state->NSErrorClass class]; //make sure the class is realized
    class_setSuperclass([__SwiftNativeNSError class], state->NSErrorClass);
  }
  if (state->NSArrayClass) {
    [state->NSArrayClass class]; //make sure the class is realized
    class_setSuperclass([__SwiftNativeNSArrayBase class],  state->NSArrayClass);
  }
  if (state->NSMutableArrayClass) {
    [state->NSMutableArrayClass class]; //make sure the class is realized
    class_setSuperclass([__SwiftNativeNSMutableArrayBase class],  state->NSMutableArrayClass);
  }
  if (state->NSDictionaryClass) {
    [state->NSDictionaryClass class]; //make sure the class is realized
    class_setSuperclass([__SwiftNativeNSDictionaryBase class],  state->NSDictionaryClass);
  }
  if (state->NSSetClass) {
    [state->NSSetClass class]; //make sure the class is realized
    class_setSuperclass([__SwiftNativeNSSetBase class],  state->NSSetClass);
  }
  if (state->NSEnumeratorClass) {
    [state->NSEnumeratorClass class]; //make sure the class is realized
    class_setSuperclass([__SwiftNativeNSEnumeratorBase class],  state->NSEnumeratorClass);
  }
#pragma clang diagnostic pop
}

static inline bool initializeBridgingFunctions() {
  auto state = getBridgingState();
  if (state == nullptr) {
    auto getStringTypeID = (CFTypeID(*)(void))dlsym(RTLD_DEFAULT, "CFStringGetTypeID");
    if (!getStringTypeID) {
      return false; //CF not loaded
    }
    auto newState = (CFBridgingState *)calloc(1, sizeof(CFBridgingState));
    newState->version = 0;
    newState->_CFStringTypeID = getStringTypeID();
    newState->_CFGetTypeID = (CFTypeID(*)(CFTypeRef obj))dlsym(RTLD_DEFAULT, "CFGetTypeID");
    newState->_CFStringHashNSString = (CFHashCode(*)(id))dlsym(RTLD_DEFAULT, "CFStringHashNSString");
    newState->_CFStringHashCString = (CFHashCode(*)(const uint8_t *, CFIndex))dlsym(RTLD_DEFAULT, "CFStringHashCString");
    newState->NSErrorClass = objc_lookUpClass("NSError");
    newState->NSStringClass = objc_lookUpClass("NSString");
    newState->NSArrayClass = objc_lookUpClass("NSArray");
    newState->NSMutableArrayClass = objc_lookUpClass("NSMutableArray");
    newState->NSSetClass = objc_lookUpClass("NSSet");
    newState->NSDictionaryClass = objc_lookUpClass("NSDictionary");
    newState->NSEnumeratorClass = objc_lookUpClass("NSEnumerator");
    const CFBridgingState *nullState = nullptr;
    if (!bridgingState.compare_exchange_strong(nullState, state, std::memory_order_seq_cst, std::memory_order_seq_cst)) {
      free(const_cast<CFBridgingState *>(state));
    }
    _reparentClasses();
  }
  state = getBridgingState();
  return state && state->_CFGetTypeID != nullptr;
}

SWIFT_RUNTIME_EXPORT void swift_initializeCoreFoundationState(CFBridgingState const * const state) {
  //It's fine if this runs more than once, it's a noop if it's been done before
  //and we want to make sure it still happens if CF loads late after it failed initially
  bridgingState.store(state, std::memory_order_release);
  _reparentClasses();
}

namespace swift {
Class getNSErrorClass();
}

Class swift::getNSErrorClass() {
  if (initializeBridgingFunctions()) {
    return getBridgingState()->NSErrorClass;
  }
  return nullptr;
}

static inline void initializeBridgingFunctions() {
  swift_once(&initializeBridgingFuncsOnce,
             _initializeBridgingFunctionsImpl,
             nullptr);
}

__swift_uint8_t
_swift_stdlib_isNSString(id obj) {
  auto state = requireBridgingState();
  return state->_CFGetTypeID((CFTypeRef)obj) == state->_CFStringTypeID ? 1 : 0;
}

_swift_shims_CFHashCode
_swift_stdlib_CFStringHashNSString(id _Nonnull obj) {
  return requireBridgingState()->_CFStringHashNSString(obj);
}

_swift_shims_CFHashCode
_swift_stdlib_CFStringHashCString(const _swift_shims_UInt8 * _Nonnull bytes,
                                  _swift_shims_CFIndex length) {
  return requireBridgingState()->_CFStringHashCString(bytes, length);
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

__swift_uint8_t
_swift_stdlib_dyld_is_objc_constant_string(const void *addr) {
  return (SWIFT_RUNTIME_WEAK_CHECK(_dyld_is_objc_constant)
          && SWIFT_RUNTIME_WEAK_USE(_dyld_is_objc_constant(dyld_objc_string_kind, addr))) ? 1 : 0;
}

#endif

