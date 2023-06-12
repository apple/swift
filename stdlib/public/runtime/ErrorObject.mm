//===--- ErrorObject.mm - Cocoa-interoperable recoverable error object ----===//
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
// This implements the object representation of the standard Error
// type, which represents recoverable errors in the language. This
// implementation is designed to interoperate efficiently with Cocoa libraries
// by:
// - allowing for NSError and CFError objects to "toll-free bridge" to
//   Error existentials, which allows for cheap Cocoa to Swift interop
// - allowing a native Swift error to lazily "become" an NSError when
//   passed into Cocoa, allowing for cheap Swift to Cocoa interop
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Config.h"

#if SWIFT_OBJC_INTEROP
#include "ErrorObject.h"
#include "Private.h"
#include "SwiftObject.h"
#include "swift/Basic/Lazy.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/ObjCBridge.h"
#include <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/NSObject.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

using namespace swift;
using namespace swift::hashable_support;

@interface NSObject (MakeTheCompilerHappy)
+ (id) dictionary; //declare this so we can call it below without warnings
@end

// Mimic the memory layout of NSError so things don't go haywire when we
// switch superclasses to the real thing.
@interface __SwiftNSErrorLayoutStandin : NSObject {
  @private
  void *_reserved;
  NSInteger _code;
  id _domain;
  id _userInfo;
}
@end

@implementation __SwiftNSErrorLayoutStandin
@end

/// A subclass of NSError used to represent bridged native Swift errors.
/// This type cannot be subclassed, and should not ever be instantiated
/// except by the Swift runtime.
///
/// NOTE: older runtimes called this _SwiftNativeNSError. The two must
/// coexist, so it was renamed. The old name must not be used in the new
/// runtime.
@interface __SwiftNativeNSError : __SwiftNSErrorLayoutStandin
@end

@implementation __SwiftNativeNSError

+ (instancetype)allocWithZone:(NSZone *)zone {
  (void)zone;
  swift::crash("__SwiftNativeNSError cannot be instantiated");
}

- (void)dealloc {
  // We must destroy the contained Swift value.
  auto error = (SwiftError*)self;
  error->getType()->vw_destroy(error->getValue());

  [super dealloc];
}

// Override the domain/code/userInfo accessors to follow our idea of NSError's
// layout. This gives us a buffer in case NSError decides to change its stored
// property order.

- (id /* NSString */)domain {
  auto error = (const SwiftError*)self;
  // The domain string should not be nil; if it is, then this error box hasn't
  // been initialized yet as an NSError.
  auto domain = error->domain.load(SWIFT_MEMORY_ORDER_CONSUME);
  assert(domain
         && "Error box used as NSError before initialization");
  // Don't need to .retain.autorelease since it's immutable.
  return cf_const_cast<id>(domain);
}

- (id /* NSString */)description {
  auto error = (const SwiftError *)self;
  auto value = error->getValue();
  auto type = error->type;

  // Copy the value, since it will be consumed by getDescription.
  ValueBuffer copyBuf;
  auto copy = type->allocateBufferIn(&copyBuf);
  error->type->vw_initializeWithCopy(copy, const_cast<OpaqueValue *>(value));

  auto description = getDescription(copy, type);
  type->deallocateBufferIn(&copyBuf);
  return description;
}

- (NSInteger)code {
  auto error = (const SwiftError*)self;
  return error->code.load(SWIFT_MEMORY_ORDER_CONSUME);
}

- (id /* NSDictionary */)userInfo {
  auto error = (const SwiftError*)self;
  auto userInfo = error->userInfo.load(SWIFT_MEMORY_ORDER_CONSUME);
  assert(userInfo
         && "Error box used as NSError before initialization");
  // Don't need to .retain.autorelease since it's immutable.
  return cf_const_cast<id>(userInfo);
}

- (id)copyWithZone:(NSZone *)zone {
  (void)zone;
  // __SwiftNativeNSError is immutable, so we can return the same instance back.
  return [self retain];
}

- (Class)classForCoder {
  // This is a runtime-private subclass. When archiving or unarchiving, do so
  // as an NSError.
  return getNSErrorClass();
}

// Note: We support comparing cases of `@objc` enums defined in Swift to
// pure `NSError`s.  They should compare equal as long as the domain and
// code match.  Equal values should have equal hash values.  Thus, we can't
// use the Swift hash value computation that comes from the `Hashable`
// conformance if one exists, and we must use the `NSError` hashing
// algorithm.
//
// So we are not overriding the `hash` method, even though we are
// overriding `isEqual:`.

- (BOOL)isEqual:(id)other {
  auto self_ = (const SwiftError *)self;
  auto other_ = (const SwiftError *)other;
  assert(!self_->isPureNSError());

  if (self == other) {
    return YES;
  }

  if (!other) {
    return NO;
  }

  if (other_->isPureNSError()) {
    return [super isEqual:other];
  }

  auto hashableBaseType = self_->getHashableBaseType();
  if (!hashableBaseType || other_->getHashableBaseType() != hashableBaseType) {
    return [super isEqual:other];
  }

  auto hashableConformance = self_->getHashableConformance();
  if (!hashableConformance) {
    return [super isEqual:other];
  }

  return _swift_stdlib_Hashable_isEqual_indirect(
      self_->getValue(), other_->getValue(), hashableBaseType,
      hashableConformance);
}

@end

Class swift::getNSErrorClass() {
  return SWIFT_LAZY_CONSTANT(objc_lookUpClass("NSError"));
}

const Metadata *swift::getNSErrorMetadata() {
  return SWIFT_LAZY_CONSTANT(
    swift_getObjCClassMetadata((const ClassMetadata *)getNSErrorClass()));
}

extern "C" const ProtocolDescriptor PROTOCOL_DESCR_SYM(s5Error);

const WitnessTable *swift::findErrorWitness(const Metadata *srcType) {
  return swift_conformsToProtocol(srcType, &PROTOCOL_DESCR_SYM(s5Error));
}

id swift::dynamicCastValueToNSError(OpaqueValue *src,
                                    const Metadata *srcType,
                                    const WitnessTable *srcErrorWitness,
                                    DynamicCastFlags flags) {
  // Check whether there is an embedded NSError.
  if (id embedded = getErrorEmbeddedNSErrorIndirect(src, srcType,
                                                    srcErrorWitness)) {
    if (flags & DynamicCastFlags::TakeOnSuccess)
      srcType->vw_destroy(src);

    return embedded;
  }

  BoxPair errorBox = swift_allocError(srcType, srcErrorWitness, src,
                            /*isTake*/ flags & DynamicCastFlags::TakeOnSuccess);
  auto *error = (SwiftError *)errorBox.object;
  return _swift_stdlib_bridgeErrorToNSError(error);
}

static Class getAndBridgeSwiftNativeNSErrorClass() {
  Class nsErrorClass = swift::getNSErrorClass();
  Class ourClass = [__SwiftNativeNSError class];
  // We want "err as AnyObject" to do *something* even without Foundation
  if (nsErrorClass) {
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdeprecated-declarations"
      class_setSuperclass(ourClass, nsErrorClass);
    #pragma clang diagnostic pop
  }
  return ourClass;
}

static Class getSwiftNativeNSErrorClass() {
  return SWIFT_LAZY_CONSTANT(getAndBridgeSwiftNativeNSErrorClass());
}

static id getEmptyNSDictionary() {
  return [objc_lookUpClass("NSDictionary") dictionary];
}

/// Allocate a catchable error object.
BoxPair
swift::swift_allocError(const Metadata *type,
                        const WitnessTable *errorConformance,
                        OpaqueValue *initialValue,
                        bool isTake) {
  auto TheSwiftNativeNSError = getSwiftNativeNSErrorClass();
  assert(class_getInstanceSize(TheSwiftNativeNSError) == sizeof(SwiftErrorHeader)
         && "NSError layout changed!");

  // Determine the extra allocated space necessary to carry the value.
  // TODO: If the error type is a simple enum with no associated values, we
  // could emplace it in the "code" slot of the NSError and save ourselves
  // some work.

  unsigned size = type->getValueWitnesses()->getSize();
  unsigned alignMask = type->getValueWitnesses()->getAlignmentMask();

  size_t alignmentPadding = -sizeof(SwiftError) & alignMask;
  size_t totalExtraSize = sizeof(SwiftError) - sizeof(SwiftErrorHeader)
    + alignmentPadding + size;
  size_t valueOffset = alignmentPadding + sizeof(SwiftError);

  // Allocate the instance as if it were a CFError. We won't really initialize
  // the CFError parts until forced to though.
  auto instance
    = (SwiftError *)class_createInstance(TheSwiftNativeNSError, totalExtraSize);

  // Leave the NSError bits zero-initialized. We'll lazily instantiate them when
  // needed.

  // Initialize the Swift type metadata.
  instance->type = type;
  instance->errorConformance = errorConformance;
  instance->hashableBaseType = nullptr;
  instance->hashableConformance = nullptr;

  auto valueBytePtr = reinterpret_cast<char*>(instance) + valueOffset;
  auto valuePtr = reinterpret_cast<OpaqueValue*>(valueBytePtr);

  // If an initial value was given, copy or take it in.
  if (initialValue) {
    if (isTake)
      type->vw_initializeWithTake(valuePtr, initialValue);
    else
      type->vw_initializeWithCopy(valuePtr, initialValue);
  }

  // Return the SwiftError reference and a pointer to the uninitialized value
  // inside.
  return BoxPair{reinterpret_cast<HeapObject*>(instance), valuePtr};
}

/// Deallocate an error object whose contained object has already been
/// destroyed.
void
swift::swift_deallocError(SwiftError *error, const Metadata *type) {
  object_dispose((id)error);
}

static const WitnessTable *getNSErrorConformanceToError() {
  // CFError and NSError are toll-free-bridged, so we can use either type's
  // witness table interchangeably. CFError's is potentially slightly more
  // efficient since it doesn't need to dispatch for an unsubclassed NSCFError.
  // The error bridging info lives in the Foundation overlay, but it should be
  // safe to assume that that's been linked in if a user is using NSError in
  // their Swift source.

  auto *conformance = SWIFT_LAZY_CONSTANT(
    reinterpret_cast<const ProtocolConformanceDescriptor *>(
      dlsym(RTLD_DEFAULT,
            MANGLE_AS_STRING(MANGLE_SYM(So10CFErrorRefas5Error10FoundationMc)))));
  assert(conformance &&
         "Foundation overlay not loaded, or 'CFError : Error' conformance "
         "not available");
  return swift_getWitnessTable(conformance,
                               conformance->getCanonicalTypeMetadata(),
                               nullptr);
}

static const HashableWitnessTable *getNSErrorConformanceToHashable() {
  auto *conformance = SWIFT_LAZY_CONSTANT(
    reinterpret_cast<const ProtocolConformanceDescriptor *>(
      dlsym(RTLD_DEFAULT,
            MANGLE_AS_STRING(MANGLE_SYM(So8NSObjectCSH10ObjectiveCMc)))));
  assert(conformance &&
         "ObjectiveC overlay not loaded, or 'NSObject : Hashable' conformance "
         "not available");
  return (const HashableWitnessTable *)swift_getWitnessTable(
           conformance,
           conformance->getCanonicalTypeMetadata(),
           nullptr);
}

bool SwiftError::isPureNSError() const {
  // We can do an exact type check; __SwiftNativeNSError shouldn't be subclassed
  // or proxied.
  return _swift_getClass(this) != (ClassMetadata *)getSwiftNativeNSErrorClass();
}

const Metadata *SwiftError::getType() const {
  if (isPureNSError()) {
    id asError = reinterpret_cast<id>(const_cast<SwiftError *>(this));
    return swift_getObjCClassMetadata((ClassMetadata*)[asError class]);
  }
  return type;
}

const WitnessTable *SwiftError::getErrorConformance() const {
  if (isPureNSError()) {
    return getNSErrorConformanceToError();
  }
  return errorConformance;
}

const Metadata *SwiftError::getHashableBaseType() const {
  if (isPureNSError()) {
    return getNSErrorMetadata();
  }
  if (auto type = hashableBaseType.load(std::memory_order_acquire)) {
    if (reinterpret_cast<uintptr_t>(type) == 1) {
      return nullptr;
    }
    return type;
  }

  const Metadata *expectedType = nullptr;
  const Metadata *hashableBaseType = findHashableBaseType(type);
  this->hashableBaseType.compare_exchange_strong(
      expectedType, hashableBaseType ? hashableBaseType
                                     : reinterpret_cast<const Metadata *>(1),
      std::memory_order_acq_rel);
  return type;
}

const HashableWitnessTable *SwiftError::getHashableConformance() const {
  if (isPureNSError()) {
    return getNSErrorConformanceToHashable();
  }
  if (auto wt = hashableConformance.load(std::memory_order_acquire)) {
    if (reinterpret_cast<uintptr_t>(wt) == 1) {
      return nullptr;
    }
    return wt;
  }

  const HashableWitnessTable *expectedWT = nullptr;
  const HashableWitnessTable *wt =
      reinterpret_cast<const HashableWitnessTable *>(
          swift_conformsToProtocol(type, &HashableProtocolDescriptor));
  hashableConformance.compare_exchange_strong(
      expectedWT, wt ? wt : reinterpret_cast<const HashableWitnessTable *>(1),
      std::memory_order_acq_rel);
  return wt;
}

/// Extract a pointer to the value, the type metadata, and the Error
/// protocol witness from an error object.
///
/// The "scratch" pointer should point to an uninitialized word-sized
/// temporary buffer. The implementation may write a reference to itself to
/// that buffer if the error object is a toll-free-bridged NSError instead of
/// a native Swift error, in which case the object itself is the "boxed" value.
///
/// This function is called by compiler-generated code.
void
swift::swift_getErrorValue(const SwiftError *errorObject,
                           void **scratch,
                           ErrorValueResult *out) {
  // TODO: Would be great if Clang had a return-three convention so we didn't
  // need the out parameter here.

  out->type = errorObject->getType();

  // Check for a bridged Cocoa NSError.
  if (errorObject->isPureNSError()) {
    // Return a pointer to the scratch buffer.
    *scratch = (void*)errorObject;
    out->value = (const OpaqueValue *)scratch;
    out->errorConformance = getNSErrorConformanceToError();
  } else {
    out->value = errorObject->getValue();
    out->errorConformance = errorObject->errorConformance;
  }
}

// internal func _getErrorDomainNSString<T : Error>
//   (_ x: UnsafePointer<T>) -> AnyObject
#define getErrorDomainNSString \
  MANGLE_SYM(s23_getErrorDomainNSStringyyXlSPyxGs0B0RzlF)
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
id getErrorDomainNSString(const OpaqueValue *error,
                          const Metadata *T,
                          const WitnessTable *Error);

// internal func _getErrorCode<T : Error>(_ x: UnsafePointer<T>) -> Int
#define getErrorCode \
  MANGLE_SYM(s13_getErrorCodeySiSPyxGs0B0RzlF)
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
NSInteger getErrorCode(const OpaqueValue *error,
                       const Metadata *T,
                       const WitnessTable *Error);

// internal func _getErrorUserInfoNSDictionary<T : Error>(_ x: UnsafePointer<T>) -> AnyObject
#define getErrorUserInfoNSDictionary \
  MANGLE_SYM(s29_getErrorUserInfoNSDictionaryyyXlSgSPyxGs0B0RzlF)
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
id getErrorUserInfoNSDictionary(
                const OpaqueValue *error,
                const Metadata *T,
                const WitnessTable *Error);

// @_silgen_name("_swift_stdlib_getErrorDefaultUserInfo")
// internal func _getErrorDefaultUserInfo<T : Error>(_ x: T) -> AnyObject
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
id _swift_stdlib_getErrorDefaultUserInfo(OpaqueValue *error,
                                         const Metadata *T,
                                         const WitnessTable *Error) {
  // public func Foundation._getErrorDefaultUserInfo<T: Error>(_ error: T)
  //   -> AnyObject?
  typedef SWIFT_CC(swift) NSDictionary *(*GetErrorDefaultUserInfoFunction)(
    const OpaqueValue *error,
    const Metadata *T,
    const WitnessTable *Error);
  auto foundationGetDefaultUserInfo = SWIFT_LAZY_CONSTANT(
    reinterpret_cast<GetErrorDefaultUserInfoFunction>(
      dlsym(RTLD_DEFAULT,
            MANGLE_AS_STRING(MANGLE_SYM(10Foundation24_getErrorDefaultUserInfoyyXlSgxs0C0RzlF)))));

  if (!foundationGetDefaultUserInfo) {
    return nullptr;
  }

  // +0 Convention: In the case where we have the +1 convention, this will
  // destroy the error for us, otherwise, it will take the value guaranteed. The
  // conclusion is that we can leave this alone.
  return foundationGetDefaultUserInfo(error, T, Error);
}

/// Take an Error box and turn it into a valid NSError instance. Error is passed
/// at +1.
id
swift::_swift_stdlib_bridgeErrorToNSError(SwiftError *errorObject) {
  id ns = reinterpret_cast<id>(errorObject);

  // If we already have a domain set, then we've already initialized.
  // If this is a real NSError, then Cocoa and Core Foundation's initializers
  // guarantee that the domain is never nil, so if this test fails, we can
  // assume we're working with a bridged error. (Note that Cocoa and CF
  // **will** allow the userInfo storage to be initialized to nil.)
  //
  // If this is a bridged error, then the domain, code, and user info are
  // lazily computed, and the domain will be nil if they haven't been computed
  // yet. The initialization is ordered in such a way that all other lazy
  // initialization of the object happens-before the domain initialization so
  // that the domain can be used alone as a flag for the initialization of the
  // object.
  if (errorObject->domain.load(std::memory_order_acquire)) {
    return ns;
  }

  // Otherwise, calculate the domain, code, and user info, and
  // initialize the NSError.
  auto value = SwiftError::getIndirectValue(&errorObject);
  auto type = errorObject->getType();
  auto witness = errorObject->getErrorConformance();

  id domain = getErrorDomainNSString(value, type, witness);
  NSInteger code = getErrorCode(value, type, witness);
  id userInfo = getErrorUserInfoNSDictionary(value, type, witness);

  // Never produce an empty userInfo dictionary.
  if (!userInfo)
    userInfo = SWIFT_LAZY_CONSTANT(getEmptyNSDictionary());

  // The error code shouldn't change, so we can store it blindly, even if
  // somebody beat us to it. The store can be relaxed, since we'll do a
  // store(release) of the domain last thing to publish the initialized
  // NSError.
  errorObject->code.store(code, std::memory_order_relaxed);

  // However, we need to cmpxchg the userInfo; if somebody beat us to it,
  // we need to release.
  CFDictionaryRef expectedUserInfo = nullptr;
  if (!errorObject->userInfo.compare_exchange_strong(expectedUserInfo,
                                                     (CFDictionaryRef)userInfo,
                                                     std::memory_order_acq_rel))
    objc_release(userInfo);

  // We also need to cmpxchg in the domain; if somebody beat us to it,
  // we need to release.
  //
  // Storing the domain must be the **LAST THING** we do, since it's
  // also the flag that the NSError has been initialized.
  CFStringRef expectedDomain = nullptr;
  if (!errorObject->domain.compare_exchange_strong(expectedDomain,
                                                   (CFStringRef)domain,
                                                   std::memory_order_acq_rel))
    objc_release(domain);

  return ns;
}

extern "C" const ProtocolDescriptor PROTOCOL_DESCR_SYM(s5Error);

static IMP
getNSProxyLookupMethod() {
  Class NSProxyClass = objc_lookUpClass("NSProxy");
  return class_getMethodImplementation(NSProxyClass, @selector(methodSignatureForSelector:));
}

// A safer alternative to calling `isKindOfClass:` directly.
static bool
isKindOfClass(HeapObject *object, Class cls) {
  IMP NSProxyLookupMethod = SWIFT_LAZY_CONSTANT(getNSProxyLookupMethod());
  // People sometimes fail to override `methodSignatureForSelector:` in their
  // NSProxy subclasses, which causes `isKindOfClass:` to crash.  Avoid that...
  Class objectClass = object_getClass((id)object);
  IMP objectLookupMethod = class_getMethodImplementation(objectClass, @selector(methodSignatureForSelector:));
  if (objectLookupMethod == NSProxyLookupMethod) {
    return false;
  }
  return [reinterpret_cast<id>(object) isKindOfClass: cls];
}

bool
swift::tryDynamicCastNSErrorObjectToValue(HeapObject *object,
                                          OpaqueValue *dest,
                                          const Metadata *destType,
                                          DynamicCastFlags flags) {
  Class NSErrorClass = getNSErrorClass();

  // The object must be an NSError subclass.
  if (isObjCTaggedPointerOrNull(object) || !isKindOfClass(object, NSErrorClass))
    return false;

  id srcInstance = reinterpret_cast<id>(object);

  // A __SwiftNativeNSError box can always be unwrapped to cast the value back
  // out as an Error existential.
  if (!reinterpret_cast<SwiftError*>(srcInstance)->isPureNSError()) {
    ProtocolDescriptorRef theErrorProtocol(&PROTOCOL_DESCR_SYM(s5Error),
                                           ProtocolDispatchStrategy::Swift);
    auto theErrorTy =
      swift_getExistentialTypeMetadata(ProtocolClassConstraint::Any,
                                       nullptr, 1, &theErrorProtocol);
    return swift_dynamicCast(dest, reinterpret_cast<OpaqueValue *>(&object),
                             theErrorTy, destType, flags);
  }

  // public func Foundation._bridgeNSErrorToError<
  //   T : _ObjectiveCBridgeableError
  // >(error: NSError, out: UnsafeMutablePointer<T>) -> Bool {
  typedef SWIFT_CC(swift) bool (*BridgeErrorToNSErrorFunction)(
    NSError *, OpaqueValue*, const Metadata *,
    const WitnessTable *);
  auto bridgeNSErrorToError = SWIFT_LAZY_CONSTANT(
    reinterpret_cast<BridgeErrorToNSErrorFunction>(
      dlsym(RTLD_DEFAULT,
            MANGLE_AS_STRING(MANGLE_SYM(10Foundation21_bridgeNSErrorToError_3outSbSo0C0C_SpyxGtAA021_ObjectiveCBridgeableE0RzlF)))));
  
  // protocol _ObjectiveCBridgeableError
  auto TheObjectiveCBridgeableError = SWIFT_LAZY_CONSTANT(
    reinterpret_cast<ProtocolDescriptor *>(
      dlsym(RTLD_DEFAULT,
            MANGLE_AS_STRING(MANGLE_SYM(10Foundation26_ObjectiveCBridgeableErrorMp)))));

  // If the Foundation overlay isn't loaded, then arbitrary NSErrors can't be
  // bridged.
  if (!bridgeNSErrorToError || !TheObjectiveCBridgeableError)
    return false;

  // Is the target type a bridgeable error?
  auto witness = swift_conformsToProtocol(destType,
                                          TheObjectiveCBridgeableError);

  if (witness) {
    // If so, attempt the bridge.
    if (bridgeNSErrorToError(srcInstance, dest, destType, witness)) {
      if (flags & DynamicCastFlags::TakeOnSuccess)
        objc_release(srcInstance);
      return true;
    }
  }

  // If the destination is just an Error then we can bridge directly.
  auto *destTypeExistential = dyn_cast<ExistentialTypeMetadata>(destType);
  if (destTypeExistential &&
      destTypeExistential->getRepresentation() == ExistentialTypeRepresentation::Error) {
    auto destBoxAddr = reinterpret_cast<id*>(dest);
    *destBoxAddr = objc_retain(srcInstance);
    return true;
  }

  return false;
}

bool
swift::tryDynamicCastNSErrorToValue(OpaqueValue *dest,
                                    OpaqueValue *src,
                                    const Metadata *srcType,
                                    const Metadata *destType,
                                    DynamicCastFlags flags) {
  // NSError instances must be class instances, anything else automatically fails.
  switch (srcType->getKind()) {
  case MetadataKind::Class:
  case MetadataKind::ObjCClassWrapper:
  case MetadataKind::ForeignClass:
    return tryDynamicCastNSErrorObjectToValue(*reinterpret_cast<HeapObject **>(src),
                                              dest, destType, flags);

  // Not a class.
  // Foreign reference types don't support casting to parent/child types yet
  // (rdar://85881664&85881794).
  case MetadataKind::ForeignReferenceType:
  default:
    return false;
  }
}

SwiftError *
swift::swift_errorRetain(SwiftError *error) {
  // For now, SwiftError is always objc-refcounted.
  return (SwiftError*)objc_retain((id)error);
}

void
swift::swift_errorRelease(SwiftError *error) {
  // For now, SwiftError is always objc-refcounted.
  return objc_release((id)error);
}

#endif

