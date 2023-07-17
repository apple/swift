//===--- SwiftObject.mm - Native Swift Object root class ------------------===//
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
// This implements the Objective-C root class that provides basic `id`-
// compatibility and `NSObject` protocol conformance for pure Swift classes.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Config.h"

#if SWIFT_OBJC_INTEROP
#include <objc/NSObject.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <objc/objc.h>
#endif
#include "llvm/ADT/StringRef.h"
#include "swift/Basic/Lazy.h"
#include "swift/Runtime/Bincompat.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/CustomRRABI.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/EnvironmentVariables.h"
#include "swift/Runtime/Heap.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/ObjCBridge.h"
#include "swift/Runtime/Portability.h"
#include "swift/Strings.h"
#include "swift/shims/RuntimeShims.h"
#include "swift/shims/AssertionReporting.h"
#include "../CompatibilityOverride/CompatibilityOverride.h"
#include "ErrorObject.h"
#include "Private.h"
#include "SwiftObject.h"
#include "SwiftValue.h"
#include "WeakReference.h"
#if SWIFT_OBJC_INTEROP
#include <dlfcn.h>
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unordered_map>
#if SWIFT_OBJC_INTEROP
# import <CoreFoundation/CFBase.h> // for CFTypeID
# import <Foundation/Foundation.h>
# include <malloc/malloc.h>
# include <dispatch/dispatch.h>
#endif

using namespace swift;
using namespace hashable_support;

#if SWIFT_HAS_ISA_MASKING
OBJC_EXPORT __attribute__((__weak_import__))
const uintptr_t objc_debug_isa_class_mask;

uintptr_t swift::swift_isaMask = SWIFT_ISA_MASK;
#endif

const ClassMetadata *swift::_swift_getClass(const void *object) {
#if SWIFT_OBJC_INTEROP
  if (!isObjCTaggedPointer(object))
    return _swift_getClassOfAllocated(object);
  return reinterpret_cast<const ClassMetadata*>(
    object_getClass(id_const_cast(object)));
#else
  return _swift_getClassOfAllocated(object);
#endif
}

#if SWIFT_OBJC_INTEROP
/// Replacement for ObjC object_isClass(), which is unavailable on
/// deployment targets macOS 10.9 and iOS 7.
static bool objcObjectIsClass(id object) {
  // same as object_isClass(object)
  return class_isMetaClass(object_getClass(object));
}

/// Same as _swift_getClassOfAllocated() but returns type Class.
static Class _swift_getObjCClassOfAllocated(const void *object) {
  return class_const_cast(_swift_getClassOfAllocated(object));
}

/// Fetch the ObjC class object associated with the formal dynamic
/// type of the given (possibly Objective-C) object.  The formal
/// dynamic type ignores dynamic subclasses such as those introduced
/// by KVO.
///
/// The object pointer may be a tagged pointer, but cannot be null.
const ClassMetadata *swift::swift_getObjCClassFromObject(HeapObject *object) {
  auto classAsMetadata = _swift_getClass(object);

  // Walk up the superclass chain skipping over artificial Swift classes.
  // If we find a non-Swift class use the result of [object class] instead.

  while (classAsMetadata && classAsMetadata->isTypeMetadata()) {
    if (!classAsMetadata->isArtificialSubclass())
      return classAsMetadata;
    classAsMetadata = classAsMetadata->Superclass;
  }

  id objcObject = reinterpret_cast<id>(object);
  Class objcClass = [objcObject class];
  if (objcObjectIsClass(objcObject)) {
    // Original object is a class. We want a
    // metaclass but +class doesn't give that to us.
    objcClass = object_getClass(objcClass);
  }
  classAsMetadata = reinterpret_cast<const ClassMetadata *>(objcClass);
  return classAsMetadata;
}
#endif

/// Fetch the type metadata associated with the formal dynamic
/// type of the given (possibly Objective-C) object.  The formal
/// dynamic type ignores dynamic subclasses such as those introduced
/// by KVO.
///
/// The object pointer may be a tagged pointer, but cannot be null.
const Metadata *swift::swift_getObjectType(HeapObject *object) {
  auto classAsMetadata = _swift_getClass(object);

#if SWIFT_OBJC_INTEROP
  // Walk up the superclass chain skipping over artificial Swift classes.
  // If we find a non-Swift class use the result of [object class] instead.

  while (classAsMetadata && classAsMetadata->isTypeMetadata()) {
    if (!classAsMetadata->isArtificialSubclass())
      return classAsMetadata;
    classAsMetadata = classAsMetadata->Superclass;
  }

  id objcObject = reinterpret_cast<id>(object);
  Class objcClass = [objcObject class];
  if (objcObjectIsClass(objcObject)) {
    // Original object is a class. We want a
    // metaclass but +class doesn't give that to us.
    objcClass = object_getClass(objcClass);
  }
  classAsMetadata = reinterpret_cast<const ClassMetadata *>(objcClass);
  return swift_getObjCClassMetadata(classAsMetadata);
#else
  assert(classAsMetadata &&
         classAsMetadata->isTypeMetadata() &&
         !classAsMetadata->isArtificialSubclass());
  return classAsMetadata;
#endif
}

#if SWIFT_OBJC_INTEROP
static SwiftObject *_allocHelper(Class cls) {
  // XXX FIXME
  // When we have layout information, do precise alignment rounding
  // For now, assume someone is using hardware vector types
#if defined(__x86_64__) || defined(__i386__)
  const size_t mask = 32 - 1;
#else
  const size_t mask = 16 - 1;
#endif
  return reinterpret_cast<SwiftObject *>(swift::swift_allocObject(
    reinterpret_cast<HeapMetadata const *>(cls),
    class_getInstanceSize(cls), mask));
}

SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
Class _swift_classOfObjCHeapObject(OpaqueValue *value) {
  return _swift_getObjCClassOfAllocated(value);
}

SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
id swift_stdlib_getDescription(OpaqueValue *value,
                                      const Metadata *type);

id swift::getDescription(OpaqueValue *value, const Metadata *type) {
  id result = swift_stdlib_getDescription(value, type);
  type->vw_destroy(value);
  return [result autorelease];
}

static id _getObjectDescription(SwiftObject *obj) {
  swift_retain((HeapObject*)obj);
  return getDescription((OpaqueValue*)&obj,
                        _swift_getClassOfAllocated(obj));
}

static id _getClassDescription(Class cls) {
  const char *name = class_getName(cls);
  int len = strlen(name);
  return [swift_stdlib_NSStringFromUTF8(name, len) autorelease];
}

@implementation SwiftObject
+ (void)initialize {
#if SWIFT_HAS_ISA_MASKING && !TARGET_OS_SIMULATOR && !NDEBUG
  uintptr_t libObjCMask = (uintptr_t)&objc_absolute_packed_isa_class_mask;
  assert(libObjCMask);

#  if __arm64__ && !__has_feature(ptrauth_calls)
  // When we're built ARM64 but running on ARM64e hardware, we will get an
  // ARM64e libobjc with an ARM64e ISA mask. This mismatch is harmless and we
  // shouldn't assert.
  assert(libObjCMask == SWIFT_ISA_MASK || libObjCMask == SWIFT_ISA_MASK_PTRAUTH);
#  else
  assert(libObjCMask == SWIFT_ISA_MASK);
#  endif
#endif
}

+ (instancetype)allocWithZone:(struct _NSZone *)zone {
  assert(zone == nullptr);
  return _allocHelper(self);
}

+ (instancetype)alloc {
  // we do not support "placement new" or zones,
  // so there is no need to call allocWithZone
  return _allocHelper(self);
}

+ (Class)class {
  return self;
}
- (Class)class {
  return _swift_getObjCClassOfAllocated(self);
}
+ (Class)superclass {
  return (Class)((const ClassMetadata*) self)->Superclass;
}
- (Class)superclass {
  return (Class)_swift_getClassOfAllocated(self)->Superclass;
}

+ (BOOL)isMemberOfClass:(Class)cls {
  return cls == _swift_getObjCClassOfAllocated(self);
}

- (BOOL)isMemberOfClass:(Class)cls {
  return cls == _swift_getObjCClassOfAllocated(self);
}

- (instancetype)self {
  return self;
}
- (BOOL)isProxy {
  return NO;
}

- (struct _NSZone *)zone {
  auto zone = malloc_zone_from_ptr(self);
  return (struct _NSZone *)(zone ? zone : malloc_default_zone());
}

- (void)doesNotRecognizeSelector: (SEL) sel {
  Class cls = _swift_getObjCClassOfAllocated(self);
  fatalError(/* flags = */ 0,
             "Unrecognized selector %c[%s %s]\n",
             class_isMetaClass(cls) ? '+' : '-',
             class_getName(cls), sel_getName(sel));
}

STANDARD_OBJC_METHOD_IMPLS_FOR_SWIFT_OBJECTS

// Retaining the class object itself is a no-op.
+ (id)retain {
  return self;
}
+ (void)release {
  /* empty */
}
+ (id)autorelease {
  return self;
}
+ (NSUInteger)retainCount {
  return ULONG_MAX;
}
+ (BOOL)_isDeallocating {
  return NO;
}
+ (BOOL)_tryRetain {
  return YES;
}
+ (BOOL)allowsWeakReference {
  return YES;
}
+ (BOOL)retainWeakReference {
  return YES;
}

- (BOOL)isKindOfClass:(Class)someClass {
  for (auto cls = _swift_getClassOfAllocated(self); cls != nullptr;
       cls = cls->Superclass)
    if (cls == (const ClassMetadata*) someClass)
      return YES;

  return NO;
}

+ (BOOL)isSubclassOfClass:(Class)someClass {
  for (auto cls = (const ClassMetadata*) self; cls != nullptr;
       cls = cls->Superclass)
    if (cls == (const ClassMetadata*) someClass)
      return YES;

  return NO;
}

+ (BOOL)respondsToSelector:(SEL)sel {
  if (!sel) return NO;
  return class_respondsToSelector(_swift_getObjCClassOfAllocated(self), sel);
}

- (BOOL)respondsToSelector:(SEL)sel {
  if (!sel) return NO;
  return class_respondsToSelector(_swift_getObjCClassOfAllocated(self), sel);
}

+ (BOOL)instancesRespondToSelector:(SEL)sel {
  if (!sel) return NO;
  return class_respondsToSelector(self, sel);
}


+ (IMP)methodForSelector:(SEL)sel {
  return class_getMethodImplementation(object_getClass((id)self), sel);
}

- (IMP)methodForSelector:(SEL)sel {
  return class_getMethodImplementation(object_getClass(self), sel);
}

+ (IMP)instanceMethodForSelector:(SEL)sel {
  return class_getMethodImplementation(self, sel);
}


- (BOOL)conformsToProtocol:(Protocol*)proto {
  if (!proto) return NO;
  auto selfClass = _swift_getObjCClassOfAllocated(self);

  // Walk the superclass chain.
  while (selfClass) {
    if (class_conformsToProtocol(selfClass, proto))
      return YES;
    selfClass = class_getSuperclass(selfClass);
  }

  return NO;
}

+ (BOOL)conformsToProtocol:(Protocol*)proto {
  if (!proto) return NO;

  // Walk the superclass chain.
  Class selfClass = self;
  while (selfClass) {
    if (class_conformsToProtocol(selfClass, proto))
      return YES;
    selfClass = class_getSuperclass(selfClass);
  }

  return NO;
}

- (NSUInteger)hash {
  return (NSUInteger)self;
}

- (BOOL)isEqual:(id)object {
  return self == object;
}

- (id)performSelector:(SEL)aSelector {
  return ((id(*)(id, SEL))objc_msgSend)(self, aSelector);
}

- (id)performSelector:(SEL)aSelector withObject:(id)object {
  return ((id(*)(id, SEL, id))objc_msgSend)(self, aSelector, object);
}

- (id)performSelector:(SEL)aSelector withObject:(id)object1
                                     withObject:(id)object2 {
  return ((id(*)(id, SEL, id, id))objc_msgSend)(self, aSelector, object1,
                                                                 object2);
}

- (id /* NSString */)description {
  return _getObjectDescription(self);
}
- (id /* NSString */)debugDescription {
  return _getObjectDescription(self);
}

+ (id /* NSString */)description {
  return _getClassDescription(self);
}
+ (id /* NSString */)debugDescription {
  return _getClassDescription(self);
}

- (id /* NSString */)_copyDescription {
  // The NSObject version of this pushes an autoreleasepool in case -description
  // autoreleases, but we're OK with leaking things if we're at the top level
  // of the main thread with no autorelease pool.
  return [[self description] retain];
}

- (CFTypeID)_cfTypeID {
  return (CFTypeID)1; //NSObject's CFTypeID is constant
}

// Foundation collections expect these to be implemented.
- (BOOL)isNSArray__      { return NO; }
- (BOOL)isNSCFConstantString__  { return NO; }
- (BOOL)isNSData__       { return NO; }
- (BOOL)isNSDate__       { return NO; }
- (BOOL)isNSDictionary__ { return NO; }
- (BOOL)isNSObject__     { return NO; }
- (BOOL)isNSOrderedSet__ { return NO; }
- (BOOL)isNSNumber__     { return NO; }
- (BOOL)isNSSet__        { return NO; }
- (BOOL)isNSString__     { return NO; }
- (BOOL)isNSTimeZone__   { return NO; }
- (BOOL)isNSValue__      { return NO; }

@end

#endif

/// Decide dynamically whether the given class uses native Swift
/// reference-counting.
bool swift::usesNativeSwiftReferenceCounting(const ClassMetadata *theClass) {
#if SWIFT_OBJC_INTEROP
  if (!theClass->isTypeMetadata()) return false;
  return (theClass->getFlags() & ClassFlags::UsesSwiftRefcounting);
#else
  return true;
#endif
}

/// Decide dynamically whether the given type metadata uses native Swift
/// reference-counting.  The metadata is known to correspond to a class
/// type, but note that does not imply being known to be a ClassMetadata
/// due to the existence of ObjCClassWrapper.
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_SPI
bool
_swift_objcClassUsesNativeSwiftReferenceCounting(const Metadata *theClass) {
#if SWIFT_OBJC_INTEROP
  // If this is ObjC wrapper metadata, the class is definitely not using
  // Swift ref-counting.
  if (isa<ObjCClassWrapperMetadata>(theClass)) return false;

  // Otherwise, it's class metadata.
  return usesNativeSwiftReferenceCounting(cast<ClassMetadata>(theClass));
#else
  return true;
#endif
}

// The non-pointer bits, excluding the tag bits.
static auto const unTaggedNonNativeBridgeObjectBits
  = heap_object_abi::SwiftSpareBitsMask
  & ~heap_object_abi::ObjCReservedBitsMask
  & ~heap_object_abi::BridgeObjectTagBitsMask;

#if SWIFT_OBJC_INTEROP

#if defined(__x86_64__)
static uintptr_t const objectPointerIsObjCBit = 0x4000000000000000ULL;
#elif defined(__LP64__)
static uintptr_t const objectPointerIsObjCBit = 0x4000000000000000ULL;
#else
static uintptr_t const objectPointerIsObjCBit = 0x00000002U;
#endif

void *swift::swift_unknownObjectRetain_n(void *object, int n) {
  if (isObjCTaggedPointerOrNull(object)) return object;
  if (objectUsesNativeSwiftReferenceCounting(object)) {
    return swift_retain_n(static_cast<HeapObject *>(object), n);
  }
  for (int i = 0; i < n; ++i)
    objc_retain(static_cast<id>(object));

  return object;
}

void swift::swift_unknownObjectRelease_n(void *object, int n) {
  if (isObjCTaggedPointerOrNull(object)) return;
  if (objectUsesNativeSwiftReferenceCounting(object))
    return swift_release_n(static_cast<HeapObject *>(object), n);
  for (int i = 0; i < n; ++i)
    objc_release(static_cast<id>(object));
}

void *swift::swift_unknownObjectRetain(void *object) {
  if (isObjCTaggedPointerOrNull(object)) return object;
  if (objectUsesNativeSwiftReferenceCounting(object)) {
    return swift_retain(static_cast<HeapObject *>(object));
  }
  return objc_retain(static_cast<id>(object));
}

void swift::swift_unknownObjectRelease(void *object) {
  if (isObjCTaggedPointerOrNull(object)) return;
  if (objectUsesNativeSwiftReferenceCounting(object))
    return swift_release(static_cast<HeapObject *>(object));
  return objc_release(static_cast<id>(object));
}

void *swift::swift_nonatomic_unknownObjectRetain_n(void *object, int n) {
  if (isObjCTaggedPointerOrNull(object)) return object;
  if (objectUsesNativeSwiftReferenceCounting(object)) {
    return swift_nonatomic_retain_n(static_cast<HeapObject *>(object), n);
  }
  for (int i = 0; i < n; ++i)
    objc_retain(static_cast<id>(object));
  return object;
}

void swift::swift_nonatomic_unknownObjectRelease_n(void *object, int n) {
  if (isObjCTaggedPointerOrNull(object)) return;
  if (objectUsesNativeSwiftReferenceCounting(object))
    return swift_nonatomic_release_n(static_cast<HeapObject *>(object), n);
  for (int i = 0; i < n; ++i)
    objc_release(static_cast<id>(object));
}

void *swift::swift_nonatomic_unknownObjectRetain(void *object) {
  if (isObjCTaggedPointerOrNull(object)) return object;
  if (objectUsesNativeSwiftReferenceCounting(object)) {
    return swift_nonatomic_retain(static_cast<HeapObject *>(object));
  }
  return objc_retain(static_cast<id>(object));
}

void swift::swift_nonatomic_unknownObjectRelease(void *object) {
  if (isObjCTaggedPointerOrNull(object)) return;
  if (objectUsesNativeSwiftReferenceCounting(object))
    return swift_release(static_cast<HeapObject *>(object));
  return objc_release(static_cast<id>(object));
}

/// Return true iff the given BridgeObject is not known to use native
/// reference-counting.
///
/// Precondition: object does not encode a tagged pointer
static bool isNonNative_unTagged_bridgeObject(void *object) {
  static_assert((heap_object_abi::SwiftSpareBitsMask & objectPointerIsObjCBit) ==
                objectPointerIsObjCBit,
                "isObjC bit not within spare bits");
  return (uintptr_t(object) & objectPointerIsObjCBit) != 0
      && (uintptr_t(object) & heap_object_abi::BridgeObjectTagBitsMask) == 0;
}

/// Return true iff the given BridgeObject is a tagged value.
static bool isBridgeObjectTaggedPointer(void *object) {
  return (uintptr_t(object) & heap_object_abi::BridgeObjectTagBitsMask) != 0;
}

#endif

// Mask out the spare bits in a bridgeObject, returning the object it
// encodes.
///
/// Precondition: object does not encode a tagged pointer
static void* toPlainObject_unTagged_bridgeObject(void *object) {
  return (void*)(uintptr_t(object) & ~unTaggedNonNativeBridgeObjectBits);
}

#if SWIFT_OBJC_INTEROP
#if __arm64__
// Marking this as noinline allows swift_bridgeObjectRetain to avoid emitting
// a stack frame for the swift_retain path on ARM64. It makes for worse codegen
// on x86-64, though, so limit it to ARM64.
SWIFT_NOINLINE
#endif
static void *objcRetainAndReturn(void *object) {
  auto const objectRef = toPlainObject_unTagged_bridgeObject(object);
  objc_retain(static_cast<id>(objectRef));
  return object;
}
#endif

void *swift::swift_bridgeObjectRetain(void *object) {
#if SWIFT_OBJC_INTEROP
  if (isObjCTaggedPointer(object) || isBridgeObjectTaggedPointer(object))
    return object;
#endif

  auto const objectRef = toPlainObject_unTagged_bridgeObject(object);

#if SWIFT_OBJC_INTEROP
  if (!isNonNative_unTagged_bridgeObject(object)) {
    return swift_retain(static_cast<HeapObject *>(objectRef));
  }

  // Put the call to objc_retain in a separate function, tail-called here. This
  // allows the fast path of swift_bridgeObjectRetain to avoid creating a stack
  // frame on ARM64. We can't directly tail-call objc_retain, because
  // swift_bridgeObjectRetain returns the pointer with objectPointerIsObjCBit
  // set, so we have to make a non-tail call and then return the value with the
  // bit set.
  SWIFT_MUSTTAIL return objcRetainAndReturn(object);
#else
  // No tail call here. When !SWIFT_OBJC_INTEROP, the value of objectRef may be
  // different from that of object, e.g. on Linux ARM64.
  swift_retain(static_cast<HeapObject *>(objectRef));
  return object;
#endif
}

CUSTOM_RR_ENTRYPOINTS_DEFINE_ENTRYPOINTS(swift_bridgeObjectRetain)

SWIFT_RUNTIME_EXPORT
void *swift::swift_nonatomic_bridgeObjectRetain(void *object) {
#if SWIFT_OBJC_INTEROP
  if (isObjCTaggedPointer(object) || isBridgeObjectTaggedPointer(object))
    return object;
#endif

  auto const objectRef = toPlainObject_unTagged_bridgeObject(object);

#if SWIFT_OBJC_INTEROP
  if (!isNonNative_unTagged_bridgeObject(object)) {
    swift_nonatomic_retain(static_cast<HeapObject *>(objectRef));
    return object;
  }
  objc_retain(static_cast<id>(objectRef));
  return object;
#else
  swift_nonatomic_retain(static_cast<HeapObject *>(objectRef));
  return object;
#endif
}

SWIFT_RUNTIME_EXPORT
void swift::swift_bridgeObjectRelease(void *object) {
#if SWIFT_OBJC_INTEROP
  if (isObjCTaggedPointer(object) || isBridgeObjectTaggedPointer(object))
    return;
#endif

  auto const objectRef = toPlainObject_unTagged_bridgeObject(object);

#if SWIFT_OBJC_INTEROP
  if (!isNonNative_unTagged_bridgeObject(object))
    return swift_release(static_cast<HeapObject *>(objectRef));
  return objc_release(static_cast<id>(objectRef));
#else
  swift_release(static_cast<HeapObject *>(objectRef));
#endif
}

CUSTOM_RR_ENTRYPOINTS_DEFINE_ENTRYPOINTS(swift_bridgeObjectRelease)

void swift::swift_nonatomic_bridgeObjectRelease(void *object) {
#if SWIFT_OBJC_INTEROP
  if (isObjCTaggedPointer(object) || isBridgeObjectTaggedPointer(object))
    return;
#endif

  auto const objectRef = toPlainObject_unTagged_bridgeObject(object);

#if SWIFT_OBJC_INTEROP
  if (!isNonNative_unTagged_bridgeObject(object))
    return swift_nonatomic_release(static_cast<HeapObject *>(objectRef));
  return objc_release(static_cast<id>(objectRef));
#else
  swift_nonatomic_release(static_cast<HeapObject *>(objectRef));
#endif
}

void *swift::swift_bridgeObjectRetain_n(void *object, int n) {
#if SWIFT_OBJC_INTEROP
  if (isObjCTaggedPointer(object) || isBridgeObjectTaggedPointer(object))
    return object;
#endif

  auto const objectRef = toPlainObject_unTagged_bridgeObject(object);

#if SWIFT_OBJC_INTEROP
  if (!isNonNative_unTagged_bridgeObject(object)) {
    swift_retain_n(static_cast<HeapObject *>(objectRef), n);
    return object;
  }
  for (int i = 0;i < n; ++i)
    objc_retain(static_cast<id>(objectRef));
  return object;
#else
  swift_retain_n(static_cast<HeapObject *>(objectRef), n);
  return object;
#endif
}

void swift::swift_bridgeObjectRelease_n(void *object, int n) {
#if SWIFT_OBJC_INTEROP
  if (isObjCTaggedPointer(object) || isBridgeObjectTaggedPointer(object))
    return;
#endif

  auto const objectRef = toPlainObject_unTagged_bridgeObject(object);

#if SWIFT_OBJC_INTEROP
  if (!isNonNative_unTagged_bridgeObject(object))
    return swift_release_n(static_cast<HeapObject *>(objectRef), n);
  for (int i = 0; i < n; ++i)
    objc_release(static_cast<id>(objectRef));
#else
  swift_release_n(static_cast<HeapObject *>(objectRef), n);
#endif
}

void *swift::swift_nonatomic_bridgeObjectRetain_n(void *object, int n) {
#if SWIFT_OBJC_INTEROP
  if (isObjCTaggedPointer(object) || isBridgeObjectTaggedPointer(object))
    return object;
#endif

  auto const objectRef = toPlainObject_unTagged_bridgeObject(object);

#if SWIFT_OBJC_INTEROP
  if (!isNonNative_unTagged_bridgeObject(object)) {
    swift_nonatomic_retain_n(static_cast<HeapObject *>(objectRef), n);
    return object;
  }
  for (int i = 0;i < n; ++i)
    objc_retain(static_cast<id>(objectRef));
  return object;
#else
  swift_nonatomic_retain_n(static_cast<HeapObject *>(objectRef), n);
  return object;
#endif
}

void swift::swift_nonatomic_bridgeObjectRelease_n(void *object, int n) {
#if SWIFT_OBJC_INTEROP
  if (isObjCTaggedPointer(object) || isBridgeObjectTaggedPointer(object))
    return;
#endif

  auto const objectRef = toPlainObject_unTagged_bridgeObject(object);

#if SWIFT_OBJC_INTEROP
  if (!isNonNative_unTagged_bridgeObject(object))
    return swift_nonatomic_release_n(static_cast<HeapObject *>(objectRef), n);
  for (int i = 0; i < n; ++i)
    objc_release(static_cast<id>(objectRef));
#else
  swift_nonatomic_release_n(static_cast<HeapObject *>(objectRef), n);
#endif
}


#if SWIFT_OBJC_INTEROP

/*****************************************************************************/
/************************ UNKNOWN UNOWNED REFERENCES *************************/
/*****************************************************************************/

// Swift's native unowned references are implemented purely with
// reference-counting: as long as an unowned reference is held to an object,
// it can be destroyed but never deallocated, being that it remains fully safe
// to pass around a pointer and perform further reference-counting operations.
//
// For imported class types (meaning ObjC, for now, but in principle any
// type which supports ObjC-style weak references but not directly Swift-style
// unowned references), we have to implement this on top of the weak-reference
// support, at least for now.  But we'd like to be able to statically take
// advantage of Swift's representational advantages when we know that all the
// objects involved are Swift-native.  That means that whatever scheme we use
// for unowned references needs to interoperate with code just doing naive
// loads and stores, at least when the ObjC case isn't triggered.
//
// We have to be sensitive about making unreasonable assumptions about the
// implementation of ObjC weak references, and we definitely cannot modify
// memory owned by the ObjC runtime.  In the long run, direct support from
// the ObjC runtime can allow an efficient implementation that doesn't violate
// those requirements, both by allowing us to directly check whether a weak
// reference was cleared by deallocation vs. just initialized to nil and by
// guaranteeing a bit pattern that distinguishes Swift references.  In the
// meantime, out-of-band allocation is inefficient but not ridiculously so.
//
// Note that unowned references need not provide guaranteed behavior in
// the presence of read/write or write/write races on the reference itself.
// Furthermore, and unlike weak references, they also do not need to be
// safe against races with the deallocation of the object.  It is the user's
// responsibility to ensure that the reference remains valid at the time
// that the unowned reference is read.

namespace {
  /// An Objective-C unowned reference.  Given an unknown unowned reference
  /// in memory, it is an ObjC unowned reference if the IsObjCFlag bit
  /// is set; if so, the pointer stored in the reference actually points
  /// to out-of-line storage containing an ObjC weak reference.
  ///
  /// It is an invariant that this out-of-line storage is only ever
  /// allocated and constructed for non-null object references, so if the
  /// weak load yields null, it can only be because the object was deallocated.
  struct ObjCUnownedReference : UnownedReference {
    // Pretending that there's a subclass relationship here means that
    // accesses to objects formally constructed as UnownedReferences will
    // technically be aliasing violations.  However, the language runtime
    // will generally not see any such objects.

    enum : uintptr_t { IsObjCMask = 0x1, IsObjCFlag = 0x1 };

    /// The out-of-line storage of an ObjC unowned reference.
    struct Storage {
      /// A weak reference registered with the ObjC runtime.
      mutable id WeakRef;

      Storage(id ref) {
        assert(ref && "creating storage for null reference?");
        objc_initWeak(&WeakRef, ref);
      }

      Storage(const Storage &other) {
        objc_copyWeak(&WeakRef, &other.WeakRef);
      }

      Storage &operator=(const Storage &other) = delete;

      Storage &operator=(id ref) {
        objc_storeWeak(&WeakRef, ref);
        return *this;
      }

      ~Storage() {
        objc_destroyWeak(&WeakRef);
      }

      // Don't use the C++ allocator.
      void *operator new(size_t size) { return malloc(size); }
      void operator delete(void *ptr) { free(ptr); }
    };

    Storage *storage() {
      assert(isa<ObjCUnownedReference>(this));
      return reinterpret_cast<Storage*>(
               reinterpret_cast<uintptr_t>(Value) & ~IsObjCMask);
    }

    static void initialize(UnownedReference *dest, id value) {
      initializeWithStorage(dest, new Storage(value));
    }

    static void initializeWithCopy(UnownedReference *dest, Storage *src) {
      initializeWithStorage(dest, new Storage(*src));
    }

    static void initializeWithStorage(UnownedReference *dest,
                                      Storage *storage) {
      dest->Value = (HeapObject*) (uintptr_t(storage) | IsObjCFlag);
    }

    static bool classof(const UnownedReference *ref) {
      return (uintptr_t(ref->Value) & IsObjCMask) == IsObjCFlag;
    }
  };
}

static bool isObjCForUnownedReference(void *value) {
  return (isObjCTaggedPointer(value) ||
          !objectUsesNativeSwiftReferenceCounting(value));
}

UnownedReference *swift::swift_unknownObjectUnownedInit(UnownedReference *dest,
                                                        void *value) {
  // Note that LLDB also needs to know about the memory layout of unowned
  // references. The implementation here needs to be kept in sync with
  // lldb_private::SwiftLanguageRuntime.
  if (!value) {
    dest->Value = nullptr;
  } else if (isObjCForUnownedReference(value)) {
    ObjCUnownedReference::initialize(dest, (id) value);
  } else {
    swift_unownedInit(dest, (HeapObject*) value);
  }
  return dest;
}

UnownedReference *
swift::swift_unknownObjectUnownedAssign(UnownedReference *dest, void *value) {
  if (!value) {
    swift_unknownObjectUnownedDestroy(dest);
    dest->Value = nullptr;
  } else if (isObjCForUnownedReference(value)) {
    if (auto objcDest = dyn_cast<ObjCUnownedReference>(dest)) {
      objc_storeWeak(&objcDest->storage()->WeakRef, (id) value);
    } else {
      swift_unownedDestroy(dest);
      ObjCUnownedReference::initialize(dest, (id) value);
    }
  } else {
    if (auto objcDest = dyn_cast<ObjCUnownedReference>(dest)) {
      delete objcDest->storage();
      swift_unownedInit(dest, (HeapObject*) value);
    } else {
      swift_unownedAssign(dest, (HeapObject*) value);
    }
  }
  return dest;
}

void *swift::swift_unknownObjectUnownedLoadStrong(UnownedReference *ref) {
  if (!ref->Value) {
    return nullptr;
  } else if (auto objcRef = dyn_cast<ObjCUnownedReference>(ref)) {
    auto result = (void*) objc_loadWeakRetained(&objcRef->storage()->WeakRef);
    if (result == nullptr) {
      swift::swift_abortRetainUnowned(nullptr);
    }
    return result;
  } else {
    return swift_unownedLoadStrong(ref);
  }
}

void *swift::swift_unknownObjectUnownedTakeStrong(UnownedReference *ref) {
  if (!ref->Value) {
    return nullptr;
  } else if (auto objcRef = dyn_cast<ObjCUnownedReference>(ref)) {
    auto storage = objcRef->storage();
    auto result = (void*) objc_loadWeakRetained(&objcRef->storage()->WeakRef);
    if (result == nullptr) {
      swift::swift_abortRetainUnowned(nullptr);
    }
    delete storage;
    return result;
  } else {
    return swift_unownedTakeStrong(ref);
  }
}

void swift::swift_unknownObjectUnownedDestroy(UnownedReference *ref) {
  if (!ref->Value) {
    // Nothing to do.
    return;
  } else if (auto objcRef = dyn_cast<ObjCUnownedReference>(ref)) {
    delete objcRef->storage();
  } else {
    swift_unownedDestroy(ref);
  }
}

UnownedReference *
swift::swift_unknownObjectUnownedCopyInit(UnownedReference *dest,
                                          UnownedReference *src) {
  assert(dest != src);
  if (!src->Value) {
    dest->Value = nullptr;
  } else if (auto objcSrc = dyn_cast<ObjCUnownedReference>(src)) {
    ObjCUnownedReference::initializeWithCopy(dest, objcSrc->storage());
  } else {
    swift_unownedCopyInit(dest, src);
  }
  return dest;
}

UnownedReference *
swift::swift_unknownObjectUnownedTakeInit(UnownedReference *dest,
                                          UnownedReference *src) {
  assert(dest != src);
  dest->Value = src->Value;
  return dest;
}

UnownedReference *
swift::swift_unknownObjectUnownedCopyAssign(UnownedReference *dest,
                                            UnownedReference *src) {
  if (dest == src) return dest;

  if (auto objcSrc = dyn_cast<ObjCUnownedReference>(src)) {
    if (auto objcDest = dyn_cast<ObjCUnownedReference>(dest)) {
      // ObjC unfortunately doesn't expose a copy-assign operation.
      objc_destroyWeak(&objcDest->storage()->WeakRef);
      objc_copyWeak(&objcDest->storage()->WeakRef,
                    &objcSrc->storage()->WeakRef);
      return dest;
    }

    swift_unownedDestroy(dest);
    ObjCUnownedReference::initializeWithCopy(dest, objcSrc->storage());
  } else {
    if (auto objcDest = dyn_cast<ObjCUnownedReference>(dest)) {
      delete objcDest->storage();
      swift_unownedCopyInit(dest, src);
    } else {
      swift_unownedCopyAssign(dest, src);
    }
  }
  return dest;
}

UnownedReference *
swift::swift_unknownObjectUnownedTakeAssign(UnownedReference *dest,
                                            UnownedReference *src) {
  assert(dest != src);

  // There's not really anything more efficient to do here than this.
  swift_unknownObjectUnownedDestroy(dest);
  dest->Value = src->Value;
  return dest;
}

bool swift::swift_unknownObjectUnownedIsEqual(UnownedReference *ref,
                                              void *value) {
  if (!ref->Value) {
    return value == nullptr;
  } else if (auto objcRef = dyn_cast<ObjCUnownedReference>(ref)) {
    id refValue = objc_loadWeakRetained(&objcRef->storage()->WeakRef);
    bool isEqual = (void*)refValue == value;
    // This ObjC case has no deliberate unowned check here,
    // unlike the Swift case.
    [refValue release];
    return isEqual;
  } else {
    return swift_unownedIsEqual(ref, (HeapObject *)value);
  }
}

/*****************************************************************************/
/************************** UNKNOWN WEAK REFERENCES **************************/
/*****************************************************************************/

WeakReference *swift::swift_unknownObjectWeakInit(WeakReference *ref,
                                                  void *value) {
  ref->unknownInit(value);
  return ref;
}

WeakReference *swift::swift_unknownObjectWeakAssign(WeakReference *ref,
                                                    void *value) {
  ref->unknownAssign(value);
  return ref;
}

void *swift::swift_unknownObjectWeakLoadStrong(WeakReference *ref) {
  return ref->unknownLoadStrong();
}

void *swift::swift_unknownObjectWeakTakeStrong(WeakReference *ref) {
  return ref->unknownTakeStrong();
}

void swift::swift_unknownObjectWeakDestroy(WeakReference *ref) {
  ref->unknownDestroy();
}

WeakReference *swift::swift_unknownObjectWeakCopyInit(WeakReference *dest,
                                                      WeakReference *src) {
  dest->unknownCopyInit(src);
  return dest;
}
WeakReference *swift::swift_unknownObjectWeakTakeInit(WeakReference *dest,
                                                      WeakReference *src) {
  dest->unknownTakeInit(src);
  return dest;
}
WeakReference *swift::swift_unknownObjectWeakCopyAssign(WeakReference *dest,
                                                        WeakReference *src) {
  dest->unknownCopyAssign(src);
  return dest;
}
WeakReference *swift::swift_unknownObjectWeakTakeAssign(WeakReference *dest,
                                                        WeakReference *src) {
  dest->unknownTakeAssign(src);
  return dest;
}

// SWIFT_OBJC_INTEROP
#endif

/*****************************************************************************/
/******************************* DYNAMIC CASTS *******************************/
/*****************************************************************************/

#if SWIFT_OBJC_INTEROP
static const void *
swift_dynamicCastObjCClassImpl(const void *object,
                               const ClassMetadata *targetType) {
  // FIXME: We need to decide if this is really how we want to treat 'nil'.
  if (object == nullptr)
    return nullptr;

  if ([id_const_cast(object) isKindOfClass:class_const_cast(targetType)]) {
    return object;
  }

  // For casts to NSError or NSObject, we might need to bridge via the Error
  // protocol. Try it now.
  if (targetType == reinterpret_cast<const ClassMetadata*>(getNSErrorClass()) ||
      targetType == reinterpret_cast<const ClassMetadata*>([NSObject class])) {
    auto srcType = swift_getObjCClassMetadata(
        reinterpret_cast<const ClassMetadata*>(
          object_getClass(id_const_cast(object))));
    if (auto srcErrorWitness = findErrorWitness(srcType)) {
      return dynamicCastValueToNSError((OpaqueValue*)&object, srcType,
                                       srcErrorWitness,
                                       DynamicCastFlags::TakeOnSuccess);
    }
  }

  return nullptr;
}

static const void *
swift_dynamicCastObjCClassUnconditionalImpl(const void *object,
                                            const ClassMetadata *targetType,
                                            const char *filename,
                                            unsigned line, unsigned column) {
  // FIXME: We need to decide if this is really how we want to treat 'nil'.
  if (object == nullptr)
    return nullptr;

  if ([id_const_cast(object) isKindOfClass:class_const_cast(targetType)]) {
    return object;
  }

  // For casts to NSError or NSObject, we might need to bridge via the Error
  // protocol. Try it now.
  if (targetType == reinterpret_cast<const ClassMetadata*>(getNSErrorClass()) ||
      targetType == reinterpret_cast<const ClassMetadata*>([NSObject class])) {
    auto srcType = swift_getObjCClassMetadata(
        reinterpret_cast<const ClassMetadata*>(
          object_getClass(id_const_cast(object))));
    if (auto srcErrorWitness = findErrorWitness(srcType)) {
      return dynamicCastValueToNSError((OpaqueValue*)&object, srcType,
                                       srcErrorWitness,
                                       DynamicCastFlags::TakeOnSuccess);
    }
  }

  Class sourceType = object_getClass(id_const_cast(object));
  swift_dynamicCastFailure(reinterpret_cast<const Metadata *>(sourceType),
                           targetType);
}

static const void *
swift_dynamicCastForeignClassImpl(const void *object,
                                  const ForeignClassMetadata *targetType) {
  // FIXME: Actually compare CFTypeIDs, once they are available in the metadata.
  return object;
}

static const void *
swift_dynamicCastForeignClassUnconditionalImpl(
         const void *object,
         const ForeignClassMetadata *targetType,
         const char *filename,
         unsigned line, unsigned column) {
  // FIXME: Actual compare CFTypeIDs, once they are available in the metadata.
  return object;
}

bool swift::objectConformsToObjCProtocol(const void *theObject,
                                         ProtocolDescriptorRef protocol) {
  return [id_const_cast(theObject)
          conformsToProtocol: protocol.getObjCProtocol()];
}


bool swift::classConformsToObjCProtocol(const void *theClass,
                                        ProtocolDescriptorRef protocol) {
  return [class_const_cast(theClass)
          conformsToProtocol: protocol.getObjCProtocol()];
}

SWIFT_RUNTIME_EXPORT
const Metadata *swift_dynamicCastTypeToObjCProtocolUnconditional(
                                               const Metadata *type,
                                               size_t numProtocols,
                                               Protocol * const *protocols,
                                               const char *filename,
                                               unsigned line, unsigned column) {
  Class classObject;

  switch (type->getKind()) {
  case MetadataKind::Class:
  case MetadataKind::ObjCClassWrapper:
    // Native class metadata is also the class object.
    // ObjC class wrappers get unwrapped.
    classObject = type->getObjCClassObject();
    break;

  // Other kinds of type can never conform to ObjC protocols.
  default:
    swift_dynamicCastFailure(type, nameForMetadata(type).c_str(),
                             protocols[0], protocol_getName(protocols[0]));

  case MetadataKind::HeapLocalVariable:
  case MetadataKind::HeapGenericLocalVariable:
  case MetadataKind::ErrorObject:
    assert(false && "not type metadata");
    break;
  }

  for (size_t i = 0; i < numProtocols; ++i) {
    if (![classObject conformsToProtocol:protocols[i]]) {
      swift_dynamicCastFailure(type, nameForMetadata(type).c_str(),
                               protocols[i], protocol_getName(protocols[i]));
    }
  }

  return type;
}

SWIFT_RUNTIME_EXPORT
const Metadata *swift_dynamicCastTypeToObjCProtocolConditional(
                                                const Metadata *type,
                                                size_t numProtocols,
                                                Protocol * const *protocols) {
  Class classObject;

  switch (type->getKind()) {
  case MetadataKind::Class:
  case MetadataKind::ObjCClassWrapper:
    // Native class metadata is also the class object.
    // ObjC class wrappers get unwrapped.
    classObject = type->getObjCClassObject();
    break;

  // Other kinds of type can never conform to ObjC protocols.
  default:
    return nullptr;

  case MetadataKind::HeapLocalVariable:
  case MetadataKind::HeapGenericLocalVariable:
  case MetadataKind::ErrorObject:
    assert(false && "not type metadata");
    break;
  }

  for (size_t i = 0; i < numProtocols; ++i) {
    if (![classObject conformsToProtocol:protocols[i]]) {
      return nullptr;
    }
  }

  return type;
}

SWIFT_RUNTIME_EXPORT
id swift_dynamicCastObjCProtocolUnconditional(id object,
                                              size_t numProtocols,
                                              Protocol * const *protocols,
                                              const char *filename,
                                              unsigned line, unsigned column) {
  for (size_t i = 0; i < numProtocols; ++i) {
    if (![object conformsToProtocol:protocols[i]]) {
      Class sourceType = object_getClass(object);
      swift_dynamicCastFailure(sourceType, class_getName(sourceType),
                               protocols[i], protocol_getName(protocols[i]));
    }
  }

  return object;
}

SWIFT_RUNTIME_EXPORT
id swift_dynamicCastObjCProtocolConditional(id object,
                                            size_t numProtocols,
                                            Protocol * const *protocols) {
  if (!runtime::bincompat::useLegacySwiftValueUnboxingInCasting()) {
    if (getAsSwiftValue(object) != nil) {
      // SwiftValue wrapper never holds a class object
      return nil;
    }
  }
  for (size_t i = 0; i < numProtocols; ++i) {
    if (![object conformsToProtocol:protocols[i]]) {
      return nil;
    }
  }

  return object;
}

void swift::swift_instantiateObjCClass(const ClassMetadata *_c) {
  static const objc_image_info ImageInfo = {0, 0};

  // Ensure the superclass is realized.
  Class c = class_const_cast(_c);
  [class_getSuperclass(c) class];

  // Register the class.
  Class registered = objc_readClassPair(c, &ImageInfo);
  assert(registered == c
         && "objc_readClassPair failed to instantiate the class in-place");
  (void)registered;
}

Class swift::swift_getInitializedObjCClass(Class c) {
  // Used when we have class metadata and we want to ensure a class has been
  // initialized by the Objective-C runtime. We need to do this because the
  // class "c" might be valid metadata, but it hasn't been initialized yet.
  // Send a message that's likely not to be overridden to minimize potential
  // side effects. Ignore the return value in case it is overridden to
  // return something different. See
  // https://github.com/apple/swift/issues/52863 for an example.
  [c self];
  return c;
}

static const ClassMetadata *
swift_dynamicCastObjCClassMetatypeImpl(const ClassMetadata *source,
                                       const ClassMetadata *dest) {
  if ([class_const_cast(source) isSubclassOfClass:class_const_cast(dest)])
    return source;
  return nil;
}

static const ClassMetadata *
swift_dynamicCastObjCClassMetatypeUnconditionalImpl(const ClassMetadata *source,
                                                    const ClassMetadata *dest,
                                                    const char *filename,
                                                    unsigned line, unsigned column) {
  if ([class_const_cast(source) isSubclassOfClass:class_const_cast(dest)])
    return source;

  swift_dynamicCastFailure(source, dest);
}

#endif

static const ClassMetadata *
swift_dynamicCastForeignClassMetatypeImpl(const ClassMetadata *sourceType,
                                          const ClassMetadata *targetType) {
  // FIXME: Actually compare CFTypeIDs, once they are available in
  // the metadata.
  return sourceType;
}

static const ClassMetadata *
swift_dynamicCastForeignClassMetatypeUnconditionalImpl(
  const ClassMetadata *sourceType,
  const ClassMetadata *targetType,
  const char *filename,
  unsigned line, unsigned column)
{
  // FIXME: Actually compare CFTypeIDs, once they arae available in
  // the metadata.
  return sourceType;
}

#if SWIFT_OBJC_INTEROP
// Given a non-nil object reference, return true iff the object uses
// native swift reference counting.
static bool usesNativeSwiftReferenceCounting_nonNull(
  const void* object
) {
  assert(object != nullptr);
  return !isObjCTaggedPointer(object) &&
    objectUsesNativeSwiftReferenceCounting(object);
}
#endif

bool swift::swift_isUniquelyReferenced_nonNull_native(const HeapObject *object){
  assert(object != nullptr);
  assert(!object->refCounts.isDeiniting());
  return object->refCounts.isUniquelyReferenced();
}

bool swift::swift_isUniquelyReferenced_native(const HeapObject* object) {
  return object != nullptr
    && swift::swift_isUniquelyReferenced_nonNull_native(object);
}

bool swift::swift_isUniquelyReferencedNonObjC_nonNull(const void* object) {
  assert(object != nullptr);
  return
#if SWIFT_OBJC_INTEROP
    usesNativeSwiftReferenceCounting_nonNull(object) &&
#endif
    swift_isUniquelyReferenced_nonNull_native((const HeapObject*)object);
}

#if SWIFT_OBJC_INTEROP
// It would be nice to weak link instead of doing this, but we can't do that
// until the new API is in the versions of libobjc that we're linking against.
static bool isUniquelyReferenced(id object) {
#if OBJC_ISUNIQUELYREFERENCED_DEFINED
  return objc_isUniquelyReferenced(object);
#else
  auto objcIsUniquelyRefd = SWIFT_LAZY_CONSTANT(reinterpret_cast<bool (*)(id)>(
      dlsym(RTLD_NEXT, "objc_isUniquelyReferenced")));

  return objcIsUniquelyRefd && objcIsUniquelyRefd(object);
#endif /* OBJC_ISUNIQUELYREFERENCED_DEFINED */
}
#endif

bool swift::swift_isUniquelyReferenced_nonNull(const void *object) {
  assert(object != nullptr);

#if SWIFT_OBJC_INTEROP
  if (isObjCTaggedPointer(object))
    return false;

  if (!usesNativeSwiftReferenceCounting_nonNull(object)) {
    return isUniquelyReferenced(id_const_cast(object));
  }
#endif
  return swift_isUniquelyReferenced_nonNull_native(
      static_cast<const HeapObject *>(object));
}

// Given an object reference, return true iff it is non-nil and refers
// to a native swift object with strong reference count of 1.
bool swift::swift_isUniquelyReferencedNonObjC(
  const void* object
) {
  return object != nullptr
    && swift_isUniquelyReferencedNonObjC_nonNull(object);
}

// Given an object reference, return true if it is non-nil and refers
// to an ObjC or native swift object with a strong reference count of 1.
bool swift::swift_isUniquelyReferenced(const void *object) {
  return object != nullptr && swift_isUniquelyReferenced_nonNull(object);
}

/// Return true if the given bits of a Builtin.BridgeObject refer to a
/// native swift object whose strong reference count is 1.
bool swift::swift_isUniquelyReferencedNonObjC_nonNull_bridgeObject(
  uintptr_t bits
) {
  auto bridgeObject = (void*)bits;

  if (isObjCTaggedPointer(bridgeObject))
    return false;

  const auto object = toPlainObject_unTagged_bridgeObject(bridgeObject);

  // Note: we could just return false if all spare bits are set,
  // but in that case the cost of a deeper check for a unique native
  // object is going to be a negligible cost for a possible big win.
#if SWIFT_OBJC_INTEROP
  return !isNonNative_unTagged_bridgeObject(bridgeObject)
             ? swift_isUniquelyReferenced_nonNull_native(
                   (const HeapObject *)object)
             : swift_isUniquelyReferencedNonObjC_nonNull(object);
#else
  return swift_isUniquelyReferenced_nonNull_native((const HeapObject *)object);
#endif
}

/// Return true if the given bits of a Builtin.BridgeObject refer to
/// an object whose strong reference count is 1.
bool swift::swift_isUniquelyReferenced_nonNull_bridgeObject(uintptr_t bits) {
  auto bridgeObject = reinterpret_cast<void *>(bits);

  if (isObjCTaggedPointer(bridgeObject))
    return false;

  const auto object = toPlainObject_unTagged_bridgeObject(bridgeObject);

#if SWIFT_OBJC_INTEROP
  return !isNonNative_unTagged_bridgeObject(bridgeObject)
             ? swift_isUniquelyReferenced_nonNull_native(
                   (const HeapObject *)object)
             : swift_isUniquelyReferenced_nonNull(object);
#else
  return swift_isUniquelyReferenced_nonNull_native((const HeapObject *)object);
#endif
}

// Given a non-@objc object reference, return true iff the
// object is non-nil and has a strong reference count greater than 1
bool swift::swift_isEscapingClosureAtFileLocation(const HeapObject *object,
                                                  const unsigned char *filename,
                                                  int32_t filenameLength,
                                                  int32_t line, int32_t column,
                                                  unsigned verificationType) {
  assert((verificationType == 0 || verificationType == 1) &&
         "Unknown verification type");

  bool isEscaping =
      object != nullptr && !object->refCounts.isUniquelyReferenced();

  // Print a message if the closure escaped.
  if (isEscaping) {
    auto *message = (verificationType == 0)
                        ? "closure argument was escaped in "
                          "withoutActuallyEscaping block"
                        : "closure argument passed as @noescape "
                          "to Objective-C has escaped";
    auto messageLength = strlen(message);

    char *log;
    swift_asprintf(
        &log, "%.*s: file %.*s, line %" PRIu32 ", column %" PRIu32 " \n",
        (int)messageLength, message, filenameLength, filename, line, column);

    printCurrentBacktrace(2/*framesToSkip*/);

    if (_swift_shouldReportFatalErrorsToDebugger()) {
      RuntimeErrorDetails details = {
          .version = RuntimeErrorDetails::currentVersion,
          .errorType = "escaping-closure-violation",
          .currentStackDescription = "Closure has escaped",
          .framesToSkip = 1,
      };
      _swift_reportToDebugger(RuntimeErrorFlagFatal, log, &details);
    }

    swift_reportError(RuntimeErrorFlagFatal, log);
    free(log);
  }
  return isEscaping;
}

struct ClassExtents {
  size_t negative;
  size_t positive; 
};

SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_SPI
ClassExtents
_swift_getSwiftClassInstanceExtents(const Metadata *c) {
  assert(c && c->isClassObject());
  auto metaData = c->getClassObject();
  return ClassExtents{
    metaData->getInstanceAddressPoint(),
    metaData->getInstanceSize() - metaData->getInstanceAddressPoint()
  };
}

#if SWIFT_OBJC_INTEROP

SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_SPI
ClassExtents
_swift_getObjCClassInstanceExtents(const ClassMetadata* c) {
  // Pure ObjC classes never have negative extents.
  if (c->isPureObjC())
    return ClassExtents{0, class_getInstanceSize(class_const_cast(c))};

  return _swift_getSwiftClassInstanceExtents(c);
}

SWIFT_CC(swift)
SWIFT_RUNTIME_EXPORT
void swift_objc_swift3ImplicitObjCEntrypoint(id self, SEL selector,
                                             const char *filename,
                                             size_t filenameLength,
                                             size_t line, size_t column,
                                             std::atomic<bool> *didLog) {
  // Only log once. We should have been given a unique zero-initialized
  // atomic flag for each entry point.
  if (didLog->exchange(true))
    return;
  
  // Figure out how much reporting we want by querying the environment
  // variable SWIFT_DEBUG_IMPLICIT_OBJC_ENTRYPOINT. We have four meaningful
  // levels:
  //
  //   0: Don't report anything
  //   1: Complain about uses of implicit @objc entrypoints.
  //   2: Complain about uses of implicit @objc entrypoints, with backtraces
  //      if possible.
  //   3: Complain about uses of implicit @objc entrypoints, then abort().
  //
  // The default, if SWIFT_DEBUG_IMPLICIT_OBJC_ENTRYPOINT is not set, is 2.
  uint8_t reportLevel =
    runtime::environment::SWIFT_DEBUG_IMPLICIT_OBJC_ENTRYPOINT();
  if (reportLevel < 1) return;

  // Report the error.
  uint32_t flags = 0;
  if (reportLevel >= 2)
    flags |= 1 << 0; // Backtrace
  bool isInstanceMethod = !class_isMetaClass(object_getClass(self));
  void (*reporter)(uint32_t, const char *, ...) =
    reportLevel > 2 ? swift::fatalError : swift::warning;
  
  if (filenameLength > INT_MAX)
    filenameLength = INT_MAX;

  char *message, *nullTerminatedFilename;
  swift_asprintf(&message,
           "implicit Objective-C entrypoint %c[%s %s] is deprecated and will "
           "be removed in Swift 4",
           isInstanceMethod ? '-' : '+',
           class_getName([self class]),
           sel_getName(selector));
  swift_asprintf(&nullTerminatedFilename, "%*s", (int)filenameLength, filename);

  RuntimeErrorDetails::FixIt fixit = {
    .filename = nullTerminatedFilename,
    .startLine = line,
    .startColumn = column,
    .endLine = line,
    .endColumn = column,
    .replacementText = "@objc "
  };
  RuntimeErrorDetails::Note note = {
    .description = "add '@objc' to expose this Swift declaration to Objective-C",
    .numFixIts = 1,
    .fixIts = &fixit
  };
  RuntimeErrorDetails details = {
    .version = RuntimeErrorDetails::currentVersion,
    .errorType = "implicit-objc-entrypoint",
    .framesToSkip = 1,
    .numNotes = 1,
    .notes = &note
  };
  uintptr_t runtime_error_flags = RuntimeErrorFlagNone;
  if (reporter == swift::fatalError)
    runtime_error_flags = RuntimeErrorFlagFatal;
  _swift_reportToDebugger(runtime_error_flags, message, &details);

  reporter(flags,
           "*** %s:%zu:%zu: %s; add explicit '@objc' to the declaration to "
           "emit the Objective-C entrypoint in Swift 4 and suppress this "
           "message\n",
           nullTerminatedFilename, line, column, message);
  free(message);
  free(nullTerminatedFilename);
}

const Metadata *swift::getNSObjectMetadata() {
  return SWIFT_LAZY_CONSTANT(
      swift_getObjCClassMetadata((const ClassMetadata *)[NSObject class]));
}

const Metadata *swift::getNSStringMetadata() {
  return SWIFT_LAZY_CONSTANT(swift_getObjCClassMetadata(
    (const ClassMetadata *)objc_lookUpClass("NSString")
  ));
}

const HashableWitnessTable *
swift::hashable_support::getNSStringHashableConformance() {
  return SWIFT_LAZY_CONSTANT(
    reinterpret_cast<const HashableWitnessTable *>(
      swift_conformsToProtocolCommon(
        getNSStringMetadata(),
        &HashableProtocolDescriptor
      )
    )
  );
}

#endif

const ClassMetadata *swift::getRootSuperclass() {
#if SWIFT_OBJC_INTEROP
  static Lazy<const ClassMetadata *> SwiftObjectClass;

  return SwiftObjectClass.get([](void *ptr) {
    *((const ClassMetadata **) ptr) =
        (const ClassMetadata *)[SwiftObject class];
  });
#else
  return nullptr;
#endif
}

#define OVERRIDE_OBJC COMPATIBILITY_OVERRIDE
#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH

#define OVERRIDE_FOREIGN COMPATIBILITY_OVERRIDE
#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH
