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

#include "Private.h"
#include "SwiftHashableSupport.h"
#include "SwiftValue.h"
#include "swift/Basic/Lazy.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/Concurrent.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/shims/Visibility.h"

#include <new>

using namespace swift;
using namespace swift::hashable_support;

namespace {
struct HashableConformanceKey {
  /// The lookup key, the metadata of a type that is possibly derived
  /// from a type that conforms to `Hashable`.
  const Metadata *derivedType;

  friend llvm::hash_code hash_value(const HashableConformanceKey &key) {
    return llvm::hash_value(key.derivedType);
  }
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

  bool matchesKey(const HashableConformanceKey &key) {
    return derivedType == key.derivedType;
  }

  friend llvm::hash_code hash_value(const HashableConformanceEntry &value) {
    return hash_value(HashableConformanceKey{value.derivedType});
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
static ConcurrentReadableHashMap<HashableConformanceEntry> HashableConformances;

template <bool KnownToConformToHashable>
SWIFT_ALWAYS_INLINE static const Metadata *
findHashableBaseTypeImpl(const Metadata *type) {
  // Check the cache first.
  {
    auto snapshot = HashableConformances.snapshot();
    if (const HashableConformanceEntry *entry =
            snapshot.find(HashableConformanceKey{type})) {
      return entry->baseTypeThatConformsToHashable;
    }
  }

  auto witnessTable =
    swift_conformsToProtocolCommon(type, &HashableProtocolDescriptor);
  if (!KnownToConformToHashable && !witnessTable) {
    // Don't cache the negative response because we don't invalidate
    // this cache when a new conformance is loaded dynamically.
    return nullptr;
  }
  // By this point, `type` is known to conform to `Hashable`.
#if SWIFT_STDLIB_USE_RELATIVE_PROTOCOL_WITNESS_TABLES
  const auto *conformance = lookThroughOptionalConditionalWitnessTable(
    reinterpret_cast<const RelativeWitnessTable*>(witnessTable))
    ->getDescription();
#else
  const auto *conformance = witnessTable->getDescription();
#endif
  const Metadata *baseTypeThatConformsToHashable =
    findConformingSuperclass(type, conformance);
  HashableConformanceKey key{type};
  HashableConformances.getOrInsert(key, [&](HashableConformanceEntry *entry,
                                            bool created) {
    if (created)
      ::new (entry) HashableConformanceEntry(key, baseTypeThatConformsToHashable);
    return true; // Keep the new entry.
  });
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
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_makeAnyHashableUsingDefaultRepresentation(
  const OpaqueValue *value,
  const void *anyHashableResultPointer,
  const Metadata *T,
  const WitnessTable *hashableWT
);

// public func _makeAnyHashableUpcastingToHashableBaseType<H : Hashable>(
//   _ value: H,
//   storingResultInto result: UnsafeMutablePointer<AnyHashable>)
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_SPI
void _swift_makeAnyHashableUpcastingToHashableBaseType(
  OpaqueValue *value,
  const void *anyHashableResultPointer,
  const Metadata *type,
  const WitnessTable *hashableWT
) {
  switch (type->getKind()) {
  case MetadataKind::Class:
  case MetadataKind::ObjCClassWrapper:
  case MetadataKind::ForeignClass:
  case MetadataKind::ForeignReferenceType: {
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
              swift_conformsToProtocolCommon(unboxedType, &HashableProtocolDescriptor)) {
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

