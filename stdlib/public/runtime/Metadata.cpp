//===--- Metadata.cpp - Swift Language ABI Metadata Support ---------------===//
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
// Implementations of the metadata ABI functions.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Metadata.h"
#include "MetadataCache.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Lazy.h"
#include "swift/Basic/Range.h"
#include "swift/Demangling/Demangler.h"
#include "swift/ABI/TypeIdentity.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/ExistentialContainer.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Mutex.h"
#include "swift/Runtime/Once.h"
#include "swift/Strings.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/PointerLikeTypeTraits.h"
#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <condition_variable>
#include <new>
#include <unordered_set>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// Avoid defining macro max(), min() which conflict with std::max(), std::min()
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
// WASI doesn't support dynamic linking yet.
#if !defined(__wasi__)
#include <dlfcn.h>
#endif // !defined(__wasi__)
#endif
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "CompatibilityOverride.h"
#include "ErrorObject.h"
#include "ExistentialMetadataImpl.h"
#include "swift/Runtime/Debug.h"
#include "Private.h"

#if defined(__APPLE__)
#include <mach/vm_page_size.h>
#endif

#if SWIFT_OBJC_INTEROP
#include "ObjCRuntimeGetImageNameFromClass.h"
#endif

#include <cstdio>

#if defined(__APPLE__) && defined(VM_MEMORY_SWIFT_METADATA)
#define VM_TAG_FOR_SWIFT_METADATA VM_MAKE_TAG(VM_MEMORY_SWIFT_METADATA)
#else
#define VM_TAG_FOR_SWIFT_METADATA (-1)
#endif

using namespace swift;
using namespace metadataimpl;

template<>
Metadata *TargetSingletonMetadataInitialization<InProcess>::allocate(
    const TypeContextDescriptor *description) const {
  // If this class has resilient ancestry, the size of the metadata is not known
  // at compile time, so we allocate it dynamically, filling it in from a
  // pattern.
  if (hasResilientClassPattern(description)) {
    auto *pattern = ResilientPattern.get();

    // If there is a relocation function, call it.
    if (auto *fn = ResilientPattern->RelocationFunction.get())
      return fn(description, pattern);

    // Otherwise, use the default behavior.
    auto *classDescription = cast<ClassDescriptor>(description);
    return swift_relocateClassMetadata(classDescription, pattern);
  }

  // Otherwise, we have a static template that we can initialize in-place.
  auto *metadata = IncompleteMetadata.get();

  // If this is a class, we have to initialize the value witness table early
  // so that two-phase initialization can proceed as if this metadata is
  // complete for layout purposes when it appears as part of an aggregate type.
  //
  // Note that we can't use (dyn_)cast<ClassMetadata> here because the static
  // template may have the "wrong" isSwift bit set in its Data pointer, if the
  // binary was built to deploy back to pre-stable-Swift Objective-C runtimes.
  // Such a template will fail the `isTypeMetadata` test and we'll think that it
  // isn't Swift metadata but a plain old ObjC class instead.
  if (metadata->getKind() == MetadataKind::Class) {
    auto *classMetadata = static_cast<ClassMetadata*>(metadata);
    classMetadata->setAsTypeMetadata();
    auto *fullMetadata = asFullMetadata(metadata);

    // Begin by initializing the value witness table; everything else is
    // initialized by swift_initClassMetadata().
#if SWIFT_OBJC_INTEROP
    fullMetadata->ValueWitnesses =
      (classMetadata->Flags & ClassFlags::UsesSwiftRefcounting)
         ? &VALUE_WITNESS_SYM(Bo)
         : &VALUE_WITNESS_SYM(BO);
#else
    fullMetadata->ValueWitnesses = &VALUE_WITNESS_SYM(Bo);
#endif
  }

  return metadata;
}

/// Copy the generic arguments into place in a newly-allocated metadata.
static void installGenericArguments(Metadata *metadata,
                                    const TypeContextDescriptor *description,
                                    const void *arguments) {
  auto &generics = description->getFullGenericContextHeader();

  // If we ever have parameter packs, we may need to do more than just
  // copy here.
  memcpy(reinterpret_cast<const void **>(metadata)
           + description->getGenericArgumentOffset(),
         reinterpret_cast<const void * const *>(arguments),
         generics.Base.getNumArguments() * sizeof(void*));
}

#if SWIFT_OBJC_INTEROP
static ClassMetadataBounds computeMetadataBoundsForObjCClass(Class cls) {
  cls = swift_getInitializedObjCClass(cls);
  auto metadata = reinterpret_cast<const ClassMetadata *>(cls);
  return metadata->getClassBoundsAsSwiftSuperclass();
}
#endif

static ClassMetadataBounds
computeMetadataBoundsForSuperclass(const void *ref,
                                   TypeReferenceKind refKind) {
  switch (refKind) {
  case TypeReferenceKind::IndirectTypeDescriptor: {
    auto description = *reinterpret_cast<const ClassDescriptor * const *>(ref);
    if (!description) {
      swift::fatalError(0, "instantiating class metadata for class with "
                           "missing weak-linked ancestor");
    }
    return description->getMetadataBounds();
  }

  case TypeReferenceKind::DirectTypeDescriptor: {
    auto description = reinterpret_cast<const ClassDescriptor *>(ref);
    return description->getMetadataBounds();
  }

  case TypeReferenceKind::DirectObjCClassName: {
#if SWIFT_OBJC_INTEROP
    auto cls = objc_lookUpClass(reinterpret_cast<const char *>(ref));
    return computeMetadataBoundsForObjCClass(cls);
#else
    break;
#endif
  }

  case TypeReferenceKind::IndirectObjCClass: {
#if SWIFT_OBJC_INTEROP
    auto cls = *reinterpret_cast<const Class *>(ref);
    return computeMetadataBoundsForObjCClass(cls);
#else
    break;
#endif
  }
  }
  swift_runtime_unreachable("unsupported superclass reference kind");
}

static ClassMetadataBounds computeMetadataBoundsFromSuperclass(
                                      const ClassDescriptor *description,
                                      StoredClassMetadataBounds &storedBounds) {
  ClassMetadataBounds bounds;

  // Compute the bounds for the superclass, extending it to the minimum
  // bounds of a Swift class.
  if (const void *superRef = description->getResilientSuperclass()) {
    bounds = computeMetadataBoundsForSuperclass(superRef,
                           description->getResilientSuperclassReferenceKind());
  } else {
    bounds = ClassMetadataBounds::forSwiftRootClass();
  }

  // Add the subclass's immediate members.
  bounds.adjustForSubclass(description->areImmediateMembersNegative(),
                           description->NumImmediateMembers);

  // Cache before returning.
  storedBounds.initialize(bounds);
  return bounds;
}

ClassMetadataBounds
swift::getResilientMetadataBounds(const ClassDescriptor *description) {
  assert(description->hasResilientSuperclass());
  auto &storedBounds = *description->ResilientMetadataBounds.get();

  ClassMetadataBounds bounds;
  if (storedBounds.tryGet(bounds)) {
    return bounds;
  }

  return computeMetadataBoundsFromSuperclass(description, storedBounds);
}

int32_t
swift::getResilientImmediateMembersOffset(const ClassDescriptor *description) {
  assert(description->hasResilientSuperclass());
  auto &storedBounds = *description->ResilientMetadataBounds.get();

  ptrdiff_t result;
  if (storedBounds.tryGetImmediateMembersOffset(result)) {
    return result / sizeof(void*);
  }

  auto bounds = computeMetadataBoundsFromSuperclass(description, storedBounds);
  return bounds.ImmediateMembersOffset / sizeof(void*);
}

static bool
areAllTransitiveMetadataComplete_cheap(const Metadata *metadata);

static MetadataDependency
checkTransitiveCompleteness(const Metadata *metadata);

static PrivateMetadataState inferStateForMetadata(Metadata *metadata) {
  if (metadata->getValueWitnesses()->isIncomplete())
    return PrivateMetadataState::Abstract;

  // TODO: internal vs. external layout-complete?
  return PrivateMetadataState::LayoutComplete;
}

namespace {
  struct GenericCacheEntry final :
      VariadicMetadataCacheEntryBase<GenericCacheEntry> {
    static const char *getName() { return "GenericCache"; }

    template <class... Args>
    GenericCacheEntry(MetadataCacheKey key, Args &&...args)
      : VariadicMetadataCacheEntryBase(key) {}

    AllocationResult allocate(const TypeContextDescriptor *description,
                              const void * const *arguments) {
      // Find a pattern.  Currently we always use the default pattern.
      auto &generics = description->getFullGenericContextHeader();
      auto pattern = generics.DefaultInstantiationPattern.get();

      // Call the pattern's instantiation function.
      auto metadata =
        pattern->InstantiationFunction(description, arguments, pattern);

      // If there's no completion function, do a quick-and-dirty check to
      // see if all of the type arguments are already complete.  If they
      // are, we can broadcast completion immediately and potentially avoid
      // some extra locking.
      PrivateMetadataState state;
      if (pattern->CompletionFunction.isNull()) {
        if (areAllTransitiveMetadataComplete_cheap(metadata)) {
          state = PrivateMetadataState::Complete;
        } else {
          state = PrivateMetadataState::NonTransitiveComplete;
        }
      } else {
        state = inferStateForMetadata(metadata);
      }

      return { metadata, state };
    }

    TryInitializeResult tryInitialize(Metadata *metadata,
                                      PrivateMetadataState state,
                               PrivateMetadataCompletionContext *context) {
      assert(state != PrivateMetadataState::Complete);

      // Finish the completion function.
      if (state < PrivateMetadataState::NonTransitiveComplete) {
        // Find a pattern.  Currently we always use the default pattern.
        auto &generics = metadata->getTypeContextDescriptor()
                                 ->getFullGenericContextHeader();
        auto pattern = generics.DefaultInstantiationPattern.get();

        // Complete the metadata's instantiation.
        auto dependency =
          pattern->CompletionFunction(metadata, &context->Public, pattern);

        // If this failed with a dependency, infer the current metadata state
        // and return.
        if (dependency) {
          return { inferStateForMetadata(metadata), dependency };
        }
      }

      // Check for transitive completeness.
      if (auto dependency = checkTransitiveCompleteness(metadata)) {
        return { PrivateMetadataState::NonTransitiveComplete, dependency };
      }

      // We're done.
      return { PrivateMetadataState::Complete, MetadataDependency() };
    }
  };
} // end anonymous namespace

namespace {
  class GenericMetadataCache : public MetadataCache<GenericCacheEntry, false> {
  public:
    uint16_t NumKeyParameters;
    uint16_t NumWitnessTables;

    GenericMetadataCache(const TargetGenericContext<InProcess> &genericContext)
        : NumKeyParameters(0), NumWitnessTables(0) {
      // Count up the # of key parameters and # of witness tables.

      // Find key generic parameters.
      for (const auto &gp : genericContext.getGenericParams()) {
        if (gp.hasKeyArgument())
          ++NumKeyParameters;
      }

      // Find witness tables.
      for (const auto &req : genericContext.getGenericRequirements()) {
        if (req.Flags.hasKeyArgument() &&
            req.getKind() == GenericRequirementKind::Protocol)
          ++NumWitnessTables;
      }
    }
  };

  using LazyGenericMetadataCache = Lazy<GenericMetadataCache>;
}

/// Fetch the metadata cache for a generic metadata structure.
static GenericMetadataCache &getCache(
                               const TypeContextDescriptor &description) {
  auto &generics = description.getFullGenericContextHeader();

  // Keep this assert even if you change the representation above.
  static_assert(sizeof(LazyGenericMetadataCache) <=
                sizeof(GenericMetadataInstantiationCache::PrivateData),
                "metadata cache is larger than the allowed space");

  auto lazyCache =
    reinterpret_cast<LazyGenericMetadataCache*>(
      generics.getInstantiationCache()->PrivateData);
  return lazyCache->getWithInit(*description.getGenericContext());
}

/// Fetch the metadata cache for a generic metadata structure,
/// in a context where it must have already been initialized.
static GenericMetadataCache &unsafeGetInitializedCache(
    const TypeGenericContextDescriptorHeader &generics) {
  // Keep this assert even if you change the representation above.
  static_assert(sizeof(LazyGenericMetadataCache) <=
                sizeof(GenericMetadataInstantiationCache::PrivateData),
                "metadata cache is larger than the allowed space");

  auto lazyCache =
    reinterpret_cast<LazyGenericMetadataCache*>(
      generics.getInstantiationCache()->PrivateData);
  return lazyCache->unsafeGetAlreadyInitialized();
}

#if SWIFT_OBJC_INTEROP
extern "C" void *_objc_empty_cache;
#endif

static void copyMetadataPattern(void **section,
                                const GenericMetadataPartialPattern *pattern) {
  memcpy(section + pattern->OffsetInWords,
         pattern->Pattern.get(),
         size_t(pattern->SizeInWords) * sizeof(void*));
}

static void
initializeClassMetadataFromPattern(ClassMetadata *metadata,
                                   ClassMetadataBounds bounds,
                                   const ClassDescriptor *description,
                                   const GenericClassMetadataPattern *pattern) {
  auto fullMetadata = asFullMetadata(metadata);
  char *rawMetadata = reinterpret_cast<char*>(metadata);

  // Install the extra-data pattern.
  void **metadataExtraData =
    reinterpret_cast<void**>(rawMetadata) + bounds.PositiveSizeInWords;
  if (pattern->hasExtraDataPattern()) {
    auto extraDataPattern = pattern->getExtraDataPattern();

    // Zero memory up to the offset.
    // [pre-5.2-extra-data-zeroing] Before Swift 5.2, the runtime did not
    // correctly zero the zero-prefix of the extra-data pattern.
    memset(metadataExtraData, 0,
           size_t(extraDataPattern->OffsetInWords) * sizeof(void *));

    // Copy the pattern into the rest of the extra data.
    copyMetadataPattern(metadataExtraData, extraDataPattern);
  }

  // Install the immediate members pattern:
  void **immediateMembers =
    reinterpret_cast<void**>(rawMetadata + bounds.ImmediateMembersOffset);

  // Zero out the entire immediate-members section.
  // TODO: only memset the parts that aren't covered by the pattern.
  memset(immediateMembers, 0, description->getImmediateMembersSize());

  // Copy in the immediate arguments.

  // Copy the immediate-members pattern.
  if (pattern->hasImmediateMembersPattern()) {
    auto immediateMembersPattern = pattern->getImmediateMembersPattern();
    copyMetadataPattern(immediateMembers, immediateMembersPattern);
  }

  // Initialize the header:

  // Heap destructor.
  fullMetadata->destroy = pattern->Destroy.get();

  // Value witness table.
#if SWIFT_OBJC_INTEROP
  fullMetadata->ValueWitnesses =
    (pattern->Flags & ClassFlags::UsesSwiftRefcounting)
       ? &VALUE_WITNESS_SYM(Bo)
       : &VALUE_WITNESS_SYM(BO);
#else
  fullMetadata->ValueWitnesses = &VALUE_WITNESS_SYM(Bo);
#endif

#if SWIFT_OBJC_INTEROP
  // Install the metaclass's RO-data pointer.
  auto metaclass = reinterpret_cast<AnyClassMetadata *>(
      metadataExtraData + pattern->MetaclassObjectOffset);
  auto metaclassRO = metadataExtraData + pattern->MetaclassRODataOffset;
  metaclass->Data = reinterpret_cast<uintptr_t>(metaclassRO);
#endif

  // MetadataKind / isa.
#if SWIFT_OBJC_INTEROP
  metadata->setClassISA(metaclass);
#else
  metadata->setKind(MetadataKind::Class);
#endif

  // Superclass.
  metadata->Superclass = nullptr;

#if SWIFT_OBJC_INTEROP
  // Cache data.  Install the same initializer that the compiler is
  // required to use.  We don't need to do this in non-ObjC-interop modes.
  metadata->CacheData[0] = &_objc_empty_cache;
  metadata->CacheData[1] = nullptr;
#endif

  // RO-data pointer.
#if SWIFT_OBJC_INTEROP
  auto classRO = metadataExtraData + pattern->ClassRODataOffset;
  metadata->Data =
    reinterpret_cast<uintptr_t>(classRO) | SWIFT_CLASS_IS_SWIFT_MASK;
#else
  metadata->Data = SWIFT_CLASS_IS_SWIFT_MASK;
#endif

  // Class flags.
  metadata->Flags = pattern->Flags;

  // Instance layout.
  metadata->InstanceAddressPoint = 0;
  metadata->InstanceSize = 0;
  metadata->InstanceAlignMask = 0;

  // Reserved.
  metadata->Reserved = 0;

  // Class metadata layout.
  metadata->ClassSize = bounds.getTotalSizeInBytes();
  metadata->ClassAddressPoint = bounds.getAddressPointInBytes();

  // Class descriptor.
  metadata->setDescription(description);

  // I-var destroyer.
  metadata->IVarDestroyer = pattern->IVarDestroyer;
}

ClassMetadata *
swift::swift_allocateGenericClassMetadata(const ClassDescriptor *description,
                                          const void *arguments,
                                    const GenericClassMetadataPattern *pattern){
  auto &generics = description->getFullGenericContextHeader();
  auto &cache = unsafeGetInitializedCache(generics);

  // Compute the formal bounds of the metadata.
  auto bounds = description->getMetadataBounds();

  // Augment that with any required extra data from the pattern.
  auto allocationBounds = bounds;
  if (pattern->hasExtraDataPattern()) {
    auto extraDataPattern = pattern->getExtraDataPattern();
    allocationBounds.PositiveSizeInWords +=
      extraDataPattern->OffsetInWords + extraDataPattern->SizeInWords;
  }

  auto bytes = (char*) 
    cache.getAllocator().Allocate(allocationBounds.getTotalSizeInBytes(),
                                  alignof(void*));

  auto addressPoint = bytes + allocationBounds.getAddressPointInBytes();
  auto metadata = reinterpret_cast<ClassMetadata *>(addressPoint);

  initializeClassMetadataFromPattern(metadata, bounds, description, pattern);

  assert(metadata->isTypeMetadata());

  // Copy the generic arguments into place.
  installGenericArguments(metadata, description, arguments);

  return metadata;
}

static void
initializeValueMetadataFromPattern(ValueMetadata *metadata,
                                   const ValueTypeDescriptor *description,
                                   const GenericValueMetadataPattern *pattern) {
  auto fullMetadata = asFullMetadata(metadata);
  char *rawMetadata = reinterpret_cast<char*>(metadata);

  if (pattern->hasExtraDataPattern()) {
    void **metadataExtraData =
      reinterpret_cast<void**>(rawMetadata + sizeof(ValueMetadata));
    auto extraDataPattern = pattern->getExtraDataPattern();

    // Zero memory up to the offset.
    memset(metadataExtraData, 0, size_t(extraDataPattern->OffsetInWords));

    // Copy the pattern into the rest of the extra data.
    copyMetadataPattern(metadataExtraData, extraDataPattern);
  }

  // Put the VWT pattern in place as if it was the real VWT.
  // The various initialization functions will instantiate this as
  // necessary.
  fullMetadata->setValueWitnesses(pattern->getValueWitnessesPattern());

  // Set the metadata kind.
  metadata->setKind(pattern->getMetadataKind());

  // Set the type descriptor.
  metadata->Description = description;
}

ValueMetadata *
swift::swift_allocateGenericValueMetadata(const ValueTypeDescriptor *description,
                                          const void *arguments,
                                    const GenericValueMetadataPattern *pattern,
                                          size_t extraDataSize) {
  auto &generics = description->getFullGenericContextHeader();
  auto &cache = unsafeGetInitializedCache(generics);

  static_assert(sizeof(StructMetadata::HeaderType)
                  == sizeof(ValueMetadata::HeaderType),
                "struct metadata header unexpectedly has extra members");
  static_assert(sizeof(StructMetadata) == sizeof(ValueMetadata),
                "struct metadata unexpectedly has extra members");
  static_assert(sizeof(EnumMetadata::HeaderType)
                  == sizeof(ValueMetadata::HeaderType),
                "enum metadata header unexpectedly has extra members");
  static_assert(sizeof(EnumMetadata) == sizeof(ValueMetadata),
                "enum metadata unexpectedly has extra members");

  size_t totalSize = sizeof(FullMetadata<ValueMetadata>) + extraDataSize;

  auto bytes = (char*) cache.getAllocator().Allocate(totalSize, alignof(void*));

  auto addressPoint = bytes + sizeof(ValueMetadata::HeaderType);
  auto metadata = reinterpret_cast<ValueMetadata *>(addressPoint);

  initializeValueMetadataFromPattern(metadata, description, pattern);

  // Copy the generic arguments into place.
  installGenericArguments(metadata, description, arguments);

  return metadata;
}

/// The primary entrypoint.
MetadataResponse
swift::swift_getGenericMetadata(MetadataRequest request,
                                const void * const *arguments,
                                const TypeContextDescriptor *description) {
  auto &cache = getCache(*description);
  assert(description->getFullGenericContextHeader().Base.NumKeyArguments ==
         cache.NumKeyParameters + cache.NumWitnessTables);
  auto key = MetadataCacheKey(cache.NumKeyParameters, cache.NumWitnessTables,
                              arguments);
  auto result = cache.getOrInsert(key, request, description, arguments);

  return result.second;
}

/***************************************************************************/
/*** In-place metadata initialization **************************************/
/***************************************************************************/

namespace {
  /// A cache entry for "in-place" metadata initializations.
  class SingletonMetadataCacheEntry final
      : public MetadataCacheEntryBase<SingletonMetadataCacheEntry, int> {
    ValueType Value = nullptr;

    friend MetadataCacheEntryBase;
    ValueType getValue() {
      return Value;
    }
    void setValue(ValueType value) {
      Value = value;
    }

  public:
    // We have to give MetadataCacheEntryBase a non-empty list of trailing
    // objects or else it gets annoyed.
    static size_t numTrailingObjects(OverloadToken<int>) { return 0; }

    static const char *getName() { return "SingletonMetadataCache"; }

    SingletonMetadataCacheEntry() {}

    AllocationResult allocate(const TypeContextDescriptor *description) {
      auto &initialization = description->getSingletonMetadataInitialization();

      // Classes with resilient superclasses might require their metadata to
      // be relocated.
      auto metadata = initialization.allocate(description);

      auto state = inferStateForMetadata(metadata);
      return { metadata, state };
    }

    TryInitializeResult tryInitialize(Metadata *metadata,
                                      PrivateMetadataState state,
                               PrivateMetadataCompletionContext *context) {
      assert(state != PrivateMetadataState::Complete);

      // Finish the completion function.
      if (state < PrivateMetadataState::NonTransitiveComplete) {
        // Find a pattern.  Currently we always use the default pattern.
        auto &initialization =
            metadata->getTypeContextDescriptor()
                    ->getSingletonMetadataInitialization();

        // Complete the metadata's instantiation.
        auto dependency =
          initialization.CompletionFunction(metadata, &context->Public,
                                            /*pattern*/ nullptr);

        // If this failed with a dependency, infer the current metadata state
        // and return.
        if (dependency) {
          return { inferStateForMetadata(metadata), dependency };
        }
      }

      // Check for transitive completeness.
      if (auto dependency = checkTransitiveCompleteness(metadata)) {
        return { PrivateMetadataState::NonTransitiveComplete, dependency };
      }

      // We're done.
      publishCompleteMetadata(metadata);
      return { PrivateMetadataState::Complete, MetadataDependency() };
    }

    void publishCompleteMetadata(Metadata *metadata) {
      auto &init = metadata->getTypeContextDescriptor()
                           ->getSingletonMetadataInitialization();
      auto &cache = *init.InitializationCache.get();
      cache.Metadata.store(metadata, std::memory_order_release);
    }
  };

  /// An implementation of LockingConcurrentMapStorage that's more
  /// appropriate for the in-place metadata cache.
  ///
  /// TODO: delete the cache entry when initialization is complete.
  class SingletonMetadataCacheStorage {
    ConcurrencyControl Concurrency;

  public:
    using KeyType = const TypeContextDescriptor *;
    using EntryType = SingletonMetadataCacheEntry;

    ConcurrencyControl &getConcurrency() { return Concurrency; }

    template <class... ArgTys>
    std::pair<EntryType*, bool>
    getOrInsert(KeyType key, ArgTys &&...args) {
      auto &init = key->getSingletonMetadataInitialization();
      auto &cache = *init.InitializationCache.get();

      // Check for an existing entry.
      auto existingEntry = cache.Private.load(std::memory_order_acquire);

      // If there isn't one there, optimistically create an entry and
      // try to swap it in.
      if (!existingEntry) {
        auto allocatedEntry = new SingletonMetadataCacheEntry();
        if (cache.Private.compare_exchange_strong(existingEntry,
                                                  allocatedEntry,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
          // If that succeeded, return the entry we allocated and tell the
          // caller we allocated it.
          return { allocatedEntry, true };
        }

        // Otherwise, use the new entry and destroy the one we allocated.
        assert(existingEntry && "spurious failure of strong compare-exchange?");
        delete allocatedEntry;
      }

      return { static_cast<SingletonMetadataCacheEntry*>(existingEntry), false };
    }

    EntryType *find(KeyType key) {
      auto &init = key->getSingletonMetadataInitialization();

      return static_cast<SingletonMetadataCacheEntry*>(
        init.InitializationCache->Private.load(std::memory_order_acquire));
    }

    /// A default implementation for resolveEntry that assumes that the
    /// key type is a lookup key for the map.
    EntryType *resolveExistingEntry(KeyType key) {
      auto entry = find(key);
      assert(entry && "entry doesn't already exist!");
      return entry;
    }
  };

  class SingletonTypeMetadataCache
      : public LockingConcurrentMap<SingletonMetadataCacheEntry,
                                    SingletonMetadataCacheStorage> {
  };
} // end anonymous namespace

/// The cache of all in-place metadata initializations.
static Lazy<SingletonTypeMetadataCache> SingletonMetadata;

MetadataResponse
swift::swift_getSingletonMetadata(MetadataRequest request,
                                  const TypeContextDescriptor *description) {
  auto result = SingletonMetadata.get().getOrInsert(description, request,
                                                    description);

  return result.second;
}

/***************************************************************************/
/*** Objective-C class wrappers ********************************************/
/***************************************************************************/

#if SWIFT_OBJC_INTEROP

namespace {
  class ObjCClassCacheEntry {
  public:
    FullMetadata<ObjCClassWrapperMetadata> Data;

    ObjCClassCacheEntry(const ClassMetadata *theClass) {
      Data.setKind(MetadataKind::ObjCClassWrapper);
      Data.ValueWitnesses = &VALUE_WITNESS_SYM(BO);
      Data.Class = theClass;
    }

    intptr_t getKeyIntValueForDump() {
      return reinterpret_cast<intptr_t>(Data.Class);
    }

    int compareWithKey(const ClassMetadata *theClass) const {
      return comparePointers(theClass, Data.Class);
    }

    static size_t getExtraAllocationSize(const ClassMetadata *key) {
      return 0;
    }
    size_t getExtraAllocationSize() const {
      return 0;
    }
  };
}

/// The uniquing structure for ObjC class-wrapper metadata.
static SimpleGlobalCache<ObjCClassCacheEntry> ObjCClassWrappers;

const Metadata *
swift::swift_getObjCClassMetadata(const ClassMetadata *theClass) {
  // Make calls resilient against receiving a null Objective-C class. This can
  // happen when classes are weakly linked and not available.
  if (theClass == nullptr)
    return nullptr;

  // If the class pointer is valid as metadata, no translation is required.
  if (theClass->isTypeMetadata()) {
    return theClass;
  }

  return &ObjCClassWrappers.getOrInsert(theClass).first->Data;
}

const ClassMetadata *
swift::swift_getObjCClassFromMetadata(const Metadata *theMetadata) {
  // Unwrap ObjC class wrappers.
  if (auto wrapper = dyn_cast<ObjCClassWrapperMetadata>(theMetadata)) {
    return wrapper->Class;
  }

  // Otherwise, the input should already be a Swift class object.
  auto theClass = cast<ClassMetadata>(theMetadata);
  assert(theClass->isTypeMetadata());
  return theClass;
}

const ClassMetadata *
swift::swift_getObjCClassFromMetadataConditional(const Metadata *theMetadata) {
  // If it's an ordinary class, return it.
  if (auto theClass = dyn_cast<ClassMetadata>(theMetadata)) {
    return theClass;
  }

  // Unwrap ObjC class wrappers.
  if (auto wrapper = dyn_cast<ObjCClassWrapperMetadata>(theMetadata)) {
    return wrapper->Class;
  }

  // Not an ObjC class after all.
  return nil;
}

#endif

/***************************************************************************/
/*** Functions *************************************************************/
/***************************************************************************/

namespace {

class FunctionCacheEntry {
public:
  FullMetadata<FunctionTypeMetadata> Data;

  struct Key {
    const FunctionTypeFlags Flags;

    const Metadata *const *Parameters;
    const uint32_t *ParameterFlags;
    const Metadata *Result;

    FunctionTypeFlags getFlags() const { return Flags; }
    const Metadata *getParameter(unsigned index) const {
      assert(index < Flags.getNumParameters());
      return Parameters[index];
    }
    const Metadata *getResult() const { return Result; }

    const uint32_t *getParameterFlags() const {
      return ParameterFlags;
    }

    ::ParameterFlags getParameterFlags(unsigned index) const {
      assert(index < Flags.getNumParameters());
      auto flags = Flags.hasParameterFlags() ? ParameterFlags[index] : 0;
      return ParameterFlags::fromIntValue(flags);
    }
  };

  FunctionCacheEntry(const Key &key);

  intptr_t getKeyIntValueForDump() {
    return 0; // No single meaningful value here.
  }

  int compareWithKey(const Key &key) const {
    auto keyFlags = key.getFlags();
    if (auto result = compareIntegers(keyFlags.getIntValue(),
                                      Data.Flags.getIntValue()))
      return result;

    if (auto result = comparePointers(key.getResult(), Data.ResultType))
      return result;

    for (unsigned i = 0, e = keyFlags.getNumParameters(); i != e; ++i) {
      if (auto result =
              comparePointers(key.getParameter(i), Data.getParameter(i)))
        return result;

      if (auto result =
              compareIntegers(key.getParameterFlags(i).getIntValue(),
                              Data.getParameterFlags(i).getIntValue()))
        return result;
    }

    return 0;
  }
  static size_t getExtraAllocationSize(const Key &key) {
    return getExtraAllocationSize(key.Flags);
  }

  size_t getExtraAllocationSize() const {
    return getExtraAllocationSize(Data.Flags);
  }

  static size_t getExtraAllocationSize(const FunctionTypeFlags &flags) {
    const auto numParams = flags.getNumParameters();
    auto size = numParams * sizeof(FunctionTypeMetadata::Parameter);
    if (flags.hasParameterFlags())
      size += numParams * sizeof(uint32_t);
    return roundUpToAlignment(size, sizeof(void *));
  }
};

} // end anonymous namespace

/// The uniquing structure for function type metadata.
static SimpleGlobalCache<FunctionCacheEntry> FunctionTypes;

const FunctionTypeMetadata *
swift::swift_getFunctionTypeMetadata0(FunctionTypeFlags flags,
                                      const Metadata *result) {
  assert(flags.getNumParameters() == 0
         && "wrong number of arguments in function metadata flags?!");
  return swift_getFunctionTypeMetadata(flags, nullptr, nullptr, result);
}

const FunctionTypeMetadata *
swift::swift_getFunctionTypeMetadata1(FunctionTypeFlags flags,
                                      const Metadata *arg0,
                                      const Metadata *result) {
  assert(flags.getNumParameters() == 1
         && "wrong number of arguments in function metadata flags?!");
  const Metadata *parameters[] = { arg0 };
  return swift_getFunctionTypeMetadata(flags, parameters, nullptr, result);
}

const FunctionTypeMetadata *
swift::swift_getFunctionTypeMetadata2(FunctionTypeFlags flags,
                                      const Metadata *arg0,
                                      const Metadata *arg1,
                                      const Metadata *result) {
  assert(flags.getNumParameters() == 2
         && "wrong number of arguments in function metadata flags?!");
  const Metadata *parameters[] = { arg0, arg1 };
  return swift_getFunctionTypeMetadata(flags, parameters, nullptr, result);
}

const FunctionTypeMetadata *
swift::swift_getFunctionTypeMetadata3(FunctionTypeFlags flags,
                                      const Metadata *arg0,
                                      const Metadata *arg1,
                                      const Metadata *arg2,
                                      const Metadata *result) {
  assert(flags.getNumParameters() == 3
         && "wrong number of arguments in function metadata flags?!");
  const Metadata *parameters[] = { arg0, arg1, arg2 };
  return swift_getFunctionTypeMetadata(flags, parameters, nullptr, result);
}

const FunctionTypeMetadata *
swift::swift_getFunctionTypeMetadata(FunctionTypeFlags flags,
                                     const Metadata *const *parameters,
                                     const uint32_t *parameterFlags,
                                     const Metadata *result) {
  FunctionCacheEntry::Key key = { flags, parameters, parameterFlags, result };
  return &FunctionTypes.getOrInsert(key).first->Data;
}

FunctionCacheEntry::FunctionCacheEntry(const Key &key) {
  auto flags = key.getFlags();

  // Pick a value witness table appropriate to the function convention.
  // All function types of a given convention have the same value semantics,
  // so they share a value witness table.
  switch (flags.getConvention()) {
  case FunctionMetadataConvention::Swift:
    if (!flags.isEscaping()) {
      Data.ValueWitnesses = &VALUE_WITNESS_SYM(NOESCAPE_FUNCTION_MANGLING);
    } else {
      Data.ValueWitnesses = &VALUE_WITNESS_SYM(FUNCTION_MANGLING);
    }
    break;

  case FunctionMetadataConvention::Thin:
  case FunctionMetadataConvention::CFunctionPointer:
    Data.ValueWitnesses = &VALUE_WITNESS_SYM(THIN_FUNCTION_MANGLING);
    break;

  case FunctionMetadataConvention::Block:
#if SWIFT_OBJC_INTEROP
    // Blocks are ObjC objects, so can share the AnyObject value
    // witnesses (stored as "BO" rather than "yXl" for ABI compat).
    Data.ValueWitnesses = &VALUE_WITNESS_SYM(BO);
#else
    assert(false && "objc block without objc interop?");
#endif
    break;
  }

  unsigned numParameters = flags.getNumParameters();

  Data.setKind(MetadataKind::Function);
  Data.Flags = flags;
  Data.ResultType = key.getResult();

  for (unsigned i = 0; i < numParameters; ++i) {
    Data.getParameters()[i] = key.getParameter(i);
    if (flags.hasParameterFlags())
      Data.getParameterFlags()[i] = key.getParameterFlags(i).getIntValue();
  }
}

/***************************************************************************/
/*** Tuples ****************************************************************/
/***************************************************************************/

namespace {

class TupleCacheEntry
    : public MetadataCacheEntryBase<TupleCacheEntry,
                                    TupleTypeMetadata::Element> {
public:
  static const char *getName() { return "TupleCache"; }

  // NOTE: if you change the layout of this type, you'll also need
  // to update tuple_getValueWitnesses().
  unsigned ExtraInhabitantProvidingElement;
  ValueWitnessTable Witnesses;
  FullMetadata<TupleTypeMetadata> Data;

  struct Key {
    size_t NumElements;
    const Metadata * const *Elements;
    const char *Labels;
  };

  ValueType getValue() {
    return &Data;
  }
  void setValue(ValueType value) {
    assert(value == &Data);
  }

  TupleCacheEntry(const Key &key, MetadataRequest request,
                  const ValueWitnessTable *proposedWitnesses);

  AllocationResult allocate(const ValueWitnessTable *proposedWitnesses);

  TryInitializeResult tryInitialize(Metadata *metadata,
                                    PrivateMetadataState state,
                                    PrivateMetadataCompletionContext *context);

  TryInitializeResult checkTransitiveCompleteness() {
    auto dependency = ::checkTransitiveCompleteness(&Data);
    return { dependency ? PrivateMetadataState::NonTransitiveComplete
                        : PrivateMetadataState::Complete,
             dependency };
  }

  size_t getNumElements() const {
    return Data.NumElements;
  }

  intptr_t getKeyIntValueForDump() {
    return 0; // No single meaningful value
  }

  int compareWithKey(const Key &key) const {
    // Order by the cheaper comparisons first:

    // The number of elements.
    if (auto result = compareIntegers(key.NumElements,(size_t)Data.NumElements))
      return result;

    // The element types.
    for (size_t i = 0, e = key.NumElements; i != e; ++i) {
      if (auto result = comparePointers(key.Elements[i],
                                        Data.getElement(i).Type))
        return result;
    }

    // It's unlikely that we'll get pointer-equality here unless we're being
    // called from the same module or both label strings are null, but
    // those are important cases.
    if (key.Labels != Data.Labels) {
      // Order no-labels before labels.
      if (!key.Labels) return -1;
      if (!Data.Labels) return 1;

      // Just do a strcmp.
      if (auto result = strcmp(key.Labels, Data.Labels))
        return result;
    }

    return 0;
  }

  size_t numTrailingObjects(OverloadToken<TupleTypeMetadata::Element>) const {
    return getNumElements();
  }

  template <class... Args>
  static size_t numTrailingObjects(OverloadToken<TupleTypeMetadata::Element>,
                                   const Key &key,
                                   Args &&...extraArgs) {
    return key.NumElements;
  }
};

class TupleCacheStorage :
  public LockingConcurrentMapStorage<TupleCacheEntry, false> {
public:
// FIXME: https://bugs.swift.org/browse/SR-1155
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
  static TupleCacheEntry *
  resolveExistingEntry(const TupleTypeMetadata *metadata) {
    // The correctness of this arithmetic is verified by an assertion in
    // the TupleCacheEntry constructor.
    auto bytes = reinterpret_cast<const char*>(asFullMetadata(metadata));
    bytes -= offsetof(TupleCacheEntry, Data);
    auto entry = reinterpret_cast<const TupleCacheEntry*>(bytes);
    return const_cast<TupleCacheEntry*>(entry);
  }
#pragma clang diagnostic pop
};

class TupleCache :
  public LockingConcurrentMap<TupleCacheEntry, TupleCacheStorage> {
};

} // end anonymous namespace

/// The uniquing structure for tuple type metadata.
static Lazy<TupleCache> TupleTypes;

/// Given a metatype pointer, produce the value-witness table for it.
/// This is equivalent to metatype->ValueWitnesses but more efficient.
static const ValueWitnessTable *tuple_getValueWitnesses(const Metadata *metatype) {
  return ((const ValueWitnessTable*) asFullMetadata(metatype)) - 1;
}

/// Generic tuple value witness for 'projectBuffer'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_projectBuffer(ValueBuffer *buffer,
                                        const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsInline)
    return reinterpret_cast<OpaqueValue*>(buffer);

  auto wtable = tuple_getValueWitnesses(metatype);
  unsigned alignMask = wtable->getAlignmentMask();
  // Compute the byte offset of the object in the box.
  unsigned byteOffset = (sizeof(HeapObject) + alignMask) & ~alignMask;
  auto *bytePtr =
      reinterpret_cast<char *>(*reinterpret_cast<HeapObject **>(buffer));
  return reinterpret_cast<OpaqueValue *>(bytePtr + byteOffset);
}

/// Generic tuple value witness for 'allocateBuffer'
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_allocateBuffer(ValueBuffer *buffer,
                                         const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsInline)
    return reinterpret_cast<OpaqueValue*>(buffer);
  BoxPair refAndValueAddr(swift_allocBox(metatype));
  *reinterpret_cast<HeapObject **>(buffer) = refAndValueAddr.object;
  return refAndValueAddr.buffer;
}

/// Generic tuple value witness for 'destroy'.
template <bool IsPOD, bool IsInline>
static void tuple_destroy(OpaqueValue *tuple, const Metadata *_metadata) {
  auto &metadata = *(const TupleTypeMetadata*) _metadata;
  assert(IsPOD == tuple_getValueWitnesses(&metadata)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(&metadata)->isValueInline());

  if (IsPOD) return;

  for (size_t i = 0, e = metadata.NumElements; i != e; ++i) {
    auto &eltInfo = metadata.getElements()[i];
    OpaqueValue *elt = eltInfo.findIn(tuple);
    auto eltWitnesses = eltInfo.Type->getValueWitnesses();
    eltWitnesses->destroy(elt, eltInfo.Type);
  }
}

// The operation doesn't have to be initializeWithCopy, but they all
// have basically the same type.
typedef ValueWitnessTypes::initializeWithCopy forEachOperation;

/// Perform an operation for each field of two tuples.
static OpaqueValue *tuple_forEachField(OpaqueValue *destTuple,
                                       OpaqueValue *srcTuple,
                                       const Metadata *_metatype,
                                       forEachOperation operation) {
  auto &metatype = *(const TupleTypeMetadata*) _metatype;
  for (size_t i = 0, e = metatype.NumElements; i != e; ++i) {
    auto &eltInfo = metatype.getElement(i);
    OpaqueValue *destElt = eltInfo.findIn(destTuple);
    OpaqueValue *srcElt = eltInfo.findIn(srcTuple);
    operation(destElt, srcElt, eltInfo.Type);
  }

  return destTuple;
}

/// Perform a naive memcpy of src into dest.
static OpaqueValue *tuple_memcpy(OpaqueValue *dest,
                                 OpaqueValue *src,
                                 const Metadata *metatype) {
  assert(metatype->getValueWitnesses()->isPOD());
  return (OpaqueValue*)
    memcpy(dest, src, metatype->getValueWitnesses()->getSize());
}

/// Generic tuple value witness for 'initializeWithCopy'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_initializeWithCopy(OpaqueValue *dest,
                                             OpaqueValue *src,
                                             const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsPOD) return tuple_memcpy(dest, src, metatype);
  return tuple_forEachField(dest, src, metatype,
      [](OpaqueValue *dest, OpaqueValue *src, const Metadata *eltType) {
    return eltType->vw_initializeWithCopy(dest, src);
  });
}

/// Generic tuple value witness for 'initializeWithTake'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_initializeWithTake(OpaqueValue *dest,
                                             OpaqueValue *src,
                                             const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsPOD) return tuple_memcpy(dest, src, metatype);
  return tuple_forEachField(dest, src, metatype,
      [](OpaqueValue *dest, OpaqueValue *src, const Metadata *eltType) {
    return eltType->vw_initializeWithTake(dest, src);
  });
}

/// Generic tuple value witness for 'assignWithCopy'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_assignWithCopy(OpaqueValue *dest,
                                         OpaqueValue *src,
                                         const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());

  if (IsPOD) return tuple_memcpy(dest, src, metatype);
  return tuple_forEachField(dest, src, metatype,
      [](OpaqueValue *dest, OpaqueValue *src, const Metadata *eltType) {
    return eltType->vw_assignWithCopy(dest, src);
  });
}

/// Generic tuple value witness for 'assignWithTake'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_assignWithTake(OpaqueValue *dest,
                                         OpaqueValue *src,
                                         const Metadata *metatype) {
  if (IsPOD) return tuple_memcpy(dest, src, metatype);
  return tuple_forEachField(dest, src, metatype,
      [](OpaqueValue *dest, OpaqueValue *src, const Metadata *eltType) {
    return eltType->vw_assignWithTake(dest, src);
  });
}

/// Generic tuple value witness for 'initializeBufferWithCopyOfBuffer'.
template <bool IsPOD, bool IsInline>
static OpaqueValue *tuple_initializeBufferWithCopyOfBuffer(ValueBuffer *dest,
                                                           ValueBuffer *src,
                                                     const Metadata *metatype) {
  assert(IsPOD == tuple_getValueWitnesses(metatype)->isPOD());
  assert(IsInline == tuple_getValueWitnesses(metatype)->isValueInline());
  if (IsInline) {
    return tuple_initializeWithCopy<IsPOD, IsInline>(
        tuple_projectBuffer<IsPOD, IsInline>(dest, metatype),
        tuple_projectBuffer<IsPOD, IsInline>(src, metatype), metatype);
  }

  auto *srcReference = *reinterpret_cast<HeapObject**>(src);
  *reinterpret_cast<HeapObject**>(dest) = srcReference;
  swift_retain(srcReference);
  return tuple_projectBuffer<IsPOD, IsInline>(dest, metatype);
}

SWIFT_CC
static void tuple_storeExtraInhabitantTag(OpaqueValue *tuple,
                                          unsigned tag,
                                          unsigned xiCount,
                                          const Metadata *_metatype) {
  auto &metatype = *(const TupleTypeMetadata*) _metatype;
  auto cacheEntry = TupleCacheStorage::resolveExistingEntry(&metatype);
  auto &eltInfo =
    metatype.getElement(cacheEntry->ExtraInhabitantProvidingElement);
  assert(xiCount == eltInfo.Type->vw_getNumExtraInhabitants());

  auto *elt = (OpaqueValue*)((uintptr_t)tuple + eltInfo.Offset);

  assert(tag >= 1);
  assert(tag <= xiCount);
  eltInfo.Type->vw_storeEnumTagSinglePayload(elt, tag, xiCount);
}

SWIFT_CC
static unsigned tuple_getExtraInhabitantTag(const OpaqueValue *tuple,
                                            unsigned xiCount,
                                            const Metadata *_metatype) {
  auto &metatype = *(const TupleTypeMetadata*) _metatype;

  auto cacheEntry = TupleCacheStorage::resolveExistingEntry(&metatype);
  auto &eltInfo =
    metatype.getElement(cacheEntry->ExtraInhabitantProvidingElement);
  assert(xiCount == eltInfo.Type->vw_getNumExtraInhabitants());

  auto *elt = (const OpaqueValue*)((uintptr_t)tuple + eltInfo.Offset);
  return eltInfo.Type->vw_getEnumTagSinglePayload(elt, xiCount);
}

template <bool IsPOD, bool IsInline>
static unsigned tuple_getEnumTagSinglePayload(const OpaqueValue *enumAddr,
                                              unsigned numEmptyCases,
                                              const Metadata *self) {
  
  auto *witnesses = tuple_getValueWitnesses(self);
  auto size = witnesses->getSize();
  auto numExtraInhabitants = witnesses->getNumExtraInhabitants();
  auto getExtraInhabitantTag = tuple_getExtraInhabitantTag;

  return getEnumTagSinglePayloadImpl(enumAddr, numEmptyCases, self, size,
                                     numExtraInhabitants,
                                     getExtraInhabitantTag);
}

template <bool IsPOD, bool IsInline>
static void
tuple_storeEnumTagSinglePayload(OpaqueValue *enumAddr, unsigned whichCase,
                                unsigned numEmptyCases, const Metadata *self) {
  auto *witnesses = tuple_getValueWitnesses(self);
  auto size = witnesses->getSize();
  auto numExtraInhabitants = witnesses->getNumExtraInhabitants();
  auto storeExtraInhabitantTag = tuple_storeExtraInhabitantTag;

  storeEnumTagSinglePayloadImpl(enumAddr, whichCase, numEmptyCases, self, size,
                                numExtraInhabitants, storeExtraInhabitantTag);
}

/// Various standard witness table for tuples.
static const ValueWitnessTable tuple_witnesses_pod_inline = {
#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define VALUE_WITNESS(LOWER_ID, UPPER_ID) &tuple_##LOWER_ID<true, true>,
#define DATA_VALUE_WITNESS(LOWER_ID, UPPER_ID, TYPE)
#include "swift/ABI/ValueWitness.def"
  0,
  0,
  ValueWitnessFlags(),
  0
};
static const ValueWitnessTable tuple_witnesses_nonpod_inline = {
#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define VALUE_WITNESS(LOWER_ID, UPPER_ID) &tuple_##LOWER_ID<false, true>,
#define DATA_VALUE_WITNESS(LOWER_ID, UPPER_ID, TYPE)
#include "swift/ABI/ValueWitness.def"
  0,
  0,
  ValueWitnessFlags(),
  0
};
static const ValueWitnessTable tuple_witnesses_pod_noninline = {
#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define VALUE_WITNESS(LOWER_ID, UPPER_ID) &tuple_##LOWER_ID<true, false>,
#define DATA_VALUE_WITNESS(LOWER_ID, UPPER_ID, TYPE)
#include "swift/ABI/ValueWitness.def"
  0,
  0,
  ValueWitnessFlags(),
  0
};
static const ValueWitnessTable tuple_witnesses_nonpod_noninline = {
#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define VALUE_WITNESS(LOWER_ID, UPPER_ID) &tuple_##LOWER_ID<false, false>,
#define DATA_VALUE_WITNESS(LOWER_ID, UPPER_ID, TYPE)
#include "swift/ABI/ValueWitness.def"
  0,
  0,
  ValueWitnessFlags(),
  0
};

static constexpr TypeLayout getInitialLayoutForValueType() {
  return {0, 0, ValueWitnessFlags().withAlignment(1).withPOD(true), 0};
}

static constexpr TypeLayout getInitialLayoutForHeapObject() {
  return {sizeof(HeapObject),
          sizeof(HeapObject),
          ValueWitnessFlags().withAlignment(alignof(HeapObject)),
          0};
}

static size_t roundUpToAlignMask(size_t size, size_t alignMask) {
  return (size + alignMask) & ~alignMask;
}

/// Perform basic sequential layout given a vector of metadata pointers,
/// calling a functor with the offset of each field, and returning the
/// final layout characteristics of the type.
///
/// GetLayoutFn should have signature:
///   const TypeLayout *(ElementType &type);
///
/// SetOffsetFn should have signature:
///   void (size_t index, ElementType &type, size_t offset)
template<typename ElementType, typename GetLayoutFn, typename SetOffsetFn>
static void performBasicLayout(TypeLayout &layout,
                               ElementType *elements,
                               size_t numElements,
                               GetLayoutFn &&getLayout,
                               SetOffsetFn &&setOffset) {
  size_t size = layout.size;
  size_t alignMask = layout.flags.getAlignmentMask();
  bool isPOD = layout.flags.isPOD();
  bool isBitwiseTakable = layout.flags.isBitwiseTakable();
  for (unsigned i = 0; i != numElements; ++i) {
    auto &elt = elements[i];

    // Lay out this element.
    const TypeLayout *eltLayout = getLayout(elt);
    size = roundUpToAlignMask(size, eltLayout->flags.getAlignmentMask());

    // Report this record to the functor.
    setOffset(i, elt, size);

    // Update the size and alignment of the aggregate..
    size += eltLayout->size;
    alignMask = std::max(alignMask, eltLayout->flags.getAlignmentMask());
    if (!eltLayout->flags.isPOD()) isPOD = false;
    if (!eltLayout->flags.isBitwiseTakable()) isBitwiseTakable = false;
  }
  bool isInline =
      ValueWitnessTable::isValueInline(isBitwiseTakable, size, alignMask + 1);

  layout.size = size;
  layout.flags = ValueWitnessFlags()
                     .withAlignmentMask(alignMask)
                     .withPOD(isPOD)
                     .withBitwiseTakable(isBitwiseTakable)
                     .withInlineStorage(isInline);
  layout.extraInhabitantCount = 0;
  layout.stride = std::max(size_t(1), roundUpToAlignMask(size, alignMask));
}


size_t swift::swift_getTupleTypeLayout2(TypeLayout *result,
                                        const TypeLayout *elt0,
                                        const TypeLayout *elt1) {
  const TypeLayout *elts[] = { elt0, elt1 };
  uint32_t offsets[2];
  swift_getTupleTypeLayout(result, offsets,
                           TupleTypeFlags().withNumElements(2), elts);
  assert(offsets[0] == 0);
  return offsets[1];
}

OffsetPair swift::swift_getTupleTypeLayout3(TypeLayout *result,
                                            const TypeLayout *elt0,
                                            const TypeLayout *elt1,
                                            const TypeLayout *elt2) {
  const TypeLayout *elts[] = { elt0, elt1, elt2 };
  uint32_t offsets[3];
  swift_getTupleTypeLayout(result, offsets,
                           TupleTypeFlags().withNumElements(3), elts);
  assert(offsets[0] == 0);
  return {offsets[1], offsets[2]};
}

void swift::swift_getTupleTypeLayout(TypeLayout *result,
                                     uint32_t *elementOffsets,
                                     TupleTypeFlags flags,
                                     const TypeLayout * const *elements) {
  *result = TypeLayout();
  unsigned numExtraInhabitants = 0;
  performBasicLayout(*result, elements, flags.getNumElements(),
    [](const TypeLayout *elt) { return elt; },
    [elementOffsets, &numExtraInhabitants]
    (size_t i, const TypeLayout *elt, size_t offset) {
      if (elementOffsets)
        elementOffsets[i] = uint32_t(offset);
      numExtraInhabitants = std::max(numExtraInhabitants,
                                     elt->getNumExtraInhabitants());
    });
  
  if (numExtraInhabitants > 0) {
    *result = TypeLayout(result->size,
                         result->stride,
                         result->flags,
                         numExtraInhabitants);
  }
}

MetadataResponse
swift::swift_getTupleTypeMetadata(MetadataRequest request,
                                  TupleTypeFlags flags,
                                  const Metadata * const *elements,
                                  const char *labels,
                                  const ValueWitnessTable *proposedWitnesses) {
  auto numElements = flags.getNumElements();

  // Bypass the cache for the empty tuple. We might reasonably get called
  // by generic code, like a demangler that produces type objects.
  if (numElements == 0)
    return { &METADATA_SYM(EMPTY_TUPLE_MANGLING), MetadataState::Complete };

  // Search the cache.
  TupleCacheEntry::Key key = { numElements, elements, labels };

  auto &cache = TupleTypes.get();

  // If we have constant labels, directly check the cache.
  if (!flags.hasNonConstantLabels())
    return cache.getOrInsert(key, request, proposedWitnesses).second;

  // If we have non-constant labels, we can't simply record the result.
  // Look for an existing result, first.
  if (auto response = cache.tryAwaitExisting(key, request))
    return *response;

  // Allocate a copy of the labels string within the tuple type allocator.
  size_t labelsLen = strlen(labels);
  size_t labelsAllocSize = roundUpToAlignment(labelsLen + 1, sizeof(void*));
  char *newLabels =
    (char *) cache.getAllocator().Allocate(labelsAllocSize, alignof(char));
  strcpy(newLabels, labels);
  key.Labels = newLabels;

  // Update the metadata cache.
  auto result = cache.getOrInsert(key, request, proposedWitnesses).second;

  // If we didn't manage to perform the insertion, free the memory associated
  // with the copy of the labels: nobody else can reference it.
  if (cast<TupleTypeMetadata>(result.Value)->Labels != newLabels) {
    cache.getAllocator().Deallocate(newLabels, labelsAllocSize);
  }

  // Done.
  return result;
}

TupleCacheEntry::TupleCacheEntry(const Key &key, MetadataRequest request,
                                 const ValueWitnessTable *proposedWitnesses) {
  Data.setKind(MetadataKind::Tuple);
  Data.NumElements = key.NumElements;
  Data.Labels = key.Labels;

  // Stash the proposed witnesses in the value-witnesses slot for now.
  Data.ValueWitnesses = proposedWitnesses;

  for (size_t i = 0, e = key.NumElements; i != e; ++i)
    Data.getElement(i).Type = key.Elements[i];

  assert(TupleCacheStorage::resolveExistingEntry(&Data) == this);
}

TupleCacheEntry::AllocationResult
TupleCacheEntry::allocate(const ValueWitnessTable *proposedWitnesses) {
  // All the work we can reasonably do here was done in the constructor.
  return { &Data, PrivateMetadataState::Abstract };
}

TupleCacheEntry::TryInitializeResult
TupleCacheEntry::tryInitialize(Metadata *metadata,
                               PrivateMetadataState state,
                               PrivateMetadataCompletionContext *context) {
  // If we've already reached non-transitive completeness, just check that.
  if (state == PrivateMetadataState::NonTransitiveComplete)
    return checkTransitiveCompleteness();

  // Otherwise, we must still be abstract, because tuples don't have an
  // intermediate state between that and non-transitive completeness.
  assert(state == PrivateMetadataState::Abstract);

  bool allElementsTransitivelyComplete = true;
  const Metadata *knownIncompleteElement = nullptr;

  // Require all of the elements to be layout-complete.
  for (size_t i = 0, e = Data.NumElements; i != e; ++i) {
    auto request = MetadataRequest(MetadataState::LayoutComplete,
                                   /*non-blocking*/ true);
    auto eltType = Data.getElement(i).Type;
    auto response = swift_checkMetadataState(request, eltType);

    // Immediately continue in the most common scenario, which is that
    // the element is transitively complete.
    if (response.State == MetadataState::Complete)
      continue;

    // If the metadata is not layout-complete, we have to suspend.
    if (!isAtLeast(response.State, MetadataState::LayoutComplete))
      return { PrivateMetadataState::Abstract,
               MetadataDependency(eltType, MetadataState::LayoutComplete) };

    // Remember that there's a non-fully-complete element.
    allElementsTransitivelyComplete = false;

    // Remember the first element that's not even non-transitively complete.
    if (!knownIncompleteElement &&
        !isAtLeast(response.State, MetadataState::NonTransitiveComplete))
      knownIncompleteElement = eltType;
  }

  // Okay, we're going to succeed now.

  // Reload the proposed witness from where we stashed them.
  auto proposedWitnesses = Data.ValueWitnesses;

  // Set the real value-witness table.
  Data.ValueWitnesses = &Witnesses;

  // Perform basic layout on the tuple.
  auto layout = getInitialLayoutForValueType();
  performBasicLayout(layout, Data.getElements(), Data.NumElements,
    [](const TupleTypeMetadata::Element &elt) {
      return elt.getTypeLayout();
    },
    [](size_t i, TupleTypeMetadata::Element &elt, size_t offset) {
      elt.Offset = offset;
    });

  Witnesses.size = layout.size;
  Witnesses.flags = layout.flags;
  Witnesses.stride = layout.stride;

  // We have extra inhabitants if any element does.
  // Pick the element with the most, favoring the earliest element in a tie.
  unsigned extraInhabitantProvidingElement = ~0u;
  unsigned numExtraInhabitants = 0;
  for (unsigned i = 0, e = Data.NumElements; i < e; ++i) {
    unsigned eltEI = Data.getElement(i).Type->getValueWitnesses()
                                            ->getNumExtraInhabitants();
    if (eltEI > numExtraInhabitants) {
      extraInhabitantProvidingElement = i;
      numExtraInhabitants = eltEI;
    }
  }
  Witnesses.extraInhabitantCount = numExtraInhabitants;
  if (numExtraInhabitants > 0) {
    ExtraInhabitantProvidingElement = extraInhabitantProvidingElement;
  }

  // Copy the function witnesses in, either from the proposed
  // witnesses or from the standard table.
  if (!proposedWitnesses) {
    // Try to pattern-match into something better than the generic witnesses.
    if (layout.flags.isInlineStorage() && layout.flags.isPOD()) {
      if (numExtraInhabitants == 0
          && layout.size == 8
          && layout.flags.getAlignmentMask() == 7)
        proposedWitnesses = &VALUE_WITNESS_SYM(Bi64_);
      else if (numExtraInhabitants == 0
               && layout.size == 4
               && layout.flags.getAlignmentMask() == 3)
        proposedWitnesses = &VALUE_WITNESS_SYM(Bi32_);
      else if (numExtraInhabitants == 0
               && layout.size == 2
               && layout.flags.getAlignmentMask() == 1)
        proposedWitnesses = &VALUE_WITNESS_SYM(Bi16_);
      else if (numExtraInhabitants == 0 && layout.size == 1)
        proposedWitnesses = &VALUE_WITNESS_SYM(Bi8_);
      else
        proposedWitnesses = &tuple_witnesses_pod_inline;
    } else if (layout.flags.isInlineStorage()
               && !layout.flags.isPOD()) {
      proposedWitnesses = &tuple_witnesses_nonpod_inline;
    } else if (!layout.flags.isInlineStorage()
               && layout.flags.isPOD()) {
      proposedWitnesses = &tuple_witnesses_pod_noninline;
    } else {
      assert(!layout.flags.isInlineStorage()
             && !layout.flags.isPOD());
      proposedWitnesses = &tuple_witnesses_nonpod_noninline;
    }
  }
#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define VALUE_WITNESS(LOWER_ID, UPPER_ID) \
  Witnesses.LOWER_ID = proposedWitnesses->LOWER_ID;
#define DATA_VALUE_WITNESS(LOWER_ID, UPPER_ID, TYPE)
#include "swift/ABI/ValueWitness.def"

  // Okay, we're all done with layout and setting up the elements.
  // Check transitive completeness.

  // We don't need to check the element statuses again in a couple of cases:

  // - If all the elements are transitively complete, we are, too.
  if (allElementsTransitivelyComplete)
    return { PrivateMetadataState::Complete, MetadataDependency() };

  // - If there was an incomplete element, wait for it to be become
  //   at least non-transitively complete.
  if (knownIncompleteElement)
    return { PrivateMetadataState::NonTransitiveComplete,
             MetadataDependency(knownIncompleteElement,
                                MetadataState::NonTransitiveComplete) };

  // Otherwise, we need to do a more expensive check.
  return checkTransitiveCompleteness();
}

MetadataResponse
swift::swift_getTupleTypeMetadata2(MetadataRequest request,
                                   const Metadata *elt0, const Metadata *elt1,
                                   const char *labels,
                                   const ValueWitnessTable *proposedWitnesses) {
  const Metadata *elts[] = { elt0, elt1 };
  return swift_getTupleTypeMetadata(request,
                                    TupleTypeFlags().withNumElements(2),
                                    elts, labels, proposedWitnesses);
}

MetadataResponse
swift::swift_getTupleTypeMetadata3(MetadataRequest request,
                                   const Metadata *elt0, const Metadata *elt1,
                                   const Metadata *elt2,
                                   const char *labels,
                                   const ValueWitnessTable *proposedWitnesses) {
  const Metadata *elts[] = { elt0, elt1, elt2 };
  return swift_getTupleTypeMetadata(request,
                                    TupleTypeFlags().withNumElements(3),
                                    elts, labels, proposedWitnesses);
}

/***************************************************************************/
/*** Nominal type descriptors **********************************************/
/***************************************************************************/

namespace {
  /// A class encapsulating everything interesting about the identity of
  /// a type context *except* the identity of the parent context.
  class TypeContextIdentity {
    StringRef Name;
  public:
    explicit TypeContextIdentity(const TypeContextDescriptor *type) {
      Name = ParsedTypeIdentity::parse(type).FullIdentity;
    }

    bool operator==(const TypeContextIdentity &other) const {
      return Name == other.Name;
    }
    int compare(const TypeContextIdentity &other) const {
      return Name.compare(other.Name);
    }
  };
}

bool swift::equalContexts(const ContextDescriptor *a,
                          const ContextDescriptor *b)
{
  // Fast path: pointer equality.
  if (a == b) return true;

  // If either context is null, we're done.
  if (a == nullptr || b == nullptr)
    return false;

  // If either descriptor is known to be unique, we're done.
  if (a->isUnique() || b->isUnique()) return false;
  
  // Do the kinds match?
  if (a->getKind() != b->getKind()) return false;
  
  // Do the parents match?
  if (!equalContexts(a->Parent.get(), b->Parent.get()))
    return false;
  
  // Compare kind-specific details.
  switch (auto kind = a->getKind()) {
  case ContextDescriptorKind::Module: {
    // Modules with the same name are equivalent.
    auto moduleA = cast<ModuleContextDescriptor>(a);
    auto moduleB = cast<ModuleContextDescriptor>(b);
    return strcmp(moduleA->Name.get(), moduleB->Name.get()) == 0;
  }
  
  case ContextDescriptorKind::Extension:
  case ContextDescriptorKind::Anonymous:
    // These context kinds are always unique.
    return false;
  
  default:
    // Types in the same context with the same name are equivalent.
    if (kind >= ContextDescriptorKind::Type_First
        && kind <= ContextDescriptorKind::Type_Last) {
      auto typeA = cast<TypeContextDescriptor>(a);
      auto typeB = cast<TypeContextDescriptor>(b);
      return TypeContextIdentity(typeA) == TypeContextIdentity(typeB);
    }
    
    // Otherwise, this runtime doesn't know anything about this context kind.
    // Conservatively return false.
    return false;
  }
}

/***************************************************************************/
/*** Common value witnesses ************************************************/
/***************************************************************************/

// Value witness methods for an arbitrary trivial type.
// The buffer operations assume that the value is stored indirectly, because
// installCommonValueWitnesses will install the direct equivalents instead.

namespace {
  template<typename T>
  struct pointer_function_cast_impl;
  
  template<typename OutRet, typename...OutArgs>
  struct pointer_function_cast_impl<OutRet * (*)(OutArgs *...)> {
    template<typename InRet, typename...InArgs>
    static constexpr auto perform(InRet * (*function)(InArgs *...))
      -> OutRet * (*)(OutArgs *...)
    {
      static_assert(sizeof...(InArgs) == sizeof...(OutArgs),
                    "cast changed number of arguments");
      return (OutRet *(*)(OutArgs *...))function;
    }
  };

  template<typename...OutArgs>
  struct pointer_function_cast_impl<void (*)(OutArgs *...)> {
    template<typename...InArgs>
    static constexpr auto perform(void (*function)(InArgs *...))
      -> void (*)(OutArgs *...)
    {
      static_assert(sizeof...(InArgs) == sizeof...(OutArgs),
                    "cast changed number of arguments");
      return (void (*)(OutArgs *...))function;
    }
  };
} // end anonymous namespace

/// Cast a function that takes all pointer arguments and returns to a
/// function type that takes different pointer arguments and returns.
/// In any reasonable calling convention the input and output function types
/// should be ABI-compatible.
template<typename Out, typename In>
static constexpr Out pointer_function_cast(In *function) {
  return pointer_function_cast_impl<Out>::perform(function);
}

static OpaqueValue *pod_indirect_initializeBufferWithCopyOfBuffer(
                    ValueBuffer *dest, ValueBuffer *src, const Metadata *self) {
  auto wtable = self->getValueWitnesses();
  auto *srcReference = *reinterpret_cast<HeapObject**>(src);
  *reinterpret_cast<HeapObject**>(dest) = srcReference;
  swift_retain(srcReference);

  // Project the address of the value in the buffer.
  unsigned alignMask = wtable->getAlignmentMask();
  // Compute the byte offset of the object in the box.
  unsigned byteOffset = (sizeof(HeapObject) + alignMask) & ~alignMask;
  auto *bytePtr = reinterpret_cast<char *>(srcReference);
  return reinterpret_cast<OpaqueValue *>(bytePtr + byteOffset);
}

static void pod_destroy(OpaqueValue *object, const Metadata *self) {}

static OpaqueValue *pod_copy(OpaqueValue *dest, OpaqueValue *src,
                             const Metadata *self) {
  memcpy(dest, src, self->getValueWitnesses()->size);
  return dest;
}

static OpaqueValue *pod_direct_initializeBufferWithCopyOfBuffer(
                    ValueBuffer *dest, ValueBuffer *src, const Metadata *self) {
  return pod_copy(reinterpret_cast<OpaqueValue*>(dest),
                  reinterpret_cast<OpaqueValue*>(src),
                  self);
}

static constexpr uint64_t sizeWithAlignmentMask(uint64_t size,
                                                uint64_t alignmentMask,
                                                uint64_t hasExtraInhabitants) {
  return (hasExtraInhabitants << 48) | (size << 16) | alignmentMask;
}

void swift::installCommonValueWitnesses(const TypeLayout &layout,
                                        ValueWitnessTable *vwtable) {
  auto flags = layout.flags;
  if (flags.isPOD()) {
    // Use POD value witnesses.
    // If the value has a common size and alignment, use specialized value
    // witnesses we already have lying around for the builtin types.
    const ValueWitnessTable *commonVWT;
    bool hasExtraInhabitants = layout.hasExtraInhabitants();
    switch (sizeWithAlignmentMask(layout.size, flags.getAlignmentMask(),
                                  hasExtraInhabitants)) {
    default:
      // For uncommon layouts, use value witnesses that work with an arbitrary
      // size and alignment.
      if (flags.isInlineStorage()) {
        vwtable->initializeBufferWithCopyOfBuffer =
          pod_direct_initializeBufferWithCopyOfBuffer;
      } else {
        vwtable->initializeBufferWithCopyOfBuffer =
          pod_indirect_initializeBufferWithCopyOfBuffer;
      }
      vwtable->destroy = pod_destroy;
      vwtable->initializeWithCopy = pod_copy;
      vwtable->initializeWithTake = pod_copy;
      vwtable->assignWithCopy = pod_copy;
      vwtable->assignWithTake = pod_copy;
      // getEnumTagSinglePayload and storeEnumTagSinglePayload are not
      // interestingly optimizable based on POD-ness.
      return;
      
    case sizeWithAlignmentMask(1, 0, 0):
      commonVWT = &VALUE_WITNESS_SYM(Bi8_);
      break;
    case sizeWithAlignmentMask(2, 1, 0):
      commonVWT = &VALUE_WITNESS_SYM(Bi16_);
      break;
    case sizeWithAlignmentMask(4, 3, 0):
      commonVWT = &VALUE_WITNESS_SYM(Bi32_);
      break;
    case sizeWithAlignmentMask(8, 7, 0):
      commonVWT = &VALUE_WITNESS_SYM(Bi64_);
      break;
    case sizeWithAlignmentMask(16, 15, 0):
      commonVWT = &VALUE_WITNESS_SYM(Bi128_);
      break;
    case sizeWithAlignmentMask(32, 31, 0):
      commonVWT = &VALUE_WITNESS_SYM(Bi256_);
      break;
    case sizeWithAlignmentMask(64, 63, 0):
      commonVWT = &VALUE_WITNESS_SYM(Bi512_);
      break;
    }

#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define VALUE_WITNESS(LOWER_ID, UPPER_ID) \
    vwtable->LOWER_ID = commonVWT->LOWER_ID;
#define DATA_VALUE_WITNESS(LOWER_ID, UPPER_ID, TYPE)
#include "swift/ABI/ValueWitness.def"
    
    return;
  }
  
  if (flags.isBitwiseTakable()) {
    // Use POD value witnesses for operations that do an initializeWithTake.
    vwtable->initializeWithTake = pod_copy;
    return;
  }

}

/***************************************************************************/
/*** Structs ***************************************************************/
/***************************************************************************/

static ValueWitnessTable *getMutableVWTableForInit(StructMetadata *self,
                                                   StructLayoutFlags flags) {
  auto oldTable = self->getValueWitnesses();

  // If we can alter the existing table in-place, do so.
  if (isValueWitnessTableMutable(flags))
    return const_cast<ValueWitnessTable*>(oldTable);

  // Otherwise, allocate permanent memory for it and copy the existing table.
  void *memory = allocateMetadata(sizeof(ValueWitnessTable),
                                  alignof(ValueWitnessTable));
  auto newTable = new (memory) ValueWitnessTable(*oldTable);

  // If we ever need to check layout-completeness asynchronously from
  // initialization, we'll need this to be a store-release (and rely on
  // consume ordering on the asynchronous check path); and we'll need to
  // ensure that the current state says that the type is incomplete.
  self->setValueWitnesses(newTable);

  return newTable;
}

/// Initialize the value witness table and struct field offset vector for a
/// struct.
void swift::swift_initStructMetadata(StructMetadata *structType,
                                     StructLayoutFlags layoutFlags,
                                     size_t numFields,
                                     const TypeLayout *const *fieldTypes,
                                     uint32_t *fieldOffsets) {
  auto layout = getInitialLayoutForValueType();
  performBasicLayout(layout, fieldTypes, numFields,
    [&](const TypeLayout *fieldType) { return fieldType; },
    [&](size_t i, const TypeLayout *fieldType, uint32_t offset) {
      assignUnlessEqual(fieldOffsets[i], offset);
    });

  // We have extra inhabitants if any element does. Use the field with the most.
  unsigned extraInhabitantCount = 0;
  for (unsigned i = 0; i < numFields; ++i) {
    unsigned fieldExtraInhabitantCount =
      fieldTypes[i]->getNumExtraInhabitants();
    if (fieldExtraInhabitantCount > extraInhabitantCount) {
      extraInhabitantCount = fieldExtraInhabitantCount;
    }
  }

  auto vwtable = getMutableVWTableForInit(structType, layoutFlags);

  layout.extraInhabitantCount = extraInhabitantCount;

  // Substitute in better value witnesses if we have them.
  installCommonValueWitnesses(layout, vwtable);

  vwtable->publishLayout(layout);
}

/***************************************************************************/
/*** Classes ***************************************************************/
/***************************************************************************/

static MetadataAllocator &getResilientMetadataAllocator() {
  // This should be constant-initialized, but this is safe.
  static MetadataAllocator allocator;
  return allocator;
}

ClassMetadata *
swift::swift_relocateClassMetadata(const ClassDescriptor *description,
                                   const ResilientClassMetadataPattern *pattern) {
  auto bounds = description->getMetadataBounds();

  auto metadata = reinterpret_cast<ClassMetadata *>(
      (char*) getResilientMetadataAllocator().Allocate(
        bounds.getTotalSizeInBytes(), sizeof(void*)) +
        bounds.getAddressPointInBytes());

  auto fullMetadata = asFullMetadata(metadata);
  char *rawMetadata = reinterpret_cast<char*>(metadata);

  // Zero out the entire immediate-members section.
  void **immediateMembers =
    reinterpret_cast<void**>(rawMetadata + bounds.ImmediateMembersOffset);
  memset(immediateMembers, 0, description->getImmediateMembersSize());

  // Initialize the header:

  // Heap destructor.
  fullMetadata->destroy = pattern->Destroy.get();

  // Value witness table.
#if SWIFT_OBJC_INTEROP
  fullMetadata->ValueWitnesses =
    (pattern->Flags & ClassFlags::UsesSwiftRefcounting)
       ? &VALUE_WITNESS_SYM(Bo)
       : &VALUE_WITNESS_SYM(BO);
#else
  fullMetadata->ValueWitnesses = &VALUE_WITNESS_SYM(Bo);
#endif

  // MetadataKind / isa.
#if SWIFT_OBJC_INTEROP
  metadata->setClassISA(pattern->Metaclass.get());
#else
  metadata->setKind(MetadataKind::Class);
#endif

  // Superclass.
  metadata->Superclass = nullptr;

#if SWIFT_OBJC_INTEROP
  // Cache data.  Install the same initializer that the compiler is
  // required to use.  We don't need to do this in non-ObjC-interop modes.
  metadata->CacheData[0] = &_objc_empty_cache;
  metadata->CacheData[1] = nullptr;
#endif

  // RO-data pointer.
#if SWIFT_OBJC_INTEROP
  auto classRO = pattern->Data.get();
  metadata->Data =
    reinterpret_cast<uintptr_t>(classRO) | SWIFT_CLASS_IS_SWIFT_MASK;
#else
  metadata->Data = SWIFT_CLASS_IS_SWIFT_MASK;
#endif

  // Class flags.
  metadata->Flags = pattern->Flags;

  // Instance layout.
  metadata->InstanceAddressPoint = 0;
  metadata->InstanceSize = 0;
  metadata->InstanceAlignMask = 0;

  // Reserved.
  metadata->Reserved = 0;

  // Class metadata layout.
  metadata->ClassSize = bounds.getTotalSizeInBytes();
  metadata->ClassAddressPoint = bounds.getAddressPointInBytes();

  // Class descriptor.
  metadata->setDescription(description);

  // I-var destroyer.
  metadata->IVarDestroyer = pattern->IVarDestroyer;

  return metadata;
}

namespace {
  /// The structure of ObjC class ivars as emitted by compilers.
  struct ClassIvarEntry {
    size_t *Offset;
    const char *Name;
    const char *Type;
    uint32_t Log2Alignment;
    uint32_t Size;
  };

  /// The structure of ObjC class ivar lists as emitted by compilers.
  struct ClassIvarList {
    uint32_t EntrySize;
    uint32_t Count;

    ClassIvarEntry *getIvars() {
      return reinterpret_cast<ClassIvarEntry*>(this+1);
    }
    const ClassIvarEntry *getIvars() const {
      return reinterpret_cast<const ClassIvarEntry*>(this+1);
    }
  };

  /// The structure of ObjC class rodata as emitted by compilers.
  struct ClassROData {
    uint32_t Flags;
    uint32_t InstanceStart;
    uint32_t InstanceSize;
#if __POINTER_WIDTH__ == 64
    uint32_t Reserved;
#endif
    union {
      const uint8_t *IvarLayout;
      ClassMetadata *NonMetaClass;
    };
    const char *Name;
    const void *MethodList;
    const void *ProtocolList;
    ClassIvarList *IvarList;
    const uint8_t *WeakIvarLayout;
    const void *PropertyList;
  };
} // end anonymous namespace

#if SWIFT_OBJC_INTEROP
static uint32_t getLog2AlignmentFromMask(size_t alignMask) {
  assert(((alignMask + 1) & alignMask) == 0 &&
         "not an alignment mask!");

  uint32_t log2 = 0;
  while ((1 << log2) != (alignMask + 1))
    log2++;
  return log2;
}

static inline ClassROData *getROData(ClassMetadata *theClass) {
  return (ClassROData*)(theClass->Data & ~uintptr_t(SWIFT_CLASS_IS_SWIFT_MASK));
}

static char *copyGenericClassObjCName(ClassMetadata *theClass) {
  // Use the remangler to generate a mangled name from the type metadata.
  Demangle::StackAllocatedDemangler<4096> Dem;

  auto demangling = _swift_buildDemanglingForMetadata(theClass, Dem);

  // Remangle that into a new type mangling string.
  auto typeNode = Dem.createNode(Demangle::Node::Kind::TypeMangling);
  typeNode->addChild(demangling, Dem);
  auto globalNode = Dem.createNode(Demangle::Node::Kind::Global);
  globalNode->addChild(typeNode, Dem);

  llvm::StringRef string = Demangle::mangleNodeOld(globalNode, Dem);

  // If the class is in the Swift module, add a $ to the end of the ObjC
  // name. The old and new Swift libraries must be able to coexist in
  // the same process, and this avoids warnings due to the ObjC names
  // colliding.
  bool addSuffix = string.startswith("_TtGCs");

  size_t allocationSize = string.size() + 1;
  if (addSuffix)
    allocationSize += 1;
  
  auto fullNameBuf = (char*)swift_slowAlloc(allocationSize, 0);
  memcpy(fullNameBuf, string.data(), string.size());

  if (addSuffix) {
    fullNameBuf[string.size()] = '$';
    fullNameBuf[string.size() + 1] = '\0';
  } else {
    fullNameBuf[string.size()] = '\0';
  }
  return fullNameBuf;
}

static void initGenericClassObjCName(ClassMetadata *theClass) {
  auto theMetaclass = (ClassMetadata *)object_getClass((id)theClass);

  char *name = copyGenericClassObjCName(theClass);
  getROData(theClass)->Name = name;
  getROData(theMetaclass)->Name = name;
}

static bool installLazyClassNameHook() {
#if !OBJC_SETHOOK_LAZYCLASSNAMER_DEFINED
  using objc_hook_lazyClassNamer =
    const char * _Nullable (*)(_Nonnull Class cls);
  auto objc_setHook_lazyClassNamer =
    (void (*)(objc_hook_lazyClassNamer, objc_hook_lazyClassNamer *))
    dlsym(RTLD_NEXT, "objc_setHook_lazyClassNamer");  
#endif

  static objc_hook_lazyClassNamer oldHook;
  auto myHook = [](Class theClass) -> const char * {
    ClassMetadata *metadata = (ClassMetadata *)theClass;
    if (metadata->isTypeMetadata())
      return copyGenericClassObjCName(metadata);
    return oldHook(theClass);
  };

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"
  if (objc_setHook_lazyClassNamer == nullptr)
    return false;
  objc_setHook_lazyClassNamer(myHook, &oldHook);
#pragma clang diagnostic pop

  return true;
}

static void setUpGenericClassObjCName(ClassMetadata *theClass) {
  bool supportsLazyNames = SWIFT_LAZY_CONSTANT(installLazyClassNameHook());
  if (supportsLazyNames) {
    getROData(theClass)->Name = nullptr;
    auto theMetaclass = (ClassMetadata *)object_getClass((id)theClass);
    getROData(theMetaclass)->Name = nullptr;
    getROData(theMetaclass)->NonMetaClass = theClass;
  } else {
    initGenericClassObjCName(theClass);
  }
}
#endif

/// Initialize the invariant superclass components of a class metadata,
/// such as the generic type arguments, field offsets, and so on.
static void copySuperclassMetadataToSubclass(ClassMetadata *theClass,
                                             ClassLayoutFlags layoutFlags) {
  const ClassMetadata *theSuperclass = theClass->Superclass;
  if (theSuperclass == nullptr)
    return;

  // If any ancestor classes have generic parameters, field offset vectors
  // or virtual methods, inherit them.
  //
  // Note that the caller is responsible for installing overrides of
  // superclass methods; here we just copy them verbatim.
  auto ancestor = theSuperclass;
  auto *classWords = reinterpret_cast<uintptr_t *>(theClass);
  auto *superWords = reinterpret_cast<const uintptr_t *>(theSuperclass);
  while (ancestor && ancestor->isTypeMetadata()) {
    const auto *description = ancestor->getDescription();

    // Copy the generic requirements.
    if (description->isGeneric()
        && description->getGenericContextHeader().hasArguments()) {
      auto genericOffset = description->getGenericArgumentOffset();
      memcpy(classWords + genericOffset,
             superWords + genericOffset,
             description->getGenericContextHeader().getNumArguments() *
               sizeof(uintptr_t));
    }

    // Copy the vtable entries.
    if (description->hasVTable() && !hasStaticVTable(layoutFlags)) {
      auto *vtable = description->getVTableDescriptor();
      auto vtableOffset = vtable->getVTableOffset(description);
      memcpy(classWords + vtableOffset,
             superWords + vtableOffset,
             vtable->VTableSize * sizeof(uintptr_t));
    }

    // Copy the field offsets.
    if (description->hasFieldOffsetVector()) {
      unsigned fieldOffsetVector =
        description->getFieldOffsetVectorOffset();
      memcpy(classWords + fieldOffsetVector,
             superWords + fieldOffsetVector,
             description->NumFields * sizeof(uintptr_t));
    }
    ancestor = ancestor->Superclass;
  }

#if SWIFT_OBJC_INTEROP
  if (theClass->getDescription()->isGeneric() ||
      (theSuperclass->isTypeMetadata() &&
       theSuperclass->getDescription()->isGeneric())) {
    // Set up the superclass of the metaclass, which is the metaclass of the
    // superclass.
    auto theMetaclass = (ClassMetadata *)object_getClass((id)theClass);
    auto theSuperMetaclass
      = (const ClassMetadata *)object_getClass(id_const_cast(theSuperclass));
    theMetaclass->Superclass = theSuperMetaclass;
  }
#endif
}

/// Using the information in the class context descriptor, fill in in the
/// immediate vtable entries for the class and install overrides of any
/// superclass vtable entries.
static void initClassVTable(ClassMetadata *self) {
  const auto *description = self->getDescription();
  auto *classWords = reinterpret_cast<void **>(self);

  if (description->hasVTable()) {
    auto *vtable = description->getVTableDescriptor();
    auto vtableOffset = vtable->getVTableOffset(description);
    for (unsigned i = 0, e = vtable->VTableSize; i < e; ++i)
      classWords[vtableOffset + i] = description->getMethod(i);
  }

  if (description->hasOverrideTable()) {
    auto *overrideTable = description->getOverrideTable();
    auto overrideDescriptors = description->getMethodOverrideDescriptors();

    for (unsigned i = 0, e = overrideTable->NumEntries; i < e; ++i) {
      auto &descriptor = overrideDescriptors[i];

      // Get the base class and method.
      auto *baseClass = cast_or_null<ClassDescriptor>(descriptor.Class.get());
      auto *baseMethod = descriptor.Method.get();

      // If the base method is null, it's an unavailable weak-linked
      // symbol.
      if (baseClass == nullptr || baseMethod == nullptr)
        continue;

      // Calculate the base method's vtable offset from the
      // base method descriptor. The offset will be relative
      // to the base class's vtable start offset.
      auto baseClassMethods = baseClass->getMethodDescriptors();

      // If the method descriptor doesn't land within the bounds of the
      // method table, abort.
      if (baseMethod < baseClassMethods.begin() ||
          baseMethod >= baseClassMethods.end()) {
        fatalError(0, "resilient vtable at %p contains out-of-bounds "
                   "method descriptor %p\n",
                   overrideTable, baseMethod);
      }

      // Install the method override in our vtable.
      auto baseVTable = baseClass->getVTableDescriptor();
      auto offset = baseMethod - baseClassMethods.data();
      classWords[baseVTable->getVTableOffset(baseClass) + offset]
        = descriptor.Impl.get();
    }
  }
}

static void initClassFieldOffsetVector(ClassMetadata *self,
                                       size_t numFields,
                                       const TypeLayout * const *fieldTypes,
                                       size_t *fieldOffsets) {
  // Start layout by appending to a standard heap object header.
  size_t size, alignMask;

#if SWIFT_OBJC_INTEROP
  ClassROData *rodata = getROData(self);
#endif

  // If we have a superclass, start from its size and alignment instead.
  if (classHasSuperclass(self)) {
    auto *super = self->Superclass;

    // This is straightforward if the superclass is Swift.
#if SWIFT_OBJC_INTEROP
    if (super->isTypeMetadata()) {
#endif
      size = super->getInstanceSize();
      alignMask = super->getInstanceAlignMask();

#if SWIFT_OBJC_INTEROP
    // If it's Objective-C, start layout from our static notion of
    // where the superclass starts.  Objective-C expects us to have
    // generated a correct ivar layout, which it will simply slide if
    // it needs to.
    } else {
      size = rodata->InstanceStart;
      alignMask = 0xF; // malloc alignment guarantee
    }
#endif

  // If we don't have a formal superclass, start with the basic heap header.
  } else {
    auto heapLayout = getInitialLayoutForHeapObject();
    size = heapLayout.size;
    alignMask = heapLayout.flags.getAlignmentMask();
  }

#if SWIFT_OBJC_INTEROP
  // Ensure that Objective-C does layout starting from the right
  // offset.  This needs to exactly match the superclass rodata's
  // InstanceSize in cases where the compiler decided that we didn't
  // really have a resilient ObjC superclass, because the compiler
  // might hardcode offsets in that case, so we can't slide ivars.
  // Fortunately, the cases where that happens are exactly the
  // situations where our entire superclass hierarchy is defined
  // in Swift.  (But note that ObjC might think we have a superclass
  // even if Swift doesn't, because of SwiftObject.)
  rodata->InstanceStart = size;
#endif

  // Okay, now do layout.
  for (unsigned i = 0; i != numFields; ++i) {
    auto *eltLayout = fieldTypes[i];

    // Skip empty fields.
    if (fieldOffsets[i] == 0 && eltLayout->size == 0)
      continue;
    auto offset = roundUpToAlignMask(size,
                                     eltLayout->flags.getAlignmentMask());
    fieldOffsets[i] = offset;
    size = offset + eltLayout->size;
    alignMask = std::max(alignMask, eltLayout->flags.getAlignmentMask());
  }

  // Save the final size and alignment into the metadata record.
  assert(self->isTypeMetadata());
  self->setInstanceSize(size);
  self->setInstanceAlignMask(alignMask);

#if SWIFT_OBJC_INTEROP
  // Save the size into the Objective-C metadata as well.
  rodata->InstanceSize = size;
#endif
}

#if SWIFT_OBJC_INTEROP
/// Non-generic classes only. Initialize the Objective-C ivar descriptors and
/// field offset globals. Does *not* register the class with the Objective-C
/// runtime; that must be done by the caller.
///
/// This function copies the ivar descriptors and updates each ivar global with
/// the corresponding offset in \p fieldOffsets, before asking the Objective-C
/// runtime to realize the class. The Objective-C runtime will then slide the
/// offsets stored in those globals.
///
/// Note that \p fieldOffsets remains unchanged in this case.
static void initObjCClass(ClassMetadata *self,
                          size_t numFields,
                          const TypeLayout * const *fieldTypes,
                          size_t *fieldOffsets) {
  ClassROData *rodata = getROData(self);

  // Always clone the ivar descriptors.
  if (numFields) {
    const ClassIvarList *dependentIvars = rodata->IvarList;
    assert(dependentIvars->Count == numFields);
    assert(dependentIvars->EntrySize == sizeof(ClassIvarEntry));

    auto ivarListSize = sizeof(ClassIvarList) +
                        numFields * sizeof(ClassIvarEntry);
    auto ivars = (ClassIvarList*) getResilientMetadataAllocator()
      .Allocate(ivarListSize, alignof(ClassIvarList));
    memcpy(ivars, dependentIvars, ivarListSize);
    rodata->IvarList = ivars;

    for (unsigned i = 0; i != numFields; ++i) {
      auto *eltLayout = fieldTypes[i];

      ClassIvarEntry &ivar = ivars->getIvars()[i];

      // Fill in the field offset global, if this ivar has one.
      if (ivar.Offset) {
        if (*ivar.Offset != fieldOffsets[i])
          *ivar.Offset = fieldOffsets[i];
      }

      // If the ivar's size doesn't match the field layout we
      // computed, overwrite it and give it better type information.
      if (ivar.Size != eltLayout->size) {
        ivar.Size = eltLayout->size;
        ivar.Type = nullptr;
        ivar.Log2Alignment =
          getLog2AlignmentFromMask(eltLayout->flags.getAlignmentMask());
      }
    }
  }
}

/// Generic classes only. Initialize the Objective-C ivar descriptors and field
/// offset globals and register the class with the runtime.
///
/// This function copies the ivar descriptors and points each ivar offset at the
/// corresponding entry in \p fieldOffsets, before asking the Objective-C
/// runtime to realize the class. The Objective-C runtime will then slide the
/// offsets in \p fieldOffsets.
static MetadataDependency
initGenericObjCClass(ClassMetadata *self, size_t numFields,
                     const TypeLayout * const *fieldTypes,
                     size_t *fieldOffsets) {
  // If the class is generic, we need to give it a name for Objective-C.
  setUpGenericClassObjCName(self);

  ClassROData *rodata = getROData(self);

  // In ObjC interop mode, we have up to two places we need each correct
  // ivar offset to end up:
  //
  // - the global ivar offset in the RO-data; this should only exist
  //   if the class layout (up to this ivar) is not actually dependent
  //
  // - the field offset vector (fieldOffsets)
  //
  // When we ask the ObjC runtime to lay out this class, we need the
  // RO-data to point to the field offset vector, even if the layout
  // is not dependent.  The RO-data is not shared between
  // instantiations, but the global ivar offset is (by definition).
  // If the compiler didn't have the correct static size for the
  // superclass (i.e. if rodata->InstanceStart is wrong), a previous
  // instantiation might have already slid the global offset to the
  // correct place; we need the ObjC runtime to see a pre-slid value,
  // and it's not safe to briefly unslide it and let the runtime slide
  // it back because there might already be concurrent code relying on
  // the global ivar offset.
  //
  // So we need to the remember the addresses of the global ivar offsets.
  // We use this lazily-filled array to do so.
  const unsigned NumInlineGlobalIvarOffsets = 8;
  size_t *_inlineGlobalIvarOffsets[NumInlineGlobalIvarOffsets];
  size_t **_globalIvarOffsets = nullptr;
  auto getGlobalIvarOffsets = [&]() -> size_t** {
    if (!_globalIvarOffsets) {
      if (numFields <= NumInlineGlobalIvarOffsets) {
        _globalIvarOffsets = _inlineGlobalIvarOffsets;
      } else {
        _globalIvarOffsets = new size_t*[numFields];
      }

      // Make sure all the entries start out null.
      memset(_globalIvarOffsets, 0, sizeof(size_t*) * numFields);
    }
    return _globalIvarOffsets;
  };

  // Always clone the ivar descriptors.
  if (numFields) {
    const ClassIvarList *dependentIvars = rodata->IvarList;
    assert(dependentIvars->Count == numFields);
    assert(dependentIvars->EntrySize == sizeof(ClassIvarEntry));

    auto ivarListSize = sizeof(ClassIvarList) +
                        numFields * sizeof(ClassIvarEntry);
    auto ivars = (ClassIvarList*) getResilientMetadataAllocator()
      .Allocate(ivarListSize, alignof(ClassIvarList));
    memcpy(ivars, dependentIvars, ivarListSize);
    rodata->IvarList = ivars;

    for (unsigned i = 0; i != numFields; ++i) {
      auto *eltLayout = fieldTypes[i];

      ClassIvarEntry &ivar = ivars->getIvars()[i];

      // Remember the global ivar offset if present.
      if (ivar.Offset) {
        getGlobalIvarOffsets()[i] = ivar.Offset;
      }

      // Change the ivar offset to point to the respective entry of
      // the field-offset vector, as discussed above.
      ivar.Offset = &fieldOffsets[i];

      // If the ivar's size doesn't match the field layout we
      // computed, overwrite it and give it better type information.
      if (ivar.Size != eltLayout->size) {
        ivar.Size = eltLayout->size;
        ivar.Type = nullptr;
        ivar.Log2Alignment =
          getLog2AlignmentFromMask(eltLayout->flags.getAlignmentMask());
      }
    }
  }

  // Register this class with the runtime. This will also cause the
  // runtime to slide the entries in the field offset vector.
  swift_instantiateObjCClass(self);

  // If we saved any global ivar offsets, make sure we write back to them.
  if (_globalIvarOffsets) {
    for (unsigned i = 0; i != numFields; ++i) {
      if (!_globalIvarOffsets[i]) continue;

      // To avoid dirtying memory, only write to the global ivar
      // offset if it's actually wrong.
      if (*_globalIvarOffsets[i] != fieldOffsets[i])
        *_globalIvarOffsets[i] = fieldOffsets[i];
    }

    // Free the out-of-line if we allocated one.
    if (_globalIvarOffsets != _inlineGlobalIvarOffsets) {
      delete [] _globalIvarOffsets;
    }
  }

  return MetadataDependency();
}
#endif

SWIFT_CC
static std::pair<MetadataDependency, const ClassMetadata *>
getSuperclassMetadata(ClassMetadata *self, bool allowDependency) {
  // If there is a mangled superclass name, demangle it to the superclass
  // type.
  const ClassMetadata *super = nullptr;
  if (auto superclassNameBase = self->getDescription()->SuperclassType.get()) {
    StringRef superclassName =
      Demangle::makeSymbolicMangledNameStringRef(superclassNameBase);
    SubstGenericParametersFromMetadata substitutions(self);
    MetadataRequest request(allowDependency
                              ? MetadataState::NonTransitiveComplete
                              : /*FIXME*/ MetadataState::Abstract,
                            /*non-blocking*/ allowDependency);
    MetadataResponse response =
      swift_getTypeByMangledName(request, superclassName,
        substitutions.getGenericArgs(),
        [&substitutions](unsigned depth, unsigned index) {
          return substitutions.getMetadata(depth, index);
        },
        [&substitutions](const Metadata *type, unsigned index) {
          return substitutions.getWitnessTable(type, index);
        }).getResponse();
    auto superclass = response.Value;
    if (!superclass) {
      fatalError(0,
                 "failed to demangle superclass of %s from mangled name '%s'\n",
                 self->getDescription()->Name.get(),
                 superclassName.str().c_str());
    }

    // If the request isn't satisfied, we have a new dependency.
    if (!request.isSatisfiedBy(response.State)) {
      assert(allowDependency);
      return {MetadataDependency(superclass, request.getState()),
              cast<ClassMetadata>(superclass)};
    }

#if SWIFT_OBJC_INTEROP
    if (auto objcWrapper = dyn_cast<ObjCClassWrapperMetadata>(superclass))
      superclass = objcWrapper->Class;
#endif

    super = cast<ClassMetadata>(superclass);
  }

  return {MetadataDependency(), super};
}

// Suppress diagnostic about the availability of _objc_realizeClassFromSwift.
// We test availability with a nullptr check, but the compiler doesn't see that.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

static SWIFT_CC MetadataDependency
_swift_initClassMetadataImpl(ClassMetadata *self,
                             ClassLayoutFlags layoutFlags,
                             size_t numFields,
                             const TypeLayout * const *fieldTypes,
                             size_t *fieldOffsets,
                             bool allowDependency) {
  // Try to install the superclass.
  auto superDependencyAndSuper = getSuperclassMetadata(self, allowDependency);
  if (superDependencyAndSuper.first)
    return superDependencyAndSuper.first;
  auto super = superDependencyAndSuper.second;

  self->Superclass = super;

#if SWIFT_OBJC_INTEROP
  // Set the superclass to SwiftObject if this is a root class.
  if (!super)
    self->Superclass = getRootSuperclass();

  // Register our custom implementation of class_getImageName.
  static swift_once_t onceToken;
  swift_once(&onceToken, [](void *unused) {
    (void)unused;
    setUpObjCRuntimeGetImageNameFromClass();
  }, nullptr);

#ifndef OBJC_REALIZECLASSFROMSWIFT_DEFINED
  // Temporary workaround until _objc_realizeClassFromSwift is in the SDK.
  static auto _objc_realizeClassFromSwift =
    (Class (*)(Class _Nullable, void *_Nullable))
    dlsym(RTLD_NEXT, "_objc_realizeClassFromSwift");
#endif

  // Temporary workaround until objc_loadClassref is in the SDK.
  static auto objc_loadClassref =
    (Class (*)(void *))
    dlsym(RTLD_NEXT, "objc_loadClassref");
#endif

  // Copy field offsets, generic arguments and (if necessary) vtable entries
  // from our superclass.
  copySuperclassMetadataToSubclass(self, layoutFlags);

  // Copy the class's immediate methods from the nominal type descriptor
  // to the class metadata.
  if (!hasStaticVTable(layoutFlags))
    initClassVTable(self);

  initClassFieldOffsetVector(self, numFields, fieldTypes, fieldOffsets);

#if SWIFT_OBJC_INTEROP
  auto *description = self->getDescription();
  if (description->isGeneric()) {
    assert(!description->hasObjCResilientClassStub());
    initGenericObjCClass(self, numFields, fieldTypes, fieldOffsets);
  } else {
    initObjCClass(self, numFields, fieldTypes, fieldOffsets);

    // Register this class with the runtime. This will also cause the
    // runtime to slide the field offsets stored in the field offset
    // globals. Note that the field offset vector is *not* updated;
    // however we should not be using it for anything in a non-generic
    // class.
    auto *stub = description->getObjCResilientClassStub();

    // On a new enough runtime, register the class as a replacement for
    // its stub if we have one, which attaches any categories referencing
    // the stub.
    //
    // On older runtimes, just register the class via the usual mechanism.
    // The compiler enforces that @objc methods in extensions of classes
    // with resilient ancestry have the correct availability, so it should
    // be safe to ignore the stub in this case.
    if (stub != nullptr &&
        objc_loadClassref != nullptr &&
        _objc_realizeClassFromSwift != nullptr) {
      _objc_realizeClassFromSwift((Class) self, const_cast<void *>(stub));
    } else {
      swift_instantiateObjCClass(self);
    }
  }
#else
  assert(!self->getDescription()->hasObjCResilientClassStub());
#endif

  return MetadataDependency();
}

#pragma clang diagnostic pop

void swift::swift_initClassMetadata(ClassMetadata *self,
                                    ClassLayoutFlags layoutFlags,
                                    size_t numFields,
                                    const TypeLayout * const *fieldTypes,
                                    size_t *fieldOffsets) {
  (void) _swift_initClassMetadataImpl(self, layoutFlags, numFields,
                                      fieldTypes, fieldOffsets,
                                      /*allowDependency*/ false);
}

MetadataDependency
swift::swift_initClassMetadata2(ClassMetadata *self,
                                ClassLayoutFlags layoutFlags,
                                size_t numFields,
                                const TypeLayout * const *fieldTypes,
                                size_t *fieldOffsets) {
  return _swift_initClassMetadataImpl(self, layoutFlags, numFields,
                                      fieldTypes, fieldOffsets,
                                      /*allowDependency*/ true);
}

#if SWIFT_OBJC_INTEROP

// Suppress diagnostic about the availability of _objc_realizeClassFromSwift.
// We test availability with a nullptr check, but the compiler doesn't see that.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"


static SWIFT_CC MetadataDependency
_swift_updateClassMetadataImpl(ClassMetadata *self,
                               ClassLayoutFlags layoutFlags,
                               size_t numFields,
                               const TypeLayout * const *fieldTypes,
                               size_t *fieldOffsets,
                               bool allowDependency) {
#ifndef OBJC_REALIZECLASSFROMSWIFT_DEFINED
  // Temporary workaround until _objc_realizeClassFromSwift is in the SDK.
  static auto _objc_realizeClassFromSwift =
    (Class (*)(Class _Nullable, void *_Nullable))
    dlsym(RTLD_NEXT, "_objc_realizeClassFromSwift");
#endif

  bool requiresUpdate = (_objc_realizeClassFromSwift != nullptr);

  // If we're on a newer runtime, we're going to be initializing the
  // field offset vector. Realize the superclass metadata first, even
  // though our superclass field references it statically.
  auto superDependencyAndSuper = getSuperclassMetadata(self, allowDependency);
  if (superDependencyAndSuper.first)
    return superDependencyAndSuper.first;
  const ClassMetadata *super = superDependencyAndSuper.second;

  // Check that it matches what's already in there.
  if (!super)
    assert(self->Superclass == getRootSuperclass());
  else
    assert(self->Superclass == super);

  (void) super;

  // If we're running on a older Objective-C runtime, just realize
  // the class.
  if (!requiresUpdate) {
    // If we don't have a backward deployment layout, we cannot proceed here.
    if (self->getInstanceSize() == 0 ||
        self->getInstanceAlignMask() == 0) {
      fatalError(0, "class %s does not have a fragile layout; "
                 "the deployment target was newer than this OS\n",
                 self->getDescription()->Name.get());
    }

    // Realize the class. This causes the runtime to slide the field offsets
    // stored in the field offset globals.
    //
    // Note that the field offset vector is *not* updated; however in
    // Objective-C interop mode, we don't actually use the field offset vector
    // of non-generic classes.
    //
    // In particular, class mirrors always use the Objective-C ivar descriptors,
    // which point at field offset globals and not the field offset vector.
    swift_getInitializedObjCClass((Class)self);
  } else {
    // Update the field offset vector using runtime type information; the layout
    // of resilient types might be different than the statically-emitted layout.
    initClassFieldOffsetVector(self, numFields, fieldTypes, fieldOffsets);

    // Copy field offset vector entries to the field offset globals.
    initObjCClass(self, numFields, fieldTypes, fieldOffsets);

    // See remark above about how this slides field offset globals.
    _objc_realizeClassFromSwift((Class)self, (Class)self);
  }

  return MetadataDependency();
}

void swift::swift_updateClassMetadata(ClassMetadata *self,
                                      ClassLayoutFlags layoutFlags,
                                      size_t numFields,
                                      const TypeLayout * const *fieldTypes,
                                      size_t *fieldOffsets) {
  (void) _swift_updateClassMetadataImpl(self, layoutFlags, numFields,
                                        fieldTypes, fieldOffsets,
                                        /*allowDependency*/ false);
}

MetadataDependency
swift::swift_updateClassMetadata2(ClassMetadata *self,
                                  ClassLayoutFlags layoutFlags,
                                  size_t numFields,
                                  const TypeLayout * const *fieldTypes,
                                  size_t *fieldOffsets) {
  return _swift_updateClassMetadataImpl(self, layoutFlags, numFields,
                                        fieldTypes, fieldOffsets,
                                        /*allowDependency*/ true);
}

#pragma clang diagnostic pop
#endif

#ifndef NDEBUG
static bool isAncestorOf(const ClassMetadata *metadata,
                         const ClassDescriptor *description) {
  auto ancestor = metadata;
  while (ancestor && ancestor->isTypeMetadata()) {
    if (ancestor->getDescription() == description)
      return true;
    ancestor = ancestor->Superclass;
  }
  return false;
}
#endif

void *
swift::swift_lookUpClassMethod(const ClassMetadata *metadata,
                               const MethodDescriptor *method,
                               const ClassDescriptor *description) {
  assert(metadata->isTypeMetadata());

  assert(isAncestorOf(metadata, description));

  auto *vtable = description->getVTableDescriptor();
  assert(vtable != nullptr);

  auto methods = description->getMethodDescriptors();
  unsigned index = method - methods.data();
  assert(index < methods.size());

  auto vtableOffset = vtable->getVTableOffset(description) + index;
  auto *words = reinterpret_cast<void * const *>(metadata);

  return *(words + vtableOffset);
}

/***************************************************************************/
/*** Metatypes *************************************************************/
/***************************************************************************/

/// Find the appropriate value witness table for the given type.
static const ValueWitnessTable *
getMetatypeValueWitnesses(const Metadata *instanceType) {
  // When metatypes are accessed opaquely, they always have a "thick"
  // representation.
  return &getUnmanagedPointerPointerValueWitnesses();
}

namespace {
  class MetatypeCacheEntry {
  public:
    FullMetadata<MetatypeMetadata> Data;

    MetatypeCacheEntry(const Metadata *instanceType) {
      Data.setKind(MetadataKind::Metatype);
      Data.ValueWitnesses = getMetatypeValueWitnesses(instanceType);
      Data.InstanceType = instanceType;
    }

    intptr_t getKeyIntValueForDump() {
      return reinterpret_cast<intptr_t>(Data.InstanceType);
    }

    int compareWithKey(const Metadata *instanceType) const {
      return comparePointers(instanceType, Data.InstanceType);
    }

    static size_t getExtraAllocationSize(const Metadata *instanceType) {
      return 0;
    }
    size_t getExtraAllocationSize() const {
      return 0;
    }
  };
} // end anonymous namespace

/// The uniquing structure for metatype type metadata.
static SimpleGlobalCache<MetatypeCacheEntry> MetatypeTypes;

/// Fetch a uniqued metadata for a metatype type.
SWIFT_RUNTIME_EXPORT
const MetatypeMetadata *
swift::swift_getMetatypeMetadata(const Metadata *instanceMetadata) {
  return &MetatypeTypes.getOrInsert(instanceMetadata).first->Data;
}

/***************************************************************************/
/*** Existential Metatypes *************************************************/
/***************************************************************************/

namespace {

/// A cache entry for existential metatype witness tables.
class ExistentialMetatypeValueWitnessTableCacheEntry {
public:
  ValueWitnessTable Data;

  unsigned getNumWitnessTables() const {
    return (Data.size - sizeof(ExistentialMetatypeContainer))
              / sizeof(const ValueWitnessTable*);
  }

  ExistentialMetatypeValueWitnessTableCacheEntry(unsigned numWitnessTables);

  intptr_t getKeyIntValueForDump() {
    return static_cast<intptr_t>(getNumWitnessTables());
  }

  int compareWithKey(unsigned key) const {
    return compareIntegers(key, getNumWitnessTables());
  }

  static size_t getExtraAllocationSize(unsigned numTables) {
    return 0;
  }
  size_t getExtraAllocationSize() const {
    return 0;
  }
};

class ExistentialMetatypeCacheEntry {
public:
  FullMetadata<ExistentialMetatypeMetadata> Data;

  ExistentialMetatypeCacheEntry(const Metadata *instanceMetadata);

  intptr_t getKeyIntValueForDump() {
    return reinterpret_cast<intptr_t>(Data.InstanceType);
  }

  int compareWithKey(const Metadata *instanceType) const {
    return comparePointers(instanceType, Data.InstanceType);
  }

  static size_t getExtraAllocationSize(const Metadata *key) {
    return 0;
  }
  size_t getExtraAllocationSize() const {
    return 0;
  }
};

} // end anonymous namespace

/// The uniquing structure for existential metatype value witness tables.
static SimpleGlobalCache<ExistentialMetatypeValueWitnessTableCacheEntry>
ExistentialMetatypeValueWitnessTables;

/// The uniquing structure for existential metatype type metadata.
static SimpleGlobalCache<ExistentialMetatypeCacheEntry> ExistentialMetatypes;

static const ValueWitnessTable
ExistentialMetatypeValueWitnesses_1 =
  ValueWitnessTableForBox<ExistentialMetatypeBox<1>>::table;
static const ValueWitnessTable
ExistentialMetatypeValueWitnesses_2 =
  ValueWitnessTableForBox<ExistentialMetatypeBox<2>>::table;

/// Instantiate a value witness table for an existential metatype
/// container with the given number of witness table pointers.
static const ValueWitnessTable *
getExistentialMetatypeValueWitnesses(unsigned numWitnessTables) {
  if (numWitnessTables == 0)
    return &getUnmanagedPointerPointerValueWitnesses();
  if (numWitnessTables == 1)
    return &ExistentialMetatypeValueWitnesses_1;
  if (numWitnessTables == 2)
    return &ExistentialMetatypeValueWitnesses_2;

  static_assert(3 * sizeof(void*) >= sizeof(ValueBuffer),
                "not handling all possible inline-storage class existentials!");

  return &ExistentialMetatypeValueWitnessTables.getOrInsert(numWitnessTables)
                                               .first->Data;
}

ExistentialMetatypeValueWitnessTableCacheEntry::
ExistentialMetatypeValueWitnessTableCacheEntry(unsigned numWitnessTables) {
  using Box = NonFixedExistentialMetatypeBox;
  using Witnesses = NonFixedValueWitnesses<Box, /*known allocated*/ true>;

#define WANT_REQUIRED_VALUE_WITNESSES 1
#define WANT_EXTRA_INHABITANT_VALUE_WITNESSES 1
#define WANT_ENUM_VALUE_WITNESSES 0
#define VALUE_WITNESS(LOWER_ID, UPPER_ID) \
  Data.LOWER_ID = Witnesses::LOWER_ID;
#define DATA_VALUE_WITNESS(LOWER_ID, UPPER_ID, TYPE)
#include "swift/ABI/ValueWitness.def"

  Data.size = Box::Container::getSize(numWitnessTables);
  Data.flags = ValueWitnessFlags()
    .withAlignment(Box::Container::getAlignment(numWitnessTables))
    .withPOD(true)
    .withBitwiseTakable(true)
    .withInlineStorage(false);
  Data.stride = Box::Container::getStride(numWitnessTables);
  Data.extraInhabitantCount = Witnesses::numExtraInhabitants;

  assert(getNumWitnessTables() == numWitnessTables);
}

/// Fetch a uniqued metadata for a metatype type.
SWIFT_RUNTIME_EXPORT
const ExistentialMetatypeMetadata *
swift::swift_getExistentialMetatypeMetadata(const Metadata *instanceMetadata) {
  return &ExistentialMetatypes.getOrInsert(instanceMetadata).first->Data;
}

ExistentialMetatypeCacheEntry::ExistentialMetatypeCacheEntry(
                                            const Metadata *instanceMetadata) {
  ExistentialTypeFlags flags;
  if (instanceMetadata->getKind() == MetadataKind::Existential) {
    flags = static_cast<const ExistentialTypeMetadata*>(instanceMetadata)
      ->Flags;
  } else {
    assert(instanceMetadata->getKind() == MetadataKind::ExistentialMetatype);
    flags = static_cast<const ExistentialMetatypeMetadata*>(instanceMetadata)
      ->Flags;
  }

  Data.setKind(MetadataKind::ExistentialMetatype);
  Data.ValueWitnesses =
    getExistentialMetatypeValueWitnesses(flags.getNumWitnessTables());
  Data.InstanceType = instanceMetadata;
  Data.Flags = flags;
}

/***************************************************************************/
/*** Existential types *****************************************************/
/***************************************************************************/

namespace {

class ExistentialCacheEntry {
public:
  FullMetadata<ExistentialTypeMetadata> Data;

  struct Key {
    const Metadata *SuperclassConstraint;
    ProtocolClassConstraint ClassConstraint : 1;
    uint32_t NumProtocols : 31;
    const ProtocolDescriptorRef *Protocols;
  };

  ExistentialCacheEntry(Key key);

  intptr_t getKeyIntValueForDump() {
    return 0;
  }

  int compareWithKey(Key key) const {
    if (auto result = compareIntegers(key.ClassConstraint,
                                      Data.Flags.getClassConstraint()))
      return result;

    if (auto result = comparePointers(key.SuperclassConstraint,
                                      Data.getSuperclassConstraint()))
      return result;

    if (auto result = compareIntegers(key.NumProtocols,
                                      Data.NumProtocols))
      return result;

    auto dataProtocols = Data.getProtocols();
    for (size_t i = 0; i != key.NumProtocols; ++i) {
      if (auto result = compareIntegers(key.Protocols[i].getRawData(),
                                        dataProtocols[i].getRawData()))
        return result;
    }

    return 0;
  }

  static size_t getExtraAllocationSize(Key key) {
    return ExistentialTypeMetadata::additionalSizeToAlloc<
             const Metadata *, ProtocolDescriptorRef
           >(key.SuperclassConstraint != nullptr, key.NumProtocols);
  }

  size_t getExtraAllocationSize() const {
    return ExistentialTypeMetadata::additionalSizeToAlloc<
             const Metadata *, ProtocolDescriptorRef
           >(Data.Flags.hasSuperclassConstraint(), Data.NumProtocols);
  }
};

class OpaqueExistentialValueWitnessTableCacheEntry {
public:
  ValueWitnessTable Data;

  OpaqueExistentialValueWitnessTableCacheEntry(unsigned numTables);

  unsigned getNumWitnessTables() const {
    return (Data.size - sizeof(OpaqueExistentialContainer))
              / sizeof(const WitnessTable *);
  }

  intptr_t getKeyIntValueForDump() {
    return getNumWitnessTables();
  }

  int compareWithKey(unsigned key) const {
    return compareIntegers(key, getNumWitnessTables());
  }

  static size_t getExtraAllocationSize(unsigned numTables) {
    return 0;
  }
  size_t getExtraAllocationSize() const {
    return 0;
  }
};

class ClassExistentialValueWitnessTableCacheEntry {
public:
  ValueWitnessTable Data;

  ClassExistentialValueWitnessTableCacheEntry(unsigned numTables);

  unsigned getNumWitnessTables() const {
    return (Data.size - sizeof(ClassExistentialContainer))
              / sizeof(const WitnessTable *);
  }

  intptr_t getKeyIntValueForDump() {
    return getNumWitnessTables();
  }

  int compareWithKey(unsigned key) const {
    return compareIntegers(key, getNumWitnessTables());
  }

  static size_t getExtraAllocationSize(unsigned numTables) {
    return 0;
  }
  size_t getExtraAllocationSize() const {
    return 0;
  }
};

} // end anonymous namespace

/// The uniquing structure for existential type metadata.
static SimpleGlobalCache<ExistentialCacheEntry> ExistentialTypes;

static const ValueWitnessTable
OpaqueExistentialValueWitnesses_0 =
  ValueWitnessTableForBox<OpaqueExistentialBox<0>>::table;
static const ValueWitnessTable
OpaqueExistentialValueWitnesses_1 =
  ValueWitnessTableForBox<OpaqueExistentialBox<1>>::table;

/// The standard metadata for Any.
const FullMetadata<ExistentialTypeMetadata> swift::
METADATA_SYM(ANY_MANGLING) = {
  { &OpaqueExistentialValueWitnesses_0 }, // ValueWitnesses
  ExistentialTypeMetadata(
    ExistentialTypeFlags() // Flags
      .withNumWitnessTables(0)
      .withClassConstraint(ProtocolClassConstraint::Any)
      .withHasSuperclass(false)
      .withSpecialProtocol(SpecialProtocol::None)),
};

/// The standard metadata for AnyObject.
const FullMetadata<ExistentialTypeMetadata> swift::
METADATA_SYM(ANYOBJECT_MANGLING) = {
  {
#if SWIFT_OBJC_INTEROP
    &VALUE_WITNESS_SYM(BO)
#else
    &VALUE_WITNESS_SYM(Bo)
#endif
  },
  ExistentialTypeMetadata(
    ExistentialTypeFlags() // Flags
      .withNumWitnessTables(0)
      .withClassConstraint(ProtocolClassConstraint::Class)
      .withHasSuperclass(false)
      .withSpecialProtocol(SpecialProtocol::None)),
};

/// The uniquing structure for opaque existential value witness tables.
static SimpleGlobalCache<OpaqueExistentialValueWitnessTableCacheEntry>
OpaqueExistentialValueWitnessTables;

/// Instantiate a value witness table for an opaque existential container with
/// the given number of witness table pointers.
static const ValueWitnessTable *
getOpaqueExistentialValueWitnesses(unsigned numWitnessTables) {
  // We pre-allocate a couple of important cases.
  if (numWitnessTables == 0)
    return &OpaqueExistentialValueWitnesses_0;
  if (numWitnessTables == 1)
    return &OpaqueExistentialValueWitnesses_1;

  return &OpaqueExistentialValueWitnessTables.getOrInsert(numWitnessTables)
                                             .first->Data;
}

OpaqueExistentialValueWitnessTableCacheEntry::
OpaqueExistentialValueWitnessTableCacheEntry(unsigned numWitnessTables) {
  using Box = NonFixedOpaqueExistentialBox;
  using Witnesses = NonFixedValueWitnesses<Box, /*known allocated*/ true>;

#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define VALUE_WITNESS(LOWER_ID, UPPER_ID) \
  Data.LOWER_ID = Witnesses::LOWER_ID;
#define DATA_VALUE_WITNESS(LOWER_ID, UPPER_ID, TYPE)
#include "swift/ABI/ValueWitness.def"

  Data.size = Box::Container::getSize(numWitnessTables);
  Data.flags = ValueWitnessFlags()
    .withAlignment(Box::Container::getAlignment(numWitnessTables))
    .withPOD(false)
    .withBitwiseTakable(true)
    .withInlineStorage(false);
  Data.extraInhabitantCount = Witnesses::numExtraInhabitants;
  Data.stride = Box::Container::getStride(numWitnessTables);

  assert(getNumWitnessTables() == numWitnessTables);
}

static const ValueWitnessTable ClassExistentialValueWitnesses_1 =
  ValueWitnessTableForBox<ClassExistentialBox<1>>::table;
static const ValueWitnessTable ClassExistentialValueWitnesses_2 =
  ValueWitnessTableForBox<ClassExistentialBox<2>>::table;

/// The uniquing structure for class existential value witness tables.
static SimpleGlobalCache<ClassExistentialValueWitnessTableCacheEntry>
ClassExistentialValueWitnessTables;

/// Instantiate a value witness table for a class-constrained existential
/// container with the given number of witness table pointers.
static const ValueWitnessTable *
getClassExistentialValueWitnesses(const Metadata *superclass,
                                  unsigned numWitnessTables) {
  // FIXME: If the superclass is not @objc, use native reference counting.
  if (numWitnessTables == 0) {
#if SWIFT_OBJC_INTEROP
    return &VALUE_WITNESS_SYM(BO);
#else
    return &VALUE_WITNESS_SYM(Bo);
#endif
  }
  if (numWitnessTables == 1)
    return &ClassExistentialValueWitnesses_1;
  if (numWitnessTables == 2)
    return &ClassExistentialValueWitnesses_2;

  static_assert(3 * sizeof(void*) >= sizeof(ValueBuffer),
                "not handling all possible inline-storage class existentials!");

  return &ClassExistentialValueWitnessTables.getOrInsert(numWitnessTables)
                                            .first->Data;
}

ClassExistentialValueWitnessTableCacheEntry::
ClassExistentialValueWitnessTableCacheEntry(unsigned numWitnessTables) {
  using Box = NonFixedClassExistentialBox;
  using Witnesses = NonFixedValueWitnesses<Box, /*known allocated*/ true>;

#define WANT_REQUIRED_VALUE_WITNESSES 1
#define WANT_EXTRA_INHABITANT_VALUE_WITNESSES 1
#define WANT_ENUM_VALUE_WITNESSES 0
#define VALUE_WITNESS(LOWER_ID, UPPER_ID) \
  Data.LOWER_ID = Witnesses::LOWER_ID;
#define DATA_VALUE_WITNESS(LOWER_ID, UPPER_ID, TYPE)
#include "swift/ABI/ValueWitness.def"

  Data.size = Box::Container::getSize(numWitnessTables);
  Data.flags = ValueWitnessFlags()
    .withAlignment(Box::Container::getAlignment(numWitnessTables))
    .withPOD(false)
    .withBitwiseTakable(true)
    .withInlineStorage(false);
  Data.extraInhabitantCount = Witnesses::numExtraInhabitants;
  Data.stride = Box::Container::getStride(numWitnessTables);

  assert(getNumWitnessTables() == numWitnessTables);
}

/// Get the value witness table for an existential type, first trying to use a
/// shared specialized table for common cases.
static const ValueWitnessTable *
getExistentialValueWitnesses(ProtocolClassConstraint classConstraint,
                             const Metadata *superclassConstraint,
                             unsigned numWitnessTables,
                             SpecialProtocol special) {
  // Use special representation for special protocols.
  switch (special) {
  case SpecialProtocol::Error:
#if SWIFT_OBJC_INTEROP
    // Error always has a single-ObjC-refcounted representation.
    return &VALUE_WITNESS_SYM(BO);
#else
    // Without ObjC interop, Error is native-refcounted.
    return &VALUE_WITNESS_SYM(Bo);
#endif
      
  // Other existentials use standard representation.
  case SpecialProtocol::None:
    break;
  }
  
  switch (classConstraint) {
  case ProtocolClassConstraint::Class:
    return getClassExistentialValueWitnesses(superclassConstraint,
                                             numWitnessTables);
  case ProtocolClassConstraint::Any:
    assert(superclassConstraint == nullptr);
    return getOpaqueExistentialValueWitnesses(numWitnessTables);
  }

  swift_runtime_unreachable("Unhandled ProtocolClassConstraint in switch.");
}

template<> ExistentialTypeRepresentation
ExistentialTypeMetadata::getRepresentation() const {
  // Some existentials use special containers.
  switch (Flags.getSpecialProtocol()) {
  case SpecialProtocol::Error:
    return ExistentialTypeRepresentation::Error;
  case SpecialProtocol::None:
    break;
  }
  // The layout of standard containers depends on whether the existential is
  // class-constrained.
  if (isClassBounded())
    return ExistentialTypeRepresentation::Class;
  return ExistentialTypeRepresentation::Opaque;
}

template<> bool
ExistentialTypeMetadata::mayTakeValue(const OpaqueValue *container) const {
  switch (getRepresentation()) {
  // Owning a reference to a class existential is equivalent to owning a
  // reference to the contained class instance.
  case ExistentialTypeRepresentation::Class:
    return true;
  // Opaque existential containers uniquely own their contained value.
  case ExistentialTypeRepresentation::Opaque: {
    // We can't take from a shared existential box without checking uniqueness.
    auto *opaque =
        reinterpret_cast<const OpaqueExistentialContainer *>(container);
    return opaque->isValueInline();
  }
  // References to boxed existential containers may be shared.
  case ExistentialTypeRepresentation::Error: {
    // We can only take the value if the box is a bridged NSError, in which case
    // owning a reference to the box is owning a reference to the NSError.
    // TODO: Or if the box is uniquely referenced. We don't have intimate
    // enough knowledge of CF refcounting to check for that dynamically yet.
    const SwiftError *errorBox
      = *reinterpret_cast<const SwiftError * const *>(container);
    return errorBox->isPureNSError();
  }
  }

  swift_runtime_unreachable(
      "Unhandled ExistentialTypeRepresentation in switch.");
}

template<> void
ExistentialTypeMetadata::deinitExistentialContainer(OpaqueValue *container)
const {
  switch (getRepresentation()) {
  case ExistentialTypeRepresentation::Class:
    // Nothing to clean up after taking the class reference.
    break;
  
  case ExistentialTypeRepresentation::Opaque: {
    auto *opaque = reinterpret_cast<OpaqueExistentialContainer *>(container);
    opaque->deinit();
    break;
  }
  
  case ExistentialTypeRepresentation::Error:
    // TODO: If we were able to claim the value from a uniquely-owned
    // existential box, we would want to deallocError here.
    break;
  }
}

template<> const OpaqueValue *
ExistentialTypeMetadata::projectValue(const OpaqueValue *container) const {
  switch (getRepresentation()) {
  case ExistentialTypeRepresentation::Class: {
    auto classContainer =
      reinterpret_cast<const ClassExistentialContainer*>(container);
    return reinterpret_cast<const OpaqueValue *>(&classContainer->Value);
  }
  case ExistentialTypeRepresentation::Opaque: {
    auto *opaqueContainer =
      reinterpret_cast<const OpaqueExistentialContainer*>(container);
    return opaqueContainer->projectValue();
  }
  case ExistentialTypeRepresentation::Error: {
    const SwiftError *errorBox
      = *reinterpret_cast<const SwiftError * const *>(container);
    // If the error is a bridged NSError, then the "box" is in fact itself
    // the value.
    if (errorBox->isPureNSError())
      return container;
    return errorBox->getValue();
  }
  }

  swift_runtime_unreachable(
      "Unhandled ExistentialTypeRepresentation in switch.");
}

template<> const Metadata *
ExistentialTypeMetadata::getDynamicType(const OpaqueValue *container) const {
  switch (getRepresentation()) {
  case ExistentialTypeRepresentation::Class: {
    auto classContainer =
      reinterpret_cast<const ClassExistentialContainer*>(container);
    void *obj = classContainer->Value;
    return swift_getObjectType(reinterpret_cast<HeapObject*>(obj));
  }
  case ExistentialTypeRepresentation::Opaque: {
    auto opaqueContainer =
      reinterpret_cast<const OpaqueExistentialContainer*>(container);
    return opaqueContainer->Type;
  }
  case ExistentialTypeRepresentation::Error: {
    const SwiftError *errorBox
      = *reinterpret_cast<const SwiftError * const *>(container);
    return errorBox->getType();
  }
  }

  swift_runtime_unreachable(
      "Unhandled ExistentialTypeRepresentation in switch.");
}

template<> const WitnessTable *
ExistentialTypeMetadata::getWitnessTable(const OpaqueValue *container,
                                         unsigned i) const {
  assert(i < Flags.getNumWitnessTables());

  // The layout of the container depends on whether it's class-constrained
  // or a special protocol.
  const WitnessTable * const *witnessTables;
  
  switch (getRepresentation()) {
  case ExistentialTypeRepresentation::Class: {
    auto classContainer =
      reinterpret_cast<const ClassExistentialContainer*>(container);
    witnessTables = classContainer->getWitnessTables();
    break;
  }
  case ExistentialTypeRepresentation::Opaque: {
    auto opaqueContainer =
      reinterpret_cast<const OpaqueExistentialContainer*>(container);
    witnessTables = opaqueContainer->getWitnessTables();
    break;
  }
  case ExistentialTypeRepresentation::Error: {
    // Only one witness table we should be able to return, which is the
    // Error.
    assert(i == 0 && "only one witness table in an Error box");
    const SwiftError *errorBox
      = *reinterpret_cast<const SwiftError * const *>(container);
    return errorBox->getErrorConformance();
  }
  }

  // The return type here describes extra structure for the protocol
  // witness table for some reason.  We should probably have a nominal
  // type for these, just for type safety reasons.
  return witnessTables[i];
}

#ifndef NDEBUG
/// Determine whether any of the given protocols is class-bound.
static bool anyProtocolIsClassBound(
                                size_t numProtocols,
                                const ProtocolDescriptorRef *protocols) {
  for (unsigned i = 0; i != numProtocols; ++i) {
    if (protocols[i].getClassConstraint() == ProtocolClassConstraint::Class)
      return true;
  }

  return false;
}
#endif

const Metadata *
swift::_getSimpleProtocolTypeMetadata(const ProtocolDescriptor *protocol) {
  auto protocolRef = ProtocolDescriptorRef::forSwift(protocol);
  auto constraint =
    protocol->getProtocolContextDescriptorFlags().getClassConstraint();
  return swift_getExistentialTypeMetadata(constraint,
                                          /*superclass bound*/ nullptr,
                                          /*num protocols*/ 1,
                                          &protocolRef);
}

/// Fetch a uniqued metadata for an existential type. The array
/// referenced by \c protocols will be sorted in-place.
const ExistentialTypeMetadata *
swift::swift_getExistentialTypeMetadata(
                                  ProtocolClassConstraint classConstraint,
                                  const Metadata *superclassConstraint,
                                  size_t numProtocols,
                                  const ProtocolDescriptorRef *protocols) {

  // The empty compositions Any and AnyObject have fixed metadata.
  if (numProtocols == 0 && !superclassConstraint) {
    switch (classConstraint) {
    case ProtocolClassConstraint::Any:
      return &METADATA_SYM(ANY_MANGLING);
    case ProtocolClassConstraint::Class:
      return &METADATA_SYM(ANYOBJECT_MANGLING);
    }
  }

  // We entrust that the compiler emitting the call to
  // swift_getExistentialTypeMetadata always sorts the `protocols` array using
  // a globally stable ordering that's consistent across modules.
  
  // Ensure that the "class constraint" bit is set whenever we have a
  // superclass or a one of the protocols is class-bound.
  assert(classConstraint == ProtocolClassConstraint::Class ||
         (!superclassConstraint &&
          !anyProtocolIsClassBound(numProtocols, protocols)));
  ExistentialCacheEntry::Key key = {
    superclassConstraint, classConstraint, (uint32_t)numProtocols, protocols
  };
  return &ExistentialTypes.getOrInsert(key).first->Data;
}

ExistentialCacheEntry::ExistentialCacheEntry(Key key) {
  // Calculate the class constraint and number of witness tables for the
  // protocol set.
  unsigned numWitnessTables = 0;
  for (auto p : make_range(key.Protocols, key.Protocols + key.NumProtocols)) {
    if (p.needsWitnessTable())
      ++numWitnessTables;
  }

  // Get the special protocol kind for an uncomposed protocol existential.
  // Protocol compositions are currently never special.
  auto special = SpecialProtocol::None;
  if (key.NumProtocols == 1)
    special = key.Protocols[0].getSpecialProtocol();

  Data.setKind(MetadataKind::Existential);
  Data.ValueWitnesses = getExistentialValueWitnesses(key.ClassConstraint,
                                                     key.SuperclassConstraint,
                                                     numWitnessTables,
                                                     special);
  Data.Flags = ExistentialTypeFlags()
    .withNumWitnessTables(numWitnessTables)
    .withClassConstraint(key.ClassConstraint)
    .withSpecialProtocol(special);

  if (key.SuperclassConstraint != nullptr) {
    Data.Flags = Data.Flags.withHasSuperclass(true);
    Data.setSuperclassConstraint(key.SuperclassConstraint);
  }

  Data.NumProtocols = key.NumProtocols;
  auto dataProtocols = Data.getMutableProtocols();
  for (size_t i = 0; i < key.NumProtocols; ++i) {
    dataProtocols[i] = key.Protocols[i];
  }
}

/// Perform a copy-assignment from one existential container to another.
/// Both containers must be of the same existential type representable with no
/// witness tables.
OpaqueValue *swift::swift_assignExistentialWithCopy0(OpaqueValue *dest,
                                                     const OpaqueValue *src,
                                                     const Metadata *type) {
  using Witnesses = ValueWitnesses<OpaqueExistentialBox<0>>;
  return Witnesses::assignWithCopy(dest, const_cast<OpaqueValue*>(src), type);
}

/// Perform a copy-assignment from one existential container to another.
/// Both containers must be of the same existential type representable with one
/// witness table.
OpaqueValue *swift::swift_assignExistentialWithCopy1(OpaqueValue *dest,
                                                     const OpaqueValue *src,
                                                     const Metadata *type) {
  using Witnesses = ValueWitnesses<OpaqueExistentialBox<1>>;
  return Witnesses::assignWithCopy(dest, const_cast<OpaqueValue*>(src), type);
}

/// Perform a copy-assignment from one existential container to another.
/// Both containers must be of the same existential type representable with the
/// same number of witness tables.
OpaqueValue *swift::swift_assignExistentialWithCopy(OpaqueValue *dest,
                                                    const OpaqueValue *src,
                                                    const Metadata *type) {
  assert(!type->getValueWitnesses()->isValueInline());
  using Witnesses = NonFixedValueWitnesses<NonFixedOpaqueExistentialBox,
                                           /*known allocated*/ true>;
  return Witnesses::assignWithCopy(dest, const_cast<OpaqueValue*>(src), type);
}

/***************************************************************************/
/*** Foreign types *********************************************************/
/***************************************************************************/

// We use a DenseMap over what are essentially StringRefs instead of a
// StringMap because we don't need to actually copy the string.
namespace {

static const TypeContextDescriptor *
getForeignTypeDescription(Metadata *metadata) {
  if (auto foreignClass = dyn_cast<ForeignClassMetadata>(metadata))
    return foreignClass->getDescription();
  return cast<ValueMetadata>(metadata)->getDescription();
}

class ForeignMetadataCacheEntry
  : public MetadataCacheEntryBase<ForeignMetadataCacheEntry, /*spurious*/ int> {

  Metadata *Value;

  friend MetadataCacheEntryBase;
  ValueType getValue() {
    return Value;
  }
  void setValue(ValueType value) {
    swift_runtime_unreachable("should never be called");
  }

public:
  static const char *getName() { return "ForeignMetadataCache"; }

  template <class... Args>
  ForeignMetadataCacheEntry(const TypeContextDescriptor *description,
                            MetadataRequest request, Metadata *candidate)
      : Value(candidate) {
    // Remember that the metadata is still just a candidate until this
    // is actually successfully installed in the concurrent map.

    auto &init = description->getForeignMetadataInitialization();

    PrivateMetadataState state;
    if (!init.CompletionFunction) {
      if (areAllTransitiveMetadataComplete_cheap(candidate)) {
        state = PrivateMetadataState::Complete;
      } else {
        state = PrivateMetadataState::NonTransitiveComplete;
      }
    } else {
      if (candidate->getValueWitnesses() == nullptr) {
        assert(isa<ForeignClassMetadata>(candidate) &&
               "cannot set default value witnesses for non-class foreign types");
        // Fill in the default VWT if it was not set in the candidate at build
        // time.
#if SWIFT_OBJC_INTEROP
        candidate->setValueWitnesses(&VALUE_WITNESS_SYM(BO));
#else
        candidate->setValueWitnesses(&VALUE_WITNESS_SYM(Bo));
#endif
      }
      state = inferStateForMetadata(candidate);
    }

    flagAllocatedDuringConstruction(state);
  }

  enum : bool { MayFlagAllocatedDuringConstruction = true };

  const TypeContextDescriptor *getDescription() const {
    return getForeignTypeDescription(Value);
  }

  template <class... Args>
  static size_t numTrailingObjects(OverloadToken<int>, Args &&...) {
    return 0;
  }

  intptr_t getKeyIntValueForDump() const {
    return reinterpret_cast<intptr_t>(getDescription()->Name.get());
  }

  int compareWithKey(const TypeContextDescriptor *key) const {
    // We can just compare unparented type-context identities because
    // we assume that foreign types don't have interesting parenting
    // structure.
    return TypeContextIdentity(key)
             .compare(TypeContextIdentity(getDescription()));
  }

  AllocationResult allocate(Metadata *candidate) {
    swift_runtime_unreachable(
                        "always flags allocation complete during construction");
  }

  TryInitializeResult tryInitialize(Metadata *metadata,
                                    PrivateMetadataState state,
                                    PrivateMetadataCompletionContext *ctxt) {
    assert(state != PrivateMetadataState::Complete);

    // Finish the completion function.
    auto &init = getDescription()->getForeignMetadataInitialization();
    if (init.CompletionFunction) {
      // Try to complete the metadata's instantiation.
      auto dependency =
        init.CompletionFunction(metadata, &ctxt->Public, nullptr);

      // If this failed with a dependency, infer the current metadata state
      // and return.
      if (dependency) {
        return { inferStateForMetadata(metadata), dependency };
      }
    }

    // Check for transitive completeness.
    if (auto dependency = checkTransitiveCompleteness(metadata)) {
      return { PrivateMetadataState::NonTransitiveComplete, dependency };
    }

    // We're done.
    return { PrivateMetadataState::Complete, MetadataDependency() };
  }
};

} // end anonymous namespace

static Lazy<MetadataCache<ForeignMetadataCacheEntry, false>> ForeignMetadata;

MetadataResponse
swift::swift_getForeignTypeMetadata(MetadataRequest request,
                                    ForeignTypeMetadata *candidate) {
  auto description = getForeignTypeDescription(candidate);
  return ForeignMetadata->getOrInsert(description, request, candidate).second;
}

/// Unique-ing of foreign types' witness tables.
namespace {
  class ForeignWitnessTableCacheEntry {
  public:
    struct Key {
      const TypeContextDescriptor *type;
      const ProtocolDescriptor *protocol;
    };

    const Key key;
    const WitnessTable *data;

    ForeignWitnessTableCacheEntry(const ForeignWitnessTableCacheEntry::Key k,
                                  const WitnessTable *d)
        : key(k), data(d) {}

    intptr_t getKeyIntValueForDump() {
      return reinterpret_cast<intptr_t>(key.type);
    }

    int compareWithKey(const Key other) const {
      if (auto r = comparePointers(other.protocol, key.protocol))
        return r;

      return TypeContextIdentity(other.type).compare(TypeContextIdentity(key.type));
    }

    static size_t getExtraAllocationSize(const Key,
                                         const WitnessTable *) {
      return 0;
    }

    size_t getExtraAllocationSize() const {
      return 0;
    }
  };
}

static SimpleGlobalCache<ForeignWitnessTableCacheEntry> ForeignWitnessTables;

static const WitnessTable *_getForeignWitnessTable(
    const WitnessTable *witnessTableCandidate,
    const TypeContextDescriptor *contextDescriptor,
    const ProtocolDescriptor *protocol) {
  auto result =
      ForeignWitnessTables
          .getOrInsert(
              ForeignWitnessTableCacheEntry::Key{contextDescriptor, protocol},
              witnessTableCandidate)
          .first->data;
  return result;
}

/***************************************************************************/
/*** Other metadata routines ***********************************************/
/***************************************************************************/

template <> OpaqueValue *Metadata::allocateBoxForExistentialIn(ValueBuffer *buffer) const {
  auto *vwt = getValueWitnesses();
  if (vwt->isValueInline())
    return reinterpret_cast<OpaqueValue *>(buffer);

  // Allocate the box.
  BoxPair refAndValueAddr(swift_allocBox(this));
  buffer->PrivateData[0] = refAndValueAddr.object;
  return refAndValueAddr.buffer;
}

template <> void Metadata::deallocateBoxForExistentialIn(ValueBuffer *buffer) const {
  auto *vwt = getValueWitnesses();
  if (vwt->isValueInline())
    return;
  swift_deallocBox(reinterpret_cast<HeapObject *>(buffer->PrivateData[0]));
}

template <> OpaqueValue *Metadata::allocateBufferIn(ValueBuffer *buffer) const {
  auto *vwt = getValueWitnesses();
  if (vwt->isValueInline())
    return reinterpret_cast<OpaqueValue *>(buffer);
  // Allocate temporary outline buffer.
  auto size = vwt->getSize();
  auto alignMask = vwt->getAlignmentMask();
  auto *ptr = swift_slowAlloc(size, alignMask);
  buffer->PrivateData[0] = ptr;
  return reinterpret_cast<OpaqueValue *>(ptr);
}

template <> OpaqueValue *Metadata::projectBufferFrom(ValueBuffer *buffer) const{
  auto *vwt = getValueWitnesses();
  if (vwt->isValueInline())
    return reinterpret_cast<OpaqueValue *>(buffer);
  return reinterpret_cast<OpaqueValue *>(buffer->PrivateData[0]);
}

template <> void Metadata::deallocateBufferIn(ValueBuffer *buffer) const {
  auto *vwt = getValueWitnesses();
  if (vwt->isValueInline())
    return;
  auto size = vwt->getSize();
  auto alignMask = vwt->getAlignmentMask();
  swift_slowDealloc(buffer->PrivateData[0], size, alignMask);
}

#ifndef NDEBUG
SWIFT_RUNTIME_EXPORT
void _swift_debug_verifyTypeLayoutAttribute(Metadata *type,
                                            const void *runtimeValue,
                                            const void *staticValue,
                                            size_t size,
                                            const char *description) {
  auto presentValue = [&](const void *value) {
    if (size <= sizeof(uint64_t)) {
      uint64_t intValue = 0;
      auto ptr = reinterpret_cast<uint8_t *>(&intValue);
#if defined(__BIG_ENDIAN__)
      ptr += sizeof(uint64_t) - size;
#endif
      memcpy(ptr, value, size);
      fprintf(stderr, "%" PRIu64 " (%#" PRIx64 ")\n", intValue, intValue);
      fprintf(stderr, "                  ");
    }
    auto bytes = reinterpret_cast<const uint8_t *>(value);
    for (unsigned i = 0; i < size; ++i) {
      fprintf(stderr, "%02x ", bytes[i]);
    }
    fprintf(stderr, "\n");
  };
  
  if (memcmp(runtimeValue, staticValue, size) != 0) {
    auto typeName = nameForMetadata(type);
    fprintf(stderr, "*** Type verification of %s %s failed ***\n",
            typeName.c_str(), description);
    
    fprintf(stderr, "  runtime value:  ");
    presentValue(runtimeValue);
    fprintf(stderr, "  compiler value: ");
    presentValue(staticValue);
  }
}
#endif

StringRef swift::getStringForMetadataKind(MetadataKind kind) {
  switch (kind) {
#define METADATAKIND(NAME, VALUE) \
    case MetadataKind::NAME: \
      return #NAME;
#include "swift/ABI/MetadataKind.def"
      
  default:
    return "<unknown>";
  }
}

/***************************************************************************/
/*** Debugging dump methods ************************************************/
/***************************************************************************/

#ifndef NDEBUG
template <>
LLVM_ATTRIBUTE_USED
void Metadata::dump() const {
  printf("TargetMetadata.\n");
  printf("Kind: %s.\n", getStringForMetadataKind(getKind()).data());
  printf("Value Witnesses: %p.\n", getValueWitnesses());

  if (auto *contextDescriptor = getTypeContextDescriptor()) {
    printf("Name: %s.\n", contextDescriptor->Name.get());
    printf("Type Context Description: %p.\n", contextDescriptor);

    if (contextDescriptor->isGeneric()) {
      auto genericCount = contextDescriptor->getFullGenericContextHeader().Base.getNumArguments();
      auto *args = getGenericArgs();
      printf("Generic Args: %u: [", genericCount);
      for (uint32_t i = 0; i < genericCount; i++) {
        if (i > 0)
          printf(", ");
        printf("%p", args[i]);
      }
      printf("]\n");
    }
  }

  if (auto *tuple = dyn_cast<TupleTypeMetadata>(this)) {
    printf("Labels: %s.\n", tuple->Labels);
  }

  if (auto *existential = dyn_cast<ExistentialTypeMetadata>(this)) {
    printf("Is class bounded: %s.\n",
           existential->isClassBounded() ? "true" : "false");
    auto protocols = existential->getProtocols();
    bool first = true;
    printf("Protocols: ");
    for (auto protocol : protocols) {
      if (!first)
        printf(" & ");
      printf("%s", protocol.getName());
      first = false;
    }
    if (auto *superclass = existential->getSuperclassConstraint())
      if (auto *contextDescriptor = superclass->getTypeContextDescriptor())
        printf("Superclass constraint: %s.\n", contextDescriptor->Name.get());
    printf("\n");
  }

#if SWIFT_OBJC_INTEROP
  if (auto *classObject = getClassObject()) {
    printf("ObjC Name: %s.\n", class_getName(
        reinterpret_cast<Class>(const_cast<ClassMetadata *>(classObject))));
    printf("Class Object: %p.\n", classObject);
  }
#endif
}

template <>
LLVM_ATTRIBUTE_USED
void ContextDescriptor::dump() const {
  printf("TargetTypeContextDescriptor.\n");
  printf("Flags: 0x%x.\n", this->Flags.getIntValue());
  printf("Parent: %p.\n", this->Parent.get());
  if (auto *typeDescriptor = dyn_cast<TypeContextDescriptor>(this)) {
    printf("Name: %s.\n", typeDescriptor->Name.get());
    printf("Fields: %p.\n", typeDescriptor->Fields.get());
    printf("Access function: %p.\n",
           static_cast<void *>(typeDescriptor->getAccessFunction()));
  }
}

template<>
LLVM_ATTRIBUTE_USED
void EnumDescriptor::dump() const {
  printf("TargetEnumDescriptor.\n");
  printf("Flags: 0x%x.\n", this->Flags.getIntValue());
  printf("Parent: %p.\n", this->Parent.get());
  printf("Name: %s.\n", Name.get());
  printf("Access function: %p.\n", static_cast<void *>(getAccessFunction()));
  printf("Fields: %p.\n", Fields.get());
  printf("NumPayloadCasesAndPayloadSizeOffset: 0x%08x "
         "(payload cases: %u - payload size offset: %zu).\n",
         NumPayloadCasesAndPayloadSizeOffset,
         getNumPayloadCases(), getPayloadSizeOffset());
  printf("NumEmptyCases: %u\n", NumEmptyCases);
}
#endif

/***************************************************************************/
/*** Protocol witness tables ***********************************************/
/***************************************************************************/

namespace {

/// A cache-entry type suitable for use with LockingConcurrentMap.
class WitnessTableCacheEntry :
    public SimpleLockingCacheEntryBase<WitnessTableCacheEntry, WitnessTable*> {
  /// The type for which this table was instantiated.
  const Metadata * const Type;

  /// The protocol conformance descriptor. This is only kept around so that we
  /// can compute the size of an entry correctly in case of a race to
  /// allocate the entry.
  const ProtocolConformanceDescriptor * const Conformance;

public:
  /// Do the structural initialization necessary for this entry to appear
  /// in a concurrent map.
  WitnessTableCacheEntry(const Metadata *type,
                         const ProtocolConformanceDescriptor *conformance,
                         const void * const *instantiationArgs)
    : Type(type), Conformance(conformance) {}

  intptr_t getKeyIntValueForDump() const {
    return reinterpret_cast<intptr_t>(Type);
  }

  /// The key value of the entry is just its type pointer.
  int compareWithKey(const Metadata *type) const {
    return comparePointers(Type, type);
  }

  static size_t getExtraAllocationSize(
                             const Metadata *type,
                             const ProtocolConformanceDescriptor *conformance,
                             const void * const *instantiationArgs) {
    return getWitnessTableSize(conformance);
  }

  size_t getExtraAllocationSize() const {
    return getWitnessTableSize(Conformance);
  }

  static size_t getWitnessTableSize(
                            const ProtocolConformanceDescriptor *conformance) {
    auto protocol = conformance->getProtocol();
    auto genericTable = conformance->getGenericWitnessTable();
    size_t numPrivateWords = genericTable->getWitnessTablePrivateSizeInWords();
    size_t numRequirementWords =
      WitnessTableFirstRequirementOffset + protocol->NumRequirements;
    return (numPrivateWords + numRequirementWords) * sizeof(void*);
  }

  WitnessTable *allocate(const ProtocolConformanceDescriptor *conformance,
                         const void * const *instantiationArgs);
};

} // end anonymous namespace

using GenericWitnessTableCache =
  MetadataCache<WitnessTableCacheEntry, /*destructor*/ false>;
using LazyGenericWitnessTableCache = Lazy<GenericWitnessTableCache>;

/// Fetch the cache for a generic witness-table structure.
static GenericWitnessTableCache &getCache(const GenericWitnessTable *gen) {
  // Keep this assert even if you change the representation above.
  static_assert(sizeof(LazyGenericWitnessTableCache) <=
                sizeof(GenericWitnessTable::PrivateDataType),
                "metadata cache is larger than the allowed space");

  auto lazyCache =
    reinterpret_cast<LazyGenericWitnessTableCache*>(gen->PrivateData.get());
  return lazyCache->get();
}

/// If there's no initializer, no private storage, and all requirements
/// are present, we don't have to instantiate anything; just return the
/// witness table template.
static bool doesNotRequireInstantiation(
                              const ProtocolConformanceDescriptor *conformance,
                              const GenericWitnessTable *genericTable) {
  // If the table says it requires instantiation, it does.
  if (genericTable->requiresInstantiation()) {
    return false;
  }

  // If we have resilient witnesses, we require instantiation.
  if (!conformance->getResilientWitnesses().empty()) {
    return false;
  }

  // If we don't have the exact number of witnesses expected, we require
  // instantiation.
  if (genericTable->WitnessTableSizeInWords !=
          (conformance->getProtocol()->NumRequirements +
           WitnessTableFirstRequirementOffset)) {
    return false;
  }

  // If we have an instantiation function or private data, we require
  // instantiation.
  if (!genericTable->Instantiator.isNull() ||
      genericTable->getWitnessTablePrivateSizeInWords() > 0) {
    return false;
  }

  return true;
}

/// Initialize witness table entries from order independent resilient
/// witnesses stored in the generic witness table structure itself.
static void initializeResilientWitnessTable(
                              const ProtocolConformanceDescriptor *conformance,
                              const Metadata *conformingType,
                              const GenericWitnessTable *genericTable,
                              void **table) {
  auto protocol = conformance->getProtocol();

  auto requirements = protocol->getRequirements();
  auto witnesses = conformance->getResilientWitnesses();

  // Loop over the provided witnesses, filling in appropriate entry.
  for (const auto &witness : witnesses) {
    // Retrieve the requirement descriptor.
    auto reqDescriptor = witness.Requirement.get();

    // The requirement descriptor may be NULL, in which case this is a
    // requirement introduced in a later version of the protocol.
    if (!reqDescriptor) continue;

    // If the requirement descriptor doesn't land within the bounds of the
    // requirements, abort.
    if (reqDescriptor < requirements.begin() ||
        reqDescriptor >= requirements.end()) {
      fatalError(0, "generic witness table at %p contains out-of-bounds "
                 "requirement descriptor %p\n",
                 genericTable, reqDescriptor);
    }

    unsigned witnessIndex = (reqDescriptor - requirements.data()) +
      WitnessTableFirstRequirementOffset;

    table[witnessIndex] = witness.Witness.get();
  }

  // Loop over the requirements, filling in default implementations where
  // needed.
  for (size_t i = 0, e = protocol->NumRequirements; i < e; ++i) {
    unsigned witnessIndex = WitnessTableFirstRequirementOffset + i;

    // If we don't have a witness, fill in the default implementation.
    // If we already have a witness, there's nothing to do.
    auto &reqt = requirements[i];
    if (!table[witnessIndex]) {
      void *impl = reqt.DefaultImplementation.get();
      table[witnessIndex] = impl;
    }

    // Realize base protocol witnesses.
    if (reqt.Flags.getKind() == ProtocolRequirementFlags::Kind::BaseProtocol &&
        table[witnessIndex]) {
      // Realize the base protocol witness table.
      auto baseReq = protocol->getRequirementBaseDescriptor();
      (void)swift_getAssociatedConformanceWitness((WitnessTable *)table,
                                                  conformingType,
                                                  conformingType,
                                                  baseReq, &reqt);
    }
  }
}

/// Instantiate a brand new witness table for a resilient or generic
/// protocol conformance.
WitnessTable *
WitnessTableCacheEntry::allocate(
                               const ProtocolConformanceDescriptor *conformance,
                               const void * const *instantiationArgs) {
  auto protocol = conformance->getProtocol();
  auto genericTable = conformance->getGenericWitnessTable();

  // The number of witnesses provided by the table pattern.
  size_t numPatternWitnesses = genericTable->WitnessTableSizeInWords;

  // The total number of requirements.
  size_t numRequirements =
    protocol->NumRequirements + WitnessTableFirstRequirementOffset;
  assert(numPatternWitnesses <= numRequirements);
  (void)numRequirements;

  // Number of bytes for any private storage used by the conformance itself.
  size_t privateSizeInWords = genericTable->getWitnessTablePrivateSizeInWords();

  // Find the allocation.
  void **fullTable = reinterpret_cast<void**>(this + 1);

  // Zero out the private storage area.
  memset(fullTable, 0, privateSizeInWords * sizeof(void*));

  // Advance the address point; the private storage area is accessed via
  // negative offsets.
  auto table = fullTable + privateSizeInWords;
  if (auto pattern =
          reinterpret_cast<void * const *>(
            &*conformance->getWitnessTablePattern())) {
    // Fill in the provided part of the requirements from the pattern.
    for (size_t i = 0, e = numPatternWitnesses; i < e; ++i) {
      table[i] = pattern[i];
    }
  } else {
    // Put the conformance descriptor in place. Instantiation will fill in the
    // rest.
    assert(numPatternWitnesses == 0);
    table[0] = (void *)conformance;
  }

  // Copy any instantiation arguments that correspond to conditional
  // requirements into the private area.
  {
    unsigned currentInstantiationArg = 0;
    auto copyNextInstantiationArg = [&] {
      assert(currentInstantiationArg < privateSizeInWords);
      table[-1 - (int)currentInstantiationArg] =
        const_cast<void *>(instantiationArgs[currentInstantiationArg]);
      ++currentInstantiationArg;
    };

    for (const auto &conditionalRequirement
          : conformance->getConditionalRequirements()) {
      if (conditionalRequirement.Flags.hasKeyArgument())
        copyNextInstantiationArg();
      if (conditionalRequirement.Flags.hasExtraArgument())
        copyNextInstantiationArg();
    }
  }

  // Fill in any default requirements.
  initializeResilientWitnessTable(conformance, Type, genericTable, table);
  auto castTable = reinterpret_cast<WitnessTable*>(table);

  // Call the instantiation function if present.
  if (!genericTable->Instantiator.isNull()) {
    genericTable->Instantiator(castTable, Type, instantiationArgs);
  }

  return castTable;
}

const WitnessTable *
swift::swift_getWitnessTable(const ProtocolConformanceDescriptor *conformance,
                             const Metadata *type,
                             const void * const *instantiationArgs) {
  /// Local function to unique a foreign witness table, if needed.
  auto uniqueForeignWitnessTableRef =
      [conformance](const WitnessTable *candidate) {
        if (!candidate || !conformance->isSynthesizedNonUnique())
          return candidate;

        auto conformingType =
          cast<TypeContextDescriptor>(conformance->getTypeDescriptor());

        return _getForeignWitnessTable(candidate,
                                       conformingType,
                                       conformance->getProtocol());
      };

  // When there is no generic table, or it doesn't require instantiation,
  // use the pattern directly.
  // accessor directly.
  auto genericTable = conformance->getGenericWitnessTable();
  if (!genericTable || doesNotRequireInstantiation(conformance, genericTable)) {
    return uniqueForeignWitnessTableRef(conformance->getWitnessTablePattern());
  }

  auto &cache = getCache(genericTable);
  auto result = cache.getOrInsert(type, conformance, instantiationArgs);

  // Our returned 'status' is the witness table itself.
  return uniqueForeignWitnessTableRef(result.second);
}

/// Find the name of the associated type with the given descriptor.
static StringRef findAssociatedTypeName(const ProtocolDescriptor *protocol,
                                        const ProtocolRequirement *assocType) {
  // If we don't have associated type names, there's nothing to do.
  const char *associatedTypeNamesPtr = protocol->AssociatedTypeNames.get();
  if (!associatedTypeNamesPtr) return StringRef();

  StringRef associatedTypeNames(associatedTypeNamesPtr);
  for (const auto &req : protocol->getRequirements()) {
    if (req.Flags.getKind() !=
          ProtocolRequirementFlags::Kind::AssociatedTypeAccessFunction)
      continue;

    // If we've found the requirement, we're done.
    auto splitIdx = associatedTypeNames.find(' ');
    if (&req == assocType) {
      return associatedTypeNames.substr(0, splitIdx);
    }

    // Skip this associated type name.
    associatedTypeNames = associatedTypeNames.substr(splitIdx).substr(1);
  }

  return StringRef();
}

SWIFT_CC
static MetadataResponse swift_getAssociatedTypeWitnessSlowImpl(
                                      MetadataRequest request,
                                      WitnessTable *wtable,
                                      const Metadata *conformingType,
                                      const ProtocolRequirement *reqBase,
                                      const ProtocolRequirement *assocType) {
#ifndef NDEBUG
  {
    const ProtocolConformanceDescriptor *conformance = wtable->Description;
    const ProtocolDescriptor *protocol = conformance->getProtocol();
    auto requirements = protocol->getRequirements();
    assert(assocType >= requirements.begin() &&
           assocType < requirements.end());
    assert(reqBase == requirements.data() - WitnessTableFirstRequirementOffset);
    assert(assocType->Flags.getKind() ==
           ProtocolRequirementFlags::Kind::AssociatedTypeAccessFunction);
  }
#endif

  // If the low bit of the witness is clear, it's already a metadata pointer.
  unsigned witnessIndex = assocType - reqBase;
  auto witness = ((const void* const *)wtable)[witnessIndex];
  if (LLVM_LIKELY((uintptr_t(witness) &
        ProtocolRequirementFlags::AssociatedTypeMangledNameBit) == 0)) {
    // Cached metadata pointers are always complete.
    return MetadataResponse{(const Metadata *)witness, MetadataState::Complete};
  }

  // Find the mangled name.
  const char *mangledNameBase =
    (const char *)(uintptr_t(witness) &
                   ~ProtocolRequirementFlags::AssociatedTypeMangledNameBit);

  // Check whether the mangled name has the prefix byte indicating that
  // the mangled name is relative to the protocol itself.
  bool inProtocolContext = false;
  if ((uint8_t)*mangledNameBase ==
        ProtocolRequirementFlags::AssociatedTypeInProtocolContextByte) {
    inProtocolContext = true;
    ++mangledNameBase;
  }

  // Dig out the protocol.
  const ProtocolConformanceDescriptor *conformance = wtable->Description;
  const ProtocolDescriptor *protocol = conformance->getProtocol();

  // Extract the mangled name itself.
  StringRef mangledName =
    Demangle::makeSymbolicMangledNameStringRef(mangledNameBase);

  // Demangle the associated type.
  MetadataResponse response;
  if (inProtocolContext) {
    // The protocol's Self is the only generic parameter that can occur in the
    // type.
    response =
      swift_getTypeByMangledName(request, mangledName, nullptr,
        [conformingType](unsigned depth, unsigned index) -> const Metadata * {
          if (depth == 0 && index == 0)
            return conformingType;

          return nullptr;
        },
        [&](const Metadata *type, unsigned index) -> const WitnessTable * {
          auto requirements = protocol->getRequirements();
          auto dependentDescriptor = requirements.data() + index;
          if (dependentDescriptor < requirements.begin() ||
              dependentDescriptor >= requirements.end())
            return nullptr;

          return swift_getAssociatedConformanceWitness(wtable, conformingType,
                                                       type, reqBase,
                                                       dependentDescriptor);
        }).getResponse();
  } else {
    // The generic parameters in the associated type name are those of the
    // conforming type.

    // For a class, chase the superclass chain up until we hit the
    // type that specified the conformance.
    auto originalConformingType = findConformingSuperclass(conformingType,
                                                           conformance);
    SubstGenericParametersFromMetadata substitutions(originalConformingType);
    response = swift_getTypeByMangledName(request, mangledName,
      substitutions.getGenericArgs(),
      [&substitutions](unsigned depth, unsigned index) {
        return substitutions.getMetadata(depth, index);
      },
      [&substitutions](const Metadata *type, unsigned index) {
        return substitutions.getWitnessTable(type, index);
      }).getResponse();
  }
  auto assocTypeMetadata = response.Value;

  if (!assocTypeMetadata) {
    auto conformingTypeNameInfo = swift_getTypeName(conformingType, true);
    StringRef conformingTypeName(conformingTypeNameInfo.data,
                                 conformingTypeNameInfo.length);
    StringRef assocTypeName = findAssociatedTypeName(protocol, assocType);
    fatalError(0,
               "failed to demangle witness for associated type '%s' in "
               "conformance '%s: %s' from mangled name '%s'\n",
               assocTypeName.str().c_str(),
               conformingTypeName.str().c_str(),
               protocol->Name.get(),
               mangledName.str().c_str());
  }

  assert((uintptr_t(assocTypeMetadata) &
            ProtocolRequirementFlags::AssociatedTypeMangledNameBit) == 0);

  // If the metadata was completed, record it in the witness table.
  if (response.State == MetadataState::Complete) {
    reinterpret_cast<const void**>(wtable)[witnessIndex] = assocTypeMetadata;
  }

  return response;
}

MetadataResponse
swift::swift_getAssociatedTypeWitness(MetadataRequest request,
                                      WitnessTable *wtable,
                                      const Metadata *conformingType,
                                      const ProtocolRequirement *reqBase,
                                      const ProtocolRequirement *assocType) {
  // If the low bit of the witness is clear, it's already a metadata pointer.
  unsigned witnessIndex = assocType - reqBase;
  auto witness = ((const void* const *)wtable)[witnessIndex];
  if (LLVM_LIKELY((uintptr_t(witness) &
        ProtocolRequirementFlags::AssociatedTypeMangledNameBit) == 0)) {
    // Cached metadata pointers are always complete.
    return MetadataResponse{(const Metadata *)witness, MetadataState::Complete};
  }

  return swift_getAssociatedTypeWitnessSlow(request, wtable, conformingType,
                                            reqBase, assocType);
}

SWIFT_CC
static const WitnessTable *swift_getAssociatedConformanceWitnessSlowImpl(
                                  WitnessTable *wtable,
                                  const Metadata *conformingType,
                                  const Metadata *assocType,
                                  const ProtocolRequirement *reqBase,
                                  const ProtocolRequirement *assocConformance) {
#ifndef NDEBUG
  {
    const ProtocolConformanceDescriptor *conformance = wtable->Description;
    const ProtocolDescriptor *protocol = conformance->getProtocol();
    auto requirements = protocol->getRequirements();
    assert(assocConformance >= requirements.begin() &&
           assocConformance < requirements.end());
    assert(reqBase == requirements.data() - WitnessTableFirstRequirementOffset);
    assert(
      assocConformance->Flags.getKind() ==
        ProtocolRequirementFlags::Kind::AssociatedConformanceAccessFunction ||
      assocConformance->Flags.getKind() ==
        ProtocolRequirementFlags::Kind::BaseProtocol);
  }
#endif

  // Retrieve the witness.
  unsigned witnessIndex = assocConformance - reqBase;
  auto witness = ((const void* const *)wtable)[witnessIndex];
  // Fast path: we've already resolved this to a witness table, so return it.
  if (LLVM_LIKELY((uintptr_t(witness) &
         ProtocolRequirementFlags::AssociatedTypeMangledNameBit) == 0)) {
    return static_cast<const WitnessTable *>(witness);
  }

  // Find the mangled name.
  const char *mangledNameBase =
    (const char *)(uintptr_t(witness) &
                     ~ProtocolRequirementFlags::AssociatedTypeMangledNameBit);


  // Extract the mangled name itself.
  if (*mangledNameBase == '\xFF')
    ++mangledNameBase;

  StringRef mangledName =
    Demangle::makeSymbolicMangledNameStringRef(mangledNameBase);

  // Relative reference to an associate conformance witness function.
  // FIXME: This is intended to be a temporary mangling, to be replaced
  // by a real "protocol conformance" mangling.
  if (mangledName.size() == 5 &&
      (mangledName[0] == '\x07' || mangledName[0] == '\x08')) {
    // Resolve the relative reference to the witness function.
    int32_t offset;
    memcpy(&offset, mangledName.data() + 1, 4);
    auto ptr = detail::applyRelativeOffset(mangledName.data() + 1, offset);

    // Call the witness function.
    auto witnessFn = (AssociatedWitnessTableAccessFunction *)ptr;
    auto assocWitnessTable = witnessFn(assocType, conformingType, wtable);

    assert((uintptr_t(assocWitnessTable) &
            ProtocolRequirementFlags::AssociatedTypeMangledNameBit) == 0);

    // Update the cache.
    reinterpret_cast<const void**>(wtable)[witnessIndex] = assocWitnessTable;

    return assocWitnessTable;
  }

  swift_runtime_unreachable("Invalid mangled associate conformance");
}

const WitnessTable *swift::swift_getAssociatedConformanceWitness(
                                  WitnessTable *wtable,
                                  const Metadata *conformingType,
                                  const Metadata *assocType,
                                  const ProtocolRequirement *reqBase,
                                  const ProtocolRequirement *assocConformance) {
  // Retrieve the witness.
  unsigned witnessIndex = assocConformance - reqBase;
  auto witness = ((const void* const *)wtable)[witnessIndex];
  // Fast path: we've already resolved this to a witness table, so return it.
  if (LLVM_LIKELY((uintptr_t(witness) &
         ProtocolRequirementFlags::AssociatedTypeMangledNameBit) == 0)) {
    return static_cast<const WitnessTable *>(witness);
  }

  return swift_getAssociatedConformanceWitnessSlow(wtable, conformingType,
                                                   assocType, reqBase,
                                                   assocConformance);
}

/***************************************************************************/
/*** Recursive metadata dependencies ***************************************/
/***************************************************************************/

template <class Result, class Callbacks>
static Result performOnMetadataCache(const Metadata *metadata,
                                     Callbacks &&callbacks) {
  // TODO: Once more than just structs have canonical statically specialized
  //       metadata, calling an updated
  //       isCanonicalStaticallySpecializedGenericMetadata would entail
  //       dyn_casting to the same type more than once.  Avoid that by combining
  //       that function's implementation with the dyn_casts below.
  if (metadata->isCanonicalStaticallySpecializedGenericMetadata())
    return std::move(callbacks).forOtherMetadata(metadata);

  // Handle different kinds of type that can delay their metadata.
  const TypeContextDescriptor *description;
  if (auto classMetadata = dyn_cast<ClassMetadata>(metadata)) {
    description = classMetadata->getDescription();
  } else if (auto valueMetadata = dyn_cast<ValueMetadata>(metadata)) {
    description = valueMetadata->getDescription();
  } else if (auto tupleMetadata = dyn_cast<TupleTypeMetadata>(metadata)) {
    // The empty tuple is special and doesn't belong to a metadata cache.
    if (tupleMetadata->NumElements == 0)
      return std::move(callbacks).forOtherMetadata(tupleMetadata);
    return std::move(callbacks).forTupleMetadata(tupleMetadata);
  } else if (auto foreignClass = dyn_cast<ForeignClassMetadata>(metadata)) {
    return std::move(callbacks).forForeignMetadata(foreignClass,
                                                foreignClass->getDescription());
  } else {
    return std::move(callbacks).forOtherMetadata(metadata);
  }

  if (!description->isGeneric()) {
    switch (description->getMetadataInitialization()) {
    case TypeContextDescriptorFlags::NoMetadataInitialization:
      return std::move(callbacks).forOtherMetadata(metadata);

    case TypeContextDescriptorFlags::ForeignMetadataInitialization:
      return std::move(callbacks).forForeignMetadata(metadata, description);

    case TypeContextDescriptorFlags::SingletonMetadataInitialization:
      return std::move(callbacks).forSingletonMetadata(description);
    }
    swift_runtime_unreachable("bad metadata initialization kind");
  }

  auto &generics = description->getFullGenericContextHeader();

  auto genericArgs =
    reinterpret_cast<const void * const *>(
                                    description->getGenericArguments(metadata));
  auto &cache = getCache(*description);
  size_t numGenericArgs = generics.Base.NumKeyArguments;
  assert(numGenericArgs == cache.NumKeyParameters + cache.NumWitnessTables);
  (void)numGenericArgs;
  auto key = MetadataCacheKey(cache.NumKeyParameters, cache.NumWitnessTables,
                              genericArgs);

  return std::move(callbacks).forGenericMetadata(metadata, description,
                                                 cache, key);
}

bool swift::addToMetadataQueue(MetadataCompletionQueueEntry *queueEntry,
                               MetadataDependency dependency) {
  struct EnqueueCallbacks {
    MetadataCompletionQueueEntry *QueueEntry;
    MetadataDependency Dependency;

    bool forGenericMetadata(const Metadata *metadata,
                            const TypeContextDescriptor *description,
                            GenericMetadataCache &cache,
                            MetadataCacheKey key) && {
      return cache.enqueue(key, QueueEntry, Dependency);
    }

    bool forForeignMetadata(const Metadata *metadata,
                            const TypeContextDescriptor *description) {
      return ForeignMetadata.get().enqueue(description, QueueEntry, Dependency);
    }

    bool forSingletonMetadata(const TypeContextDescriptor *description) && {
      return SingletonMetadata.get().enqueue(description, QueueEntry, Dependency);
    }

    bool forTupleMetadata(const TupleTypeMetadata *metadata) {
      return TupleTypes.get().enqueue(metadata, QueueEntry, Dependency);
    }

    bool forOtherMetadata(const Metadata *metadata) && {
      swift_runtime_unreachable("metadata should always be complete");
    }
  } callbacks = { queueEntry, dependency };

  return performOnMetadataCache<bool>(dependency.Value, std::move(callbacks));
}

void swift::resumeMetadataCompletion(MetadataCompletionQueueEntry *queueEntry) {
  struct ResumeCallbacks {
    MetadataCompletionQueueEntry *QueueEntry;

    void forGenericMetadata(const Metadata *metadata,
                            const TypeContextDescriptor *description,
                            GenericMetadataCache &cache,
                            MetadataCacheKey key) && {
      cache.resumeInitialization(key, QueueEntry);
    }

    void forForeignMetadata(const Metadata *metadata,
                            const TypeContextDescriptor *description) {
      ForeignMetadata.get().resumeInitialization(description, QueueEntry);
    }

    void forSingletonMetadata(const TypeContextDescriptor *description) && {
      SingletonMetadata.get().resumeInitialization(description, QueueEntry);
    }

    void forTupleMetadata(const TupleTypeMetadata *metadata) {
      TupleTypes.get().resumeInitialization(metadata, QueueEntry);
    }

    void forOtherMetadata(const Metadata *metadata) && {
      swift_runtime_unreachable("metadata should always be complete");
    }
  } callbacks = { queueEntry };

  auto metadata = queueEntry->Value;
  performOnMetadataCache<void>(metadata, std::move(callbacks));
}

MetadataResponse swift::swift_checkMetadataState(MetadataRequest request,
                                                 const Metadata *type) {
  struct CheckStateCallbacks {
    MetadataRequest Request;

    /// Generic types just need to be awaited.
    MetadataResponse forGenericMetadata(const Metadata *type,
                                      const TypeContextDescriptor *description,
                                      GenericMetadataCache &cache,
                                      MetadataCacheKey key) && {
      return cache.await(key, Request);
    }

    MetadataResponse forForeignMetadata(const Metadata *metadata,
                            const TypeContextDescriptor *description) {
      return ForeignMetadata.get().await(description, Request);
    }

    MetadataResponse forSingletonMetadata(
                                  const TypeContextDescriptor *description) && {
      return SingletonMetadata.get().await(description, Request);
    }

    MetadataResponse forTupleMetadata(const TupleTypeMetadata *metadata) {
      return TupleTypes.get().await(metadata, Request);
    }

    /// All other type metadata are always complete.
    MetadataResponse forOtherMetadata(const Metadata *type) && {
      return MetadataResponse{type, MetadataState::Complete};
    }
  } callbacks = { request };

  return performOnMetadataCache<MetadataResponse>(type, std::move(callbacks));
}

/// Search all the metadata that the given type has transitive completeness
/// requirements on for something that matches the given predicate.
template <class T>
static bool findAnyTransitiveMetadata(const Metadata *type, T &&predicate) {
  const TypeContextDescriptor *description;

  // Classes require their superclass to be transitively complete,
  // and they can be generic.
  if (auto classType = dyn_cast<ClassMetadata>(type)) {
    description = classType->getDescription();
    if (auto super = classType->Superclass) {
      if (super->isTypeMetadata() && predicate(super))
        return true;
    }

  // Value types can be generic.
  } else if (auto valueType = dyn_cast<ValueMetadata>(type)) {
    description = valueType->getDescription();

  // Tuples require their element types to be transitively complete.
  } else if (auto tupleType = dyn_cast<TupleTypeMetadata>(type)) {
    for (size_t i = 0, e = tupleType->NumElements; i != e; ++i)
      if (predicate(tupleType->getElement(i).Type))
        return true;

    return false;

  // Foreign classes require their superclass to be transitively complete.
  } else if (auto foreignClassType = dyn_cast<ForeignClassMetadata>(type)) {
    if (auto super = foreignClassType->Superclass) {
      if (predicate(super))
        return true;
    }
    return false;

  // Other types do not have transitive completeness requirements.
  } else {
    return false;
  }

  // Generic types require their type arguments to be transitively complete.
  if (description->isGeneric()) {
    auto &generics = description->getFullGenericContextHeader();

    auto keyArguments = description->getGenericArguments(type);
    auto extraArguments = keyArguments + generics.Base.NumKeyArguments;

    for (auto &param : description->getGenericParams()) {
      if (param.hasKeyArgument()) {
        if (predicate(*keyArguments++))
          return true;
      } else if (param.hasExtraArgument()) {
        if (predicate(*extraArguments++))
          return true;
      }
      // Ignore parameters that don't have a key or an extra argument.
    }
  }

  // Didn't find anything.
  return false;
}

/// Do a quick check to see if all the transitive type metadata are complete.
static bool
areAllTransitiveMetadataComplete_cheap(const Metadata *type) {
  // Look for any transitive metadata that's *incomplete*.
  return !findAnyTransitiveMetadata(type, [](const Metadata *type) {
    struct IsIncompleteCallbacks {
      bool forGenericMetadata(const Metadata *type,
                              const TypeContextDescriptor *description,
                              GenericMetadataCache &cache,
                              MetadataCacheKey key) && {
        // Metadata cache lookups aren't really cheap enough for this
        // optimization.
        return true;
      }

      bool forForeignMetadata(const Metadata *metadata,
                              const TypeContextDescriptor *description) {
        // If the type doesn't have a completion function, we can assume
        // it's transitively complete by construction.
        if (!description->getForeignMetadataInitialization().CompletionFunction)
          return false;

        // TODO: it might be worth doing a quick check against the cache here.
        return false;
      }

      bool forSingletonMetadata(const TypeContextDescriptor *description) && {
        // TODO: this could be cheap enough.
        return true;
      }

      bool forTupleMetadata(const TupleTypeMetadata *metadata) {
        // TODO: this could be cheap enough.
        return true;
      }

      bool forOtherMetadata(const Metadata *type) && {
        return false;
      }
    } callbacks;

    return performOnMetadataCache<bool>(type, std::move(callbacks));
  });
}

/// Check for transitive completeness.
///
/// The key observation here is that all we really care about is whether
/// the transitively-observable types are *actually* all complete; we don't
/// need them to *think* they're transitively complete.  So if we find
/// something that thinks it's still transitively incomplete, we can just
/// scan its transitive metadata and actually try to find something that's
/// incomplete.  If we don't find anything, then we know all the transitive
/// dependencies actually hold, and we can keep going.
static MetadataDependency
checkTransitiveCompleteness(const Metadata *initialType) {
  llvm::SmallVector<const Metadata *, 8> worklist;
  
  // An efficient hash-set implementation in the spirit of llvm's SmallPtrSet:
  // The first 8 elements are stored in an inline-allocated array to avoid
  // malloc calls in the common case. Lookup is still reasonable fast because
  // there are max 8 elements in the array.
  const int InlineCapacity = 8;
  const Metadata *inlinedPresumedCompleteTypes[InlineCapacity];
  int numInlinedTypes = 0;
  std::unordered_set<const Metadata *> overflowPresumedCompleteTypes;

  MetadataDependency dependency;
  auto isIncomplete = [&](const Metadata *type) -> bool {
    // Add the type to the presumed-complete-types set.  If this doesn't
    // succeed, we've already inserted it, which means we must have already
    // decided it was complete.
    // First, try to find the type in the inline-storage of the set.
    const Metadata **end = inlinedPresumedCompleteTypes + numInlinedTypes;
    if (std::find(inlinedPresumedCompleteTypes, end, type) != end)
      return false;

    // We didn't find the type in the inline-storage.
    if (numInlinedTypes < InlineCapacity) {
      assert(overflowPresumedCompleteTypes.size() == 0);
      inlinedPresumedCompleteTypes[numInlinedTypes++] = type;
    } else {
      // The inline-storage is full. So try to insert the type into the
      // overflow set.
      if (!overflowPresumedCompleteTypes.insert(type).second)
        return false;
    }

    // Check the metadata's current state with a non-blocking request.
    auto request = MetadataRequest(MetadataState::Complete,
                                   /*non-blocking*/ true);
    auto state = swift_checkMetadataState(request, type).State;

    // If it's transitively complete, we're done.
    // This is the most likely result.
    if (state == MetadataState::Complete)
      return false;

    // Otherwise, if the state is actually incomplete, set a dependency
    // and leave.  We set the dependency at non-transitive completeness
    // because we can potentially resolve ourselves if we find completeness.
    if (!isAtLeast(state, MetadataState::NonTransitiveComplete)) {
      dependency = MetadataDependency{type,
                                      MetadataState::NonTransitiveComplete};
      return true;
    }

    // Otherwise, we have to add it to the worklist.
    worklist.push_back(type);
    return false;
  };

  // Consider the type itself to be presumed-complete.  We're looking for
  // a greatest fixed point.
  assert(numInlinedTypes == 0 && overflowPresumedCompleteTypes.size() == 0);
  inlinedPresumedCompleteTypes[0] = initialType;
  numInlinedTypes = 1;
  if (findAnyTransitiveMetadata(initialType, isIncomplete))
    return dependency;

  // Drain the worklist.  The order we do things in doesn't really matter,
  // so optimize for locality and convenience by using a stack.
  while (!worklist.empty()) {
    auto type = worklist.back();
    worklist.pop_back();

    // Search for incomplete dependencies.  This will set Dependency
    // if it finds anything.
    if (findAnyTransitiveMetadata(type, isIncomplete))
      return dependency;
  }

  // Otherwise, we're transitively complete.
  return { nullptr, MetadataState::Complete };
}

/// Diagnose a metadata dependency cycle.
LLVM_ATTRIBUTE_NORETURN
static void diagnoseMetadataDependencyCycle(const Metadata *start,
                                            ArrayRef<MetadataDependency> links){
  assert(start == links.back().Value);

  std::string diagnostic =
    "runtime error: unresolvable type metadata dependency cycle detected\n  ";
  diagnostic += nameForMetadata(start);

  for (auto &link : links) {
    // If the diagnostic gets too large, just cut it short.
    if (diagnostic.size() >= 128 * 1024) {
      diagnostic += "\n  (limiting diagnostic text at 128KB)";
      break;
    }

    diagnostic += "\n  depends on ";

    switch (link.Requirement) {
    case MetadataState::Complete:
      diagnostic += "transitive completion of ";
      break;
    case MetadataState::NonTransitiveComplete:
      diagnostic += "completion of ";
      break;
    case MetadataState::LayoutComplete:
      diagnostic += "layout of ";
      break;
    case MetadataState::Abstract:
      diagnostic += "<corruption> of ";
      break;
    }

    diagnostic += nameForMetadata(link.Value);
  }

  diagnostic += "\nAborting!\n";

  if (_swift_shouldReportFatalErrorsToDebugger()) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc99-extensions"
    RuntimeErrorDetails details = {
      .version = RuntimeErrorDetails::currentVersion,
      .errorType = "type-metadata-cycle",
      .currentStackDescription = "fetching metadata", // TODO?
      .framesToSkip = 1, // skip out to the check function
      .memoryAddress = start
      // TODO: describe the cycle using notes instead of one huge message?
    };
#pragma GCC diagnostic pop

    _swift_reportToDebugger(RuntimeErrorFlagFatal, diagnostic.c_str(),
                            &details);
  }

  fatalError(0, diagnostic.c_str());
}

/// Check whether the given metadata dependency is satisfied, and if not,
/// return its current dependency, if one exists.
static MetadataDependency
checkMetadataDependency(MetadataDependency dependency) {
  struct IsIncompleteCallbacks {
    MetadataState Requirement;
    MetadataDependency forGenericMetadata(const Metadata *type,
                                          const TypeContextDescriptor *desc,
                                          GenericMetadataCache &cache,
                                          MetadataCacheKey key) && {
      return cache.checkDependency(key, Requirement);
    }

    MetadataDependency forForeignMetadata(const Metadata *metadata,
                                    const TypeContextDescriptor *description) {
      return ForeignMetadata.get().checkDependency(description, Requirement);
    }

    MetadataDependency forSingletonMetadata(
                              const TypeContextDescriptor *description) && {
      return SingletonMetadata.get().checkDependency(description, Requirement);
    }

    MetadataDependency forTupleMetadata(const TupleTypeMetadata *metadata) {
      return TupleTypes.get().checkDependency(metadata, Requirement);
    }

    MetadataDependency forOtherMetadata(const Metadata *type) && {
      return MetadataDependency();
    }
  } callbacks = { dependency.Requirement };

  return performOnMetadataCache<MetadataDependency>(dependency.Value,
                                                    std::move(callbacks));
}

/// Check for an unbreakable metadata-dependency cycle.
void swift::checkMetadataDependencyCycle(const Metadata *startMetadata,
                                         MetadataDependency firstLink,
                                         MetadataDependency secondLink) {
  std::vector<MetadataDependency> links;
  auto checkNewLink = [&](MetadataDependency newLink) {
    links.push_back(newLink);
    if (newLink.Value == startMetadata)
      diagnoseMetadataDependencyCycle(startMetadata, links);
    for (auto i = links.begin(), e = links.end() - 1; i != e; ++i) {
      if (i->Value == newLink.Value) {
        auto next = i + 1;
        diagnoseMetadataDependencyCycle(i->Value,
                                llvm::makeArrayRef(&*next, links.end() - next));
      }
    }
  };

  checkNewLink(firstLink);
  checkNewLink(secondLink);
  while (true) {
    auto newLink = checkMetadataDependency(links.back());
    if (!newLink) return;
    checkNewLink(newLink);
  }
}

/***************************************************************************/
/*** Allocator implementation **********************************************/
/***************************************************************************/

namespace {
  struct alignas(sizeof(uintptr_t) * 2) PoolRange {
    static constexpr uintptr_t PageSize = 16 * 1024;
    static constexpr uintptr_t MaxPoolAllocationSize = PageSize / 2;

    /// The start of the allocation.
    char *Begin;

    /// The number of bytes remaining.
    size_t Remaining;
  };
} // end anonymous namespace

// A statically-allocated pool.  It's zero-initialized, so this
// doesn't cost us anything in binary size.
alignas(void *) static char InitialAllocationPool[64 * 1024];
static std::atomic<PoolRange>
AllocationPool{PoolRange{InitialAllocationPool,
                         sizeof(InitialAllocationPool)}};

void *MetadataAllocator::Allocate(size_t size, size_t alignment) {
  assert(alignment <= alignof(void*));
  assert(size % alignof(void*) == 0);

  // If the size is larger than the maximum, just use malloc.
  if (size > PoolRange::MaxPoolAllocationSize)
    return malloc(size);

  // Allocate out of the pool.
  PoolRange curState = AllocationPool.load(std::memory_order_relaxed);
  while (true) {
    char *allocation;
    PoolRange newState;
    bool allocatedNewPage;

    // Try to allocate out of the current page.
    if (size <= curState.Remaining) {
      allocatedNewPage = false;
      allocation = curState.Begin;
      newState = PoolRange{curState.Begin + size, curState.Remaining - size};
    } else {
      allocatedNewPage = true;
      allocation = new char[PoolRange::PageSize];
      newState = PoolRange{allocation + size, PoolRange::PageSize - size};
      __asan_poison_memory_region(allocation, PoolRange::PageSize);
    }

    // Swap in the new state.
    if (std::atomic_compare_exchange_weak_explicit(&AllocationPool,
                                                   &curState, newState,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
      // If that succeeded, we've successfully allocated.
      __msan_allocated_memory(allocation, size);
      __asan_unpoison_memory_region(allocation, size);
      return allocation;
    }

    // If it failed, go back to a neutral state and try again.
    if (allocatedNewPage) {
      delete[] allocation;
    }
  }
}

void MetadataAllocator::Deallocate(const void *allocation, size_t size) {
  __asan_poison_memory_region(allocation, size);

  if (size > PoolRange::MaxPoolAllocationSize) {
    free(const_cast<void*>(allocation));
    return;
  }

  // Check whether the allocation pool is still in the state it was in
  // immediately after the given allocation.
  PoolRange curState = AllocationPool.load(std::memory_order_relaxed);
  if (reinterpret_cast<const char*>(allocation) + size != curState.Begin) {
    return;
  }

  // Try to swap back to the pre-allocation state.  If this fails,
  // don't bother trying again; we'll just leak the allocation.
  PoolRange newState = { reinterpret_cast<char*>(const_cast<void*>(allocation)),
                         curState.Remaining + size };
  (void)
    std::atomic_compare_exchange_strong_explicit(&AllocationPool,
                                                 &curState, newState,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed);
}

void *swift::allocateMetadata(size_t size, size_t alignment) {
  return MetadataAllocator().Allocate(size, alignment);
}

template<>
bool Metadata::satisfiesClassConstraint() const {
  // existential types marked with @objc satisfy class requirement.
  if (auto *existential = dyn_cast<ExistentialTypeMetadata>(this))
    return existential->isObjC();

  // or it's a class.
  return isAnyClass();
}

template <>
bool Metadata::isCanonicalStaticallySpecializedGenericMetadata() const {
  if (auto *metadata = dyn_cast<StructMetadata>(this))
    return metadata->isCanonicalStaticallySpecializedGenericMetadata();
  if (auto *metadata = dyn_cast<EnumMetadata>(this))
    return metadata->isCanonicalStaticallySpecializedGenericMetadata();

  return false;
}

#if !NDEBUG
static bool referencesAnonymousContext(Demangle::Node *node) {
  if (node->getKind() == Demangle::Node::Kind::AnonymousContext)
    return true;
  for (unsigned i = 0, e = node->getNumChildren(); i < e; ++i)
    if (referencesAnonymousContext(node->getChild(i)))
      return true;

  return false;
}

void swift::verifyMangledNameRoundtrip(const Metadata *metadata) {
  // Enable verification when a special environment variable is set.
  // Some metatypes crash when going through the mangler or demangler. A
  // lot of tests currently trigger those crashes, resulting in failing
  // tests which are still usefully testing something else. This
  // variable lets us easily turn on verification to find and fix these
  // bugs. Remove this and leave it permanently on once everything works
  // with it enabled.
  bool verificationEnabled =
    SWIFT_LAZY_CONSTANT((bool)getenv("SWIFT_ENABLE_MANGLED_NAME_VERIFICATION"));
  
  if (!verificationEnabled) return;
  
  Demangle::StackAllocatedDemangler<1024> Dem;
  auto node = _swift_buildDemanglingForMetadata(metadata, Dem);
  // If the mangled node involves types in an AnonymousContext, then by design,
  // it cannot be looked up by name.
  if (referencesAnonymousContext(node))
    return;
  
  auto mangledName = Demangle::mangleNode(node);
  auto result =
    swift_getTypeByMangledName(MetadataState::Abstract,
                          mangledName,
                          nullptr,
                          [](unsigned, unsigned){ return nullptr; },
                          [](const Metadata *, unsigned) { return nullptr; })
      .getMetadata();
  if (metadata != result)
    swift::warning(RuntimeErrorFlagNone,
                   "Metadata mangled name failed to roundtrip: %p -> %s -> %p\n",
                   metadata, mangledName.c_str(), (const Metadata *)result);
}
#endif

const TypeContextDescriptor *swift::swift_getTypeContextDescriptor(const Metadata *type) {
    return type->getTypeContextDescriptor();
}

// Emit compatibility override shims for keypath runtime functionality. The
// implementation of these functions is in the standard library in
// KeyPath.swift.

SWIFT_RUNTIME_STDLIB_SPI
const HeapObject *swift_getKeyPathImpl(const void *pattern,
                                       const void *arguments);

#define OVERRIDE_KEYPATH COMPATIBILITY_OVERRIDE
#define OVERRIDE_WITNESSTABLE COMPATIBILITY_OVERRIDE
#include "CompatibilityOverride.def"

#if defined(_WIN32) && defined(_M_ARM64)
namespace std {
template <>
inline void _Atomic_storage<::PoolRange, 16>::_Unlock() const noexcept {
  __dmb(0x8);
  __iso_volatile_store32(&reinterpret_cast<volatile int &>(_Spinlock), 0);
  __dmb(0x8);
}
}
#endif
