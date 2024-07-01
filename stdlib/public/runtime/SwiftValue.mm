//===--- SwiftValue.mm - Boxed Swift value class --------------------------===//
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
// This implements the Objective-C class that is used to carry Swift values
// that have been bridged to Objective-C objects without special handling.
// The class is opaque to user code, but is `NSObject`- and `NSCopying`-
// conforming and is understood by the Swift runtime for dynamic casting
// back to the contained type.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Config.h"

#if SWIFT_OBJC_INTEROP
#include "SwiftObject.h"
#include "SwiftValue.h"
#include "swift/Basic/Lazy.h"
#include "swift/Runtime/Bincompat.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/ObjCBridge.h"
#include "swift/Runtime/Debug.h"
#include "swift/Threading/Mutex.h"
#include "Private.h"
#include "SwiftEquatableSupport.h"
#include "SwiftHashableSupport.h"
#include <objc/runtime.h>
#include <Foundation/Foundation.h>

#include <new>
#include <unordered_set>

using namespace swift;
using namespace swift::hashable_support;

// TODO: Making this a SwiftObject subclass would let us use Swift refcounting,
// but we would need to be able to emit __SwiftValue's Objective-C class object
// with the Swift destructor pointer prefixed before it.
//
// The layout of `__SwiftValue` is:
// - object header,
// - `SwiftValueHeader` instance,
// - the payload, tail-allocated (the Swift value contained in this box).
//
// NOTE: older runtimes called this _SwiftValue. The two must
// coexist, so it was renamed. The old name must not be used in the new
// runtime.
@interface __SwiftValue : NSObject <NSCopying>

- (id)copyWithZone:(NSZone *)zone;

@end

/// The fixed-size ivars of `__SwiftValue`.  The actual boxed value is
/// tail-allocated.
struct SwiftValueHeader {
  /// The type of the value contained in the `__SwiftValue` box.
  const Metadata *type;

  /// The base type that introduces the `Hashable` or `Equatable` conformance.
  /// This member is lazily-initialized.
  /// Instead of using it directly, call `getHashableBaseType()` or `getEquatableBaseType()`
  /// Value here is encoded:
  /// * Least-significant bit set: This is an Equatable base type
  /// * Least-significant bit not set: This is a Hashable base type
  mutable std::atomic<uintptr_t> cachedBaseType;

  /// The witness table for `Hashable` conformance.
  /// This member is lazily-initialized.
  /// Instead of using it directly, call `getHashableConformance()`.
  /// Value here is encoded:
  /// * Least-significant bit set: This is an Equatable conformance
  /// * Least-significant bit not set: This is a Hashable conformance
  mutable std::atomic<uintptr_t> cachedConformance;

  /// Get the base type that conforms to `Hashable`.
  /// Returns NULL if the type does not conform.
  const Metadata *getHashableBaseType() const;

  /// Get the base type that conforms to `Equatable`.
  /// Returns NULL if the type does not conform.
  const Metadata *getEquatableBaseType() const;

  /// Get the `Hashable` protocol witness table for the contained type.
  /// Returns NULL if the type does not conform.
  const hashable_support::HashableWitnessTable *getHashableConformance() const;

  /// Get the `Equatable` protocol witness table for the contained type.
  /// Returns NULL if the type does not conform.
  const equatable_support::EquatableWitnessTable *getEquatableConformance() const;

  /// Populate the `cachedConformance` with the Hashable conformance
  /// (if there is one), else the Equatable conformance.
  /// Returns the encoded conformance:  least-significant
  /// bit is set if this is an Equatable conformance,
  /// else it is a Hashable conformance.  0 (or 1) indicates
  /// neither was found.
  uintptr_t cacheHashableEquatableConformance() const;


  SwiftValueHeader()
      : cachedBaseType(0), cachedConformance(0) {}
};

// Set cachedConformance to the Hashable conformance if
// there is one, else the Equatable conformance.
// Also set cachedBaseType to the parent type that
// introduced the Hashable/Equatable conformance.
// The cached conformance and type are encoded:
// * If the LSbit is not set, it's the Hashable conformance
// * If the value is exactly 1, neither conformance is present
// * If the LSbit is 1, strip it and you'll have the Equatable conformance
// (Null indicates the cache has not been initialized yet;
// that will never be true on exit of this function.)
// Return: encoded cachedConformance value
uintptr_t
SwiftValueHeader::cacheHashableEquatableConformance() const {
    // Relevant conformance and baseType
    uintptr_t conformance;
    uintptr_t baseType;

    // First, see if it's Hashable
    const HashableWitnessTable *hashable =
      reinterpret_cast<const HashableWitnessTable *>(
	swift_conformsToProtocolCommon(type, &HashableProtocolDescriptor));
    if (hashable != nullptr) {
      conformance = reinterpret_cast<uintptr_t>(hashable);
      baseType = reinterpret_cast<uintptr_t>(findHashableBaseType(type));
    } else {
      // If not Hashable, maybe Equatable?
      auto equatable = 
	swift_conformsToProtocolCommon(type, &equatable_support::EquatableProtocolDescriptor);
      // Encode the equatable conformance
      conformance = reinterpret_cast<uintptr_t>(equatable) | 1;

      if (equatable != nullptr) {
	// Find equatable base type
#if SWIFT_STDLIB_USE_RELATIVE_PROTOCOL_WITNESS_TABLES
	const auto *description = lookThroughOptionalConditionalWitnessTable(
	  reinterpret_cast<const RelativeWitnessTable*>(equatable))
	  ->getDescription();
#else
	const auto *description = equatable->getDescription();
#endif
	const Metadata *baseTypeThatConformsToEquatable =
	  findConformingSuperclass(type, description);
	// Encode the equatable base type
	baseType = reinterpret_cast<uintptr_t>(baseTypeThatConformsToEquatable) | 1;
      } else {
	baseType = 1; // Neither equatable nor hashable
      }
    }

    // Set the conformance/baseType caches atomically
    uintptr_t expectedConformance = 0;
    uintptr_t expectedType = 0;
    cachedConformance.compare_exchange_strong(expectedConformance, conformance,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed);
    cachedBaseType.compare_exchange_strong(expectedType, baseType,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed);

    return conformance;
}

const Metadata *SwiftValueHeader::getHashableBaseType() const {
  auto type = cachedBaseType.load(std::memory_order_relaxed);
  if (type == 0) {
    cacheHashableEquatableConformance();
    type = cachedBaseType.load(std::memory_order_relaxed);
  }
  if ((type & 1) == 0) {
    // A Hashable conformance was found
    return reinterpret_cast<const Metadata *>(type);
  } else {
    // Equatable conformance (or no conformance) found
    return nullptr;
  }
}

const Metadata *SwiftValueHeader::getEquatableBaseType() const {
  auto type = cachedBaseType.load(std::memory_order_relaxed);
  if (type == 0) {
    cacheHashableEquatableConformance();
    type = cachedBaseType.load(std::memory_order_relaxed);
  }
  if ((type & 1) == 0) {
    // A Hashable conformance was found
    return nullptr;
  } else {
    // An Equatable conformance (or neither) was found
    return reinterpret_cast<const Metadata *>(type & ~1ULL);
  }
}

const hashable_support::HashableWitnessTable *
SwiftValueHeader::getHashableConformance() const {
  uintptr_t wt = cachedConformance.load(std::memory_order_relaxed);
  if (wt == 0) {
    wt = cacheHashableEquatableConformance();
  }
  if ((wt & 1) == 0) {
    // Hashable conformance found
    return reinterpret_cast<const hashable_support::HashableWitnessTable *>(wt);
  } else {
    // Equatable conformance (or no conformance) found
    return nullptr;
  }
}

const equatable_support::EquatableWitnessTable *
SwiftValueHeader::getEquatableConformance() const {
  uintptr_t wt = cachedConformance.load(std::memory_order_relaxed);
  if (wt == 0) {
    wt = cacheHashableEquatableConformance();
  }
  if ((wt & 1) == 0) {
    // Hashable conformance found
    return nullptr;
  } else {
    // Equatable conformance (or no conformance) found
    return reinterpret_cast<const equatable_support::EquatableWitnessTable *>(wt & ~1ULL);
  }
}

static constexpr const size_t SwiftValueHeaderOffset
  = sizeof(Class); // isa pointer
static constexpr const size_t SwiftValueMinAlignMask
  = alignof(Class) - 1;
/* TODO: If we're able to become a SwiftObject subclass in the future,
 * change to this:
static constexpr const size_t SwiftValueHeaderOffset
  = sizeof(HeapObject);
static constexpr const size_t SwiftValueMinAlignMask
  = alignof(HeapObject) - 1;
 */

static Class _getSwiftValueClass() {
  auto theClass = [__SwiftValue class];
  // Fixed instance size of __SwiftValue should be same as object header.
  assert(class_getInstanceSize(theClass) == SwiftValueHeaderOffset
         && "unexpected size of __SwiftValue?!");
  return theClass;
}

static Class getSwiftValueClass() {
  return SWIFT_LAZY_CONSTANT(_getSwiftValueClass());
}

static constexpr size_t getSwiftValuePayloadOffset(size_t alignMask) {
  return (SwiftValueHeaderOffset + sizeof(SwiftValueHeader) + alignMask) &
         ~alignMask;
}

static SwiftValueHeader *getSwiftValueHeader(__SwiftValue *v) {
  auto instanceBytes = reinterpret_cast<char *>(v);
  return reinterpret_cast<SwiftValueHeader *>(instanceBytes +
                                              SwiftValueHeaderOffset);
}

static OpaqueValue *getSwiftValuePayload(__SwiftValue *v, size_t alignMask) {
  auto instanceBytes = reinterpret_cast<char *>(v);
  return reinterpret_cast<OpaqueValue *>(instanceBytes +
                                         getSwiftValuePayloadOffset(alignMask));
}

static size_t getSwiftValuePayloadAlignMask(const Metadata *type) {
  return type->getValueWitnesses()->getAlignmentMask() | SwiftValueMinAlignMask;
}

const Metadata *swift::getSwiftValueTypeMetadata(__SwiftValue *v) {
  return getSwiftValueHeader(v)->type;
}

std::pair<const Metadata *, const OpaqueValue *>
swift::getValueFromSwiftValue(__SwiftValue *v) {
  auto instanceType = getSwiftValueTypeMetadata(v);
  size_t alignMask = getSwiftValuePayloadAlignMask(instanceType);
  return {instanceType, getSwiftValuePayload(v, alignMask)};
}

__SwiftValue *swift::bridgeAnythingToSwiftValueObject(OpaqueValue *src,
                                                    const Metadata *srcType,
                                                    bool consume) {
  size_t alignMask = getSwiftValuePayloadAlignMask(srcType);

  size_t totalSize =
      getSwiftValuePayloadOffset(alignMask) + srcType->getValueWitnesses()->size;

  void *instanceMemory = swift_slowAlloc(totalSize, alignMask);
  __SwiftValue *instance
    = objc_constructInstance(getSwiftValueClass(), instanceMemory);
  /* TODO: If we're able to become a SwiftObject subclass in the future,
   * change to this:
  auto instance = swift_allocObject(getSwiftValueClass(), totalSize,
                                    alignMask);
   */

  auto header = getSwiftValueHeader(instance);
  ::new (header) SwiftValueHeader();
  header->type = srcType;

  auto payload = getSwiftValuePayload(instance, alignMask);
  if (consume)
    srcType->vw_initializeWithTake(payload, src);
  else
    srcType->vw_initializeWithCopy(payload, src);

  return instance;
}

__SwiftValue *swift::getAsSwiftValue(id object) {
  // __SwiftValue should have no subclasses or proxies. We can do an exact
  // class check.
  if (object_getClass(object) == getSwiftValueClass())
    return object;
  return nil;
}

bool
swift::findSwiftValueConformances(const ExistentialTypeMetadata *existentialType,
                                  const WitnessTable **tablesBuffer) {
  // __SwiftValue never satisfies a superclass constraint.
  if (existentialType->getSuperclassConstraint() != nullptr)
    return false;

  Class cls = nullptr;

  // Note that currently we never modify tablesBuffer because
  // __SwiftValue doesn't conform to any protocols that need witness tables.

  for (auto protocol : existentialType->getProtocols()) {
    // __SwiftValue only conforms to ObjC protocols.  We specifically
    // don't want to say that __SwiftValue conforms to the Swift protocols
    // that NSObject conforms to because that would create a situation
    // where arguably an arbitrary type would conform to those protocols
    // by way of coercion through __SwiftValue.  Eventually we want to
    // change __SwiftValue to not be an NSObject subclass at all.

    if (!protocol.isObjC())
      return false;

    if (!cls) cls = _getSwiftValueClass();

    // Check whether the class conforms to the protocol.
    if (![cls conformsToProtocol: protocol.getObjCProtocol()])
      return false;
  }

  return true;
}

@implementation __SwiftValue

+ (instancetype)allocWithZone:(NSZone *)zone {
  swift::crash("__SwiftValue cannot be instantiated");
}

- (id)copyWithZone:(NSZone *)zone {
  // Instances are immutable, so we can just retain.
  return objc_retain(self);

  /* TODO: If we're able to become a SwiftObject subclass in the future,
   * change to this:
   swift_retain((HeapObject*)self);
   return self;
   */
}

// Since we allocate using Swift's allocator to properly handle alignment,
// we need to deallocate ourselves instead of delegating to
// -[NSObject dealloc].
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-missing-super-calls"
- (void)dealloc {
  // TODO: If we're able to become a SwiftObject subclass in the future,
  // this should move to the heap metadata destructor function.

  // Destroy the contained value.
  auto instanceType = getSwiftValueTypeMetadata(self);
  size_t alignMask = getSwiftValuePayloadAlignMask(instanceType);
  getSwiftValueHeader(self)->~SwiftValueHeader();
  instanceType->vw_destroy(getSwiftValuePayload(self, alignMask));

  // Deallocate ourselves.
  objc_destructInstance(self);
  auto totalSize = getSwiftValuePayloadOffset(alignMask) +
                   instanceType->getValueWitnesses()->size;
  swift_slowDealloc(self, totalSize, alignMask);
}
#pragma clang diagnostic pop

- (BOOL)isEqual:(id)other {
  if (self == other) {
    return YES;
  }

  if (!other) {
    return NO;
  }

  // `other` must also be a _SwiftValue box
  if (![other isKindOfClass:getSwiftValueClass()]) {
    return NO;
  }

  auto selfHeader = getSwiftValueHeader(self);
  auto otherHeader = getSwiftValueHeader(other);

  if (auto hashableConformance = selfHeader->getHashableConformance()) {
    if (auto selfHashableBaseType = selfHeader->getHashableBaseType()) {
      auto otherHashableBaseType = otherHeader->getHashableBaseType();
      if (selfHashableBaseType == otherHashableBaseType) {
        return _swift_stdlib_Hashable_isEqual_indirect(
          getSwiftValuePayload(self,
                               getSwiftValuePayloadAlignMask(selfHeader->type)),
          getSwiftValuePayload(other,
                               getSwiftValuePayloadAlignMask(otherHeader->type)),
          selfHashableBaseType, hashableConformance);
      }
    }
  }

//  if (runtime::bincompat::useLegacySwiftObjCHashing()) {
//    // Legacy behavior only proxies isEqual: for Hashable, not Equatable
//    return NO;
//  }

  if (auto equatableConformance = selfHeader->getEquatableConformance()) {
    if (auto selfEquatableBaseType = selfHeader->getEquatableBaseType()) {
      auto otherEquatableBaseType = otherHeader->getEquatableBaseType();
      if (selfEquatableBaseType == otherEquatableBaseType) {
        return _swift_stdlib_Equatable_isEqual_indirect(
          getSwiftValuePayload(self,
                               getSwiftValuePayloadAlignMask(selfHeader->type)),
          getSwiftValuePayload(other,
                               getSwiftValuePayloadAlignMask(otherHeader->type)),
          selfEquatableBaseType, equatableConformance);
      }
    }
  }

  // Not Equatable, not Hashable, and not the same box
  return NO;
}

- (NSUInteger)hash {
  // If Swift type is Hashable, get the hash value from there
  auto selfHeader = getSwiftValueHeader(self);
  auto hashableConformance = selfHeader->getHashableConformance();
  if (hashableConformance) {
	  return _swift_stdlib_Hashable_hashValue_indirect(
	    getSwiftValuePayload(self,
				 getSwiftValuePayloadAlignMask(selfHeader->type)),
	    selfHeader->type, hashableConformance);
  }

//  if (runtime::bincompat::useLegacySwiftObjCHashing()) {
//    // Legacy behavior doesn't honor Equatable conformance, only Hashable
//    return (NSUInteger)self;
//  }

  // If Swift type is Equatable but not Hashable,
  // we have to return something here that is compatible
  // with the `isEqual:` above.
  auto equatableConformance = selfHeader->getEquatableConformance();
  if (equatableConformance) {
    // Warn once per type about this
    auto metadata = getSwiftValueTypeMetadata(self);
    static Lazy<std::unordered_set<const Metadata *>> warned;
    static LazyMutex warnedLock;
    LazyMutex::ScopedLock guard(warnedLock);
    auto result = warned.get().insert(metadata);
    auto inserted = std::get<1>(result);
    if (inserted) {
      TypeNamePair typeName = swift_getTypeName(metadata, true);
      warning(0,
	      "Obj-C `-hash` invoked on a Swift value of type `%s` that is Equatable but not Hashable; "
	      "this can lead to severe performance problems.\n",
	      typeName.data);
    }
    // Constant value (yuck!) is the only choice here
    return (NSUInteger)1;
  }

  // If the Swift type is neither Equatable nor Hashable,
  // then we can hash the identity, which should be pretty
  // good in practice.
  return (NSUInteger)self;
}

static id getValueDescription(__SwiftValue *self) {
  const Metadata *type;
  const OpaqueValue *value;
  std::tie(type, value) = getValueFromSwiftValue(self);

  // Copy the value, since it will be consumed by getSummary.
  ValueBuffer copyBuf;
  auto copy = type->allocateBufferIn(&copyBuf);
  type->vw_initializeWithCopy(copy, const_cast<OpaqueValue *>(value));

  id string = getDescription(copy, type);
  type->deallocateBufferIn(&copyBuf);
  return string;
}

- (id /* NSString */)description {
  return getValueDescription(self);
}
- (id /* NSString */)debugDescription {
  return getValueDescription(self);
}

// Private methods for debugging purposes.

- (const Metadata *)_swiftTypeMetadata {
  return getSwiftValueTypeMetadata(self);
}
- (id /* NSString */)_swiftTypeName {
  TypeNamePair typeName
    = swift_getTypeName(getSwiftValueTypeMetadata(self), true);
  id str = swift_stdlib_NSStringFromUTF8(typeName.data, typeName.length);
  return [str autorelease];
}
- (const OpaqueValue *)_swiftValue {
  return getValueFromSwiftValue(self).second;
}

@end
#endif

// TODO: We could pick specialized __SwiftValue subclasses for trivial types
// or for types with known size and alignment characteristics. Probably
// not enough of a real perf bottleneck to be worth it...
