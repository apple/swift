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

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// Avoid defining macro max(), min() which conflict with std::max(), std::min()
#define NOMINMAX
#include <windows.h>
#endif

#include "MetadataCache.h"
#include "BytecodeLayouts.h"
#include "swift/ABI/TypeIdentity.h"
#include "swift/Basic/Lazy.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Demangling/Demangler.h"
#include "swift/RemoteInspection/GenericMetadataCacheEntry.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/EnvironmentVariables.h"
#include "swift/Runtime/ExistentialContainer.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/Once.h"
#include "swift/Runtime/Portability.h"
#include "swift/Strings.h"
#include "swift/Threading/Mutex.h"
#include "llvm/ADT/StringExtras.h"
#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <condition_variable>
#include <new>
#include <unordered_set>
#include <vector>
#if SWIFT_PTRAUTH
#include <ptrauth.h>
#endif
#if SWIFT_OBJC_INTEROP
extern "C" void _objc_setClassCopyFixupHandler(void (* _Nonnull newFixupHandler)
    (Class _Nonnull oldClass, Class _Nonnull newClass));
#endif
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "../CompatibilityOverride/CompatibilityOverride.h"
#include "ErrorObject.h"
#include "ExistentialMetadataImpl.h"
#include "swift/Runtime/Debug.h"
#include "Private.h"

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

#if defined(__APPLE__)
// Binaries using noncopyable types check the address of the symbol
// `swift_runtimeSupportsNoncopyableTypes` before exposing any noncopyable
// type metadata through in-process reflection, to prevent existing code
// that expects all types to be copyable from crashing or causing bad behavior
// by copying noncopyable types. The runtime does not yet support noncopyable
// types, so we explicitly define this symbol to be zero for now. Binaries
// weak-import this symbol so they will resolve it to a zero address on older
// runtimes as well.
__asm__("  .globl _swift_runtimeSupportsNoncopyableTypes\n");
__asm__(".set _swift_runtimeSupportsNoncopyableTypes, 0\n");
#endif

// GenericParamDescriptor is a single byte, so while it's difficult to
// imagine needing even a quarter this many generic params, there's very
// little harm in doing it.
const GenericParamDescriptor
swift::ImplicitGenericParamDescriptors[MaxNumImplicitGenericParamDescriptors] = {
#define D GenericParamDescriptor::implicit()
  D,D,D,D, D,D,D,D, D,D,D,D, D,D,D,D, D,D,D,D, D,D,D,D, D,D,D,D, D,D,D,D,
  D,D,D,D, D,D,D,D, D,D,D,D, D,D,D,D, D,D,D,D, D,D,D,D, D,D,D,D, D,D,D,D
#undef D
};
static_assert(MaxNumImplicitGenericParamDescriptors == 64, "length mismatch");

static ClassMetadata *
_swift_relocateClassMetadata(const ClassDescriptor *description,
                             const ResilientClassMetadataPattern *pattern);

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
    return _swift_relocateClassMetadata(classDescription, pattern);
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
    auto *fullMetadata = asFullMetadata(metadata);

    // Begin by initializing the value witness table; everything else is
    // initialized by swift_initClassMetadata().
#if SWIFT_OBJC_INTEROP
    auto *classMetadata = static_cast<ClassMetadata*>(metadata);
    classMetadata->setAsTypeMetadata();

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

void MetadataCacheKey::installGenericArguments(
    uint16_t numKeyArguments,
    uint16_t numPacks,
    const GenericPackShapeDescriptor *PackShapeDescriptors,
    const void **dst, const void * const *src) {
  memcpy(dst, src, numKeyArguments * sizeof(void *));

  // If we don't have any pack arguments, there is nothing more to do.
  if (numPacks == 0)
    return;

  // Heap-allocate all installed metadata and witness table packs.
  for (unsigned i = 0; i < numPacks; ++i) {
    auto pack = PackShapeDescriptors[i];
    size_t count = reinterpret_cast<size_t>(dst[pack.ShapeClass]);

    switch (pack.Kind) {
    case GenericPackKind::Metadata:
      dst[pack.Index] = swift_allocateMetadataPack(
          reinterpret_cast<const Metadata * const *>(dst[pack.Index]),
          count);
      break;

    case GenericPackKind::WitnessTable:
      dst[pack.Index] = swift_allocateWitnessTablePack(
          reinterpret_cast<const WitnessTable * const *>(dst[pack.Index]),
          count);
      break;
    }
  }
}

/// Copy the generic arguments into place in a newly-allocated metadata.
static void installGenericArguments(Metadata *metadata,
                                    const TypeContextDescriptor *description,
                                    const void *arguments) {
  const auto &genericContext = *description->getGenericContext();
  const auto &header = genericContext.getGenericContextHeader();

  auto dst = (reinterpret_cast<const void **>(metadata) +
              description->getGenericArgumentOffset());
  auto src = reinterpret_cast<const void * const *>(arguments);

  auto packShapeHeader = genericContext.getGenericPackShapeHeader();

  MetadataCacheKey::installGenericArguments(
      header.NumKeyArguments,
      packShapeHeader.NumPacks,
      genericContext.getGenericPackShapeDescriptors().data(),
      dst, src);
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
    auto description = *reinterpret_cast<const ClassDescriptor * const __ptrauth_swift_type_descriptor *>(ref);
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
  swift_unreachable("unsupported superclass reference kind");
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

    // The constructor/allocate operations that take a `const Metadata *`
    // are used for the insertion of canonical specializations.
    // The metadata is always complete after construction.

    GenericCacheEntry(MetadataCacheKey key,
                      MetadataWaitQueue::Worker &worker,
                      MetadataRequest request,
                      const Metadata *candidate)
      : VariadicMetadataCacheEntryBase(key, worker,
                                       PrivateMetadataState::Complete,
                                       const_cast<Metadata*>(candidate)) {}

    AllocationResult allocate(const Metadata *candidate) {
      swift_unreachable("always short-circuited");
    }

    static bool allowMangledNameVerification(const Metadata *candidate) {
      // Disallow mangled name verification for specialized candidates
      // because it will trigger recursive entry into the swift_once
      // in cacheCanonicalSpecializedMetadata.
      // TODO: verify mangled names in a second pass in that function.
      return false;
    }

    // The constructor/allocate operations that take a descriptor
    // and arguments are used along the normal allocation path.

    GenericCacheEntry(MetadataCacheKey key,
                      MetadataWaitQueue::Worker &worker,
                      MetadataRequest request,
                      const TypeContextDescriptor *description,
                      const void * const *arguments)
      : VariadicMetadataCacheEntryBase(key, worker,
                                       PrivateMetadataState::Allocating,
                                       /*candidate*/ nullptr) {}

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

    static bool allowMangledNameVerification(
                                  const TypeContextDescriptor *description,
                                             const void * const *arguments) {
      return true;
    }

    MetadataStateWithDependency tryInitialize(Metadata *metadata,
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

namespace swift {
  struct StaticAssertGenericMetadataCacheEntryValueOffset {
    static_assert(
      offsetof(GenericCacheEntry, Value) ==
      offsetof(swift::GenericMetadataCacheEntry<InProcess::StoredPointer>,
               Value),
      "The generic metadata cache entry layout mismatch");
  };
}

namespace {
  class GenericMetadataCache :
    public MetadataCache<GenericCacheEntry, GenericMetadataCacheTag> {
  public:
    GenericSignatureLayout<InProcess> SigLayout;

    GenericMetadataCache(const TargetGenericContext<InProcess> &genericContext)
      : SigLayout(genericContext.getGenericSignature()) {
    }
  };

  using LazyGenericMetadataCache = Lazy<GenericMetadataCache>;

  class GlobalMetadataCacheEntry {
  public:
    const TypeContextDescriptor *Description;
    GenericMetadataCache Cache;

    GlobalMetadataCacheEntry(const TypeContextDescriptor *description)
        : Description(description), Cache(*description->getGenericContext()) {}

    intptr_t getKeyIntValueForDump() {
      return reinterpret_cast<intptr_t>(Description);
    }
    bool matchesKey(const TypeContextDescriptor *description) const {
      return description == Description;
    }
    friend llvm::hash_code hash_value(const GlobalMetadataCacheEntry &value) {
      return llvm::hash_value(value.Description);
    }
    static size_t
    getExtraAllocationSize(const TypeContextDescriptor *description) {
      return 0;
    }
    size_t getExtraAllocationSize() const { return 0; }
  };

  static SimpleGlobalCache<GlobalMetadataCacheEntry, GlobalMetadataCacheTag>
      GlobalMetadataCache;

} // end anonymous namespace

/// Fetch the metadata cache for a generic metadata structure.
static GenericMetadataCache &getCache(
                               const TypeContextDescriptor &description) {
  auto &generics = description.getFullGenericContextHeader();

  // Keep this assert even if you change the representation above.
  static_assert(sizeof(LazyGenericMetadataCache) <=
                sizeof(GenericMetadataInstantiationCache::PrivateData),
                "metadata cache is larger than the allowed space");

  auto *cacheStorage = generics.getInstantiationCache();
  if (cacheStorage == nullptr) {
    return GlobalMetadataCache.getOrInsert(&description).first->Cache;
  }

  auto lazyCache =
    reinterpret_cast<LazyGenericMetadataCache*>(
      generics.getInstantiationCache()->PrivateData);
  return lazyCache->getWithInit(*description.getGenericContext());
}

#if SWIFT_PTRAUTH && SWIFT_OBJC_INTEROP
// See [NOTE: Dynamic-subclass-KVO]
static void swift_objc_classCopyFixupHandler(Class oldClass, Class newClass) {
  auto oldClassMetadata = reinterpret_cast<const ClassMetadata *>(oldClass);

  // Bail out if this isn't a Swift.
  if (!oldClassMetadata->isTypeMetadata())
   return;

  // Copy the value witness table pointer for pointer authentication.
  auto newClassMetadata = reinterpret_cast<ClassMetadata *>(newClass);
  newClassMetadata->setValueWitnesses(oldClassMetadata->getValueWitnesses());

 // Otherwise, re-sign v-table entries using the extra discriminators stored
 // in the v-table descriptor.

  auto *srcWords = reinterpret_cast<void **>(oldClass);
  auto *dstWords = reinterpret_cast<void **>(newClass);

  while (oldClassMetadata && oldClassMetadata->isTypeMetadata()) {
    const auto *description = oldClassMetadata->getDescription();

    // Copy the vtable entries.
    if (description && description->hasVTable()) {
      auto *vtable = description->getVTableDescriptor();
      auto descriptors = description->getMethodDescriptors();
      auto src = srcWords + vtable->getVTableOffset(description);
      auto dest = dstWords + vtable->getVTableOffset(description);
      for (size_t i = 0, e = vtable->VTableSize; i != e; ++i) {
        swift_ptrauth_copy_code_or_data(
            reinterpret_cast<void **>(&dest[i]),
            reinterpret_cast<void *const *>(&src[i]),
            descriptors[i].Flags.getExtraDiscriminator(),
            !descriptors[i].Flags.isAsync(),
            /*allowNull*/ true); // NULL allowed for VFE (methods in the vtable
                                 // might be proven unused and null'ed)
      }
    }

    oldClassMetadata = oldClassMetadata->Superclass;
  }
}

SWIFT_ALLOWED_RUNTIME_GLOBAL_CTOR_BEGIN
static bool fixupHandlerInstaller = [] {
  _objc_setClassCopyFixupHandler(&swift_objc_classCopyFixupHandler);
  return true;
}();
SWIFT_ALLOWED_RUNTIME_GLOBAL_CTOR_END
#endif

#if SWIFT_OBJC_INTEROP
extern "C" void *_objc_empty_cache;
#endif

template <> bool Metadata::isStaticallySpecializedGenericMetadata() const {
  if (auto *metadata = dyn_cast<StructMetadata>(this))
    return metadata->isStaticallySpecializedGenericMetadata();
  if (auto *metadata = dyn_cast<EnumMetadata>(this))
    return metadata->isStaticallySpecializedGenericMetadata();
  if (auto *metadata = dyn_cast<ClassMetadata>(this))
    return metadata->isStaticallySpecializedGenericMetadata();

  return false;
}

template <> const TypeContextDescriptor *Metadata::getDescription() const {
  if (auto *metadata = dyn_cast<StructMetadata>(this))
    return metadata->getDescription();
  if (auto *metadata = dyn_cast<EnumMetadata>(this))
    return metadata->getDescription();
  if (auto *metadata = dyn_cast<ClassMetadata>(this))
    return metadata->getDescription();

  return nullptr;
}

template <>
bool Metadata::isCanonicalStaticallySpecializedGenericMetadata() const {
  if (auto *metadata = dyn_cast<StructMetadata>(this))
    return metadata->isCanonicalStaticallySpecializedGenericMetadata();
  if (auto *metadata = dyn_cast<EnumMetadata>(this))
    return metadata->isCanonicalStaticallySpecializedGenericMetadata();
  if (auto *metadata = dyn_cast<ClassMetadata>(this))
    return metadata->isCanonicalStaticallySpecializedGenericMetadata();

  return false;
}

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
  description = swift_auth_data_non_address(
    description, SpecialPointerAuthDiscriminators::TypeDescriptor);

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
    MetadataAllocator(GenericClassMetadataTag)
      .Allocate(allocationBounds.getTotalSizeInBytes(), alignof(void*));

  auto addressPoint = bytes + allocationBounds.getAddressPointInBytes();
  auto metadata = reinterpret_cast<ClassMetadata *>(addressPoint);

  initializeClassMetadataFromPattern(metadata, bounds, description, pattern);

  assert(metadata->isTypeMetadata());

  // Copy the generic arguments into place.
  installGenericArguments(metadata, description, arguments);

  return metadata;
}

ClassMetadata *
swift::swift_allocateGenericClassMetadataWithLayoutString(
    const ClassDescriptor *description,
    const void *arguments,
    const GenericClassMetadataPattern *pattern) {
  return swift::swift_allocateGenericClassMetadata(description,
                                                   arguments,
                                                   pattern);
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
    // [pre-5.3-extra-data-zeroing] Before Swift 5.3, the runtime did not
    // correctly zero the zero-prefix of the extra-data pattern.
    memset(metadataExtraData, 0,
           size_t(extraDataPattern->OffsetInWords) * sizeof(void *));

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
  description = swift_auth_data_non_address(description, SpecialPointerAuthDiscriminators::TypeDescriptor);

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
  assert(!pattern->hasExtraDataPattern() ||
         (extraDataSize == (pattern->getExtraDataPattern()->OffsetInWords +
                            pattern->getExtraDataPattern()->SizeInWords) *
                               sizeof(void *)));

  size_t totalSize = sizeof(FullMetadata<ValueMetadata>) + extraDataSize;

  auto bytes = (char*) MetadataAllocator(GenericValueMetadataTag)
    .Allocate(totalSize, alignof(void*));

  auto addressPoint = bytes + sizeof(ValueMetadata::HeaderType);
  auto metadata = reinterpret_cast<ValueMetadata *>(addressPoint);

  initializeValueMetadataFromPattern(metadata, description, pattern);

  // Copy the generic arguments into place.
  installGenericArguments(metadata, description, arguments);

  return metadata;
}

ValueMetadata *
swift::swift_allocateGenericValueMetadataWithLayoutString(
    const ValueTypeDescriptor *description,
    const void *arguments,
    const GenericValueMetadataPattern *pattern,
    size_t extraDataSize) {
  return swift::swift_allocateGenericValueMetadata(description,
                                                   arguments,
                                                   pattern,
                                                   extraDataSize);
}

// Look into the canonical prespecialized metadata attached to the type
// descriptor and add them to the metadata cache.
static void
_cacheCanonicalSpecializedMetadata(const TypeContextDescriptor *description) {
  auto &cache = getCache(*description);
  auto request =
      MetadataRequest(MetadataState::Complete, /*isNonBlocking*/ true);
  assert(description->getFullGenericContextHeader().Base.NumKeyArguments ==
         cache.SigLayout.sizeInWords());
  if (auto *classDescription = dyn_cast<ClassDescriptor>(description)) {
    auto canonicalMetadataAccessors = classDescription->getCanonicalMetadataPrespecializationAccessors();
    for (auto &canonicalMetadataAccessorPtr : canonicalMetadataAccessors) {
      auto *canonicalMetadataAccessor = canonicalMetadataAccessorPtr.get();
      auto response = canonicalMetadataAccessor(request);
      auto *canonicalMetadata = response.Value;
      const void *const *arguments =
          reinterpret_cast<const void *const *>(canonicalMetadata->getGenericArgs());
      auto key = MetadataCacheKey(cache.SigLayout, arguments);
      auto result = cache.getOrInsert(key, MetadataRequest(MetadataState::Complete, /*isNonBlocking*/true), canonicalMetadata);
      (void)result;
      assert(result.second.Value == canonicalMetadata);
    }
  } else {
    auto canonicalMetadatas = description->getCanonicalMetadataPrespecializations();
    for (auto &canonicalMetadataPtr : canonicalMetadatas) {
      Metadata *canonicalMetadata = canonicalMetadataPtr.get();
      const void *const *arguments =
          reinterpret_cast<const void *const *>(canonicalMetadata->getGenericArgs());
      auto key = MetadataCacheKey(cache.SigLayout, arguments);
      auto result = cache.getOrInsert(key, MetadataRequest(MetadataState::Complete, /*isNonBlocking*/true), canonicalMetadata);
      (void)result;
      assert(result.second.Value == canonicalMetadata);
    }
  }
}

static void
cacheCanonicalSpecializedMetadata(const TypeContextDescriptor *description,
                                  swift_once_t *token) {
  swift::once(
      *token,
      [](void *uncastDescription) {
        auto *description = (const TypeContextDescriptor *)uncastDescription;
        _cacheCanonicalSpecializedMetadata(description);
      },
      (void *)description);
}

MetadataResponse swift::swift_getCanonicalSpecializedMetadata(
    MetadataRequest request, const Metadata *candidate,
    const Metadata **cacheMetadataPtr) {
  assert(candidate->isStaticallySpecializedGenericMetadata() &&
         !candidate->isCanonicalStaticallySpecializedGenericMetadata());
  auto *description = candidate->getDescription();
  assert(description);

  using CachedMetadata = std::atomic<const Metadata *>;
  auto cachedMetadataAddr = ((CachedMetadata *)cacheMetadataPtr);
  auto *cachedMetadata = cachedMetadataAddr->load(SWIFT_MEMORY_ORDER_CONSUME);
  if (SWIFT_LIKELY(cachedMetadata != nullptr)) {
    // Cached metadata pointers are always complete.
    return MetadataResponse{(const Metadata *)cachedMetadata,
                            MetadataState::Complete};
  }

  if (auto *token =
          description
              ->getCanonicalMetadataPrespecializationCachingOnceToken()) {
    cacheCanonicalSpecializedMetadata(description, token);
    // NOTE: If there is no token, then there are no canonical prespecialized
    //       metadata records, either.
  }
  const void *const *arguments =
      reinterpret_cast<const void *const *>(candidate->getGenericArgs());
  auto &cache = getCache(*description);
  auto key = MetadataCacheKey(cache.SigLayout, arguments);
  auto result = cache.getOrInsert(key, request, candidate);

  cachedMetadataAddr->store(result.second.Value, std::memory_order_release);

  return result.second;
}

SWIFT_CC(swift)
static MetadataResponse
_swift_getGenericMetadata(MetadataRequest request, const void *const *arguments,
                          const TypeContextDescriptor *description) {
  auto &cache = getCache(*description);
  assert(description->getFullGenericContextHeader().Base.NumKeyArguments ==
         cache.SigLayout.sizeInWords());
  auto key = MetadataCacheKey(cache.SigLayout, arguments);
  auto result = cache.getOrInsert(key, request, description, arguments);

  return result.second;
}

/// The primary entrypoint.
MetadataResponse
swift::swift_getGenericMetadata(MetadataRequest request,
                                const void *const *arguments,
                                const TypeContextDescriptor *description) {
  description = swift_auth_data_non_address(
      description, SpecialPointerAuthDiscriminators::TypeDescriptor);
  return _swift_getGenericMetadata(request, arguments, description);
}

MetadataResponse swift::swift_getCanonicalPrespecializedGenericMetadata(
    MetadataRequest request, const void *const *arguments,
    const TypeContextDescriptor *description, swift_once_t *token) {
  description = swift_auth_data_non_address(
      description, SpecialPointerAuthDiscriminators::TypeDescriptor);
  cacheCanonicalSpecializedMetadata(description, token);

  return _swift_getGenericMetadata(request, arguments, description);
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

    SingletonMetadataCacheEntry(MetadataWaitQueue::Worker &worker,
                                MetadataRequest request,
                                const TypeContextDescriptor *description)
      : MetadataCacheEntryBase(worker) {}

    AllocationResult allocate(const TypeContextDescriptor *description) {
      auto &initialization = description->getSingletonMetadataInitialization();

      // Classes with resilient superclasses might require their metadata to
      // be relocated.
      auto metadata = initialization.allocate(description);

      auto state = inferStateForMetadata(metadata);
      return { metadata, state };
    }

    MetadataStateWithDependency tryInitialize(Metadata *metadata,
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
        auto allocatedEntry = swift_cxx_newObject<SingletonMetadataCacheEntry>(
            std::forward<ArgTys>(args)...);
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

    bool matchesKey(const ClassMetadata *theClass) const {
      return theClass == Data.Class;
    }

    friend llvm::hash_code hash_value(const ObjCClassCacheEntry &value) {
      return llvm::hash_value(value.Data.Class);
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
static SimpleGlobalCache<ObjCClassCacheEntry, ObjCClassWrappersTag>
  ObjCClassWrappers;

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
  // We're not supposed to accept NULL, but older runtimes somehow did as a
  // side effect of UB in dyn_cast, so we'll keep that going.
  if (!theMetadata)
    return nullptr;

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
  // We're not supposed to accept NULL, but older runtimes somehow did as a
  // side effect of UB in dyn_cast, so we'll keep that going.
  if (!theMetadata)
    return nullptr;

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
/*** Metadata and witness table packs **************************************/
/***************************************************************************/

namespace {

template<typename PackType>
class PackCacheEntry {
public:
  size_t Count;

  const PackType * const * getElements() const {
    return reinterpret_cast<const PackType * const *>(this + 1);
  }

  const PackType ** getElements() {
    return reinterpret_cast<const PackType **>(this + 1);
  }

  struct Key {
    const PackType *const *Data;
    const size_t Count;

    size_t getCount() const {
      return Count;
    }

    const PackType *getElement(size_t index) const {
      assert(index < Count);
      return Data[index];
    }

    friend llvm::hash_code hash_value(const Key &key) {
      llvm::hash_code hash = 0;
      for (size_t i = 0; i != key.getCount(); ++i)
        hash = llvm::hash_combine(hash, key.getElement(i));
      return hash;
    }
  };

  PackCacheEntry(const Key &key);

  intptr_t getKeyIntValueForDump() {
    return 0; // No single meaningful value here.
  }

  bool matchesKey(const Key &key) const {
    if (key.getCount() != Count)
      return false;
    for (unsigned i = 0; i != Count; ++i) {
      if (key.getElement(i) != getElements()[i])
        return false;
    }
    return true;
  }

  friend llvm::hash_code hash_value(const PackCacheEntry<PackType> &value) {
    llvm::hash_code hash = 0;
    for (size_t i = 0; i != value.Count; ++i)
      hash = llvm::hash_combine(hash, value.getElements()[i]);
    return hash;
  }

  static size_t getExtraAllocationSize(const Key &key) {
    return getExtraAllocationSize(key.Count);
  }

  size_t getExtraAllocationSize() const {
    return getExtraAllocationSize(Count);
  }

  static size_t getExtraAllocationSize(unsigned count) {
    return count * sizeof(const Metadata * const *);
  }
};

template<typename PackType>
PackCacheEntry<PackType>::PackCacheEntry(
    const typename PackCacheEntry<PackType>::Key &key) {
  Count = key.getCount();

  for (unsigned i = 0; i < Count; ++i)
    getElements()[i] = key.getElement(i);
}

} // end anonymous namespace

/// The uniquing structure for metadata packs.
static SimpleGlobalCache<PackCacheEntry<Metadata>,
                         MetadataPackTag> MetadataPacks;

SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
const Metadata * const *
swift_allocateMetadataPack(const Metadata * const *ptr, size_t count) {
  if (MetadataPackPointer(reinterpret_cast<uintptr_t>(ptr)).getLifetime()
        == PackLifetime::OnHeap)
    return ptr;

  PackCacheEntry<Metadata>::Key key{ptr, count};
  auto bytes = MetadataPacks.getOrInsert(key).first->getElements();

  MetadataPackPointer pack(bytes, PackLifetime::OnHeap);
  assert(pack.getNumElements() == count);
  return pack.getPointer();
}

/// The uniquing structure for witness table packs.
static SimpleGlobalCache<PackCacheEntry<WitnessTable>,
                         WitnessTablePackTag> WitnessTablePacks;

SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
const WitnessTable * const *
swift_allocateWitnessTablePack(const WitnessTable * const *ptr, size_t count) {
  if (WitnessTablePackPointer(reinterpret_cast<uintptr_t>(ptr)).getLifetime()
        == PackLifetime::OnHeap)
    return ptr;

  PackCacheEntry<WitnessTable>::Key key{ptr, count};
  auto bytes = WitnessTablePacks.getOrInsert(key).first->getElements();

  WitnessTablePackPointer pack(bytes, PackLifetime::OnHeap);
  assert(pack.getNumElements() == count);
  return pack.getPointer();
}

/***************************************************************************/
/*** Functions *************************************************************/
/***************************************************************************/

namespace {

class FunctionCacheEntry {
public:
  FullMetadata<FunctionTypeMetadata> Data;

  struct Key {
    const FunctionTypeFlags Flags;
    const FunctionMetadataDifferentiabilityKind DifferentiabilityKind;
    const Metadata *const *Parameters;
    const uint32_t *ParameterFlags;
    const Metadata *Result;
    const Metadata *GlobalActor;

    FunctionTypeFlags getFlags() const { return Flags; }

    FunctionMetadataDifferentiabilityKind getDifferentiabilityKind() const {
      return DifferentiabilityKind;
    }

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

    const Metadata *getGlobalActor() const { return GlobalActor; }

    friend llvm::hash_code hash_value(const Key &key) {
      auto hash = llvm::hash_combine(
          key.Flags.getIntValue(),
          key.DifferentiabilityKind.getIntValue(),
          key.Result, key.GlobalActor);
      for (unsigned i = 0, e = key.getFlags().getNumParameters(); i != e; ++i) {
        hash = llvm::hash_combine(hash, key.getParameter(i));
        hash = llvm::hash_combine(hash, key.getParameterFlags(i).getIntValue());
      }
      return hash;
    }
  };

  FunctionCacheEntry(const Key &key);

  intptr_t getKeyIntValueForDump() {
    return 0; // No single meaningful value here.
  }

  bool matchesKey(const Key &key) const {
    if (key.getFlags().getIntValue() != Data.Flags.getIntValue())
      return false;
    if (key.getDifferentiabilityKind().Value !=
        Data.getDifferentiabilityKind().Value)
      return false;
    if (key.getResult() != Data.ResultType)
      return false;
    if (key.getGlobalActor() != Data.getGlobalActor())
      return false;
    for (unsigned i = 0, e = key.getFlags().getNumParameters(); i != e; ++i) {
      if (key.getParameter(i) != Data.getParameter(i))
        return false;
      if (key.getParameterFlags(i).getIntValue() !=
          Data.getParameterFlags(i).getIntValue())
        return false;
    }
    return true;
  }

  friend llvm::hash_code hash_value(const FunctionCacheEntry &value) {
    Key key = {value.Data.Flags, value.Data.getDifferentiabilityKind(),
               value.Data.getParameters(), value.Data.getParameterFlags(),
               value.Data.ResultType, value.Data.getGlobalActor()};
    return hash_value(key);
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
    if (flags.isDifferentiable())
      size = roundUpToAlignment(size, sizeof(void *)) +
          sizeof(FunctionMetadataDifferentiabilityKind);
    if (flags.hasGlobalActor())
      size = roundUpToAlignment(size, sizeof(void *)) + sizeof(Metadata *);
    return roundUpToAlignment(size, sizeof(void *));
  }
};

} // end anonymous namespace

/// The uniquing structure for function type metadata.
static SimpleGlobalCache<FunctionCacheEntry, FunctionTypesTag> FunctionTypes;

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
  assert(!flags.isDifferentiable()
         && "Differentiable function type metadata should be obtained using "
            "'swift_getFunctionTypeMetadataDifferentiable'");
  assert(!flags.hasGlobalActor()
         && "Global actor function type metadata should be obtained using "
            "'swift_getFunctionTypeMetadataGlobalActor'");
  FunctionCacheEntry::Key key = {
    flags, FunctionMetadataDifferentiabilityKind::NonDifferentiable, parameters,
    parameterFlags, result, nullptr
  };
  return &FunctionTypes.getOrInsert(key).first->Data;
}

const FunctionTypeMetadata *
swift::swift_getFunctionTypeMetadataDifferentiable(
    FunctionTypeFlags flags, FunctionMetadataDifferentiabilityKind diffKind,
    const Metadata *const *parameters, const uint32_t *parameterFlags,
    const Metadata *result) {
  assert(!flags.hasGlobalActor()
         && "Global actor function type metadata should be obtained using "
            "'swift_getFunctionTypeMetadataGlobalActor'");
  assert(flags.isDifferentiable());
  assert(diffKind.isDifferentiable());
  FunctionCacheEntry::Key key = {
    flags, diffKind, parameters, parameterFlags, result, nullptr
  };
  return &FunctionTypes.getOrInsert(key).first->Data;
}

const FunctionTypeMetadata *
swift::swift_getFunctionTypeMetadataGlobalActor(
    FunctionTypeFlags flags, FunctionMetadataDifferentiabilityKind diffKind,
    const Metadata *const *parameters, const uint32_t *parameterFlags,
    const Metadata *result, const Metadata *globalActor) {
  assert(flags.hasGlobalActor());
  FunctionCacheEntry::Key key = {
    flags, diffKind, parameters, parameterFlags, result, globalActor
  };
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
      switch (key.getDifferentiabilityKind().Value) {
      case FunctionMetadataDifferentiabilityKind::Reverse:
        Data.ValueWitnesses = &VALUE_WITNESS_SYM(DIFF_FUNCTION_MANGLING);
        break;
      default:
        swift_unreachable("unsupported function witness");
      case FunctionMetadataDifferentiabilityKind::NonDifferentiable:
        Data.ValueWitnesses = &VALUE_WITNESS_SYM(FUNCTION_MANGLING);
        break;
      }
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
  if (flags.hasGlobalActor())
    *Data.getGlobalActorAddr() = key.getGlobalActor();
  if (flags.isDifferentiable())
    *Data.getDifferentiabilityKindAddress() = key.getDifferentiabilityKind();

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

    template <class Range>
    static llvm::hash_code hash_value(Range elements, const char *labels) {
      auto hash = llvm::hash_combine_range(elements.begin(), elements.end());
      hash = llvm::hash_combine(hash, llvm::StringRef(labels));
      return hash;
    }

    friend llvm::hash_code hash_value(const Key &key) {
      auto elements =
          llvm::ArrayRef<const Metadata *>(key.Elements, key.NumElements);
      return hash_value(elements, key.Labels);
    }
  };

  ValueType getValue() {
    return &Data;
  }
  void setValue(ValueType value) {
    assert(value == &Data);
  }

  TupleCacheEntry(const Key &key, MetadataWaitQueue::Worker &worker,
                  MetadataRequest request,
                  const ValueWitnessTable *proposedWitnesses);

  AllocationResult allocate(const ValueWitnessTable *proposedWitnesses) {
    swift_unreachable("allocated during construction");
  }

  MetadataStateWithDependency tryInitialize(Metadata *metadata,
                                    PrivateMetadataState state,
                                    PrivateMetadataCompletionContext *context);

  MetadataStateWithDependency checkTransitiveCompleteness() {
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

  friend llvm::hash_code hash_value(const TupleCacheEntry &value) {
    auto elements = llvm::ArrayRef<TupleTypeMetadata::Element>(
        value.Data.getElements(), value.Data.NumElements);
    auto types =
        makeTransformRange(elements, [](TupleTypeMetadata::Element element) {
          return element.Type;
        });
    return Key::hash_value(types, value.Data.Labels);
  }

  bool matchesKey(const Key &key) {
    if (key.NumElements != Data.NumElements)
      return false;

    for (size_t i = 0, e = key.NumElements; i != e; ++i)
      if (key.Elements[i] != Data.getElement(i).Type)
        return false;

    // It's unlikely that we'll get pointer-equality here unless we're being
    // called from the same module or both label strings are null, but
    // those are important cases.
    if (key.Labels == Data.Labels)
      return true;
    if (!key.Labels || !Data.Labels)
      return false;

    return strcmp(key.Labels, Data.Labels) == 0;
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
  public LockingConcurrentMapStorage<TupleCacheEntry, TupleCacheTag> {
public:
// FIXME: https://github.com/apple/swift/issues/43763.
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
  return asFullMetadata(metatype)->ValueWitnesses;
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
typedef ValueWitnessTypes::initializeWithCopyUnsigned forEachOperation;

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

SWIFT_CC(swift)
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

SWIFT_CC(swift)
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
    const TypeLayout *eltLayout = getLayout(i, elt);
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
    [](size_t i, const TypeLayout *elt) { return elt; },
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
    return MetadataResponse{ &METADATA_SYM(EMPTY_TUPLE_MANGLING), MetadataState::Complete };

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
  size_t labelsAllocSize = roundUpToAlignment(labelsLen + 2, sizeof(void *));
  char *newLabels =
    (char *) MetadataAllocator(TupleCacheTag).Allocate(labelsAllocSize, alignof(char));
  _swift_strlcpy(newLabels, labels, labelsAllocSize);
  key.Labels = newLabels;

  // Update the metadata cache.
  auto result = cache.getOrInsert(key, request, proposedWitnesses).second;

  // If we didn't manage to perform the insertion, free the memory associated
  // with the copy of the labels: nobody else can reference it.
  if (cast<TupleTypeMetadata>(result.Value)->Labels != newLabels) {
    MetadataAllocator(TupleCacheTag).Deallocate(newLabels, labelsAllocSize, alignof(char));
  }

  // Done.
  return result;
}

TupleCacheEntry::TupleCacheEntry(const Key &key,
                                 MetadataWaitQueue::Worker &worker,
                                 MetadataRequest request,
                                 const ValueWitnessTable *proposedWitnesses)
    : MetadataCacheEntryBase(worker, PrivateMetadataState::Abstract) {
  Data.setKind(MetadataKind::Tuple);
  Data.NumElements = key.NumElements;
  Data.Labels = key.Labels;

  // Stash the proposed witnesses in the value-witnesses slot for now.
  Data.ValueWitnesses = proposedWitnesses;

  for (size_t i = 0, e = key.NumElements; i != e; ++i)
    Data.getElement(i).Type = key.Elements[i];

  assert(TupleCacheStorage::resolveExistingEntry(&Data) == this);
}

MetadataStateWithDependency
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
    MetadataResponse response = swift_checkMetadataState(request, eltType);

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
    [](size_t i, const TupleTypeMetadata::Element &elt) {
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
    friend llvm::hash_code hash_value(const TypeContextIdentity &value) {
      return llvm::hash_value(value.Name);
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

SWIFT_CC(swift)
bool swift::swift_compareTypeContextDescriptors(
    const TypeContextDescriptor *a, const TypeContextDescriptor *b) {
  a = swift_auth_data_non_address(
      a, SpecialPointerAuthDiscriminators::TypeDescriptor);
  b = swift_auth_data_non_address(
      b, SpecialPointerAuthDiscriminators::TypeDescriptor);

  // The implementation is the same as the implementation of
  // swift::equalContexts except that the handling of non-type
  // context descriptors and casts to TypeContextDescriptor are removed.

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

  return TypeContextIdentity(a) == TypeContextIdentity(b);
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
  auto newTable = ::new (memory) ValueWitnessTable(*oldTable);

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
  performBasicLayout(
      layout, fieldTypes, numFields,
      [&](size_t i, const TypeLayout *fieldType) { return fieldType; },
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

void swift::swift_initStructMetadataWithLayoutString(
    StructMetadata *structType, StructLayoutFlags layoutFlags, size_t numFields,
    const uint8_t *const *fieldTypes, const uint8_t *fieldTags,
    uint32_t *fieldOffsets) {
  assert(structType->hasLayoutString());

  auto layout = getInitialLayoutForValueType();
  performBasicLayout(
      layout, fieldTypes, numFields,
      [&](size_t i, const uint8_t *fieldType) {
        if (fieldTags[i]) {
          return (const TypeLayout*)fieldType;
        }
        return ((const Metadata*)fieldType)->getTypeLayout();
      },
      [&](size_t i, const uint8_t *fieldType, uint32_t offset) {
        assignUnlessEqual(fieldOffsets[i], offset);
      });

  // We have extra inhabitants if any element does. Use the field with the most.
  unsigned extraInhabitantCount = 0;
  // Compute total combined size of the layout string
  size_t refCountBytes = 0;
  for (unsigned i = 0; i < numFields; ++i) {

    auto fieldTag = fieldTags[i];
    if (fieldTag) {
      if (fieldTag <= 0x4) {
        refCountBytes += sizeof(uint64_t);
      }

      const TypeLayout *fieldType = (const TypeLayout*)fieldTypes[i];

      unsigned fieldExtraInhabitantCount = fieldType->getNumExtraInhabitants();

      if (fieldExtraInhabitantCount > extraInhabitantCount) {
        extraInhabitantCount = fieldExtraInhabitantCount;
      }

      continue;
    }

    const Metadata *fieldType = (const Metadata*)fieldTypes[i];

    unsigned fieldExtraInhabitantCount =
      fieldType->vw_getNumExtraInhabitants();

    if (fieldExtraInhabitantCount > extraInhabitantCount) {
      extraInhabitantCount = fieldExtraInhabitantCount;
    }

    refCountBytes += _swift_refCountBytesForMetatype(fieldType);
  }

  const size_t fixedLayoutStringSize = layoutStringHeaderSize +
                                       sizeof(uint64_t) * 2;

  uint8_t *layoutStr =
      (uint8_t *)MetadataAllocator(LayoutStringTag)
          .Allocate(llvm::alignTo(fixedLayoutStringSize + refCountBytes,
                                  sizeof(void *)),
                    alignof(uint8_t));

  LayoutStringWriter writer{layoutStr, sizeof(uint64_t)};

  writer.writeBytes(refCountBytes);
  size_t fullOffset = 0;
  size_t previousFieldOffset = 0;
  LayoutStringFlags flags = LayoutStringFlags::Empty;
  for (unsigned i = 0; i < numFields; ++i) {
    size_t unalignedOffset = fullOffset;

    auto fieldTag = fieldTags[i];
    if (fieldTag) {
      const TypeLayout *fieldType = (const TypeLayout*)fieldTypes[i];
      auto alignmentMask = fieldType->flags.getAlignmentMask();
      fullOffset = roundUpToAlignMask(fullOffset, alignmentMask);

      if (fieldTag <= 0x4) {
        size_t offset = fullOffset - unalignedOffset + previousFieldOffset;

        auto tag = fieldTag <= 0x2 ? RefCountingKind::UnknownUnowned :
                                     RefCountingKind::UnknownWeak;

        auto tagAndOffset = ((uint64_t)tag << 56) | offset;
        writer.writeBytes(tagAndOffset);
      }

      fullOffset += fieldType->size;
      previousFieldOffset = fieldType->size;

      continue;
    }

    const Metadata *fieldType = (const Metadata*)fieldTypes[i];

    _swift_addRefCountStringForMetatype(writer, flags, fieldType, fullOffset,
                                        previousFieldOffset);
  }

  writer.writeBytes((uint64_t)previousFieldOffset);
  writer.writeBytes((uint64_t)0);

  // we mask out HasRelativePointers, because at this point they have all been
  // resolved to metadata pointers
  writer.offset = 0;
  writer.writeBytes(((uint64_t)flags) &
                    ~((uint64_t)LayoutStringFlags::HasRelativePointers));

  structType->setLayoutString(layoutStr);

  auto *vwtable = getMutableVWTableForInit(structType, layoutFlags);
  vwtable->destroy = swift_generic_destroy;
  vwtable->initializeWithCopy = swift_generic_initWithCopy;
  vwtable->initializeWithTake = swift_generic_initWithTake;
  vwtable->assignWithCopy = swift_generic_assignWithCopy;
  vwtable->assignWithTake = swift_generic_assignWithTake;

  layout.extraInhabitantCount = extraInhabitantCount;

  // Substitute in better value witnesses if we have them.
  installCommonValueWitnesses(layout, vwtable);

  vwtable->publishLayout(layout);
}

size_t swift::_swift_refCountBytesForMetatype(const Metadata *type) {
  if (type->vw_size() == 0 || type->getValueWitnesses()->isPOD()) {
    return 0;
  } else if (type->hasLayoutString()) {
    size_t offset = sizeof(uint64_t);
    return LayoutStringReader{type->getLayoutString(), offset}
        .readBytes<size_t>();
  } else if (type->isClassObject() || type->isAnyExistentialType()) {
    return sizeof(uint64_t);
  } else if (auto *tuple = dyn_cast<TupleTypeMetadata>(type)) {
    size_t res = 0;
    for (InProcess::StoredSize i = 0; i < tuple->NumElements; i++) {
      res += _swift_refCountBytesForMetatype(tuple->getElement(i).Type);
    }
    return res;
  } else {
    return sizeof(uint64_t) + sizeof(uintptr_t);
  }
}

void swift::_swift_addRefCountStringForMetatype(LayoutStringWriter &writer,
                                                LayoutStringFlags &flags,
                                                const Metadata *fieldType,
                                                size_t &fullOffset,
                                                size_t &previousFieldOffset) {
  size_t unalignedOffset = fullOffset;
  fullOffset = roundUpToAlignMask(fullOffset, fieldType->vw_alignment() - 1);
  size_t offset = fullOffset - unalignedOffset + previousFieldOffset;
  if (fieldType->vw_size() == 0) {
    return;
  } else if (fieldType->getValueWitnesses()->isPOD()) {
    // No need to handle PODs
    previousFieldOffset = offset + fieldType->vw_size();
    fullOffset += fieldType->vw_size();
  } else if (fieldType->hasLayoutString()) {
    LayoutStringReader reader{fieldType->getLayoutString(), 0};
    const auto fieldFlags = reader.readBytes<LayoutStringFlags>();
    const auto fieldRefCountBytes = reader.readBytes<size_t>();
    if (fieldRefCountBytes > 0) {
      flags |= fieldFlags;
      memcpy(writer.layoutStr + writer.offset,
             reader.layoutStr + layoutStringHeaderSize, fieldRefCountBytes);

      if (fieldFlags & LayoutStringFlags::HasRelativePointers) {
        swift_resolve_resilientAccessors(writer.layoutStr, writer.offset,
                                         reader.layoutStr, fieldType);
      }

      if (offset) {
        auto writerOffsetCopy = writer.offset;
        reader.offset = layoutStringHeaderSize;
        auto firstTagAndOffset = reader.readBytes<uint64_t>();
        firstTagAndOffset += offset;
        writer.writeBytes(firstTagAndOffset);
        writer.offset = writerOffsetCopy;
      }

      reader.offset = layoutStringHeaderSize + fieldRefCountBytes;
      previousFieldOffset = reader.readBytes<uint64_t>();
      writer.skip(fieldRefCountBytes);
    } else {
      previousFieldOffset += fieldType->vw_size();
    }
    fullOffset += fieldType->vw_size();
  } else if (auto *tuple = dyn_cast<TupleTypeMetadata>(fieldType)) {
    for (InProcess::StoredSize i = 0; i < tuple->NumElements; i++) {
      _swift_addRefCountStringForMetatype(writer, flags,
                                          tuple->getElement(i).Type, fullOffset,
                                          previousFieldOffset);
    }
  } else if (auto *cls = fieldType->getClassObject()) {
    RefCountingKind tag;
    if (!cls->isTypeMetadata()) {
    #if SWIFT_OBJC_INTEROP
      tag = RefCountingKind::ObjC;
    #else
      tag = RefCountingKind::Unknown;
    #endif
    } else {
      auto *vwt = cls->getValueWitnesses();
      if (vwt == &VALUE_WITNESS_SYM(Bo)) {
        tag = RefCountingKind::NativeStrong;
      } else if (vwt == &VALUE_WITNESS_SYM(BO)) {
      #if SWIFT_OBJC_INTEROP
        tag = RefCountingKind::ObjC;
      #else
        tag = RefCountingKind::Unknown;
      #endif
      } else if (vwt == &VALUE_WITNESS_SYM(Bb)) {
        tag = RefCountingKind::Bridge;
      } else {
        goto metadata;
      };
    }

    writer.writeBytes(((uint64_t)tag << 56) | offset);
    previousFieldOffset = fieldType->vw_size();
    fullOffset += previousFieldOffset;
  } else if (fieldType->isAnyExistentialType()) {
    auto *existential = dyn_cast<ExistentialTypeMetadata>(fieldType);
    assert(existential);
    auto tag = existential->isClassBounded() ? RefCountingKind::Unknown
                                             : RefCountingKind::Existential;
    writer.writeBytes(((uint64_t)tag << 56) | offset);
    previousFieldOffset = fieldType->vw_size();
    fullOffset += previousFieldOffset;
  } else {
metadata:
  writer.writeBytes(((uint64_t)RefCountingKind::Metatype << 56) | offset);
  writer.writeBytes(fieldType);
  previousFieldOffset = fieldType->vw_size();
  fullOffset += previousFieldOffset;
  }
}

/***************************************************************************/
/*** Classes ***************************************************************/
/***************************************************************************/

static MetadataAllocator &getResilientMetadataAllocator() {
  // This should be constant-initialized, but this is safe.
  static MetadataAllocator allocator(ResilientMetadataAllocatorTag);
  return allocator;
}

ClassMetadata *
swift::swift_relocateClassMetadata(const ClassDescriptor *description,
                                   const ResilientClassMetadataPattern *pattern) {
  description = swift_auth_data_non_address(
    description, SpecialPointerAuthDiscriminators::TypeDescriptor);

  return _swift_relocateClassMetadata(description, pattern);
}

static ClassMetadata *
_swift_relocateClassMetadata(const ClassDescriptor *description,
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
    ++log2;
  return log2;
}

static inline ClassROData *getROData(ClassMetadata *theClass) {
  return (ClassROData*)(theClass->Data & ~uintptr_t(SWIFT_CLASS_IS_SWIFT_MASK));
}

// This gets called if we fail during copyGenericClassObjcName().  Its job is
// to generate a unique name, even though the name won't be very helpful if
// we end up looking at it in a debugger.
#define EMERGENCY_PREFIX "$SwiftEmergencyPlaceholderClassName"
static char *copyEmergencyName(ClassMetadata *theClass) {
  char *nameBuf = nullptr;
  asprintf(&nameBuf,
           EMERGENCY_PREFIX "%016" PRIxPTR,
           (uintptr_t)theClass);
  return nameBuf;
}

static char *copyGenericClassObjCName(ClassMetadata *theClass) {
  // Use the remangler to generate a mangled name from the type metadata.
  Demangle::StackAllocatedDemangler<4096> Dem;

  auto demangling = _swift_buildDemanglingForMetadata(theClass, Dem);
  if (!demangling) {
    return copyEmergencyName(theClass);
  }

  // Remangle that into a new type mangling string.
  auto typeNode = Dem.createNode(Demangle::Node::Kind::TypeMangling);
  typeNode->addChild(demangling, Dem);
  auto globalNode = Dem.createNode(Demangle::Node::Kind::Global);
  globalNode->addChild(typeNode, Dem);

  auto mangling = Demangle::mangleNodeOld(globalNode, Dem);
  if (!mangling.isSuccess()) {
    return copyEmergencyName(theClass);
  }
  llvm::StringRef string = mangling.result();

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
  static objc_hook_lazyClassNamer oldHook;
  auto myHook = [](Class theClass) -> const char * {
    ClassMetadata *metadata = (ClassMetadata *)theClass;
    if (metadata->isTypeMetadata())
      return copyGenericClassObjCName(metadata);
    return oldHook(theClass);
  };

  if (SWIFT_RUNTIME_WEAK_CHECK(objc_setHook_lazyClassNamer)) {
    SWIFT_RUNTIME_WEAK_USE(objc_setHook_lazyClassNamer(myHook, &oldHook));
    return true;
  }

  return false;
}

__attribute__((constructor)) SWIFT_RUNTIME_ATTRIBUTE_ALWAYS_INLINE static bool
supportsLazyObjcClassNames() {
  return SWIFT_LAZY_CONSTANT(installLazyClassNameHook());
}

static void setUpGenericClassObjCName(ClassMetadata *theClass) {
  if (supportsLazyObjcClassNames()) {
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
      // This should be okay even with variadic packs because we're
      // copying from an existing metadata, so we've already uniqued.
      auto genericOffset = description->getGenericArgumentOffset();
      memcpy(classWords + genericOffset,
             superWords + genericOffset,
             description->getGenericContextHeader()
                 .getArgumentLayoutSizeInWords() * sizeof(uintptr_t));
    }

    // Copy the vtable entries.
    if (description->hasVTable() && !hasStaticVTable(layoutFlags)) {
      auto *vtable = description->getVTableDescriptor();
      auto vtableOffset = vtable->getVTableOffset(description);
      auto dest = classWords + vtableOffset;
      auto src = superWords + vtableOffset;
#if SWIFT_PTRAUTH
      auto descriptors = description->getMethodDescriptors();
      for (size_t i = 0, e = vtable->VTableSize; i != e; ++i) {
        swift_ptrauth_copy_code_or_data(
            reinterpret_cast<void **>(&dest[i]),
            reinterpret_cast<void *const *>(&src[i]),
            descriptors[i].Flags.getExtraDiscriminator(),
            !descriptors[i].Flags.isAsync(),
            /*allowNull*/ true); // NULL allowed for VFE (methods in the vtable
                                 // might be proven unused and null'ed)
      }
#else
      memcpy(dest, src, vtable->VTableSize * sizeof(uintptr_t));
#endif
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
    auto descriptors = description->getMethodDescriptors();
    for (unsigned i = 0, e = vtable->VTableSize; i < e; ++i) {
      auto &methodDescription = descriptors[i];
      swift_ptrauth_init_code_or_data(
          &classWords[vtableOffset + i], methodDescription.getImpl(),
          methodDescription.Flags.getExtraDiscriminator(),
          !methodDescription.Flags.isAsync());
    }
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
      auto offset = (baseVTable->getVTableOffset(baseClass) +
                     (baseMethod - baseClassMethods.data()));
      swift_ptrauth_init_code_or_data(&classWords[offset],
                                      descriptor.getImpl(),
                                      baseMethod->Flags.getExtraDiscriminator(),
                                      !baseMethod->Flags.isAsync());
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
  //
  // The rodata may be in read-only memory if the compiler knows that the size
  // it generates is already definitely correct. Don't write to this value
  // unless it's necessary.
  if (rodata->InstanceStart != size)
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
  if (rodata->InstanceSize != size)
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
        // Make sure all the entries start out null.
        memset(_globalIvarOffsets, 0, sizeof(size_t *) * numFields);
      } else {
        _globalIvarOffsets =
            static_cast<size_t **>(calloc(sizeof(size_t *), numFields));
      }
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
      free(_globalIvarOffsets);
    }
  }

  return MetadataDependency();
}
#endif

SWIFT_CC(swift)
SWIFT_RUNTIME_STDLIB_INTERNAL MetadataResponse
getSuperclassMetadata(MetadataRequest request, const ClassMetadata *self) {
  // If there is a mangled superclass name, demangle it to the superclass
  // type.
  if (auto superclassNameBase = self->getDescription()->SuperclassType.get()) {
    StringRef superclassName =
      Demangle::makeSymbolicMangledNameStringRef(superclassNameBase);
    SubstGenericParametersFromMetadata substitutions(self);
    auto result = swift_getTypeByMangledName(
        request, superclassName, substitutions.getGenericArgs(),
        [&substitutions](unsigned depth, unsigned index) {
          // FIXME: Variadic generics
          return substitutions.getMetadata(depth, index).getMetadata();
        },
        [&substitutions](const Metadata *type, unsigned index) {
          return substitutions.getWitnessTable(type, index);
        });
    if (auto *error = result.getError()) {
      fatalError(
          0, "failed to demangle superclass of %s from mangled name '%s': %s\n",
          self->getDescription()->Name.get(), superclassName.str().c_str(),
          error->copyErrorString());
    }

    return result.getType().getResponse();
  } else {
    return MetadataResponse();
  }
}

SWIFT_CC(swift)
static std::pair<MetadataDependency, const ClassMetadata *>
getSuperclassMetadata(ClassMetadata *self, bool allowDependency) {
  MetadataRequest request(allowDependency ? MetadataState::NonTransitiveComplete
                                          : /*FIXME*/ MetadataState::Abstract,
                          /*non-blocking*/ allowDependency);
  auto response = getSuperclassMetadata(request, self);

  auto *superclass = response.Value;
  if (!superclass)
    return {MetadataDependency(), nullptr};

  const ClassMetadata *second;
#if SWIFT_OBJC_INTEROP
  if (auto objcWrapper = dyn_cast<ObjCClassWrapperMetadata>(superclass)) {
    second = objcWrapper->Class;
  } else {
    second = cast<ClassMetadata>(superclass);
  }
#else
  second = cast<ClassMetadata>(superclass);
#endif

  // If the request isn't satisfied, we have a new dependency.
  if (!request.isSatisfiedBy(response.State)) {
    assert(allowDependency);
    return {MetadataDependency(superclass, request.getState()), second};
  }

  return {MetadataDependency(), second};
}

static SWIFT_CC(swift) MetadataDependency
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
  static swift::once_t onceToken;
  swift::once(
      onceToken,
      [](void *unused) {
        (void)unused;
        setUpObjCRuntimeGetImageNameFromClass();
      },
      nullptr);
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
    if (stub != nullptr && SWIFT_RUNTIME_WEAK_CHECK(_objc_realizeClassFromSwift)) {
      SWIFT_RUNTIME_WEAK_USE(_objc_realizeClassFromSwift((Class) self, const_cast<void *>(stub)));
    } else {
      swift_instantiateObjCClass(self);
    }
  }
#else
  assert(!self->getDescription()->hasObjCResilientClassStub());
#endif

  return MetadataDependency();
}

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

static SWIFT_CC(swift) MetadataDependency
_swift_updateClassMetadataImpl(ClassMetadata *self,
                               ClassLayoutFlags layoutFlags,
                               size_t numFields,
                               const TypeLayout * const *fieldTypes,
                               size_t *fieldOffsets,
                               bool allowDependency) {
  bool requiresUpdate = SWIFT_RUNTIME_WEAK_CHECK(_objc_realizeClassFromSwift);

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
    SWIFT_RUNTIME_WEAK_USE(_objc_realizeClassFromSwift((Class)self, (Class)self));
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

#ifndef NDEBUG
  assert(isAncestorOf(metadata, description));
#endif

  auto *vtable = description->getVTableDescriptor();
  assert(vtable != nullptr);

  auto methods = description->getMethodDescriptors();
  unsigned index = method - methods.data();
  assert(index < methods.size());

  auto vtableOffset = vtable->getVTableOffset(description) + index;
  auto *words = reinterpret_cast<void * const *>(metadata);

  auto *const *methodPtr = (words + vtableOffset);

#if SWIFT_PTRAUTH
  // Re-sign the return value without the address.
  unsigned extra = method->Flags.getExtraDiscriminator();
  if (method->Flags.isAsync()) {
    return ptrauth_auth_and_resign(
        *methodPtr, ptrauth_key_process_independent_data,
        ptrauth_blend_discriminator(methodPtr, extra),
        ptrauth_key_process_independent_data, extra);
  } else {
    return ptrauth_auth_and_resign(
        *methodPtr, ptrauth_key_function_pointer,
        ptrauth_blend_discriminator(methodPtr, extra),
        ptrauth_key_function_pointer, extra);
  }
#else
  return *methodPtr;
#endif
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

    bool matchesKey(const Metadata *instanceType) const {
      return instanceType == Data.InstanceType;
    }

    friend llvm::hash_code hash_value(const MetatypeCacheEntry &value) {
      return llvm::hash_value(value.Data.InstanceType);
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
static SimpleGlobalCache<MetatypeCacheEntry, MetatypeTypesTag> MetatypeTypes;

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

  bool matchesKey(unsigned key) const { return key == getNumWitnessTables(); }

  friend llvm::hash_code
  hash_value(const ExistentialMetatypeValueWitnessTableCacheEntry &value) {
    return llvm::hash_value(value.getNumWitnessTables());
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

  bool matchesKey(const Metadata *instanceType) const {
    return instanceType == Data.InstanceType;
  }

  friend llvm::hash_code
  hash_value(const ExistentialMetatypeCacheEntry &value) {
    return llvm::hash_value(value.Data.InstanceType);
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
static SimpleGlobalCache<ExistentialMetatypeValueWitnessTableCacheEntry,
                         ExistentialMetatypeValueWitnessTablesTag>
ExistentialMetatypeValueWitnessTables;

/// The uniquing structure for existential metatype type metadata.
static SimpleGlobalCache<ExistentialMetatypeCacheEntry,
                         ExistentialMetatypesTag> ExistentialMetatypes;

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
  switch (instanceMetadata->getKind()) {
  case MetadataKind::Existential:
    flags = static_cast<const ExistentialTypeMetadata*>(instanceMetadata)
      ->Flags;
    break;
  case MetadataKind::ExistentialMetatype:
    flags = static_cast<const ExistentialMetatypeMetadata*>(instanceMetadata)
      ->Flags;
    break;
  default:
    assert(false && "expected existential metadata");
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

    friend llvm::hash_code hash_value(const Key &key) {
      auto hash = llvm::hash_combine(key.SuperclassConstraint,
                                     key.ClassConstraint, key.NumProtocols);
      for (size_t i = 0; i != key.NumProtocols; i++)
        hash = llvm::hash_combine(hash, key.Protocols[i].getRawData());
      return hash;
    }
  };

  ExistentialCacheEntry(Key key);

  intptr_t getKeyIntValueForDump() {
    return 0;
  }

  bool matchesKey(Key key) const {
    if (key.ClassConstraint != Data.Flags.getClassConstraint())
      return false;

    if (key.SuperclassConstraint != Data.getSuperclassConstraint())
      return false;

    if (key.NumProtocols != Data.NumProtocols)
      return false;

    auto dataProtocols = Data.getProtocols();
    for (size_t i = 0; i != key.NumProtocols; ++i) {
      if (key.Protocols[i].getRawData() != dataProtocols[i].getRawData())
        return false;
    }

    return true;
  }

  friend llvm::hash_code hash_value(const ExistentialCacheEntry &value) {
    Key key = {value.Data.getSuperclassConstraint(),
               value.Data.Flags.getClassConstraint(), value.Data.NumProtocols,
               value.Data.getProtocols().data()};
    return hash_value(key);
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

  bool matchesKey(unsigned key) const { return key == getNumWitnessTables(); }

  friend llvm::hash_code
  hash_value(const OpaqueExistentialValueWitnessTableCacheEntry &value) {
    return llvm::hash_value(value.getNumWitnessTables());
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

  bool matchesKey(unsigned key) const { return key == getNumWitnessTables(); }

  friend llvm::hash_code
  hash_value(const ClassExistentialValueWitnessTableCacheEntry &value) {
    return llvm::hash_value(value.getNumWitnessTables());
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
static SimpleGlobalCache<ExistentialCacheEntry, ExistentialTypesTag> ExistentialTypes;

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
static SimpleGlobalCache<OpaqueExistentialValueWitnessTableCacheEntry,
                         OpaqueExistentialValueWitnessTablesTag>
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
static SimpleGlobalCache<ClassExistentialValueWitnessTableCacheEntry,
                         ClassExistentialValueWitnessTablesTag>
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

  swift_unreachable("Unhandled ProtocolClassConstraint in switch.");
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

  swift_unreachable(
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

  swift_unreachable(
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

  swift_unreachable(
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
#ifndef NDEBUG
  assert(classConstraint == ProtocolClassConstraint::Class ||
         (!superclassConstraint &&
          !anyProtocolIsClassBound(numProtocols, protocols)));
#endif
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
/*** Extended existential type descriptors *********************************/
/***************************************************************************/

namespace {

class ExtendedExistentialTypeShapeCacheEntry {
public:
  const NonUniqueExtendedExistentialTypeShape *
    __ptrauth_swift_nonunique_extended_existential_type_shape Data;

  struct Key {
    const NonUniqueExtendedExistentialTypeShape *Candidate;
    llvm::StringRef TypeString;

    Key(const NonUniqueExtendedExistentialTypeShape *candidate)
      : Candidate(candidate),
        TypeString(candidate->getExistentialTypeStringForUniquing()) {}

    friend llvm::hash_code hash_value(const Key &key) {
      return hash_value(key.TypeString);
    }
  };

  ExtendedExistentialTypeShapeCacheEntry(Key key)
    : Data(key.Candidate) {}

  intptr_t getKeyIntValueForDump() {
    return 0;
  }

  bool matchesKey(Key key) const {
    auto self = Data;
    auto other = key.Candidate;
    if (self == other) return true;
    return self->getExistentialTypeStringForUniquing() == key.TypeString;
  }

  friend llvm::hash_code hash_value(
                        const ExtendedExistentialTypeShapeCacheEntry &value) {
    return hash_value(Key(value.Data));
  }

  static size_t getExtraAllocationSize(Key key) {
    return 0;
  }
};

}

/// The uniquing structure for extended existential type descriptors.
static SimpleGlobalCache<ExtendedExistentialTypeShapeCacheEntry,
                         ExtendedExistentialTypeShapesTag>
  ExtendedExistentialTypeShapes;

const ExtendedExistentialTypeShape *
swift::swift_getExtendedExistentialTypeShape(
            const NonUniqueExtendedExistentialTypeShape *nonUnique) {
#if SWIFT_PTRAUTH
  // The description pointer is expected to be signed with an
  // address-undiversified schema when passed in.
  nonUnique = ptrauth_auth_data(nonUnique,
      ptrauth_key_process_independent_data,
      SpecialPointerAuthDiscriminators::NonUniqueExtendedExistentialTypeShape);
#endif

  // Check the cache.
  auto &cache = *nonUnique->UniqueCache.get();
  if (auto ptr = cache.load(std::memory_order_acquire)) {
#if SWIFT_PTRAUTH
    // Resign the returned pointer from an address-diversified to an
    // undiversified schema.
    ptr = ptrauth_auth_and_resign(ptr,
        ptrauth_key_process_independent_data,
        ptrauth_blend_discriminator(&cache,
          SpecialPointerAuthDiscriminators::ExtendedExistentialTypeShape),
        ptrauth_key_process_independent_data,
        SpecialPointerAuthDiscriminators::ExtendedExistentialTypeShape);
#endif
    return ptr;
  }

  // Find the unique entry.
  auto uniqueEntry = ExtendedExistentialTypeShapes.getOrInsert(
      ExtendedExistentialTypeShapeCacheEntry::Key(nonUnique));

  const ExtendedExistentialTypeShape *unique =
    &uniqueEntry.first->Data->LocalCopy;

  // Cache the uniqued description, signing it with an
  // address-diversified schema.
  auto uniqueForCache = unique;
#if SWIFT_PTRAUTH
  uniqueForCache = ptrauth_sign_unauthenticated(uniqueForCache,
                     ptrauth_key_process_independent_data,
                     ptrauth_blend_discriminator(&cache,
          SpecialPointerAuthDiscriminators::ExtendedExistentialTypeShape));
#endif
  cache.store(uniqueForCache, std::memory_order_release);

  // Return the uniqued description, signing it with an
  // address-undiversified schema.
#if SWIFT_PTRAUTH
  unique = ptrauth_sign_unauthenticated(unique,
             ptrauth_key_process_independent_data,
             SpecialPointerAuthDiscriminators::ExtendedExistentialTypeShape);
#endif
  return unique;
}

/***************************************************************************/
/*** Extended existential types ********************************************/
/***************************************************************************/

namespace {

class ExtendedExistentialTypeCacheEntry {
public:
  FullMetadata<ExtendedExistentialTypeMetadata> Data;

  struct Key {
    MetadataCacheKey Arguments;
    const ExtendedExistentialTypeShape *Shape;

    Key(const ExtendedExistentialTypeShape *shape,
        const void * const *arguments)
      : Arguments(shape->getGeneralizationSignature(), arguments),
        Shape(shape) {}

    friend llvm::hash_code hash_value(const Key &key) {
      return llvm::hash_combine(key.Shape, // by address
                                key.Arguments.hash());
    }

    bool operator==(const Key &other) {
      return Shape == other.Shape && Arguments == other.Arguments;
    }
  };

  ExtendedExistentialTypeCacheEntry(Key key)
      : Data{ TargetTypeMetadataHeader<InProcess>({getOrCreateTypeLayout(key)}, {getOrCreateVWT(key)}), key.Shape} {
    key.Arguments.installInto(Data.getTrailingObjects<const void *>());
  }

  static const ValueWitnessTable *getOrCreateVWT(Key key);
  static const uint8_t *getOrCreateTypeLayout(Key key);

  intptr_t getKeyIntValueForDump() {
    return 0;
  }

  Key getKey() const {
    return Key{Data.Shape, Data.getGeneralizationArguments()};
  }

  bool matchesKey(Key key) const {
    // Bypass the eager hashing done in the Key constructor in the most
    // important negative case.
    if (Data.Shape != key.Shape)
      return false;

    return (getKey() == key);
  }

  friend llvm::hash_code hash_value(const ExtendedExistentialTypeCacheEntry &value) {
    return hash_value(value.getKey());
  }

  static size_t getExtraAllocationSize(Key key) {
    return ExtendedExistentialTypeMetadata::additionalSizeToAlloc<
             const void *
           >(key.Shape->getGenSigArgumentLayoutSizeInWords());
  }

  size_t getExtraAllocationSize() const {
    return ExtendedExistentialTypeMetadata::additionalSizeToAlloc<
             const void *
           >(Data.Shape->getGenSigArgumentLayoutSizeInWords());
  }
};

} // end anonymous namespace

const ValueWitnessTable *
ExtendedExistentialTypeCacheEntry::getOrCreateVWT(Key key) {
  auto shape = key.Shape;

  if (auto witnesses = shape->getSuggestedValueWitnesses())
    return witnesses;

  // The type head must name all the type parameters, so we must not have
  // multiple type parameters if we have an opaque type head.
  auto sigSizeInWords = shape->ReqSigHeader.getArgumentLayoutSizeInWords();

#ifndef NDEBUG
  auto layout =
      GenericSignatureLayout<InProcess>(shape->getRequirementSignature());
  assert(layout.NumKeyParameters == shape->ReqSigHeader.NumParams &&
         "requirement signature for existential includes a "
         "redundant parameter?");
  assert(layout.NumWitnessTables
            == sigSizeInWords - shape->ReqSigHeader.NumParams &&
         "requirement signature for existential includes an "
         "unexpected key argument?");
#endif

  // We're lowering onto existing witnesses for existential types,
  // which are parameterized only by the number of witness tables they
  // need to copy around.
  // TODO: variadic-parameter-packs?  Or is a memcpy okay, because we
  // can assume existentials store permanent packs, in the unlikely
  // case that the requirement signature includes a pack parameter?
  unsigned wtableStorageSizeInWords =
    sigSizeInWords - shape->ReqSigHeader.NumParams;

  using SpecialKind = ExtendedExistentialTypeShape::SpecialKind;
  switch (shape->Flags.getSpecialKind()) {
  case SpecialKind::None:
    assert(shape->isTypeExpressionOpaque() &&
           "shape with a non-opaque type expression has no suggested VWT");
    // Use the standard opaque-existential representation.
    return getExistentialValueWitnesses(ProtocolClassConstraint::Any,
                                        /*superclass*/ nullptr,
                                        wtableStorageSizeInWords,
                                        SpecialProtocol::None);

  case SpecialKind::ExplicitLayout:
    swift_unreachable("shape with explicit layout but no suggested VWT");

  case SpecialKind::Class:
    // Class-constrained existentials don't store type metadata.
    // TODO: pull out a superclass constraint if there is one so that
    // we can use native reference counting.
    return getExistentialValueWitnesses(ProtocolClassConstraint::Class,
                                        /*superclass*/ nullptr,
                                        wtableStorageSizeInWords,
                                        SpecialProtocol::None);

  case SpecialKind::Metatype:
    // Existential metatypes don't store type metadata.
    return getExistentialMetatypeValueWitnesses(wtableStorageSizeInWords);
  }

  // We can support back-deployment of new special kinds (at least here)
  // if we just require them to provide suggested value witnesses.
  swift_unreachable("shape with unknown special kind had no suggested VWT");
}

const uint8_t *
ExtendedExistentialTypeCacheEntry::getOrCreateTypeLayout(Key key) {
  // TODO: implement
  return nullptr;
}

/// The uniquing structure for extended existential type metadata.
static SimpleGlobalCache<ExtendedExistentialTypeCacheEntry,
                         ExtendedExistentialTypesTag>
  ExtendedExistentialTypes;

const ExtendedExistentialTypeMetadata *
swift::swift_getExtendedExistentialTypeMetadata_unique(
            const ExtendedExistentialTypeShape *shape,
            const void * const *generalizationArguments) {
#if SWIFT_PTRAUTH
  shape = ptrauth_auth_data(shape, ptrauth_key_process_independent_data,
            SpecialPointerAuthDiscriminators::ExtendedExistentialTypeShape);
#endif

  ExtendedExistentialTypeCacheEntry::Key key(shape, generalizationArguments);

  auto entry = ExtendedExistentialTypes.getOrInsert(key);
  return &entry.first->Data;
}

/// Fetch a unique existential shape descriptor for an extended
/// existential type.
SWIFT_RUNTIME_EXPORT
const ExtendedExistentialTypeMetadata *
swift_getExtendedExistentialTypeMetadata(
            const NonUniqueExtendedExistentialTypeShape *nonUniqueShape,
            const void * const *generalizationArguments) {
  auto uniqueShape = swift_getExtendedExistentialTypeShape(nonUniqueShape);
  return swift_getExtendedExistentialTypeMetadata_unique(uniqueShape,
                                              generalizationArguments);
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
  else if (auto foreignClass = dyn_cast<ForeignReferenceTypeMetadata>(metadata))
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
    swift_unreachable("should never be called");
  }

public:
  struct Key {
    const TypeContextDescriptor *Description;

    friend llvm::hash_code hash_value(const Key &key) {
      return hash_value(TypeContextIdentity(key.Description));
    }
  };

  static const char *getName() { return "ForeignMetadataCache"; }

  ForeignMetadataCacheEntry(Key key, MetadataWaitQueue::Worker &worker,
                            MetadataRequest request, Metadata *candidate)
      : MetadataCacheEntryBase(worker, configureCandidate(key, candidate)),
        Value(candidate) {
  }

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

  friend llvm::hash_code hash_value(const ForeignMetadataCacheEntry &value) {
    return hash_value(TypeContextIdentity(value.getDescription()));
  }

  bool matchesKey(Key key) {
    // We can just compare unparented type-context identities because
    // we assume that foreign types don't have interesting parenting
    // structure.
    return TypeContextIdentity(key.Description) == TypeContextIdentity(getDescription());
  }

  AllocationResult allocate(Metadata *candidate) {
    swift_unreachable("allocation is short-circuited during construction");
  }

  MetadataStateWithDependency tryInitialize(Metadata *metadata,
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

private:
  /// Do as much candidate initialization as we reasonably can during
  /// construction.  Remember, though, that this is just construction;
  /// we won't have committed to this candidate as the metadata until
  /// this entry is successfully installed in the concurrent map.
  static PrivateMetadataState configureCandidate(Key key, Metadata *candidate) {
    auto &init = key.Description->getForeignMetadataInitialization();

    if (!init.CompletionFunction) {
      if (areAllTransitiveMetadataComplete_cheap(candidate)) {
        return PrivateMetadataState::Complete;
      } else {
        return PrivateMetadataState::NonTransitiveComplete;
      }
    }

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
    return inferStateForMetadata(candidate);
  }
};

} // end anonymous namespace

static Lazy<MetadataCache<ForeignMetadataCacheEntry, ForeignMetadataCacheTag>> ForeignMetadata;

MetadataResponse
swift::swift_getForeignTypeMetadata(MetadataRequest request,
                                    ForeignTypeMetadata *candidate) {
  auto description = getForeignTypeDescription(candidate);
  ForeignMetadataCacheEntry::Key key{description};
  return ForeignMetadata->getOrInsert(key, request, candidate).second;
}

/// Unique-ing of foreign types' witness tables.
namespace {
  class ForeignWitnessTableCacheEntry {
  public:
    struct Key {
      const TypeContextDescriptor *type;
      const ProtocolDescriptor *protocol;

      friend llvm::hash_code hash_value(const Key &value) {
        return llvm::hash_combine(value.protocol,
                                  TypeContextIdentity(value.type));
      }
    };

    const TypeContextDescriptor *type;
    const ProtocolDescriptor *protocol;
    const WitnessTable *data;

    ForeignWitnessTableCacheEntry(const ForeignWitnessTableCacheEntry::Key k,
                                  const WitnessTable *d)
        : type(k.type), protocol(k.protocol), data(d) {}

    intptr_t getKeyIntValueForDump() {
      return reinterpret_cast<intptr_t>(type);
    }

    bool matchesKey(const Key other) const {
      return other.protocol == protocol &&
             TypeContextIdentity(other.type) == TypeContextIdentity(type);
    }

    friend llvm::hash_code
    hash_value(const ForeignWitnessTableCacheEntry &value) {
      Key key{value.type, value.protocol};
      return hash_value(key);
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

static ConcurrentReadableHashMap<ForeignWitnessTableCacheEntry>
    ForeignWitnessTables;

static const WitnessTable *_getForeignWitnessTable(
    const WitnessTable *witnessTableCandidate,
    const TypeContextDescriptor *contextDescriptor,
    const ProtocolDescriptor *protocol) {
  const WitnessTable *result = nullptr;
  ForeignWitnessTableCacheEntry::Key key{contextDescriptor, protocol};
  ForeignWitnessTables.getOrInsert(
      key, [&](ForeignWitnessTableCacheEntry *entryPtr, bool created) {
        if (created)
          ::new (entryPtr)
              ForeignWitnessTableCacheEntry(key, witnessTableCandidate);
        result = entryPtr->data;
        return true;
      });
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
template <> SWIFT_USED void Metadata::dump() const {
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
      for (uint32_t i = 0; i < genericCount; ++i) {
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

template <> SWIFT_USED void ContextDescriptor::dump() const {
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

template <> SWIFT_USED void EnumDescriptor::dump() const {
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
                         WaitQueue::Worker &worker,
                         const ProtocolConformanceDescriptor *conformance,
                         const void * const *instantiationArgs)
    : SimpleLockingCacheEntryBase(worker),
      Type(type), Conformance(conformance) {}

  intptr_t getKeyIntValueForDump() const {
    return reinterpret_cast<intptr_t>(Type);
  }

  friend llvm::hash_code hash_value(const WitnessTableCacheEntry &value) {
    return llvm::hash_value(value.Type);
  }

  /// The key value of the entry is just its type pointer.
  bool matchesKey(const Metadata *type) {
    return Type == type;
  }

  static size_t getExtraAllocationSize(
                             const Metadata *type,
                             WaitQueue::Worker &worker,
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

using GenericWitnessTableCache =
  MetadataCache<WitnessTableCacheEntry, GenericWitnessTableCacheTag>;
using LazyGenericWitnessTableCache = Lazy<GenericWitnessTableCache>;

class GlobalWitnessTableCacheEntry {
public:
  const GenericWitnessTable *Gen;
  GenericWitnessTableCache Cache;

  GlobalWitnessTableCacheEntry(const GenericWitnessTable *gen)
      : Gen(gen), Cache() {}

  intptr_t getKeyIntValueForDump() {
    return reinterpret_cast<intptr_t>(Gen);
  }
  bool matchesKey(const GenericWitnessTable *gen) const {
    return gen == Gen;
  }
  friend llvm::hash_code hash_value(const GlobalWitnessTableCacheEntry &value) {
    return llvm::hash_value(value.Gen);
  }
  static size_t
  getExtraAllocationSize(const GenericWitnessTable *gen) {
    return 0;
  }
  size_t getExtraAllocationSize() const { return 0; }
};

static SimpleGlobalCache<GlobalWitnessTableCacheEntry, GlobalWitnessTableCacheTag>
    GlobalWitnessTableCache;

} // end anonymous namespace

/// Fetch the cache for a generic witness-table structure.
static GenericWitnessTableCache &getCache(const GenericWitnessTable *gen) {
  // Keep this assert even if you change the representation above.
  static_assert(sizeof(LazyGenericWitnessTableCache) <=
                sizeof(GenericWitnessTable::PrivateDataType),
                "metadata cache is larger than the allowed space");

  if (gen->PrivateData == nullptr) {
    return GlobalWitnessTableCache.getOrInsert(gen).first->Cache;
  }

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

#if SWIFT_PTRAUTH
static const unsigned swift_ptrauth_key_associated_type =
  ptrauth_key_process_independent_code;

/// Given an unsigned pointer to an associated-type protocol witness,
/// fill in the appropriate slot in the witness table we're building.
static void initAssociatedTypeProtocolWitness(const Metadata **slot,
                                              const Metadata *witness,
                                              const ProtocolRequirement &reqt) {
  assert(reqt.Flags.getKind() ==
           ProtocolRequirementFlags::Kind::AssociatedTypeAccessFunction);
  // FIXME: this should use ptrauth_key_process_independent_data
  // now that it no longer stores a function pointer.
  swift_ptrauth_init(slot, witness, reqt.Flags.getExtraDiscriminator());
}

static const unsigned swift_ptrauth_key_associated_conformance =
  ptrauth_key_process_independent_code;

/// Given an unsigned pointer to an associated-conformance protocol witness,
/// fill in the appropriate slot in the witness table we're building.
static void initAssociatedConformanceProtocolWitness(void **slot, void *witness,
                                              const ProtocolRequirement &reqt) {
  assert(reqt.Flags.getKind() ==
           ProtocolRequirementFlags::Kind::AssociatedConformanceAccessFunction);
  // FIXME: this should use ptrauth_key_process_independent_data
  // now that it no longer stores a function pointer.
  swift_ptrauth_init(slot, witness, reqt.Flags.getExtraDiscriminator());
}
#endif

/// Given an unsigned pointer to an arbitrary protocol witness, fill
/// in a slot in the witness table we're building.
static void initProtocolWitness(void **slot, void *witness,
                                const ProtocolRequirement &reqt) {
#if SWIFT_PTRAUTH
  switch (reqt.Flags.getKind()) {
  // Base protocols use no signing at all right now.
  case ProtocolRequirementFlags::Kind::BaseProtocol:
    *slot = witness;
    return;

  // Method requirements use address-discriminated signing with the
  // function-pointer key.
  case ProtocolRequirementFlags::Kind::Method:
  case ProtocolRequirementFlags::Kind::Init:
  case ProtocolRequirementFlags::Kind::Getter:
  case ProtocolRequirementFlags::Kind::Setter:
  case ProtocolRequirementFlags::Kind::ReadCoroutine:
  case ProtocolRequirementFlags::Kind::ModifyCoroutine:
    swift_ptrauth_init_code_or_data(slot, witness,
                                    reqt.Flags.getExtraDiscriminator(),
                                    !reqt.Flags.isAsync());
    return;

  case ProtocolRequirementFlags::Kind::AssociatedConformanceAccessFunction:
    initAssociatedConformanceProtocolWitness(slot, witness, reqt);
    return;

  case ProtocolRequirementFlags::Kind::AssociatedTypeAccessFunction:
    initAssociatedTypeProtocolWitness(reinterpret_cast<const Metadata **>(
                                        const_cast<const void**>(slot)),
                                      reinterpret_cast<const Metadata *>(
                                        witness),
                                      reqt);
    return;
  }
  swift_unreachable("bad witness kind");
#else
  *slot = witness;
#endif
}

/// Copy an arbitrary protocol witness from another table.
static void copyProtocolWitness(void **dest, void * const *src,
                                const ProtocolRequirement &reqt) {
#if SWIFT_PTRAUTH
  switch (reqt.Flags.getKind()) {
  // Base protocols use no signing at all right now.
  case ProtocolRequirementFlags::Kind::BaseProtocol:
    *dest = *src;
    return;

  // Method requirements use address-discriminated signing with the
  // function-pointer key.
  case ProtocolRequirementFlags::Kind::Method:
  case ProtocolRequirementFlags::Kind::Init:
  case ProtocolRequirementFlags::Kind::Getter:
  case ProtocolRequirementFlags::Kind::Setter:
  case ProtocolRequirementFlags::Kind::ReadCoroutine:
  case ProtocolRequirementFlags::Kind::ModifyCoroutine:
    swift_ptrauth_copy_code_or_data(
        dest, src, reqt.Flags.getExtraDiscriminator(), !reqt.Flags.isAsync(),
        /*allowNull*/ true); // NULL allowed for VFE (methods in the vtable
                             // might be proven unused and null'ed)
    return;

  // FIXME: these should both use ptrauth_key_process_independent_data now.
  case ProtocolRequirementFlags::Kind::AssociatedConformanceAccessFunction:
  case ProtocolRequirementFlags::Kind::AssociatedTypeAccessFunction:
    swift_ptrauth_copy(
        dest, src, reqt.Flags.getExtraDiscriminator(),
        /*allowNull*/ true); // NULL allowed for VFE (methods in the vtable
                             // might be proven unused and null'ed)
    return;
  }
  swift_unreachable("bad witness kind");
#else
  *dest = *src;
#endif
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

    auto &reqt = requirements[reqDescriptor - requirements.begin()];
    // This is an unsigned pointer formed from a relative address.
    void *impl = witness.getWitness(reqt.Flags);
    initProtocolWitness(&table[witnessIndex], impl, reqt);
  }

  // Loop over the requirements, filling in default implementations where
  // needed.
  for (size_t i = 0, e = protocol->NumRequirements; i < e; ++i) {
    unsigned witnessIndex = WitnessTableFirstRequirementOffset + i;

    // If we don't have a witness, fill in the default implementation.
    // If we already have a witness, there's nothing to do.
    auto &reqt = requirements[i];
    if (!table[witnessIndex]) {
      // This is an unsigned pointer formed from a relative address.
      void *impl = reqt.getDefaultImplementation();
      initProtocolWitness(&table[witnessIndex], impl, reqt);
    }

    // Realize base protocol witnesses.
    if (reqt.Flags.getKind() == ProtocolRequirementFlags::Kind::BaseProtocol &&
        table[witnessIndex]) {
      // Realize the base protocol witness table.  We call the slow function
      // because the fast function doesn't allow base protocol requirements.
      auto baseReq = protocol->getRequirementBaseDescriptor();
      (void)swift_getAssociatedConformanceWitnessSlow((WitnessTable *)table,
                                                      conformingType,
                                                      conformingType,
                                                      baseReq, &reqt);
    }
  }
}

// Instantiate a generic or resilient witness table into a `buffer`
// that has already been allocated of the appropriate size and zeroed out.
static WitnessTable *
instantiateWitnessTable(const Metadata *Type,
                        const ProtocolConformanceDescriptor *conformance,
                        const void * const *instantiationArgs,
                        void **fullTable) {
  auto protocol = conformance->getProtocol();
  auto genericTable = conformance->getGenericWitnessTable();
  auto *genericArgs = Type->getGenericArgs();

  // The number of witnesses provided by the table pattern.
  size_t numPatternWitnesses = genericTable->WitnessTableSizeInWords;

  // The total number of requirements.
  size_t numRequirements =
    protocol->NumRequirements + WitnessTableFirstRequirementOffset;
  assert(numPatternWitnesses <= numRequirements);
  (void)numRequirements;

  // Number of bytes for any private storage used by the conformance itself.
  size_t privateSizeInWords = genericTable->getWitnessTablePrivateSizeInWords();

  // Advance the address point; the private storage area is accessed via
  // negative offsets.
  auto table = fullTable + privateSizeInWords;
  if (auto pattern =
          reinterpret_cast<void * const *>(
            &*conformance->getWitnessTablePattern())) {
    auto requirements = protocol->getRequirements();

    // Fill in the provided part of the requirements from the pattern.
    for (size_t i = 0, e = numPatternWitnesses; i < e; ++i) {
      size_t requirementIndex = i - WitnessTableFirstRequirementOffset;
      if (i < WitnessTableFirstRequirementOffset)
        table[i] = pattern[i];
      else
        copyProtocolWitness(&table[i], &pattern[i],
                            requirements[requirementIndex]);
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

    llvm::ArrayRef<GenericPackShapeDescriptor> packShapeDescriptors =
        conformance->getConditionalPackShapeDescriptors();
    unsigned packIdx = 0;

    for (const auto &conditionalRequirement
          : conformance->getConditionalRequirements()) {
      if (!conditionalRequirement.Flags.hasKeyArgument())
        continue;

      assert(currentInstantiationArg < privateSizeInWords);

      auto *instantiationArg = instantiationArgs[currentInstantiationArg];

      // Heap-allocate witness tables for conditional pack conformance requirements.
      if (conditionalRequirement.Flags.isPackRequirement()) {
        auto packShapeDescriptor = packShapeDescriptors[packIdx];
        assert(packShapeDescriptor.Kind == GenericPackKind::WitnessTable);
        assert(packShapeDescriptor.Index == currentInstantiationArg);
        size_t count = reinterpret_cast<const size_t>(
            genericArgs[packShapeDescriptor.ShapeClass]);

        auto *wtable = reinterpret_cast<const WitnessTable * const*>(instantiationArg);
        wtable = swift_allocateWitnessTablePack(wtable, count);

        instantiationArg = wtable;
        ++packIdx;
      }

      table[-1 - (int)currentInstantiationArg] = const_cast<void *>(instantiationArg);

      ++currentInstantiationArg;
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
/// Instantiate a brand new witness table for a resilient or generic
/// protocol conformance.
WitnessTable *
WitnessTableCacheEntry::allocate(
                               const ProtocolConformanceDescriptor *conformance,
                               const void * const *instantiationArgs) {
  // Find the allocation.
  void **fullTable = reinterpret_cast<void**>(this + 1);

  // Zero out the witness table.
  memset(fullTable, 0, getWitnessTableSize(conformance));

  // Instantiate the table.
  return instantiateWitnessTable(Type, Conformance, instantiationArgs, fullTable);
}

/// Instantiate the witness table for a nondependent conformance that only has
/// one possible instantiation.
static WitnessTable *
getNondependentWitnessTable(const ProtocolConformanceDescriptor *conformance,
                            const Metadata *type) {
  assert(conformance->getGenericWitnessTable()->PrivateData != nullptr);

  // Check whether the table has already been instantiated.
  auto tablePtr = reinterpret_cast<std::atomic<WitnessTable*> *>(
                     conformance->getGenericWitnessTable()->PrivateData.get());
  
  auto existingTable = tablePtr->load(SWIFT_MEMORY_ORDER_CONSUME);
  if (existingTable) {
    return existingTable;
  }

  // Allocate space for the table.
  auto tableSize = WitnessTableCacheEntry::getWitnessTableSize(conformance);
  TaggedMetadataAllocator<SingletonGenericWitnessTableCacheTag> allocator;
  auto buffer = (void **)allocator.Allocate(tableSize, alignof(void*));
  memset(buffer, 0, tableSize);
  
  // Instantiate the table.
  auto table = instantiateWitnessTable(type, conformance, nullptr, buffer);
  
  // See whether we can claim to be the one true table.
  WitnessTable *orig = nullptr;
  if (!tablePtr->compare_exchange_strong(orig, table, std::memory_order_release,
                                         SWIFT_MEMORY_ORDER_CONSUME)) {
    // Someone beat us to the punch. Throw away our table and return the
    // existing one.
    allocator.Deallocate(buffer);
    return orig;
  }
  
  return table;
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
  auto genericTable = conformance->getGenericWitnessTable();
  if (!genericTable || doesNotRequireInstantiation(conformance, genericTable)) {
    return uniqueForeignWitnessTableRef(conformance->getWitnessTablePattern());
  }
  
  // If the conformance is not dependent on generic arguments in the conforming
  // type, then there is only one instantiation possible, so we can try to
  // allocate only the table without the concurrent map structure.
  //
  // TODO: There is no metadata flag that directly encodes the "nondependent"
  // as of the Swift 5.3 ABI. However, we can check whether the conforming
  // type is generic; a nongeneric type's conformance can never be dependent (at
  // least, not today). However, a generic type conformance may also be
  // nondependent if it 
  auto typeDescription = conformance->getTypeDescriptor();
  if (typeDescription && !typeDescription->isGeneric() &&
      genericTable->PrivateData != nullptr) {
    return getNondependentWitnessTable(conformance, type);
  }

  auto &cache = getCache(genericTable);
  auto result = cache.getOrInsert(type, conformance, instantiationArgs);

  // Our returned 'status' is the witness table itself.
  return uniqueForeignWitnessTableRef(result.second);
}

namespace {

/// A cache-entry type suitable for use with LockingConcurrentMap.
class RelativeWitnessTableCacheEntry :
    public SimpleLockingCacheEntryBase<RelativeWitnessTableCacheEntry,
                                       RelativeWitnessTable*> {
  /// The type for which this table was instantiated.
  const Metadata * const Type;

  /// The protocol conformance descriptor. This is only kept around so that we
  /// can compute the size of an entry correctly in case of a race to
  /// allocate the entry.
  const ProtocolConformanceDescriptor * const Conformance;

public:
  /// Do the structural initialization necessary for this entry to appear
  /// in a concurrent map.
  RelativeWitnessTableCacheEntry(const Metadata *type,
                         WaitQueue::Worker &worker,
                         const ProtocolConformanceDescriptor *conformance,
                         const void * const *instantiationArgs)
    : SimpleLockingCacheEntryBase(worker),
      Type(type), Conformance(conformance) {}

  intptr_t getKeyIntValueForDump() const {
    return reinterpret_cast<intptr_t>(Type);
  }

  friend llvm::hash_code hash_value(const RelativeWitnessTableCacheEntry &value) {
    return llvm::hash_value(value.Type);
  }

  /// The key value of the entry is just its type pointer.
  bool matchesKey(const Metadata *type) {
    return Type == type;
  }

  static size_t getExtraAllocationSize(
                             const Metadata *type,
                             WaitQueue::Worker &worker,
                             const ProtocolConformanceDescriptor *conformance,
                             const void * const *instantiationArgs) {
    return getWitnessTableSize(conformance);
  }

  size_t getExtraAllocationSize() const {
    return getWitnessTableSize(Conformance);
  }

  static size_t getNumBaseProtocolRequirements(
    const ProtocolConformanceDescriptor *conformance) {

    size_t result = 0;
    size_t currIdx = 0;
    auto protocol = conformance->getProtocol();
    auto requirements = protocol->getRequirements();
    for (auto &req : requirements) {
       ++currIdx;
       if (req.Flags.getKind() ==
           ProtocolRequirementFlags::Kind::BaseProtocol) {
         ++result;
         // We currently assume that base protocol requirements preceed other
         // requirements i.e we store the base protocol pointers sequentially in
         // instantiateRelativeWitnessTable starting at index 1.
         assert(currIdx == result &&
                "base protocol requirements come before everything else");
         (void)currIdx;
       }
    }
    return result;
  }

  static size_t getWitnessTableSize(
                            const ProtocolConformanceDescriptor *conformance) {
    auto genericTable = conformance->getGenericWitnessTable();

    size_t numPrivateWords = genericTable->getWitnessTablePrivateSizeInWords();

    size_t numRequirementWords =
      WitnessTableFirstRequirementOffset +
      getNumBaseProtocolRequirements(conformance);

    return (numPrivateWords + numRequirementWords) * sizeof(void*);
  }

  RelativeWitnessTable *allocate(const ProtocolConformanceDescriptor *conformance,
                         const void * const *instantiationArgs);
};

using RelativeGenericWitnessTableCache =
  MetadataCache<RelativeWitnessTableCacheEntry, GenericWitnessTableCacheTag>;
using LazyRelativeGenericWitnessTableCache = Lazy<RelativeGenericWitnessTableCache>;

class GlobalRelativeWitnessTableCacheEntry {
public:
  const GenericWitnessTable *Gen;
  RelativeGenericWitnessTableCache Cache;

  GlobalRelativeWitnessTableCacheEntry(const GenericWitnessTable *gen)
      : Gen(gen), Cache() {}

  intptr_t getKeyIntValueForDump() {
    return reinterpret_cast<intptr_t>(Gen);
  }
  bool matchesKey(const GenericWitnessTable *gen) const {
    return gen == Gen;
  }
  friend llvm::hash_code hash_value(const GlobalRelativeWitnessTableCacheEntry &value) {
    return llvm::hash_value(value.Gen);
  }
  static size_t
  getExtraAllocationSize(const GenericWitnessTable *gen) {
    return 0;
  }
  size_t getExtraAllocationSize() const { return 0; }
};

static SimpleGlobalCache<GlobalRelativeWitnessTableCacheEntry,
                         GlobalWitnessTableCacheTag>
                         GlobalRelativeWitnessTableCache;

} // end anonymous namespace

using RelativeBaseWitness = RelativeDirectPointer<void, true /*nullable*/>;

// Instantiate a relative witness table into a `buffer`
// that has already been allocated of the appropriate size and zeroed out.
//
// The layout of a dynamically allocated relative witness table is:
//             [ conditional conformance n] ... private area
//             [ conditional conformance 0]     (negatively adressed)
// pointer ->  [ pointer to relative witness table (pattern) ]
//             [ base protocol witness table pointer 0 ] ... base protocol
//             [ base protocol witness table pointer n ]     pointers
static RelativeWitnessTable *
instantiateRelativeWitnessTable(const Metadata *Type,
                        const ProtocolConformanceDescriptor *conformance,
                        const void * const *instantiationArgs,
                        void **fullTable) {
  auto genericTable = conformance->getGenericWitnessTable();
  auto pattern = reinterpret_cast<uint32_t const *>(
            &*conformance->getWitnessTablePattern());
  assert(pattern);

  auto numBaseProtocols =
    RelativeWitnessTableCacheEntry::getNumBaseProtocolRequirements(conformance);

  // The number of witnesses provided by the table pattern.
  size_t numPatternWitnesses = genericTable->WitnessTableSizeInWords;
  assert(numBaseProtocols <= numPatternWitnesses);
  (void)numPatternWitnesses;

  // Number of bytes for any private storage used by the conformance itself.
  size_t privateSizeInWords = genericTable->getWitnessTablePrivateSizeInWords();

  // Advance the address point; the private storage area is accessed via
  // negative offsets.
  auto table = fullTable + privateSizeInWords;
#if SWIFT_PTRAUTH
  table[0] = ptrauth_sign_unauthenticated(
        (void*)pattern,
        ptrauth_key_process_independent_data,
        SpecialPointerAuthDiscriminators::RelativeProtocolWitnessTable);
#else
  table[0] = (void*)pattern;
#endif

  assert(1 == WitnessTableFirstRequirementOffset);

  // Fill in the base protocols of the requirements from the pattern.
  for (size_t i = 0, e = numBaseProtocols; i < e; ++i) {
    size_t index = i + WitnessTableFirstRequirementOffset;
#if SWIFT_PTRAUTH
    auto rawValue = ((RelativeBaseWitness const *)pattern)[index].get();
    table[index] = (rawValue == nullptr) ? rawValue :
      ptrauth_sign_unauthenticated(
        rawValue,
        ptrauth_key_process_independent_data,
        SpecialPointerAuthDiscriminators::RelativeProtocolWitnessTable);
#else
    table[index] = ((RelativeBaseWitness const *)pattern)[index].get();
#endif
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

      assert(!conditionalRequirement.Flags.isPackRequirement() &&
             "Not supported yet");
    }
  }


  // Call the instantiation function if present.
  if (!genericTable->Instantiator.isNull()) {
    auto castTable = reinterpret_cast<WitnessTable*>(table);
    genericTable->Instantiator(castTable, Type, instantiationArgs);
  }

  return reinterpret_cast<RelativeWitnessTable*>(table);
}

/// Instantiate a brand new relative witness table for a generic protocol conformance.
RelativeWitnessTable *
RelativeWitnessTableCacheEntry::allocate(
                               const ProtocolConformanceDescriptor *conformance,
                               const void * const *instantiationArgs) {
  // Find the allocation.
  void **fullTable = reinterpret_cast<void**>(this + 1);

  // Zero out the witness table.
  memset(fullTable, 0, getWitnessTableSize(conformance));

  // Instantiate the table.
  return instantiateRelativeWitnessTable(Type, Conformance, instantiationArgs,
                                         fullTable);
}

/// Fetch the cache for a generic witness-table structure.
static RelativeGenericWitnessTableCache &getCacheForRelativeWitness(
  const GenericWitnessTable *gen) {

  // Keep this assert even if you change the representation above.
  static_assert(sizeof(LazyRelativeGenericWitnessTableCache) <=
                sizeof(GenericWitnessTable::PrivateDataType),
                "metadata cache is larger than the allowed space");

  if (gen->PrivateData == nullptr) {
    return GlobalRelativeWitnessTableCache.getOrInsert(gen).first->Cache;
  }

  auto lazyCache =
    reinterpret_cast<LazyRelativeGenericWitnessTableCache*>(gen->PrivateData.get());
  return lazyCache->get();
}

const RelativeWitnessTable *
swift::swift_getWitnessTableRelative(const ProtocolConformanceDescriptor *conformance,
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

  auto genericTable = conformance->getGenericWitnessTable();

  // When there is no generic table, or it doesn't require instantiation,
  // use the pattern directly.
  if (!genericTable || doesNotRequireInstantiation(conformance, genericTable)) {
    assert(!conformance->isSynthesizedNonUnique());
    auto pattern = conformance->getWitnessTablePattern();
    auto table = uniqueForeignWitnessTableRef(pattern);

  #if SWIFT_STDLIB_USE_RELATIVE_PROTOCOL_WITNESS_TABLES && SWIFT_PTRAUTH
    table = ptrauth_sign_unauthenticated(table,
          ptrauth_key_process_independent_data,
          SpecialPointerAuthDiscriminators::RelativeProtocolWitnessTable);
  #endif

    return reinterpret_cast<const RelativeWitnessTable*>(table);
  }

  assert(genericTable &&
         !doesNotRequireInstantiation(conformance, genericTable));
  assert(!conformance->isSynthesizedNonUnique());

  auto &cache = getCacheForRelativeWitness(genericTable);
  auto result = cache.getOrInsert(type, conformance, instantiationArgs);

  // Our returned 'status' is the witness table itself.
  auto table = uniqueForeignWitnessTableRef(
    (const WitnessTable*)result.second);

  // Mark this as a dynamic (conditional conformance) protocol witness table.
  return reinterpret_cast<RelativeWitnessTable*>(((uintptr_t)table) |
                                                 (uintptr_t)0x1);
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

using AssociatedTypeWitness = std::atomic<const Metadata *>;

SWIFT_CC(swift)
static MetadataResponse
swift_getAssociatedTypeWitnessSlowImpl(
                                      MetadataRequest request,
                                      WitnessTable *wtable,
                                      const Metadata *conformingType,
                                      const ProtocolRequirement *reqBase,
                                      const ProtocolRequirement *assocType) {
#ifndef NDEBUG
  {
    const ProtocolConformanceDescriptor *conformance = wtable->getDescription();
    const ProtocolDescriptor *protocol = conformance->getProtocol();
    auto requirements = protocol->getRequirements();
    assert(assocType >= requirements.begin() &&
           assocType < requirements.end());
    assert(reqBase == requirements.data() - WitnessTableFirstRequirementOffset);
    assert(assocType->Flags.getKind() ==
           ProtocolRequirementFlags::Kind::AssociatedTypeAccessFunction);
  }
#endif

  // Retrieve the witness.
  unsigned witnessIndex = assocType - reqBase;
  auto *witnessAddr = &((AssociatedTypeWitness*)wtable)[witnessIndex];
  auto witness = witnessAddr->load(std::memory_order_acquire);

#if SWIFT_PTRAUTH
  uint16_t extraDiscriminator = assocType->Flags.getExtraDiscriminator();
  witness = ptrauth_auth_data(witness, swift_ptrauth_key_associated_type,
                              ptrauth_blend_discriminator(witnessAddr,
                                                          extraDiscriminator));
#endif
  
  // If the low bit of the witness is clear, it's already a metadata pointer.
  if (SWIFT_LIKELY((reinterpret_cast<uintptr_t>(witness) &
                    ProtocolRequirementFlags::AssociatedTypeMangledNameBit) ==
                   0)) {
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
  const ProtocolConformanceDescriptor *conformance = wtable->getDescription();
  const ProtocolDescriptor *protocol = conformance->getProtocol();

  // Extract the mangled name itself.
  StringRef mangledName =
    Demangle::makeSymbolicMangledNameStringRef(mangledNameBase);

  // Demangle the associated type.
  TypeLookupErrorOr<TypeInfo> result;
  if (inProtocolContext) {
    // The protocol's Self is the only generic parameter that can occur in the
    // type.
    result = swift_getTypeByMangledName(
        request, mangledName, nullptr,
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
        });
  } else {
    // The generic parameters in the associated type name are those of the
    // conforming type.

    // For a class, chase the superclass chain up until we hit the
    // type that specified the conformance.
    auto originalConformingType = findConformingSuperclass(conformingType,
                                                           conformance);
    SubstGenericParametersFromMetadata substitutions(originalConformingType);
    result = swift_getTypeByMangledName(
        request, mangledName, substitutions.getGenericArgs(),
        [&substitutions](unsigned depth, unsigned index) {
          // FIXME: Variadic generics
          return substitutions.getMetadata(depth, index).getMetadata();
        },
        [&substitutions](const Metadata *type, unsigned index) {
          return substitutions.getWitnessTable(type, index);
        });
  }
  auto *error = result.getError();
  MetadataResponse response = result.getType().getResponse();
  auto assocTypeMetadata = response.Value;
  if (error || !assocTypeMetadata) {
    const char *errStr = error ? error->copyErrorString()
                               : "NULL metadata but no error was provided";
    auto conformingTypeNameInfo = swift_getTypeName(conformingType, true);
    StringRef conformingTypeName(conformingTypeNameInfo.data,
                                 conformingTypeNameInfo.length);
    StringRef assocTypeName = findAssociatedTypeName(protocol, assocType);
    fatalError(0,
               "failed to demangle witness for associated type '%s' in "
               "conformance '%s: %s' from mangled name '%s' - %s\n",
               assocTypeName.str().c_str(), conformingTypeName.str().c_str(),
               protocol->Name.get(), mangledName.str().c_str(), errStr);
  }

  assert((uintptr_t(assocTypeMetadata) &
            ProtocolRequirementFlags::AssociatedTypeMangledNameBit) == 0);

  // If the metadata was completed, record it in the witness table.
  if (response.State == MetadataState::Complete) {
    // We pass type metadata around as unsigned pointers, but we sign them
    // in witness tables, which doesn't provide all that much extra security.
    auto valueToStore = assocTypeMetadata;
#if SWIFT_PTRAUTH
    valueToStore = ptrauth_sign_unauthenticated(valueToStore,
                       swift_ptrauth_key_associated_type,
                       ptrauth_blend_discriminator(witnessAddr,
                                                   extraDiscriminator));
#endif
    witnessAddr->store(valueToStore, std::memory_order_release);
  }

  return response;
}

MetadataResponse
swift::swift_getAssociatedTypeWitness(MetadataRequest request,
                                      WitnessTable *wtable,
                                      const Metadata *conformingType,
                                      const ProtocolRequirement *reqBase,
                                      const ProtocolRequirement *assocType) {
  assert(assocType->Flags.getKind() ==
           ProtocolRequirementFlags::Kind::AssociatedTypeAccessFunction);

  // If the low bit of the witness is clear, it's already a metadata pointer.
  unsigned witnessIndex = assocType - reqBase;
  auto *witnessAddr = &((const AssociatedTypeWitness *)wtable)[witnessIndex];
  auto witness = witnessAddr->load(std::memory_order_acquire);

#if SWIFT_PTRAUTH
  uint16_t extraDiscriminator = assocType->Flags.getExtraDiscriminator();
  witness = ptrauth_auth_data(witness, swift_ptrauth_key_associated_type,
                              ptrauth_blend_discriminator(witnessAddr,
                                                          extraDiscriminator));
#endif

  if (SWIFT_LIKELY((reinterpret_cast<uintptr_t>(witness) &
                    ProtocolRequirementFlags::AssociatedTypeMangledNameBit) ==
                   0)) {
    // Cached metadata pointers are always complete.
    return MetadataResponse{(const Metadata *)witness, MetadataState::Complete};
  }

  return swift_getAssociatedTypeWitnessSlow(request, wtable, conformingType,
                                            reqBase, assocType);
}

RelativeWitnessTable *swift::lookThroughOptionalConditionalWitnessTable(
    const RelativeWitnessTable *wtable) {
  uintptr_t conditional_wtable = (uintptr_t)wtable;
  if (conditional_wtable & 0x1) {
    conditional_wtable = conditional_wtable & ~(uintptr_t)(0x1);
    conditional_wtable = (uintptr_t)*(void**)conditional_wtable;

  }
  auto table = (RelativeWitnessTable*)conditional_wtable;
#if SWIFT_PTRAUTH
  table = swift_auth_data_non_address(
        table,
        SpecialPointerAuthDiscriminators::RelativeProtocolWitnessTable);
#endif
  return table;
}

SWIFT_CC(swift)
static MetadataResponse
swift_getAssociatedTypeWitnessRelativeSlowImpl(
                                      MetadataRequest request,
                                      RelativeWitnessTable *wtable,
                                      const Metadata *conformingType,
                                      const ProtocolRequirement *reqBase,
                                      const ProtocolRequirement *assocType) {

  wtable = lookThroughOptionalConditionalWitnessTable(wtable);

#ifndef NDEBUG
  {
    const ProtocolConformanceDescriptor *conformance = wtable->getDescription();
    const ProtocolDescriptor *protocol = conformance->getProtocol();
    auto requirements = protocol->getRequirements();
    assert(assocType >= requirements.begin() &&
           assocType < requirements.end());
    assert(reqBase == requirements.data() - WitnessTableFirstRequirementOffset);
    assert(assocType->Flags.getKind() ==
           ProtocolRequirementFlags::Kind::AssociatedTypeAccessFunction);
  }
#endif

  // Retrieve the witness.
  unsigned witnessIndex = assocType - reqBase;

  auto *relativeDistanceAddr = &((int32_t*)wtable)[witnessIndex];
  auto relativeDistance = *relativeDistanceAddr;
  auto value = swift::detail::applyRelativeOffset(relativeDistanceAddr,
                                                  relativeDistance);
  assert((value & 0x1) && "Expecting the bit to be set");
  value = value & ~((uintptr_t)0x1);
  const char *mangledNameBase = (const char*) value;

  // Dig out the protocol.
  const ProtocolConformanceDescriptor *conformance = wtable->getDescription();
  const ProtocolDescriptor *protocol = conformance->getProtocol();

  // Extract the mangled name itself.
  StringRef mangledName =
    Demangle::makeSymbolicMangledNameStringRef(mangledNameBase);

  // The generic parameters in the associated type name are those of the
  // conforming type.

  // For a class, chase the superclass chain up until we hit the
  // type that specified the conformance.
  auto originalConformingType = findConformingSuperclass(conformingType,
                                                         conformance);
  SubstGenericParametersFromMetadata substitutions(originalConformingType);
  auto result = swift_getTypeByMangledName(
      request, mangledName, substitutions.getGenericArgs(),
      [&substitutions](unsigned depth, unsigned index) {
        // FIXME: Variadic generics
        return substitutions.getMetadata(depth, index).getMetadata();
      },
      [&substitutions](const Metadata *type, unsigned index) {
        return substitutions.getWitnessTable(type, index);
      });

  auto *error = result.getError();
  MetadataResponse response = result.getType().getResponse();
  auto assocTypeMetadata = response.Value;
  if (error || !assocTypeMetadata) {
    const char *errStr = error ? error->copyErrorString()
                               : "NULL metadata but no error was provided";
    auto conformingTypeNameInfo = swift_getTypeName(conformingType, true);
    StringRef conformingTypeName(conformingTypeNameInfo.data,
                                 conformingTypeNameInfo.length);
    StringRef assocTypeName = findAssociatedTypeName(protocol, assocType);
    fatalError(0,
               "failed to demangle witness for associated type '%s' in "
               "conformance '%s: %s' from mangled name '%s' - %s\n",
               assocTypeName.str().c_str(), conformingTypeName.str().c_str(),
               protocol->Name.get(), mangledName.str().c_str(), errStr);
  }

  assert((uintptr_t(assocTypeMetadata) &
            ProtocolRequirementFlags::AssociatedTypeMangledNameBit) == 0);

  return response;
}

MetadataResponse
swift::swift_getAssociatedTypeWitnessRelative(MetadataRequest request,
                                      RelativeWitnessTable *wtable,
                                      const Metadata *conformingType,
                                      const ProtocolRequirement *reqBase,
                                      const ProtocolRequirement *assocType) {
  assert(assocType->Flags.getKind() ==
           ProtocolRequirementFlags::Kind::AssociatedTypeAccessFunction);

  return swift_getAssociatedTypeWitnessRelativeSlowImpl(request, wtable,
                                                    conformingType, reqBase,
                                                    assocType);
}

using AssociatedConformanceWitness = std::atomic<void *>;

SWIFT_CC(swift)
static const WitnessTable *swift_getAssociatedConformanceWitnessSlowImpl(
                                  WitnessTable *wtable,
                                  const Metadata *conformingType,
                                  const Metadata *assocType,
                                  const ProtocolRequirement *reqBase,
                                  const ProtocolRequirement *assocConformance) {
#ifndef NDEBUG
  {
    const ProtocolConformanceDescriptor *conformance = wtable->getDescription();
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
  auto *witnessAddr = &((AssociatedConformanceWitness*)wtable)[witnessIndex];
  auto witness = witnessAddr->load(std::memory_order_acquire);

#if SWIFT_PTRAUTH
  // For associated protocols, the witness is signed with address
  // discrimination.
  // For base protocols, the witness isn't signed at all.
  if (assocConformance->Flags.isSignedWithAddress()) {
    uint16_t extraDiscriminator =
      assocConformance->Flags.getExtraDiscriminator();
    witness = ptrauth_auth_data(
                witness, swift_ptrauth_key_associated_conformance,
                ptrauth_blend_discriminator(witnessAddr, extraDiscriminator));
  }
#endif

  // Fast path: we've already resolved this to a witness table, so return it.
  if (SWIFT_LIKELY((reinterpret_cast<uintptr_t>(witness) &
                    ProtocolRequirementFlags::AssociatedTypeMangledNameBit) ==
                   0)) {
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
    void *ptr = TargetCompactFunctionPointer<InProcess, void>::resolve(mangledName.data() + 1, offset);

    // Call the witness function.
    AssociatedWitnessTableAccessFunction *witnessFn;
#if SWIFT_PTRAUTH
    witnessFn =
        (AssociatedWitnessTableAccessFunction *)ptrauth_sign_unauthenticated(
            (void *)ptr, ptrauth_key_function_pointer, 0);
#else
    witnessFn = (AssociatedWitnessTableAccessFunction *)ptr;
#endif

    auto assocWitnessTable = witnessFn(assocType, conformingType, wtable);
    assert((uintptr_t(assocWitnessTable) &
            ProtocolRequirementFlags::AssociatedTypeMangledNameBit) == 0);

    // The access function returns an unsigned pointer for now.

    auto valueToStore = assocWitnessTable;
#if SWIFT_PTRAUTH
    if (assocConformance->Flags.isSignedWithAddress()) {
      uint16_t extraDiscriminator =
        assocConformance->Flags.getExtraDiscriminator();
      valueToStore = ptrauth_sign_unauthenticated(valueToStore,
                         swift_ptrauth_key_associated_conformance,
                         ptrauth_blend_discriminator(witnessAddr,
                                                     extraDiscriminator));
    }
#endif
    witnessAddr->store(valueToStore, std::memory_order_release);

    return assocWitnessTable;
  }

  swift_unreachable("Invalid mangled associate conformance");
}

const WitnessTable *swift::swift_getAssociatedConformanceWitness(
                                  WitnessTable *wtable,
                                  const Metadata *conformingType,
                                  const Metadata *assocType,
                                  const ProtocolRequirement *reqBase,
                                  const ProtocolRequirement *assocConformance) {
  // We avoid using this function for initializing base protocol conformances
  // so that we can have a better fast-path.
  assert(assocConformance->Flags.getKind() ==
           ProtocolRequirementFlags::Kind::AssociatedConformanceAccessFunction);

  // Retrieve the witness.
  unsigned witnessIndex = assocConformance - reqBase;
  auto *witnessAddr = &((AssociatedConformanceWitness*)wtable)[witnessIndex];
  auto witness = witnessAddr->load(std::memory_order_acquire);

#if SWIFT_PTRAUTH
  uint16_t extraDiscriminator = assocConformance->Flags.getExtraDiscriminator();
  witness = ptrauth_auth_data(witness, swift_ptrauth_key_associated_conformance,
                              ptrauth_blend_discriminator(witnessAddr,
                                                          extraDiscriminator));
#endif

  // Fast path: we've already resolved this to a witness table, so return it.
  if (SWIFT_LIKELY((reinterpret_cast<uintptr_t>(witness) &
                    ProtocolRequirementFlags::AssociatedTypeMangledNameBit) ==
                   0)) {
    return static_cast<const WitnessTable *>(witness);
  }

  return swift_getAssociatedConformanceWitnessSlow(wtable, conformingType,
                                                   assocType, reqBase,
                                                   assocConformance);
}

SWIFT_CC(swift)
static const RelativeWitnessTable *swift_getAssociatedConformanceWitnessRelativeSlowImpl(
                                  RelativeWitnessTable *wtable,
                                  const Metadata *conformingType,
                                  const Metadata *assocType,
                                  const ProtocolRequirement *reqBase,
                                  const ProtocolRequirement *assocConformance) {
  auto origWTable = wtable;
  wtable = lookThroughOptionalConditionalWitnessTable(wtable);

#ifndef NDEBUG
  {
    const ProtocolConformanceDescriptor *conformance = wtable->getDescription();
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

  auto *relativeDistanceAddr = &((int32_t*)wtable)[witnessIndex];
  auto relativeDistance = *relativeDistanceAddr;
  auto value = swift::detail::applyRelativeOffset(relativeDistanceAddr,
                                                  relativeDistance);
  assert((value & 0x1) && "Expecting the bit to be set");
  value = value & ~((uintptr_t)0x1);
  const char *mangledNameBase = (const char*) value;

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
    void *ptr = TargetCompactFunctionPointer<InProcess, void>::resolve(mangledName.data() + 1, offset);

    // Call the witness function.
    AssociatedRelativeWitnessTableAccessFunction *witnessFn;
#if SWIFT_PTRAUTH
    witnessFn =
        (AssociatedRelativeWitnessTableAccessFunction *)ptrauth_sign_unauthenticated(
            (void *)ptr, ptrauth_key_function_pointer, 0);
#else
    witnessFn = (AssociatedRelativeWitnessTableAccessFunction *)ptr;
#endif

    auto assocWitnessTable = witnessFn(assocType, conformingType, origWTable);

    // The access function returns an signed pointer.
    return assocWitnessTable;
  }

  swift_unreachable("Invalid mangled associate conformance");
}

const RelativeWitnessTable *swift::swift_getAssociatedConformanceWitnessRelative(
                                  RelativeWitnessTable *wtable,
                                  const Metadata *conformingType,
                                  const Metadata *assocType,
                                  const ProtocolRequirement *reqBase,
                                  const ProtocolRequirement *assocConformance) {
  // We avoid using this function for initializing base protocol conformances
  // so that we can have a better fast-path.
  assert(assocConformance->Flags.getKind() ==
           ProtocolRequirementFlags::Kind::AssociatedConformanceAccessFunction);

  return swift_getAssociatedConformanceWitnessRelativeSlowImpl(wtable, conformingType,
                                                   assocType, reqBase,
                                                   assocConformance);
}

bool swift::swift_compareWitnessTables(const WitnessTable *lhs,
                                       const WitnessTable *rhs) {
  return MetadataCacheKey::areWitnessTablesEqual(lhs, rhs);
}

bool swift::swift_compareProtocolConformanceDescriptors(
    const ProtocolConformanceDescriptor *lhs,
    const ProtocolConformanceDescriptor *rhs) {
  lhs = swift_auth_data_non_address(
      lhs, SpecialPointerAuthDiscriminators::ProtocolConformanceDescriptor);
  rhs = swift_auth_data_non_address(
      rhs, SpecialPointerAuthDiscriminators::ProtocolConformanceDescriptor);

  return MetadataCacheKey::areConformanceDescriptorsEqual(lhs, rhs);
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
    swift_unreachable("bad metadata initialization kind");
  }

  auto genericArgs =
    reinterpret_cast<const void * const *>(
                                    description->getGenericArguments(metadata));
  auto &cache = getCache(*description);
  assert(description->getFullGenericContextHeader().Base.NumKeyArguments == cache.SigLayout.sizeInWords());
  auto key = MetadataCacheKey(cache.SigLayout, genericArgs);

  return std::move(callbacks).forGenericMetadata(metadata, description,
                                                 cache, key);
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
      ForeignMetadataCacheEntry::Key key{description};
      return ForeignMetadata.get().await(key, Request);
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
    auto *genericContext = description->getGenericContext();

    auto keyArguments = description->getGenericArguments(type);

    // The generic argument area begins with a pack count for each
    // shape class; skip them first.
    auto header = genericContext->getGenericPackShapeHeader();
    unsigned paramIdx = header.NumShapeClasses;

    auto packs = genericContext->getGenericPackShapeDescriptors();
    unsigned packIdx = 0;
    for (auto &param : genericContext->getGenericParams()) {
      // Ignore parameters that don't have a key argument.
      if (!param.hasKeyArgument())
        continue;

      switch (param.getKind()) {
      case GenericParamKind::Type:
        if (predicate(keyArguments[paramIdx]))
          return true;

        break;

      case GenericParamKind::TypePack: {
        assert(packIdx < header.NumPacks);
        assert(packs[packIdx].Kind == GenericPackKind::Metadata);
        assert(packs[packIdx].Index == paramIdx);
        assert(packs[packIdx].ShapeClass < header.NumShapeClasses);

        MetadataPackPointer pack(keyArguments[paramIdx]);
        assert(pack.getLifetime() == PackLifetime::OnHeap);

        uintptr_t count = reinterpret_cast<uintptr_t>(
            keyArguments[packs[packIdx].ShapeClass]);
        for (uintptr_t j = 0; j < count; ++j) {
          if (predicate(pack.getElements()[j]))
            return true;
        }

        ++packIdx;
        break;
      }

      default:
        llvm_unreachable("Unsupported generic parameter kind");
      }

      ++paramIdx;
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
    auto state =
        MetadataResponse(swift_checkMetadataState(request, type)).State;

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
  return MetadataDependency();
}

/// Diagnose a metadata dependency cycle.
SWIFT_NORETURN static void
diagnoseMetadataDependencyCycle(llvm::ArrayRef<MetadataDependency> links) {
  assert(links.size() >= 2);
  assert(links.front().Value == links.back().Value);

  auto stringForRequirement = [](MetadataState req) -> const char * {
    switch (req) {
    case MetadataState::Complete:
      return "transitive completion of ";
    case MetadataState::NonTransitiveComplete:
      return "completion of ";
    case MetadataState::LayoutComplete:
      return "layout of ";
    case MetadataState::Abstract:
      return "abstract metadata for ";
    }
    return "<corrupted requirement> for ";
  };

  std::string diagnostic =
    "runtime error: unresolvable type metadata dependency cycle detected\n"
    "  Request for ";
  diagnostic += stringForRequirement(links.front().Requirement);
  diagnostic += nameForMetadata(links.front().Value);

  for (auto &link : links.drop_front()) {
    // If the diagnostic gets too large, just cut it short.
    if (diagnostic.size() >= 128 * 1024) {
      diagnostic += "\n  (cycle too long, limiting diagnostic text)";
      break;
    }

    diagnostic += "\n  depends on ";
    diagnostic += stringForRequirement(link.Requirement);
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
      .memoryAddress = links.front().Value
      // TODO: describe the cycle using notes instead of one huge message?
    };
#pragma GCC diagnostic pop

    _swift_reportToDebugger(RuntimeErrorFlagFatal, diagnostic.c_str(),
                            &details);
  }

  fatalError(0, "%s", diagnostic.c_str());
}

/// Check whether the given metadata dependency is satisfied, and if not,
/// return its current dependency, if one exists.
static MetadataStateWithDependency
checkMetadataDependency(MetadataDependency dependency) {
  struct CheckDependencyResult {
    MetadataState Requirement;

    MetadataStateWithDependency
    forGenericMetadata(const Metadata *type,
                       const TypeContextDescriptor *desc,
                       GenericMetadataCache &cache,
                       MetadataCacheKey key) && {
      return cache.checkDependency(key, Requirement);
    }

    MetadataStateWithDependency
    forForeignMetadata(const Metadata *metadata,
                       const TypeContextDescriptor *description) {
      ForeignMetadataCacheEntry::Key key{description};
      return ForeignMetadata.get().checkDependency(key, Requirement);
    }

    MetadataStateWithDependency
    forSingletonMetadata(const TypeContextDescriptor *description) {
      return SingletonMetadata.get().checkDependency(description, Requirement);
    }

    MetadataStateWithDependency
    forTupleMetadata(const TupleTypeMetadata *metadata) {
      return TupleTypes.get().checkDependency(metadata, Requirement);
    }

    MetadataStateWithDependency forOtherMetadata(const Metadata *type) {
      return { PrivateMetadataState::Complete, MetadataDependency() };
    }
  } callbacks = { dependency.Requirement };

  return performOnMetadataCache<MetadataStateWithDependency>(dependency.Value,
                                                       std::move(callbacks));
}

/// Check for an unbreakable metadata-dependency cycle.
void swift::blockOnMetadataDependency(MetadataDependency root,
                                      MetadataDependency firstLink) {
  std::vector<MetadataDependency> links;
  auto checkNewLink = [&](MetadataDependency newLink) {
    links.push_back(newLink);
    for (auto i = links.begin(), e = links.end() - 1; i != e; ++i) {
      if (i->Value == newLink.Value) {
        diagnoseMetadataDependencyCycle(
          llvm::makeArrayRef(&*i, links.end() - i));
      }
    }
  };

  links.push_back(root);

  // Iteratively add each link, checking for a cycle, until we reach
  // something without a known dependency.
  checkNewLink(firstLink);
  while (true) {
    // Try to get a dependency for the metadata in the last link we added.
    auto checkResult = checkMetadataDependency(links.back());

    // If there isn't a known dependency, we can't do any more checking.
    if (!checkResult.Dependency) {
      // In the special case where it's the first link that doesn't have
      // a known dependency and its current metadata state now satisfies
      // the dependency leading to it, we can skip waiting.
      if (links.size() == 2 && 
          satisfies(checkResult.NewState, links.back().Requirement))
        return;

      // Otherwise, just make a blocking request for the first link in
      // the chain.
      auto request = MetadataRequest(firstLink.Requirement);
      swift_checkMetadataState(request, firstLink.Value);
      return;
    }

    // Check the new link.
    checkNewLink(checkResult.Dependency);
  }
}

/***************************************************************************/
/*** Allocator implementation **********************************************/
/***************************************************************************/

#if !SWIFT_STDLIB_PASSTHROUGH_METADATA_ALLOCATOR

namespace {
  struct alignas(sizeof(uintptr_t) * 2) PoolRange {
    static constexpr uintptr_t PageSize = 16 * 1024;
    static constexpr uintptr_t MaxPoolAllocationSize = PageSize / 2;

    /// The start of the allocation.
    char *Begin;

    /// The number of bytes remaining.
    size_t Remaining;
  };

  /// The trailer placed at the end of each pool allocation, used when
  /// SWIFT_DEBUG_ENABLE_METADATA_ALLOCATION_ITERATION is on.
  struct PoolTrailer {
    void *PrevTrailer;
    size_t PoolSize;
  };

  static constexpr size_t InitialPoolSize = 64 * 1024;

  /// The header placed before each allocation, used when
  /// SWIFT_DEBUG_ENABLE_METADATA_ALLOCATION_ITERATION is on.
  struct alignas(void *) AllocationHeader {
    uint16_t Size;
    uint16_t Tag;
  };
} // end anonymous namespace

// A statically-allocated pool.  It's zero-initialized, so this
// doesn't cost us anything in binary size.
alignas(void *) static struct {
  char Pool[InitialPoolSize];
} InitialAllocationPool;
static swift::atomic<PoolRange>
AllocationPool{PoolRange{InitialAllocationPool.Pool,
                         sizeof(InitialAllocationPool.Pool)}};

bool swift::_swift_debug_metadataAllocationIterationEnabled = false;
const void * const swift::_swift_debug_allocationPoolPointer = &AllocationPool;
std::atomic<const void *> swift::_swift_debug_metadataAllocationBacktraceList;

static void recordBacktrace(void *allocation) {
  withCurrentBacktrace([&](void **addrs, int count) {
    MetadataAllocationBacktraceHeader<InProcess> *record =
        (MetadataAllocationBacktraceHeader<InProcess> *)malloc(
            sizeof(*record) + count * sizeof(void *));
    record->Allocation = allocation;
    record->Count = count;
    memcpy(record + 1, addrs, count * sizeof(void *));

    record->Next = _swift_debug_metadataAllocationBacktraceList.load(
        std::memory_order_relaxed);
    while (!_swift_debug_metadataAllocationBacktraceList.compare_exchange_weak(
        record->Next, record, std::memory_order_release,
        std::memory_order_relaxed))
      ; // empty
  });
}

static inline bool scribbleEnabled() {
#ifndef NDEBUG
  // When DEBUG is defined, always scribble.
  return true;
#else
  // When DEBUG is not defined, only scribble when the
  // SWIFT_DEBUG_ENABLE_MALLOC_SCRIBBLE environment variable is set.
  return SWIFT_UNLIKELY(
          runtime::environment::SWIFT_DEBUG_ENABLE_MALLOC_SCRIBBLE());
#endif
}

static constexpr char scribbleByte = 0xAA;

template <typename Pointee>
static inline void memsetScribble(Pointee *bytes, size_t totalSize) {
  if (scribbleEnabled())
    memset(bytes, scribbleByte, totalSize);
}

/// When scribbling is enabled, check the specified region for the scribble
/// values to detect overflows. When scribbling is disabled, this is a no-op.
static inline void checkScribble(char *bytes, size_t totalSize) {
  if (scribbleEnabled())
    for (size_t i = 0; i < totalSize; i++)
      if (bytes[i] != scribbleByte) {
        const size_t maxToPrint = 16;
        size_t remaining = totalSize - i;
        size_t toPrint = std::min(remaining, maxToPrint);
        std::string hex = toHex(llvm::StringRef{&bytes[i], toPrint});
        swift::fatalError(
            0, "corrupt metadata allocation arena detected at %p: %s%s",
            &bytes[i], hex.c_str(), toPrint < remaining ? "..." : "");
      }
}

static void checkAllocatorDebugEnvironmentVariables(void *context) {
  memsetScribble(InitialAllocationPool.Pool, InitialPoolSize);

  _swift_debug_metadataAllocationIterationEnabled =
      runtime::environment::SWIFT_DEBUG_ENABLE_METADATA_ALLOCATION_ITERATION();
  if (!_swift_debug_metadataAllocationIterationEnabled) {
    if (runtime::environment::SWIFT_DEBUG_ENABLE_METADATA_BACKTRACE_LOGGING())
      swift::warning(RuntimeErrorFlagNone,
                     "Warning: SWIFT_DEBUG_ENABLE_METADATA_BACKTRACE_LOGGING "
                     "without SWIFT_DEBUG_ENABLE_METADATA_ALLOCATION_ITERATION "
                     "has no effect.\n");
    return;
  }

  // Write a PoolTrailer to the end of InitialAllocationPool and shrink
  // the pool accordingly.
  auto poolCopy = AllocationPool.load(std::memory_order_relaxed);
  assert(poolCopy.Begin == InitialAllocationPool.Pool);
  size_t newPoolSize = InitialPoolSize - sizeof(PoolTrailer);
  PoolTrailer trailer = {nullptr, newPoolSize};
  memcpy(InitialAllocationPool.Pool + newPoolSize, &trailer, sizeof(trailer));
  poolCopy.Remaining = newPoolSize;
  AllocationPool.store(poolCopy, std::memory_order_relaxed);
}

void *MetadataAllocator::Allocate(size_t size, size_t alignment) {
  assert(Tag != 0);
  assert(alignment <= alignof(void*));
  assert(size % alignof(void*) == 0);

  static swift::once_t getenvToken;
  swift::once(getenvToken, checkAllocatorDebugEnvironmentVariables);

  // If the size is larger than the maximum, just do a normal heap allocation.
  if (size > PoolRange::MaxPoolAllocationSize) {
    void *allocation = swift_slowAlloc(size, alignment - 1);
    memsetScribble(allocation, size);
    return allocation;
  }

  // Allocate out of the pool.
  auto sizeWithHeader = size;
  if (SWIFT_UNLIKELY(_swift_debug_metadataAllocationIterationEnabled))
    sizeWithHeader += sizeof(AllocationHeader);
  PoolRange curState = AllocationPool.load(std::memory_order_relaxed);
  while (true) {
    char *allocation;
    PoolRange newState;
    bool allocatedNewPage;

    // Try to allocate out of the current page.
    if (sizeWithHeader <= curState.Remaining) {
      allocatedNewPage = false;
      allocation = curState.Begin;
      newState = PoolRange{curState.Begin + sizeWithHeader,
                           curState.Remaining - sizeWithHeader};
    } else {
      auto poolSize = PoolRange::PageSize;
      if (SWIFT_UNLIKELY(_swift_debug_metadataAllocationIterationEnabled))
        poolSize -= sizeof(PoolTrailer);
      allocatedNewPage = true;
      allocation = reinterpret_cast<char *>(swift_slowAlloc(PoolRange::PageSize,
                                                            alignof(char) - 1));
      memsetScribble(allocation, PoolRange::PageSize);

      if (SWIFT_UNLIKELY(_swift_debug_metadataAllocationIterationEnabled)) {
        PoolTrailer *newTrailer = (PoolTrailer *)(allocation + poolSize);
        char *prevTrailer = curState.Begin + curState.Remaining;
        newTrailer->PrevTrailer = prevTrailer;
        newTrailer->PoolSize = poolSize;
      }
      newState = PoolRange{allocation + sizeWithHeader,
                           poolSize - sizeWithHeader};
      __asan_poison_memory_region(allocation, newState.Remaining);
    }

    // NULL should be impossible, but check anyway in case of bugs or corruption
    if (SWIFT_UNLIKELY(!allocation)) {
      PoolRange curStateReRead = AllocationPool.load(std::memory_order_relaxed);
      swift::fatalError(
          0,
          "Metadata allocator corruption: allocation is NULL. "
          "curState: {%p, %zu} - curStateReRead: {%p, %zu} - "
          "newState: {%p, %zu} - allocatedNewPage: %s - requested size: %zu - "
          "sizeWithHeader: %zu - alignment: %zu - Tag: %d\n",
          curState.Begin, curState.Remaining, curStateReRead.Begin,
          curStateReRead.Remaining, newState.Begin, newState.Remaining,
          allocatedNewPage ? "true" : "false", size, sizeWithHeader, alignment,
          Tag);
    }

    // Swap in the new state.
    if (AllocationPool.compare_exchange_weak(curState, newState,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
      // If that succeeded, we've successfully allocated.
      __msan_allocated_memory(allocation, sizeWithHeader);
      __asan_unpoison_memory_region(allocation, sizeWithHeader);

      if (SWIFT_UNLIKELY(_swift_debug_metadataAllocationIterationEnabled)) {
        AllocationHeader *header = (AllocationHeader *)allocation;
        header->Size = size;
        header->Tag = Tag;

        auto *returnedAllocation = allocation + sizeof(AllocationHeader);

        if (runtime::environment ::
                SWIFT_DEBUG_ENABLE_METADATA_BACKTRACE_LOGGING())
          recordBacktrace(returnedAllocation);

        checkScribble(returnedAllocation, size);
        return returnedAllocation;
      } else {
        checkScribble(allocation, size);
        return allocation;
      }
    }

    // If it failed, go back to a neutral state and try again.
    if (allocatedNewPage) {
      swift_slowDealloc(allocation, PoolRange::PageSize, alignof(char) - 1);
    }
  }
}

void MetadataAllocator::Deallocate(const void *allocation, size_t size,
                                   size_t Alignment) {
  __asan_poison_memory_region(allocation, size);

  if (size > PoolRange::MaxPoolAllocationSize) {
    swift_slowDealloc(const_cast<void *>(allocation), size, Alignment - 1);
    return;
  }

  // Check whether the allocation pool is still in the state it was in
  // immediately after the given allocation.
  PoolRange curState = AllocationPool.load(std::memory_order_relaxed);
  if (reinterpret_cast<const char*>(allocation) + size != curState.Begin) {
    return;
  }

  // If we're scribbling, re-scribble the allocation so that the next call to
  // Allocate sees what it expects.
  memsetScribble(const_cast<void *>(allocation), size);

  // Try to swap back to the pre-allocation state.  If this fails,
  // don't bother trying again; we'll just leak the allocation.
  PoolRange newState = { reinterpret_cast<char*>(const_cast<void*>(allocation)),
                         curState.Remaining + size };

  AllocationPool.compare_exchange_weak(curState, newState,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed);
}

#endif

void *swift::allocateMetadata(size_t size, size_t alignment) {
  return MetadataAllocator(MetadataTag).Allocate(size, alignment);
}

template<>
bool Metadata::satisfiesClassConstraint() const {
  // existential types marked with @objc satisfy class requirement.
  if (auto *existential = dyn_cast<ExistentialTypeMetadata>(this))
    return existential->isObjC();

  // or it's a class.
  return isAnyClass();
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
  // Enable verification when a special environment variable is set. This helps
  // us stress test the mangler/demangler and type lookup machinery.
  if (!swift::runtime::environment::SWIFT_ENABLE_MANGLED_NAME_VERIFICATION())
    return;

  Demangle::StackAllocatedDemangler<1024> Dem;
  auto node = _swift_buildDemanglingForMetadata(metadata, Dem);
  // If the mangled node involves types in an AnonymousContext, then by design,
  // it cannot be looked up by name.
  if (referencesAnonymousContext(node))
    return;

  auto mangling = Demangle::mangleNode(node);
  if (!mangling.isSuccess()) {
    swift::warning(RuntimeErrorFlagNone,
                   "Metadata mangled name failed to roundtrip: %p couldn't be mangled\n",
                   metadata);
  } else {
    std::string mangledName = mangling.result();
    auto result =
      swift_getTypeByMangledName(MetadataState::Abstract,
                                 mangledName,
                                 nullptr,
                                 [](unsigned, unsigned){ return nullptr; },
                                 [](const Metadata *, unsigned) { return nullptr; })
      .getType().getMetadata();
    if (metadata != result)
      swift::warning(RuntimeErrorFlagNone,
                     "Metadata mangled name failed to roundtrip: %p -> %s -> %p\n",
                     metadata, mangledName.c_str(), (const Metadata *)result);
  }
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
#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH

// Autolink with libc++, for cases where libswiftCore is linked statically.
#if defined(__MACH__)
asm(".linker_option \"-lc++\"\n");
#endif // defined(__MACH__)
