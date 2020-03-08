//===----------------------------------------------------------------------===//
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

#include "swift/Runtime/Config.h"
#include "swift/Basic/Lazy.h"
#include "swift/Runtime/Concurrent.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Casting.h"
#include "Private.h"
#include "SwiftValue.h"
#include "SwiftHashableSupport.h"

using namespace swift;
using namespace swift::hashable_support;

namespace {
struct HashableConformanceKey {
  /// The lookup key, the metadata of a type that is possibly derived
  /// from a type that conforms to `Hashable`.
  const Metadata *derivedType;
};

struct HashableConformanceEntry {
  /// The lookup key, the metadata of a type that is possibly derived
  /// from a type that conforms to `Hashable`.
  const Metadata *derivedType;

  /// The highest (closest to the root) type in the superclass chain
  /// that conforms to `Hashable`.
  ///
  /// Always non-NULL.  We don't cache negative responses so that we
  /// don't have to deal with cache invalidation.
  const Metadata *baseTypeThatConformsToHashable;

  HashableConformanceEntry(HashableConformanceKey key,
                           const Metadata *baseTypeThatConformsToHashable)
      : derivedType(key.derivedType),
        baseTypeThatConformsToHashable(baseTypeThatConformsToHashable) {}

  int compareWithKey(const HashableConformanceKey &key) const {
    if (key.derivedType != derivedType) {
      return (uintptr_t(key.derivedType) < uintptr_t(derivedType) ? -1 : 1);
    } else {
      return 0;
    }
  }

  static size_t
  getExtraAllocationSize(HashableConformanceKey key,
                         const Metadata *baseTypeThatConformsToHashable) {
    return 0;
  }

  size_t getExtraAllocationSize() const {
    return 0;
  }
};
} // end unnamed namespace

// FIXME(performance): consider merging this cache into the regular
// protocol conformance cache.
static ConcurrentMap<HashableConformanceEntry, /*Destructor*/ false>
  HashableConformances;

template<bool KnownToConformToHashable>
LLVM_ATTRIBUTE_ALWAYS_INLINE
static const Metadata *findHashableBaseTypeImpl(const Metadata *type) {
  // Check the cache first.
  if (HashableConformanceEntry *entry =
          HashableConformances.find(HashableConformanceKey{type})) {
    return entry->baseTypeThatConformsToHashable;
  }

  auto witnessTable =
    swift_conformsToProtocol(type, &HashableProtocolDescriptor);
  if (!KnownToConformToHashable && !witnessTable) {
    // Don't cache the negative response because we don't invalidate
    // this cache when a new conformance is loaded dynamically.
    return nullptr;
  }
  // By this point, `type` is known to conform to `Hashable`.
  const auto *conformance = witnessTable->Description;
  const Metadata *baseTypeThatConformsToHashable =
    findConformingSuperclass(type, conformance);
  HashableConformances.getOrInsert(HashableConformanceKey{type},
                                   baseTypeThatConformsToHashable);
  return baseTypeThatConformsToHashable;
}

/// Find the base type that introduces the `Hashable` conformance.
/// Because the provided type is known to conform to `Hashable`, this
/// function always returns non-null.
///
/// - Precondition: `type` conforms to `Hashable` (not checked).
const Metadata *swift::hashable_support::findHashableBaseTypeOfHashableType(
    const Metadata *type) {
  auto result =
    findHashableBaseTypeImpl</*KnownToConformToHashable=*/ true>(type);
  assert(result && "Known-hashable types should have a `Hashable` conformance.");
  return result;
}

/// Find the base type that introduces the `Hashable` conformance.
/// If `type` does not conform to `Hashable`, `nullptr` is returned.
const Metadata *swift::hashable_support::findHashableBaseType(
    const Metadata *type) {
  return findHashableBaseTypeImpl</*KnownToConformToHashable=*/ false>(type);
}

// internal func _makeAnyHashableUsingDefaultRepresentation<H : Hashable>(
//   of value: H,
//   storingResultInto result: UnsafeMutablePointer<AnyHashable>)
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_makeAnyHashableUsingDefaultRepresentation(
  const OpaqueValue *value,
  const void *anyHashableResultPointer,
  const Metadata *T,
  const WitnessTable *hashableWT
);

// public func _makeAnyHashableUpcastingToHashableBaseType<H : Hashable>(
//   _ value: H,
//   storingResultInto result: UnsafeMutablePointer<AnyHashable>)
SWIFT_CC SWIFT_RUNTIME_STDLIB_SPI
void _swift_makeAnyHashableUpcastingToHashableBaseType(
  OpaqueValue *value,
  const void *anyHashableResultPointer,
  const Metadata *type,
  const WitnessTable *hashableWT
) {
  switch (type->getKind()) {
  case MetadataKind::Class:
  case MetadataKind::ObjCClassWrapper:
  case MetadataKind::ForeignClass: {
#if SWIFT_OBJC_INTEROP
    id srcObject;
    memcpy(&srcObject, value, sizeof(id));
    // Do we have a __SwiftValue?
    if (__SwiftValue *srcSwiftValue = getAsSwiftValue(srcObject)) {
      // If so, extract the boxed value and try to cast it.
      const Metadata *unboxedType;
      const OpaqueValue *unboxedValue;
      std::tie(unboxedType, unboxedValue) =
          getValueFromSwiftValue(srcSwiftValue);

      if (auto unboxedHashableWT =
              swift_conformsToProtocol(unboxedType, &HashableProtocolDescriptor)) {
        _swift_makeAnyHashableUpcastingToHashableBaseType(
            const_cast<OpaqueValue *>(unboxedValue), anyHashableResultPointer,
            unboxedType, unboxedHashableWT);
        return;
      }
    }
#endif

    _swift_makeAnyHashableUsingDefaultRepresentation(
        value, anyHashableResultPointer,
        findHashableBaseTypeOfHashableType(type),
        hashableWT);
    return;
  }

  default:
    _swift_makeAnyHashableUsingDefaultRepresentation(
        value, anyHashableResultPointer, type, hashableWT);
    return;
  }
}

