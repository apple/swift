//===--- ProtocolConformance.cpp - Swift protocol conformance checking ----===//
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
// Checking and caching of Swift protocol conformances.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Lazy.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Runtime/Bincompat.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/Concurrent.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Basic/Unreachable.h"
#include "CompatibilityOverride.h"
#include "ImageInspection.h"
#include "Private.h"

#include <vector>

using namespace swift;

#ifndef NDEBUG
template <> SWIFT_USED void ProtocolDescriptor::dump() const {
  printf("TargetProtocolDescriptor.\n"
         "Name: \"%s\".\n",
         Name.get());
}

void ProtocolDescriptorFlags::dump() const {
  printf("ProtocolDescriptorFlags.\n");
  printf("Is Swift: %s.\n", (isSwift() ? "true" : "false"));
  printf("Needs Witness Table: %s.\n",
         (needsWitnessTable() ? "true" : "false"));
  printf("Is Resilient: %s.\n", (isResilient() ? "true" : "false"));
  printf("Special Protocol: %s.\n",
         (bool(getSpecialProtocol()) ? "Error" : "None"));
  printf("Class Constraint: %s.\n",
         (bool(getClassConstraint()) ? "Class" : "Any"));
  printf("Dispatch Strategy: %s.\n",
         (bool(getDispatchStrategy()) ? "Swift" : "ObjC"));
}

#endif

#if !defined(NDEBUG) && SWIFT_OBJC_INTEROP
#include <objc/runtime.h>

static const char *class_getName(const ClassMetadata* type) {
  return class_getName(
    reinterpret_cast<Class>(const_cast<ClassMetadata*>(type)));
}

template<> void ProtocolConformanceDescriptor::dump() const {
  auto symbolName = [&](const void *addr) -> const char * {
    SymbolInfo info;
    int ok = lookupSymbol(addr, &info);
    if (!ok)
      return "<unknown addr>";
    return info.symbolName.get();
  };

  switch (auto kind = getTypeKind()) {
  case TypeReferenceKind::DirectObjCClassName:
    printf("direct Objective-C class name %s", getDirectObjCClassName());
    break;

  case TypeReferenceKind::IndirectObjCClass:
    printf("indirect Objective-C class %s",
           class_getName(*getIndirectObjCClass()));
    break;

  case TypeReferenceKind::DirectTypeDescriptor:
  case TypeReferenceKind::IndirectTypeDescriptor:
    printf("unique nominal type descriptor %s", symbolName(getTypeDescriptor()));
    break;
  }
  
  printf(" => ");
  
  printf("witness table %pattern s\n", symbolName(getWitnessTablePattern()));
}
#endif

#ifndef NDEBUG
template <> SWIFT_USED void ProtocolConformanceDescriptor::verify() const {
  auto typeKind = unsigned(getTypeKind());
  assert(((unsigned(TypeReferenceKind::First_Kind) <= typeKind) &&
          (unsigned(TypeReferenceKind::Last_Kind) >= typeKind)) &&
         "Corrupted type metadata record kind");
}
#endif

#if SWIFT_OBJC_INTEROP
template <>
const ClassMetadata *TypeReference::getObjCClass(TypeReferenceKind kind) const {
  switch (kind) {
  case TypeReferenceKind::IndirectObjCClass:
    return *getIndirectObjCClass(kind);

  case TypeReferenceKind::DirectObjCClassName:
    return reinterpret_cast<const ClassMetadata *>(
              objc_lookUpClass(getDirectObjCClassName(kind)));

  case TypeReferenceKind::DirectTypeDescriptor:
  case TypeReferenceKind::IndirectTypeDescriptor:
    return nullptr;
  }

  swift_unreachable("Unhandled TypeReferenceKind in switch.");
}
#endif

/// Take the type reference inside a protocol conformance record and fetch the
/// canonical metadata pointer for the type it refers to.
/// Returns nil for universal or generic type references.
template <>
const Metadata *
ProtocolConformanceDescriptor::getCanonicalTypeMetadata() const {
  switch (getTypeKind()) {
  case TypeReferenceKind::IndirectObjCClass:
  case TypeReferenceKind::DirectObjCClassName:
#if SWIFT_OBJC_INTEROP
    // The class may be ObjC, in which case we need to instantiate its Swift
    // metadata. The class additionally may be weak-linked, so we have to check
    // for null.
    if (auto cls = TypeRef.getObjCClass(getTypeKind()))
      return getMetadataForClass(cls);
#endif
    return nullptr;

  case TypeReferenceKind::DirectTypeDescriptor:
  case TypeReferenceKind::IndirectTypeDescriptor: {
    if (auto anyType = getTypeDescriptor()) {
      if (auto type = dyn_cast<TypeContextDescriptor>(anyType)) {
        if (!type->isGeneric()) {
          if (auto accessFn = type->getAccessFunction())
            return accessFn(MetadataState::Abstract).Value;
        }
      } else if (auto protocol = dyn_cast<ProtocolDescriptor>(anyType)) {
        return _getSimpleProtocolTypeMetadata(protocol);
      }
    }

    return nullptr;
  }
  }

  swift_unreachable("Unhandled TypeReferenceKind in switch.");
}

template<>
const WitnessTable *
ProtocolConformanceDescriptor::getWitnessTable(const Metadata *type) const {
  // If needed, check the conditional requirements.
  llvm::SmallVector<const void *, 8> conditionalArgs;
  if (hasConditionalRequirements()) {
    SubstGenericParametersFromMetadata substitutions(type);
    auto error = _checkGenericRequirements(
        getConditionalRequirements(), conditionalArgs,
        [&substitutions](unsigned depth, unsigned index) {
          return substitutions.getMetadata(depth, index);
        },
        [&substitutions](const Metadata *type, unsigned index) {
          return substitutions.getWitnessTable(type, index);
        });
    if (error)
      return nullptr;
  }

  return swift_getWitnessTable(this, type, conditionalArgs.data());
}

namespace {
  struct ConformanceSection {
    const ProtocolConformanceRecord *Begin, *End;
    const ProtocolConformanceRecord *begin() const {
      return Begin;
    }
    const ProtocolConformanceRecord *end() const {
      return End;
    }
  };

  struct ConformanceCacheKey {
    const Metadata *Type;
    const ProtocolDescriptor *Proto;

    ConformanceCacheKey(const Metadata *type, const ProtocolDescriptor *proto)
        : Type(type), Proto(proto) {
      assert(type);
    }

    friend llvm::hash_code hash_value(const ConformanceCacheKey &key) {
      return llvm::hash_combine(key.Type, key.Proto);
    }
  };

  struct ConformanceCacheEntry {
  private:
    ConformanceCacheKey Key;
    const WitnessTable *Witness;

  public:
    ConformanceCacheEntry(ConformanceCacheKey key, const WitnessTable *witness)
        : Key(key), Witness(witness) {}

    bool matchesKey(const ConformanceCacheKey &key) const {
      return Key.Type == key.Type && Key.Proto == key.Proto;
    }

    friend llvm::hash_code hash_value(const ConformanceCacheEntry &entry) {
      return hash_value(entry.Key);
    }

    template <class... Args>
    static size_t getExtraAllocationSize(Args &&... ignored) {
      return 0;
    }

    /// Get the cached witness table, or null if we cached failure.
    const WitnessTable *getWitnessTable() const {
      return Witness;
    }
  };
} // end anonymous namespace

// Conformance Cache.
struct ConformanceState {
  ConcurrentReadableHashMap<ConformanceCacheEntry> Cache;
  ConcurrentReadableArray<ConformanceSection> SectionsToScan;
  bool scanSectionsBackwards;

  ConformanceState() {
    scanSectionsBackwards =
        runtime::bincompat::workaroundProtocolConformanceReverseIteration();
    initializeProtocolConformanceLookup();
  }

  void cacheResult(const Metadata *type, const ProtocolDescriptor *proto,
                   const WitnessTable *witness, size_t sectionsCount) {
    Cache.getOrInsert(ConformanceCacheKey(type, proto),
                      [&](ConformanceCacheEntry *entry, bool created) {
                        // Create the entry if needed. If it already exists,
                        // we're done.
                        if (!created)
                          return false;

                        // Check the current sections count against what was
                        // passed in. If a section count was passed in and they
                        // don't match, then this is not an authoritative entry
                        // and it may have been obsoleted, because the new
                        // sections could contain a conformance in a more
                        // specific type.
                        //
                        // If they DO match, then we can safely add. Another
                        // thread might be adding new sections at this point,
                        // but we will not race with them. That other thread
                        // will add the new sections, then clear the cache. When
                        // it clears the cache, it will block waiting for this
                        // code to complete and relinquish Cache's writer lock.
                        // If we cache a stale entry, it will be immediately
                        // cleared.
                        if (sectionsCount > 0 &&
                            SectionsToScan.snapshot().count() != sectionsCount)
                          return false; // abandon the new entry

                        new (entry) ConformanceCacheEntry(
                            ConformanceCacheKey(type, proto), witness);
                        return true; // keep the new entry
                      });
  }

#ifndef NDEBUG
  void verify() const SWIFT_USED;
#endif
};

#ifndef NDEBUG
void ConformanceState::verify() const {
  // Iterate over all of the sections and verify all of the protocol
  // descriptors.
  auto &Self = const_cast<ConformanceState &>(*this);
  for (const auto &Section : Self.SectionsToScan.snapshot()) {
    for (const auto &Record : Section) {
      Record.get()->verify();
    }
  }
}
#endif

static Lazy<ConformanceState> Conformances;

const void * const swift::_swift_debug_protocolConformanceStatePointer =
  &Conformances;

static void
_registerProtocolConformances(ConformanceState &C,
                              const ProtocolConformanceRecord *begin,
                              const ProtocolConformanceRecord *end) {
  C.SectionsToScan.push_back(ConformanceSection{begin, end});

  // Blow away the conformances cache to get rid of any negative entries that
  // may now be obsolete.
  C.Cache.clear();
}

void swift::addImageProtocolConformanceBlockCallbackUnsafe(
    const void *conformances, uintptr_t conformancesSize) {
  assert(conformancesSize % sizeof(ProtocolConformanceRecord) == 0 &&
         "conformances section not a multiple of ProtocolConformanceRecord");

  // If we have a section, enqueue the conformances for lookup.
  auto conformanceBytes = reinterpret_cast<const char *>(conformances);
  auto recordsBegin
    = reinterpret_cast<const ProtocolConformanceRecord*>(conformances);
  auto recordsEnd
    = reinterpret_cast<const ProtocolConformanceRecord*>
                                          (conformanceBytes + conformancesSize);
  
  // Conformance cache should always be sufficiently initialized by this point.
  _registerProtocolConformances(Conformances.unsafeGetAlreadyInitialized(),
                                recordsBegin, recordsEnd);
}

void swift::addImageProtocolConformanceBlockCallback(
    const void *conformances, uintptr_t conformancesSize) {
  Conformances.get();
  addImageProtocolConformanceBlockCallbackUnsafe(conformances,
                                                 conformancesSize);
}

void
swift::swift_registerProtocolConformances(const ProtocolConformanceRecord *begin,
                                          const ProtocolConformanceRecord *end){
  auto &C = Conformances.get();
  _registerProtocolConformances(C, begin, end);
}

/// Search for a conformance descriptor in the ConformanceCache.
/// First element of the return value is `true` if the result is authoritative
/// i.e. the result is for the type itself and not a superclass. If `false`
/// then we cached a conformance on a superclass, but that may be overridden.
/// A return value of `{ false, nullptr }` indicates nothing was cached.
static std::pair<bool, const WitnessTable *>
searchInConformanceCache(const Metadata *type,
                         const ProtocolDescriptor *protocol) {
  auto &C = Conformances.get();
  auto origType = type;
  auto snapshot = C.Cache.snapshot();

  while (type) {
    if (auto *Value = snapshot.find(ConformanceCacheKey(type, protocol))) {
      return {type == origType, Value->getWitnessTable()};
    }

    // If there is a superclass, look there.
    type = _swift_class_getSuperclass(type);
  }

  // We did not find a cache entry.
  return {false, nullptr};
}

namespace {
  /// Describes a protocol conformance "candidate" that can be checked
  /// against a type metadata.
  class ConformanceCandidate {
    const void *candidate;
    bool candidateIsMetadata;

  public:
    ConformanceCandidate() : candidate(0), candidateIsMetadata(false) { }

    ConformanceCandidate(const ProtocolConformanceDescriptor &conformance)
      : ConformanceCandidate()
    {
      if (auto description = conformance.getTypeDescriptor()) {
        candidate = description;
        candidateIsMetadata = false;
        return;
      }

      if (auto metadata = conformance.getCanonicalTypeMetadata()) {
        candidate = metadata;
        candidateIsMetadata = true;
        return;
      }
    }

    const ContextDescriptor *
    getContextDescriptor(const Metadata *conformingType) const {
      const auto *description = conformingType->getTypeContextDescriptor();
      if (description)
        return description;

      // Handle single-protocol existential types for self-conformance.
      auto *existentialType = dyn_cast<ExistentialTypeMetadata>(conformingType);
      if (existentialType == nullptr ||
          existentialType->getProtocols().size() != 1 ||
          existentialType->getSuperclassConstraint() != nullptr)
        return nullptr;

      auto proto = existentialType->getProtocols()[0];

#if SWIFT_OBJC_INTEROP
      if (proto.isObjC())
        return nullptr;
#endif

      return proto.getSwiftProtocol();
    }

    /// Whether the conforming type exactly matches the conformance candidate.
    bool matches(const Metadata *conformingType) const {
      // Check whether the types match.
      if (candidateIsMetadata && conformingType == candidate)
        return true;

      // Check whether the nominal type descriptors match.
      if (!candidateIsMetadata) {
        const auto *description = getContextDescriptor(conformingType);
        auto candidateDescription =
          static_cast<const ContextDescriptor *>(candidate);
        if (description && equalContexts(description, candidateDescription))
          return true;
      }

      return false;
    }

    /// Retrieve the type that matches the conformance candidate, which may
    /// be a superclass of the given type. Returns null if this type does not
    /// match this conformance.
    const Metadata *getMatchingType(const Metadata *conformingType) const {
      while (conformingType) {
        // Check for a match.
        if (matches(conformingType))
          return conformingType;

        // Look for a superclass.
        conformingType = _swift_class_getSuperclass(conformingType);
      }

      return nullptr;
    }
  };
}

static const WitnessTable *
swift_conformsToProtocolImpl(const Metadata *const type,
                             const ProtocolDescriptor *protocol) {
  auto &C = Conformances.get();

  // See if we have an authoritative cached conformance. The
  // ConcurrentReadableHashMap data structure allows us to search the map
  // concurrently without locking.
  auto found = searchInConformanceCache(type, protocol);
  if (found.first)
    return found.second;

  // Scan conformance records.
  auto processSection = [&](const ConformanceSection &section) {
    // Eagerly pull records for nondependent witnesses into our cache.
    auto processDescriptor = [&](const ProtocolConformanceDescriptor &descriptor) {
      // We only care about conformances for this protocol.
      if (descriptor.getProtocol() != protocol)
        return;

      // If there's a matching type, record the positive result and return it.
      // The matching type is exact, so they can't go stale, and we should
      // always cache them.
      ConformanceCandidate candidate(descriptor);
      if (auto *matchingType = candidate.getMatchingType(type)) {
        auto witness = descriptor.getWitnessTable(matchingType);
        C.cacheResult(matchingType, protocol, witness, /*always cache*/ 0);
      }
    };

    if (C.scanSectionsBackwards) {
      for (const auto &record : llvm::reverse(section))
        processDescriptor(*record.get());
    } else {
      for (const auto &record : section)
        processDescriptor(*record.get());
    }
  };

  auto snapshot = C.SectionsToScan.snapshot();
  if (C.scanSectionsBackwards) {
    for (auto &section : llvm::reverse(snapshot))
      processSection(section);
  } else {
    for (auto &section : snapshot)
      processSection(section);
  }

  // Try the search again to look for the most specific cached conformance.
  found = searchInConformanceCache(type, protocol);

  // If it's not authoritative, then add an authoritative entry for this type.
  if (!found.first)
    C.cacheResult(type, protocol, found.second, snapshot.count());

  return found.second;
}

const ContextDescriptor *
swift::_searchConformancesByMangledTypeName(Demangle::NodePointer node) {
  auto &C = Conformances.get();

  for (auto &section : C.SectionsToScan.snapshot()) {
    for (const auto &record : section) {
      if (auto ntd = record->getTypeDescriptor()) {
        if (_contextDescriptorMatchesMangling(ntd, node))
          return ntd;
      }
    }
  }
  return nullptr;
}

static MetadataState
tryGetCompleteMetadataNonblocking(const Metadata *metadata) {
  return swift_checkMetadataState(
             MetadataRequest(MetadataState::Complete, /*isNonBlocking*/ true),
             metadata)
      .State;
}

template <typename HandleObjc>
bool isSwiftClassMetadataSubclass(const ClassMetadata *subclass,
                                  const ClassMetadata *superclass,
                                  HandleObjc handleObjc) {
  assert(subclass);
  assert(superclass);

  MetadataState subclassState = tryGetCompleteMetadataNonblocking(subclass);

  do {
    if (subclassState == MetadataState::Complete) {
      // The subclass metadata is complete.  That means not just that its
      // Superclass field is valid, but that the Superclass field of the
      // referenced class metadata is valid, and the Superclass field of the
      // class metadata referenced there, and so on transitively.
      //
      // Scan the superclass chains in the ClassMetadata looking for a match.
      while ((subclass = subclass->Superclass)) {
        if (subclass == superclass)
          return true;
      }
      return false;
    }
    if (subclassState == MetadataState::NonTransitiveComplete) {
      // The subclass metadata is complete, but, unlike above, not transitively.
      // Its Superclass field is valid, so just read that field to get to the
      // superclass to proceed to the next step.
      subclass = subclass->Superclass;
      if (subclass->isPureObjC()) {
        return handleObjc(subclass, superclass);
      }
      subclassState = tryGetCompleteMetadataNonblocking(subclass);
    } else {
      // The subclass metadata is either LayoutComplete or Abstract, so the
      // Superclass field is not valid.  To get to the superclass, make the
      // expensive call to getSuperclassMetadata which demangles the superclass
      // name from the nominal type descriptor to get the metadata for the
      // superclass.
      MetadataRequest request(MetadataState::Complete,
                              /*non-blocking*/ true);
      auto response = getSuperclassMetadata(request, subclass);
      auto newMetadata = response.Value;
      if (auto newSubclass = dyn_cast<ClassMetadata>(newMetadata)) {
        subclass = newSubclass;
        subclassState = response.State;
      } else {
        return handleObjc(newMetadata, superclass);
      }
    }
    if (subclass == superclass)
      return true;
  } while (subclass);
  return false;
}

// Whether the provided `subclass` is metadata for a subclass* of the superclass
// whose metadata is specified.
//
// The function is robust against incomplete metadata for both subclass and
// superclass.  In the worst case, each intervening class between subclass and
// superclass is demangled.  Besides that slow path, there are a number of fast
// paths:
// - both classes are ObjC: swift_dynamicCastMetatype
// - Complete subclass metadata: loop over Superclass fields
// - NonTransitiveComplete: read the Superclass field once
//
// * A non-strict subclass; that is, given a class X, isSubclass(X.self, X.self)
//   is true.
static bool isSubclass(const Metadata *subclass, const Metadata *superclass) {
  assert(subclass);
  assert(superclass);
  assert(subclass->isAnyClass());
  assert(superclass->isAnyClass());

  if (subclass == superclass)
    return true;
  if (!isa<ClassMetadata>(subclass)) {
    if (!isa<ClassMetadata>(superclass)) {
      // Only ClassMetadata can be incomplete; when the class metadata is not
      // ClassMetadata, just use swift_dynamicCastMetatype.
      return swift_dynamicCastMetatype(subclass, superclass);
    } else {
      // subclass is ObjC, but superclass is not; since it is not possible for
      // any ObjC class to be a subclass of any Swift class, this subclass is
      // not a subclass of this superclass.
      return false;
    }
  }
  const ClassMetadata *swiftSubclass = cast<ClassMetadata>(subclass);
  if (auto *objcSuperclass = dyn_cast<ObjCClassWrapperMetadata>(superclass)) {
    // Walk up swiftSubclass's ancestors until we get to an ObjC class, then
    // kick over to swift_dynamicCastMetatype.
    return isSwiftClassMetadataSubclass(
        swiftSubclass, objcSuperclass->Class,
        [](const Metadata *intermediate, const Metadata *superclass) {
          // Intermediate is an ObjC class, and superclass is an ObjC class;
          // as above, just use swift_dynamicCastMetatype.
          return swift_dynamicCastMetatype(intermediate, superclass);
        });
    return false;
  }
  if (isa<ForeignClassMetadata>(superclass)) {
    // superclass is foreign, but subclass is not (if it were, the above
    // !isa<ClassMetadata> condition would have been entered).  Since it is not
    // possible for any Swift class to be a subclass of any foreign superclass,
    // this subclass is not a subclass of this superclass.
    return false;
  }
  auto swiftSuperclass = cast<ClassMetadata>(superclass);
  return isSwiftClassMetadataSubclass(swiftSubclass, swiftSuperclass,
                                      [](const Metadata *, const Metadata *) {
                                        // Because (1) no ObjC classes inherit
                                        // from Swift classes and (2)
                                        // `superclass` is not ObjC, if some
                                        // ancestor of `subclass` is ObjC, then
                                        // `subclass` cannot descend from
                                        // `superclass` (otherwise at some point
                                        // some ObjC class would have to inherit
                                        // from a Swift class).
                                        return false;
                                      });
}

llvm::Optional<TypeLookupError> swift::_checkGenericRequirements(
    llvm::ArrayRef<GenericRequirementDescriptor> requirements,
    llvm::SmallVectorImpl<const void *> &extraArguments,
    SubstGenericParameterFn substGenericParam,
    SubstDependentWitnessTableFn substWitnessTable) {
  for (const auto &req : requirements) {
    // Make sure we understand the requirement we're dealing with.
    if (!req.hasKnownKind())
      return TypeLookupError("unknown kind");

    // Resolve the subject generic parameter.
    auto result = swift_getTypeByMangledName(
        MetadataState::Abstract, req.getParam(), extraArguments.data(),
        substGenericParam, substWitnessTable);
    if (result.getError())
      return *result.getError();
    const Metadata *subjectType = result.getType().getMetadata();

    // Check the requirement.
    switch (req.getKind()) {
    case GenericRequirementKind::Protocol: {
#warning "todo"
      const WitnessTable *witnessTable = nullptr;
      if (!_conformsToProtocol(nullptr, subjectType, req.getProtocol(),
                               &witnessTable)) {
        const char *protoName =
            req.getProtocol() ? req.getProtocol().getName() : "<null>";
        return TYPE_LOOKUP_ERROR_FMT(
            "subject type %.*s does not conform to protocol %s",
            (int)req.getParam().size(), req.getParam().data(), protoName);
      }

      // If we need a witness table, add it.
      if (req.getProtocol().needsWitnessTable()) {
        assert(witnessTable);
        extraArguments.push_back(witnessTable);
      }

      continue;
    }

    case GenericRequirementKind::SameType: {
      // Demangle the second type under the given substitutions.
      auto result = swift_getTypeByMangledName(
          MetadataState::Abstract, req.getMangledTypeName(),
          extraArguments.data(), substGenericParam, substWitnessTable);
      if (result.getError())
        return *result.getError();
      auto otherType = result.getType().getMetadata();

      assert(!req.getFlags().hasExtraArgument());

      // Check that the types are equivalent.
      if (subjectType != otherType)
        return TYPE_LOOKUP_ERROR_FMT(
            "subject type %.*s does not match %.*s", (int)req.getParam().size(),
            req.getParam().data(), (int)req.getMangledTypeName().size(),
            req.getMangledTypeName().data());

      continue;
    }

    case GenericRequirementKind::Layout: {
      switch (req.getLayout()) {
      case GenericRequirementLayoutKind::Class:
        if (!subjectType->satisfiesClassConstraint())
          return TYPE_LOOKUP_ERROR_FMT(
              "subject type %.*s does not satisfy class constraint",
              (int)req.getParam().size(), req.getParam().data());
        continue;
      }

      // Unknown layout.
      return TYPE_LOOKUP_ERROR_FMT("unknown layout kind %u", req.getLayout());
    }

    case GenericRequirementKind::BaseClass: {
      // Demangle the base type under the given substitutions.
      auto result = swift_getTypeByMangledName(
          MetadataState::Abstract, req.getMangledTypeName(),
          extraArguments.data(), substGenericParam, substWitnessTable);
      if (result.getError())
        return *result.getError();
      auto baseType = result.getType().getMetadata();

      // If the type which is constrained to a base class is an existential 
      // type, and if that existential type includes a superclass constraint,
      // just require that the superclass by which the existential is
      // constrained is a subclass of the base class.
      if (auto *existential = dyn_cast<ExistentialTypeMetadata>(subjectType)) {
        if (auto *superclassConstraint = existential->getSuperclassConstraint())
          subjectType = superclassConstraint;
      }

      if (!isSubclass(subjectType, baseType))
        return TYPE_LOOKUP_ERROR_FMT(
            "%.*s is not subclass of %.*s", (int)req.getParam().size(),
            req.getParam().data(), (int)req.getMangledTypeName().size(),
            req.getMangledTypeName().data());

      continue;
    }

    case GenericRequirementKind::SameConformance: {
      // FIXME: Implement this check.
      continue;
    }
    }

    // Unknown generic requirement kind.
    return TYPE_LOOKUP_ERROR_FMT("unknown generic requirement kind %u",
                                 (unsigned)req.getKind());
  }

  // Success!
  return llvm::None;
}

const Metadata *swift::findConformingSuperclass(
                            const Metadata *type,
                            const ProtocolConformanceDescriptor *conformance) {
  // Figure out which type we're looking for.
  ConformanceCandidate candidate(*conformance);

  const Metadata *conformingType = candidate.getMatchingType(type);
  assert(conformingType);
  return conformingType;
}

#define OVERRIDE_PROTOCOLCONFORMANCE COMPATIBILITY_OVERRIDE
#include "CompatibilityOverride.def"
