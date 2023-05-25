//===--- HeapObject.cpp - Swift Language ABI Allocation Support -----------===//
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
// Allocation ABI Shims While the Language is Bootstrapped
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Lazy.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Heap.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/Once.h"
#include "swift/ABI/System.h"
#include "MetadataCache.h"
#include "Private.h"
#include "RuntimeInvocationsTracking.h"
#include "WeakReference.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/CustomRRABI.h"
#include "swift/Runtime/InstrumentsSupport.h"
#include "swift/shims/GlobalObjects.h"
#include "swift/shims/RuntimeShims.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <new>
#if SWIFT_OBJC_INTEROP
# include <objc/NSObject.h>
# include <objc/runtime.h>
# include <objc/message.h>
# include <objc/objc.h>
# include "swift/Runtime/ObjCBridge.h"
# include <dlfcn.h>
#endif
#if SWIFT_STDLIB_HAS_MALLOC_TYPE
# include <malloc_type_private.h>
#endif
#include "Leaks.h"

using namespace swift;

// Check to make sure the runtime is being built with a compiler that
// supports the Swift calling convention.
//
// If the Swift calling convention is not in use, functions such as
// swift_allocBox and swift_makeBoxUnique that rely on their return value
// being passed in a register to be compatible with Swift may miscompile on
// some platforms and silently fail.
#if !__has_attribute(swiftcall)
#error "The runtime must be built with a compiler that supports swiftcall."
#endif

/// Returns true if the pointer passed to a native retain or release is valid.
/// If false, the operation should immediately return.
SWIFT_ALWAYS_INLINE
static inline bool isValidPointerForNativeRetain(const void *p) {
#if defined(__arm64__) && (__POINTER_WIDTH__ == 32)
  // arm64_32 is special since it has 32-bit pointers but __arm64__ is true.
  // Catch it early since __POINTER_WIDTH__ is generally non-portable.
  return p != nullptr;
#elif defined(__ANDROID__) && defined(__aarch64__)
  // Check the top of the second byte instead, since Android AArch64 reserves
  // the top byte for its own pointer tagging since Android 11.
  return (intptr_t)((uintptr_t)p << 8) > 0;
#elif defined(__x86_64__) || defined(__arm64__) || defined(__aarch64__) || defined(_M_ARM64) || defined(__s390x__) || (defined(__riscv) && __riscv_xlen == 64) || (defined(__powerpc64__) && defined(__LITTLE_ENDIAN__))
  // On these platforms, except s390x, the upper half of address space is reserved for the
  // kernel, so we can assume that pointer values in this range are invalid.
  // On s390x it is theoretically possible to have high bit set but in practice
  // it is unlikely.
  return (intptr_t)p > 0;
#else
  return p != nullptr;
#endif
}

// Call the appropriate implementation of the `name` function, passing `args`
// to the call. This checks for an override in the function pointer. If an
// override is present, it calls that override. Otherwise it directly calls
// the default implementation. This allows the compiler to inline the default
// implementation and avoid the performance penalty of indirecting through
// the function pointer in the common case.
//
// NOTE: the memcpy and asm("") naming shenanigans are to convince the compiler
// not to emit a bunch of ptrauth instructions just to perform the comparison.
// We only want to authenticate the function pointer if we actually call it.
SWIFT_RETURNS_NONNULL SWIFT_NODISCARD
static HeapObject *_swift_allocObject_(HeapMetadata const *metadata,
                                       size_t requiredSize,
                                       size_t requiredAlignmentMask)
                                       asm("__swift_allocObject_");
static HeapObject *_swift_retain_(HeapObject *object) asm("__swift_retain_");
static HeapObject *_swift_retain_n_(HeapObject *object, uint32_t n)
  asm("__swift_retain_n_");
static void _swift_release_(HeapObject *object) asm("__swift_release_");
static void _swift_release_n_(HeapObject *object, uint32_t n)
  asm("__swift_release_n_");
static HeapObject *_swift_tryRetain_(HeapObject *object)
  asm("__swift_tryRetain_");
#define CALL_IMPL(name, args) do { \
    void *fptr; \
    memcpy(&fptr, (void *)&_ ## name, sizeof(fptr)); \
    extern char _ ## name ## _as_char asm("__" #name "_"); \
    fptr = __ptrauth_swift_runtime_function_entry_strip(fptr); \
    if (SWIFT_UNLIKELY(fptr != &_ ## name ## _as_char)) \
      return _ ## name args; \
    return _ ## name ## _ args; \
} while(0)

#if SWIFT_STDLIB_HAS_MALLOC_TYPE
static malloc_type_summary_t
computeMallocTypeSummary(const HeapMetadata *heapMetadata) {
  assert(isHeapMetadataKind(heapMetadata->getKind()));
  auto *classMetadata = heapMetadata->getClassObject();
  auto *typeDesc = heapMetadata->getTypeContextDescriptor();

  // Pruned metadata or unclassified
  if (!classMetadata || !typeDesc)
    return {.type_kind = MALLOC_TYPE_KIND_SWIFT};

  // Objc
  if (classMetadata->isPureObjC())
    return {.type_kind = MALLOC_TYPE_KIND_OBJC};

  malloc_type_summary_t summary = {.type_kind = MALLOC_TYPE_KIND_SWIFT};
  summary.layout_semantics.reference_count =
      (classMetadata->getFlags() & ClassFlags::UsesSwiftRefcounting);

  auto *fieldDesc = typeDesc->Fields.get();
  if (!fieldDesc)
    return summary;

  bool isGenericData = true;
  for (auto &field : *fieldDesc) {
    if (field.isIndirectCase()) {
      isGenericData = false;
      if (field.isVar())
        summary.layout_semantics.data_pointer = true;
      else
        summary.layout_semantics.immutable_pointer = true;
    }
  }
  summary.layout_semantics.generic_data = isGenericData;

  return summary;

// FIXME: these are all the things we are potentially interested in
//  typedef struct {
// 	  bool data_pointer : 1;
// 	  bool struct_pointer : 1;
// 	  bool immutable_pointer : 1;
// 	  bool anonymous_pointer : 1;
// 	  bool reference_count : 1;
// 	  bool resource_handle : 1;
// 	  bool spatial_bounds : 1;
// 	  bool tainted_data : 1;
// 	  bool generic_data : 1;
// 	  uint16_t unused : 7;
// } malloc_type_layout_semantics_t;
}

struct MallocTypeCacheEntry {
// union malloc_type_descriptor_t {
//   struct {
//     uint32_t hash;
//     malloc_type_summary_t summary;
//   };
//   malloc_type_id_t type_id;
// };
  malloc_type_descriptor_t desc;

  friend llvm::hash_code hash_value(const MallocTypeCacheEntry &entry) {
    return hash_value(entry.desc.hash);
  }
  bool matchesKey(uint32_t key) const { return desc.hash == key; }
};
static ConcurrentReadableHashMap<MallocTypeCacheEntry> MallocTypes;

static malloc_type_id_t getMallocTypeId(const HeapMetadata *heapMetadata) {
  uint64_t metadataPtrBits = reinterpret_cast<uint64_t>(heapMetadata);
  uint32_t key = (metadataPtrBits >> 32) ^ (metadataPtrBits >> 0);

  {
    auto snapshot = MallocTypes.snapshot();
    if (auto *entry = snapshot.find(key))
      return entry->desc.type_id;
  }

  malloc_type_descriptor_t desc = {
    .hash = key,
    .summary = computeMallocTypeSummary(heapMetadata)
  };

  MallocTypes.getOrInsert(
      key, [desc](MallocTypeCacheEntry *entry, bool created) {
        if (created)
          entry->desc = desc;
        return true;
      });

  return desc.type_id;
}
#endif // SWIFT_STDLIB_HAS_MALLOC_TYPE

static HeapObject *_swift_allocObject_(HeapMetadata const *metadata,
                                       size_t requiredSize,
                                       size_t requiredAlignmentMask) {
  assert(isAlignmentMask(requiredAlignmentMask));
#if SWIFT_STDLIB_HAS_MALLOC_TYPE
  auto object = reinterpret_cast<HeapObject *>(swift_slowAllocTyped(
      requiredSize, requiredAlignmentMask, getMallocTypeId(metadata)));
#else
  auto object = reinterpret_cast<HeapObject *>(
      swift_slowAlloc(requiredSize, requiredAlignmentMask));
#endif

  // NOTE: this relies on the C++17 guaranteed semantics of no null-pointer
  // check on the placement new allocator which we have observed on Windows,
  // Linux, and macOS.
  ::new (object) HeapObject(metadata);

  // If leak tracking is enabled, start tracking this object.
  SWIFT_LEAKS_START_TRACKING_OBJECT(object);

  SWIFT_RT_TRACK_INVOCATION(object, swift_allocObject);

  return object;
}

HeapObject *swift::swift_allocObject(HeapMetadata const *metadata,
                                     size_t requiredSize,
                                     size_t requiredAlignmentMask) {
  CALL_IMPL(swift_allocObject, (metadata, requiredSize, requiredAlignmentMask));
}

SWIFT_RUNTIME_EXPORT
HeapObject *(*SWIFT_RT_DECLARE_ENTRY _swift_allocObject)(
    HeapMetadata const *metadata, size_t requiredSize,
    size_t requiredAlignmentMask) = _swift_allocObject_;

HeapObject *
swift::swift_initStackObject(HeapMetadata const *metadata,
                             HeapObject *object) {
  object->metadata = metadata;
  object->refCounts.initForNotFreeing();

  SWIFT_RT_TRACK_INVOCATION(object, swift_initStackObject);
  return object;
}

struct InitStaticObjectContext {
  HeapObject *object;
  HeapMetadata const *metadata;
};

// TODO: We could generate inline code for the fast-path, i.e. the metadata
// pointer is already set. That would be a performance/codesize tradeoff.
HeapObject *
swift::swift_initStaticObject(HeapMetadata const *metadata,
                              HeapObject *object) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_initStaticObject);
  // The token is located at a negative offset from the object header.
  swift_once_t *token = ((swift_once_t *)object) - 1;

  // We have to initialize the header atomically. Otherwise we could reset the
  // refcount to 1 while another thread already incremented it - and would
  // decrement it to 0 afterwards.
  InitStaticObjectContext Ctx = { object, metadata };
  swift::once(
      *token,
      [](void *OpaqueCtx) {
        InitStaticObjectContext *Ctx = (InitStaticObjectContext *)OpaqueCtx;
        Ctx->object->metadata = Ctx->metadata;
        Ctx->object->refCounts.initImmortal();
      },
      &Ctx);

  return object;
}

void
swift::swift_verifyEndOfLifetime(HeapObject *object) {
  if (object->refCounts.getCount() != 0)
    swift::fatalError(/* flags = */ 0,
                      "Fatal error: Stack object escaped\n");

  if (object->refCounts.getUnownedCount() != 1)
    swift::fatalError(/* flags = */ 0,
                      "Fatal error: Unowned reference to stack object\n");

  if (object->refCounts.getWeakCount() != 0)
    swift::fatalError(/* flags = */ 0,
                      "Fatal error: Weak reference to stack object\n");
}

/// Allocate a reference-counted object on the heap that
/// occupies <size> bytes of maximally-aligned storage.  The object is
/// uninitialized except for its header.
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_SPI
HeapObject* swift_bufferAllocate(
  HeapMetadata const* bufferType, size_t size, size_t alignMask)
{
  return swift::swift_allocObject(bufferType, size, alignMask);
}

namespace {
/// Heap object destructor for a generic box allocated with swift_allocBox.
static SWIFT_CC(swift) void destroyGenericBox(SWIFT_CONTEXT HeapObject *o) {
  auto metadata = static_cast<const GenericBoxHeapMetadata *>(o->metadata);
  // Destroy the object inside.
  auto *value = metadata->project(o);
  metadata->BoxedType->vw_destroy(value);

  // Deallocate the box.
  swift_deallocObject(o, metadata->getAllocSize(),
                      metadata->getAllocAlignMask());
}

class BoxCacheEntry {
public:
  FullMetadata<GenericBoxHeapMetadata> Data;

  BoxCacheEntry(const Metadata *type)
    : Data{HeapMetadataHeader{ {/*type layout*/nullptr}, {destroyGenericBox},
                               {/*vwtable*/ nullptr}},
           GenericBoxHeapMetadata{MetadataKind::HeapGenericLocalVariable,
                                  GenericBoxHeapMetadata::getHeaderOffset(type),
                                  type}} {
  }

  intptr_t getKeyIntValueForDump() {
    return reinterpret_cast<intptr_t>(Data.BoxedType);
  }

  bool matchesKey(const Metadata *type) const { return type == Data.BoxedType; }

  friend llvm::hash_code hash_value(const BoxCacheEntry &value) {
    return llvm::hash_value(value.Data.BoxedType);
  }

  static size_t getExtraAllocationSize(const Metadata *key) {
    return 0;
  }
  size_t getExtraAllocationSize() const {
    return 0;
  }
};

} // end anonymous namespace

static SimpleGlobalCache<BoxCacheEntry, BoxesTag> Boxes;

BoxPair swift::swift_makeBoxUnique(OpaqueValue *buffer, const Metadata *type,
                                    size_t alignMask) {
  auto *inlineBuffer = reinterpret_cast<ValueBuffer*>(buffer);
  HeapObject *box = reinterpret_cast<HeapObject *>(inlineBuffer->PrivateData[0]);

  if (!swift_isUniquelyReferenced_nonNull_native(box)) {
    auto refAndObjectAddr = BoxPair(swift_allocBox(type));
    // Compute the address of the old object.
    auto headerOffset = sizeof(HeapObject) + alignMask & ~alignMask;
    auto *oldObjectAddr = reinterpret_cast<OpaqueValue *>(
        reinterpret_cast<char *>(box) + headerOffset);
    // Copy the data.
    type->vw_initializeWithCopy(refAndObjectAddr.buffer, oldObjectAddr);
    inlineBuffer->PrivateData[0] = refAndObjectAddr.object;
    // Release ownership of the old box.
    swift_release(box);
    return refAndObjectAddr;
  } else {
    auto headerOffset = sizeof(HeapObject) + alignMask & ~alignMask;
    auto *objectAddr = reinterpret_cast<OpaqueValue *>(
        reinterpret_cast<char *>(box) + headerOffset);
    return BoxPair{box, objectAddr};
  }
}

BoxPair swift::swift_allocBox(const Metadata *type) {
  // Get the heap metadata for the box.
  auto metadata = &Boxes.getOrInsert(type).first->Data;

  // Allocate and project the box.
  auto allocation = swift_allocObject(metadata, metadata->getAllocSize(),
                                      metadata->getAllocAlignMask());
  auto projection = metadata->project(allocation);

  return BoxPair{allocation, projection};
}

void swift::swift_deallocBox(HeapObject *o) {
  auto metadata = static_cast<const GenericBoxHeapMetadata *>(o->metadata);
  // Move the object to the deallocating state (+1 -> +0).
  o->refCounts.decrementFromOneNonAtomic();
  swift_deallocObject(o, metadata->getAllocSize(),
                      metadata->getAllocAlignMask());
}

OpaqueValue *swift::swift_projectBox(HeapObject *o) {
  // The compiler will use a nil reference as a way to avoid allocating memory
  // for boxes of empty type. The address of an empty value is always undefined,
  // so we can just return nil back in this case.
  if (!o)
    return nullptr;
  auto metadata = static_cast<const GenericBoxHeapMetadata *>(o->metadata);
  return metadata->project(o);
}

namespace { // Begin anonymous namespace.

struct _SwiftEmptyBoxStorage {
  HeapObject header;
};

swift::HeapLocalVariableMetadata _emptyBoxStorageMetadata;

/// The singleton empty box storage object.
_SwiftEmptyBoxStorage _EmptyBoxStorage = {
 // HeapObject header;
  {
    &_emptyBoxStorageMetadata,
  }
};

} // End anonymous namespace.

HeapObject *swift::swift_allocEmptyBox() {
  auto heapObject = reinterpret_cast<HeapObject*>(&_EmptyBoxStorage);
  swift_retain(heapObject);
  return heapObject;
}

// Forward-declare this, but define it after swift_release.
extern "C" SWIFT_LIBRARY_VISIBILITY SWIFT_NOINLINE SWIFT_USED void
_swift_release_dealloc(HeapObject *object);

SWIFT_ALWAYS_INLINE
static HeapObject *_swift_retain_(HeapObject *object) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_retain);
  if (isValidPointerForNativeRetain(object)) {
    // Return the result of increment() to make the eventual call to
    // incrementSlow a tail call, which avoids pushing a stack frame on the fast
    // path on ARM64.
    return object->refCounts.increment(1);
  }
  return object;
}

HeapObject *swift::swift_retain(HeapObject *object) {
#ifdef SWIFT_THREADING_NONE
  return swift_nonatomic_retain(object);
#else
  CALL_IMPL(swift_retain, (object));
#endif
}

CUSTOM_RR_ENTRYPOINTS_DEFINE_ENTRYPOINTS(swift_retain)

SWIFT_RUNTIME_EXPORT
HeapObject *(*SWIFT_RT_DECLARE_ENTRY _swift_retain)(HeapObject *object) =
    _swift_retain_;

HeapObject *swift::swift_nonatomic_retain(HeapObject *object) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_nonatomic_retain);
  if (isValidPointerForNativeRetain(object))
    object->refCounts.incrementNonAtomic(1);
  return object;
}

SWIFT_ALWAYS_INLINE
static HeapObject *_swift_retain_n_(HeapObject *object, uint32_t n) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_retain_n);
  if (isValidPointerForNativeRetain(object))
    object->refCounts.increment(n);
  return object;
}

HeapObject *swift::swift_retain_n(HeapObject *object, uint32_t n) {
#ifdef SWIFT_THREADING_NONE
  return swift_nonatomic_retain_n(object, n);
#else
  CALL_IMPL(swift_retain_n, (object, n));
#endif
}

SWIFT_RUNTIME_EXPORT
HeapObject *(*SWIFT_RT_DECLARE_ENTRY _swift_retain_n)(
    HeapObject *object, uint32_t n) = _swift_retain_n_;

HeapObject *swift::swift_nonatomic_retain_n(HeapObject *object, uint32_t n) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_nonatomic_retain_n);
  if (isValidPointerForNativeRetain(object))
    object->refCounts.incrementNonAtomic(n);
  return object;
}

SWIFT_ALWAYS_INLINE
static void _swift_release_(HeapObject *object) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_release);
  if (isValidPointerForNativeRetain(object))
    object->refCounts.decrementAndMaybeDeinit(1);
}

void swift::swift_release(HeapObject *object) {
#ifdef SWIFT_THREADING_NONE
  swift_nonatomic_release(object);
#else
  CALL_IMPL(swift_release, (object));
#endif
}

CUSTOM_RR_ENTRYPOINTS_DEFINE_ENTRYPOINTS(swift_release)

SWIFT_RUNTIME_EXPORT
void (*SWIFT_RT_DECLARE_ENTRY _swift_release)(HeapObject *object) =
    _swift_release_;

void swift::swift_nonatomic_release(HeapObject *object) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_nonatomic_release);
  if (isValidPointerForNativeRetain(object))
    object->refCounts.decrementAndMaybeDeinitNonAtomic(1);
}

SWIFT_ALWAYS_INLINE
static void _swift_release_n_(HeapObject *object, uint32_t n) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_release_n);
  if (isValidPointerForNativeRetain(object))
    object->refCounts.decrementAndMaybeDeinit(n);
}

void swift::swift_release_n(HeapObject *object, uint32_t n) {
#ifdef SWIFT_THREADING_NONE
  swift_nonatomic_release_n(object, n);
#else
  CALL_IMPL(swift_release_n, (object, n));
#endif
}

SWIFT_RUNTIME_EXPORT
void (*SWIFT_RT_DECLARE_ENTRY _swift_release_n)(HeapObject *object,
                                                uint32_t n) = _swift_release_n_;

void swift::swift_nonatomic_release_n(HeapObject *object, uint32_t n) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_nonatomic_release_n);
  if (isValidPointerForNativeRetain(object))
    object->refCounts.decrementAndMaybeDeinitNonAtomic(n);
}

size_t swift::swift_retainCount(HeapObject *object) {
  if (isValidPointerForNativeRetain(object))
    return object->refCounts.getCount();
  return 0;
}

size_t swift::swift_unownedRetainCount(HeapObject *object) {
  return object->refCounts.getUnownedCount();
}

size_t swift::swift_weakRetainCount(HeapObject *object) {
  return object->refCounts.getWeakCount();
}

HeapObject *swift::swift_unownedRetain(HeapObject *object) {
#ifdef SWIFT_THREADING_NONE
  return static_cast<HeapObject *>(swift_nonatomic_unownedRetain(object));
#else
  SWIFT_RT_TRACK_INVOCATION(object, swift_unownedRetain);
  if (!isValidPointerForNativeRetain(object))
    return object;

  object->refCounts.incrementUnowned(1);
  return object;
#endif
}

void swift::swift_unownedRelease(HeapObject *object) {
#ifdef SWIFT_THREADING_NONE
  swift_nonatomic_unownedRelease(object);
#else
  SWIFT_RT_TRACK_INVOCATION(object, swift_unownedRelease);
  if (!isValidPointerForNativeRetain(object))
    return;

  // Only class objects can be unowned-retained and unowned-released.
  assert(object->metadata->isClassObject());
  assert(static_cast<const ClassMetadata*>(object->metadata)->isTypeMetadata());

  if (object->refCounts.decrementUnownedShouldFree(1)) {
    auto classMetadata = static_cast<const ClassMetadata*>(object->metadata);

    swift_slowDealloc(object, classMetadata->getInstanceSize(),
                      classMetadata->getInstanceAlignMask());
  }
#endif
}

void *swift::swift_nonatomic_unownedRetain(HeapObject *object) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_nonatomic_unownedRetain);
  if (!isValidPointerForNativeRetain(object))
    return object;

  object->refCounts.incrementUnownedNonAtomic(1);
  return object;
}

void swift::swift_nonatomic_unownedRelease(HeapObject *object) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_nonatomic_unownedRelease);
  if (!isValidPointerForNativeRetain(object))
    return;

  // Only class objects can be unowned-retained and unowned-released.
  assert(object->metadata->isClassObject());
  assert(static_cast<const ClassMetadata*>(object->metadata)->isTypeMetadata());

  if (object->refCounts.decrementUnownedShouldFreeNonAtomic(1)) {
    auto classMetadata = static_cast<const ClassMetadata*>(object->metadata);

    swift_slowDealloc(object, classMetadata->getInstanceSize(),
                       classMetadata->getInstanceAlignMask());
  }
}

HeapObject *swift::swift_unownedRetain_n(HeapObject *object, int n) {
#ifdef SWIFT_THREADING_NONE
  return swift_nonatomic_unownedRetain_n(object, n);
#else
  SWIFT_RT_TRACK_INVOCATION(object, swift_unownedRetain_n);
  if (!isValidPointerForNativeRetain(object))
    return object;

  object->refCounts.incrementUnowned(n);
  return object;
#endif
}

void swift::swift_unownedRelease_n(HeapObject *object, int n) {
#ifdef SWIFT_THREADING_NONE
  swift_nonatomic_unownedRelease_n(object, n);
#else
  SWIFT_RT_TRACK_INVOCATION(object, swift_unownedRelease_n);
  if (!isValidPointerForNativeRetain(object))
    return;

  // Only class objects can be unowned-retained and unowned-released.
  assert(object->metadata->isClassObject());
  assert(static_cast<const ClassMetadata*>(object->metadata)->isTypeMetadata());

  if (object->refCounts.decrementUnownedShouldFree(n)) {
    auto classMetadata = static_cast<const ClassMetadata*>(object->metadata);
    swift_slowDealloc(object, classMetadata->getInstanceSize(),
                      classMetadata->getInstanceAlignMask());
  }
#endif
}

HeapObject *swift::swift_nonatomic_unownedRetain_n(HeapObject *object, int n) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_nonatomic_unownedRetain_n);
  if (!isValidPointerForNativeRetain(object))
    return object;

  object->refCounts.incrementUnownedNonAtomic(n);
  return object;
}

void swift::swift_nonatomic_unownedRelease_n(HeapObject *object, int n) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_unownedRelease_n);
  if (!isValidPointerForNativeRetain(object))
    return;

  // Only class objects can be unowned-retained and unowned-released.
  assert(object->metadata->isClassObject());
  assert(static_cast<const ClassMetadata*>(object->metadata)->isTypeMetadata());

  if (object->refCounts.decrementUnownedShouldFreeNonAtomic(n)) {
    auto classMetadata = static_cast<const ClassMetadata*>(object->metadata);
    swift_slowDealloc(object, classMetadata->getInstanceSize(),
                      classMetadata->getInstanceAlignMask());
  }
}

SWIFT_ALWAYS_INLINE
static HeapObject *_swift_tryRetain_(HeapObject *object) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_tryRetain);
  if (!isValidPointerForNativeRetain(object))
    return nullptr;

#ifdef SWIFT_THREADING_NONE
  if (object->refCounts.tryIncrementNonAtomic()) return object;
  else return nullptr;
#else
  if (object->refCounts.tryIncrement()) return object;
  else return nullptr;
#endif
}

HeapObject *swift::swift_tryRetain(HeapObject *object) {
  CALL_IMPL(swift_tryRetain, (object));
}

SWIFT_RUNTIME_EXPORT
HeapObject *(*SWIFT_RT_DECLARE_ENTRY _swift_tryRetain)(HeapObject *object) =
    _swift_tryRetain_;

bool swift::swift_isDeallocating(HeapObject *object) {
  if (!isValidPointerForNativeRetain(object))
    return false;
  return object->refCounts.isDeiniting();
}

void swift::swift_setDeallocating(HeapObject *object) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_setDeallocating);
  object->refCounts.decrementFromOneNonAtomic();
}

HeapObject *swift::swift_unownedRetainStrong(HeapObject *object) {
#ifdef SWIFT_THREADING_NONE
  return swift_nonatomic_unownedRetainStrong(object);
#else
  SWIFT_RT_TRACK_INVOCATION(object, swift_unownedRetainStrong);
  if (!isValidPointerForNativeRetain(object))
    return object;
  assert(object->refCounts.getUnownedCount() &&
         "object is not currently unowned-retained");

  if (! object->refCounts.tryIncrement())
    swift::swift_abortRetainUnowned(object);
  return object;
#endif
}

HeapObject *swift::swift_nonatomic_unownedRetainStrong(HeapObject *object) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_nonatomic_unownedRetainStrong);
  if (!isValidPointerForNativeRetain(object))
    return object;
  assert(object->refCounts.getUnownedCount() &&
         "object is not currently unowned-retained");

  if (! object->refCounts.tryIncrementNonAtomic())
    swift::swift_abortRetainUnowned(object);
  return object;
}

void swift::swift_unownedRetainStrongAndRelease(HeapObject *object) {
#ifdef SWIFT_THREADING_NONE
  swift_nonatomic_unownedRetainStrongAndRelease(object);
#else
  SWIFT_RT_TRACK_INVOCATION(object, swift_unownedRetainStrongAndRelease);
  if (!isValidPointerForNativeRetain(object))
    return;
  assert(object->refCounts.getUnownedCount() &&
         "object is not currently unowned-retained");

  if (! object->refCounts.tryIncrement())
    swift::swift_abortRetainUnowned(object);

  // This should never cause a deallocation.
  bool dealloc = object->refCounts.decrementUnownedShouldFree(1);
  assert(!dealloc && "retain-strong-and-release caused dealloc?");
  (void) dealloc;
#endif
}

void swift::swift_nonatomic_unownedRetainStrongAndRelease(HeapObject *object) {
  SWIFT_RT_TRACK_INVOCATION(object, swift_nonatomic_unownedRetainStrongAndRelease);
  if (!isValidPointerForNativeRetain(object))
    return;
  assert(object->refCounts.getUnownedCount() &&
         "object is not currently unowned-retained");

  if (! object->refCounts.tryIncrementNonAtomic())
    swift::swift_abortRetainUnowned(object);

  // This should never cause a deallocation.
  bool dealloc = object->refCounts.decrementUnownedShouldFreeNonAtomic(1);
  assert(!dealloc && "retain-strong-and-release caused dealloc?");
  (void) dealloc;
}

void swift::swift_unownedCheck(HeapObject *object) {
  if (!isValidPointerForNativeRetain(object)) return;
  assert(object->refCounts.getUnownedCount() &&
         "object is not currently unowned-retained");

  if (object->refCounts.isDeiniting())
    swift::swift_abortRetainUnowned(object);
}

void _swift_release_dealloc(HeapObject *object) {
  asFullMetadata(object->metadata)->destroy(object);
}

#if SWIFT_OBJC_INTEROP
/// Perform the root -dealloc operation for a class instance.
void swift::swift_rootObjCDealloc(HeapObject *self) {
  auto metadata = self->metadata;
  assert(metadata->isClassObject());
  auto classMetadata = static_cast<const ClassMetadata*>(metadata);
  assert(classMetadata->isTypeMetadata());
  swift_deallocClassInstance(self, classMetadata->getInstanceSize(),
                             classMetadata->getInstanceAlignMask());
}
#endif

void swift::swift_deallocClassInstance(HeapObject *object,
                                       size_t allocatedSize,
                                       size_t allocatedAlignMask) {
  size_t retainCount = swift_retainCount(object);
  if (SWIFT_UNLIKELY(retainCount > 1)) {
    auto descriptor = object->metadata->getTypeContextDescriptor();

    swift::fatalError(0,
                      "Object %p of class %s deallocated with non-zero retain "
                      "count %zd. This object's deinit, or something called "
                      "from it, may have created a strong reference to self "
                      "which outlived deinit, resulting in a dangling "
                      "reference.\n",
                      object,
                      descriptor ? descriptor->Name.get() : "<unknown>",
                      retainCount);
  }

#if SWIFT_OBJC_INTEROP
  // We need to let the ObjC runtime clean up any associated objects or weak
  // references associated with this object.
#if TARGET_OS_SIMULATOR && (__x86_64__ || __i386__)
  const bool fastDeallocSupported = false;
#else
  const bool fastDeallocSupported = true;
#endif

  if (!fastDeallocSupported || !object->refCounts.getPureSwiftDeallocation()) {
    objc_destructInstance((id)object);
  }
#endif

  swift_deallocObject(object, allocatedSize, allocatedAlignMask);
}

/// Variant of the above used in constructor failure paths.
void swift::swift_deallocPartialClassInstance(HeapObject *object,
                                              HeapMetadata const *metadata,
                                              size_t allocatedSize,
                                              size_t allocatedAlignMask) {
  if (!object)
    return;

  // Destroy ivars
  auto *classMetadata = _swift_getClassOfAllocated(object)->getClassObject();
  assert(classMetadata && "Not a class?");

#if SWIFT_OBJC_INTEROP
  // If the object's class is already pure ObjC class, just release it and move
  // on. There are no ivar destroyers. This avoids attempting to mutate
  // placeholder objects statically created in read-only memory.
  if (classMetadata->isPureObjC()) {
    objc_release((id)object);
    return;
  }
#endif

  while (classMetadata != metadata) {
#if SWIFT_OBJC_INTEROP
    // If we have hit a pure Objective-C class, we won't see another ivar
    // destroyer.
    if (classMetadata->isPureObjC()) {
      // Set the class to the pure Objective-C superclass, so that when dealloc
      // runs, it starts at that superclass.
      object_setClass((id)object, class_const_cast(classMetadata));

      // Release the object.
      objc_release((id)object);
      return;
    }
#endif

    if (classMetadata->IVarDestroyer)
      classMetadata->IVarDestroyer(object);

    classMetadata = classMetadata->Superclass->getClassObject();
    assert(classMetadata && "Given metatype not a superclass of object type?");
  }

#if SWIFT_OBJC_INTEROP
  // If this class doesn't use Swift-native reference counting, use
  // objc_release instead.
  if (!usesNativeSwiftReferenceCounting(classMetadata)) {
    // Find the pure Objective-C superclass.
    while (!classMetadata->isPureObjC())
      classMetadata = classMetadata->Superclass->getClassObject();

    // Set the class to the pure Objective-C superclass, so that when dealloc
    // runs, it starts at that superclass.
    object_setClass((id)object, class_const_cast(classMetadata));

    // Release the object.
    objc_release((id)object);
    return;
  }
#endif

  // The strong reference count should be +1 -- tear down the object
  bool shouldDeallocate = object->refCounts.decrementShouldDeinit(1);
  assert(shouldDeallocate);
  (void) shouldDeallocate;
  swift_deallocClassInstance(object, allocatedSize, allocatedAlignMask);
}

#if !defined(__APPLE__) && defined(SWIFT_RUNTIME_CLOBBER_FREED_OBJECTS)
static inline void memset_pattern8(void *b, const void *pattern8, size_t len) {
  char *ptr = static_cast<char *>(b);
  while (len >= 8) {
    memcpy(ptr, pattern8, 8);
    ptr += 8;
    len -= 8;
  }
  memcpy(ptr, pattern8, len);
}
#endif

static inline void swift_deallocObjectImpl(HeapObject *object,
                                           size_t allocatedSize,
                                           size_t allocatedAlignMask,
                                           bool isDeiniting) {
  assert(isAlignmentMask(allocatedAlignMask));
  if (!isDeiniting) {
    assert(object->refCounts.isUniquelyReferenced());
    object->refCounts.decrementFromOneNonAtomic();
  }
  assert(object->refCounts.isDeiniting());
  SWIFT_RT_TRACK_INVOCATION(object, swift_deallocObject);
#ifdef SWIFT_RUNTIME_CLOBBER_FREED_OBJECTS
  memset_pattern8((uint8_t *)object + sizeof(HeapObject),
                  "\xAB\xAD\x1D\xEA\xF4\xEE\xD0\bB9",
                  allocatedSize - sizeof(HeapObject));
#endif

  // If we are tracking leaks, stop tracking this object.
  SWIFT_LEAKS_STOP_TRACKING_OBJECT(object);


  // Drop the initial weak retain of the object.
  //
  // If the outstanding weak retain count is 1 (i.e. only the initial
  // weak retain), we can immediately call swift_slowDealloc.  This is
  // useful both as a way to eliminate an unnecessary atomic
  // operation, and as a way to avoid calling swift_unownedRelease on an
  // object that might be a class object, which simplifies the logic
  // required in swift_unownedRelease for determining the size of the
  // object.
  //
  // If we see that there is an outstanding weak retain of the object,
  // we need to fall back on swift_release, because it's possible for
  // us to race against a weak retain or a weak release.  But if the
  // outstanding weak retain count is 1, then anyone attempting to
  // increase the weak reference count is inherently racing against
  // deallocation and thus in undefined-behavior territory.  And
  // we can even do this with a normal load!  Here's why:
  //
  // 1. There is an invariant that, if the strong reference count
  // is > 0, then the weak reference count is > 1.
  //
  // 2. The above lets us say simply that, in the absence of
  // races, once a reference count reaches 0, there are no points
  // which happen-after where the reference count is > 0.
  //
  // 3. To not race, a strong retain must happen-before a point
  // where the strong reference count is > 0, and a weak retain
  // must happen-before a point where the weak reference count
  // is > 0.
  //
  // 4. Changes to either the strong and weak reference counts occur
  // in a total order with respect to each other.  This can
  // potentially be done with a weaker memory ordering than
  // sequentially consistent if the architecture provides stronger
  // ordering for memory guaranteed to be co-allocated on a cache
  // line (which the reference count fields are).
  //
  // 5. This function happens-after a point where the strong
  // reference count was 0.
  //
  // 6. Therefore, if a normal load in this function sees a weak
  // reference count of 1, it cannot be racing with a weak retain
  // that is not racing with deallocation:
  //
  //   - A weak retain must happen-before a point where the weak
  //     reference count is > 0.
  //
  //   - This function logically decrements the weak reference
  //     count.  If it is possible for it to see a weak reference
  //     count of 1, then at the end of this function, the
  //     weak reference count will logically be 0.
  //
  //   - There can be no points after that point where the
  //     weak reference count will be > 0.
  //
  //   - Therefore either the weak retain must happen-before this
  //     function, or this function cannot see a weak reference
  //     count of 1, or there is a race.
  //
  // Note that it is okay for there to be a race involving a weak
  // *release* which happens after the strong reference count drops to
  // 0.  However, this is harmless: if our load fails to see the
  // release, we will fall back on swift_unownedRelease, which does an
  // atomic decrement (and has the ability to reconstruct
  // allocatedSize and allocatedAlignMask).
  //
  // Note: This shortcut is NOT an optimization.
  // Some allocations passed to swift_deallocObject() are not compatible
  // with swift_unownedRelease() because they do not have ClassMetadata.

  if (object->refCounts.canBeFreedNow()) {
    // object state DEINITING -> DEAD
    swift_slowDealloc(object, allocatedSize, allocatedAlignMask);
  } else {
    // object state DEINITING -> DEINITED
    swift_unownedRelease(object);
  }
}

void swift::swift_deallocObject(HeapObject *object, size_t allocatedSize,
                                size_t allocatedAlignMask) {
  swift_deallocObjectImpl(object, allocatedSize, allocatedAlignMask, true);
}

void swift::swift_deallocUninitializedObject(HeapObject *object,
                                             size_t allocatedSize,
                                             size_t allocatedAlignMask) {
  swift_deallocObjectImpl(object, allocatedSize, allocatedAlignMask, false);
}

WeakReference *swift::swift_weakInit(WeakReference *ref, HeapObject *value) {
  ref->nativeInit(value);
  return ref;
}

WeakReference *swift::swift_weakAssign(WeakReference *ref, HeapObject *value) {
  ref->nativeAssign(value);
  return ref;
}

HeapObject *swift::swift_weakLoadStrong(WeakReference *ref) {
  return ref->nativeLoadStrong();
}

HeapObject *swift::swift_weakTakeStrong(WeakReference *ref) {
  return ref->nativeTakeStrong();
}

void swift::swift_weakDestroy(WeakReference *ref) {
  ref->nativeDestroy();
}

WeakReference *swift::swift_weakCopyInit(WeakReference *dest,
                                         WeakReference *src) {
  dest->nativeCopyInit(src);
  return dest;
}

WeakReference *swift::swift_weakTakeInit(WeakReference *dest,
                                         WeakReference *src) {
  dest->nativeTakeInit(src);
  return dest;
}

WeakReference *swift::swift_weakCopyAssign(WeakReference *dest,
                                           WeakReference *src) {
  dest->nativeCopyAssign(src);
  return dest;
}

WeakReference *swift::swift_weakTakeAssign(WeakReference *dest,
                                           WeakReference *src) {
  dest->nativeTakeAssign(src);
  return dest;
}

#ifndef NDEBUG

/// Returns true if the "immutable" flag is set on \p object.
///
/// Used for runtime consistency checking of COW buffers.
SWIFT_RUNTIME_EXPORT
bool _swift_isImmutableCOWBuffer(HeapObject *object) {
  return object->refCounts.isImmutableCOWBuffer();
}

/// Sets the "immutable" flag on \p object to \p immutable and returns the old
/// value of the flag.
///
/// Used for runtime consistency checking of COW buffers.
SWIFT_RUNTIME_EXPORT
bool _swift_setImmutableCOWBuffer(HeapObject *object, bool immutable) {
  return object->refCounts.setIsImmutableCOWBuffer(immutable);
}

void HeapObject::dump() const {
  auto *Self = const_cast<HeapObject *>(this);
  printf("HeapObject: %p\n", Self);
  printf("HeapMetadata Pointer: %p.\n", Self->metadata);
  printf("Strong Ref Count: %d.\n", Self->refCounts.getCount());
  printf("Unowned Ref Count: %d.\n", Self->refCounts.getUnownedCount());
  printf("Weak Ref Count: %d.\n", Self->refCounts.getWeakCount());
  if (Self->metadata->getKind() == MetadataKind::Class) {
    printf("Uses Native Retain: %s.\n",
           (objectUsesNativeSwiftReferenceCounting(Self) ? "true" : "false"));
  } else {
    printf("Uses Native Retain: Not a class. N/A.\n");
  }
  printf("RefCount Side Table: %p.\n", Self->refCounts.getSideTable());
  printf("Is Deiniting: %s.\n",
         (Self->refCounts.isDeiniting() ? "true" : "false"));
}

#endif
