//===--- MetadataImpl.h - Metadata implementation routines ------*- C++ -*-===//
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
// Declarations used to implement value witnesses for native C/C++ types.
//
// A box class defines some static members which describe the basic
// value-witness properties of a value:
//
//   - NativeBox derives a box from a C++ type
//   - SwiftRetainableBox is a box for Swift object pointers which uses
//     swift_{retain,release}.
//   - FunctionPointerBox is a box for function pointers.
//   - ObjCRetainableBox is a box for Objective-C object pointers,
//     using objc_{retain,release}.
//   - UnknownObjectRetainableBox is a box for void* using
//     swift_unknownObject{Retain,Release}.
//   - AggregateBox<T...> is a box which uses swift layout rules to
//     combine a number of different boxes.
//
// ValueWitnesses<T> takes a box class and defines all the necessary
// values and functions necessary to build a value witness table.
//
// ValueWitnessTableGenerator<T> takes an instance of ValueWitnesses
// and uses it to build a static member 'table', which can be used to
// constant-initialize a value witness table.
//
// ValueWitnessTable
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_RUNTIME_METADATAIMPL_H
#define SWIFT_RUNTIME_METADATAIMPL_H

#include "llvm/Support/Compiler.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/HeapObject.h"
#if SWIFT_OBJC_INTEROP
#include "swift/Runtime/ObjCBridge.h"
#endif

#include "WeakReference.h"
#include "EnumImpl.h"

#include <cstring>
#include <type_traits>

namespace swift {
namespace metadataimpl {

// concept Box<typename T> {
//   using type = T;
//   static constexpr size_t size;
//   static constexpr size_t alignment;
//   static constexpr size_t stride;
//   static constexpr bool isPOD;
//   static constexpr bool isBitwiseTakable;
//   static constexpr unsigned numExtraInhabitants;
//   static void destroy(T *);
//   static T *initializeWithCopy(T *dest, T *src);
//   static T *initializeWithTake(T *dest, T *src);
//   static T *assignWithCopy(T *dest, T *src);
//   static T *assignWithTake(T *dest, T *src);
//   // Only if numExtraInhabitants is non-zero:
//   static void storeExtraInhabitantTag(T *dest, unsigned index);
//   static unsigned getExtraInhabitantTag(const T *src);
// };

/// A box class implemented in terms of C/C++ primitive operations.
/// The type is assumed to be non-polymorphic and to have no extra
/// inhabitants.
///
/// The size/alignment/stride template arguments are for when we want
/// to override the language defaults for a type.
template <class T,
          size_t Alignment = alignof(T),
          size_t Size = sizeof(T),
          size_t Stride = sizeof(T)>
struct NativeBox {
  using type = T;

  static constexpr size_t size = Size;
  static constexpr size_t alignment = Alignment;
  static constexpr size_t stride = Stride;
  static constexpr size_t isPOD = std::is_pod<T>::value;
  static constexpr bool isBitwiseTakable = isPOD;
  static constexpr unsigned numExtraInhabitants = 0;

  static void destroy(T *value) {
    value->T::~T();
  }
  
  static T *initializeWithCopy(T *dest, T *src) {
    return new (dest) T(*src);
  }

  static T *initializeWithTake(T *dest, T *src) {
    T *result = new (dest) T(std::move(*src));
    src->T::~T();
    return result;
  }
  
  static T *assignWithCopy(T *dest, T *src) {
    *dest = *src;
    return dest;
  }

  static T *assignWithTake(T *dest, T *src) {
    *dest = std::move(*src);
    src->T::~T();
    return dest;
  }

private:
  static T *next(T *ptr, size_t n = 1) {
    return (T*)((char*)ptr + stride * n);
  }
  static T *prev(T *ptr, size_t n = 1) {
    return (T*)((char*)ptr - stride * n);
  }
};

/// A CRTP base class for defining boxes of retainable pointers.
template <class Impl, class T> struct RetainableBoxBase {
  using type = T;
  static constexpr size_t size = sizeof(T);
  static constexpr size_t alignment = alignof(T);
  static constexpr size_t stride = sizeof(T);
  static constexpr bool isPOD = false;
  static constexpr bool isBitwiseTakable = true;
#ifdef SWIFT_STDLIB_USE_NONATOMIC_RC
  static constexpr bool isAtomic = false;
#else
  static constexpr bool isAtomic = true;
#endif

  static void destroy(T *addr) {
    Impl::release(*addr);
  }

  static T *initializeWithCopy(T *dest, T *src) {
    *dest = Impl::retain(*src);
    return dest;
  }

  static T *initializeWithTake(T *dest, T *src) {
    *dest = *src;
    return dest;
  }
  
  static T *assignWithCopy(T *dest, T *src) {
    T oldValue = *dest;
    *dest = Impl::retain(*src);
    Impl::release(oldValue);
    return dest;
  }

  static T *assignWithTake(T *dest, T *src) {
    T oldValue = *dest;
    *dest = *src;
    Impl::release(oldValue);
    return dest;
  }

  // Right now, all object pointers are brought down to the least
  // common denominator for extra inhabitants, so that we don't have
  // to worry about e.g. type substitution on an enum type
  // fundamentally changing the layout.
  static constexpr unsigned numExtraInhabitants =
    swift_getHeapObjectExtraInhabitantCount();

  static void storeExtraInhabitantTag(T *dest, unsigned tag) {
    swift_storeHeapObjectExtraInhabitant((HeapObject**) dest, tag - 1);
  }

  static unsigned getExtraInhabitantTag(const T *src) {
    return swift_getHeapObjectExtraInhabitantIndex((HeapObject* const *) src) +1;
  }
};

/// A box implementation class for Swift object pointers.
struct SwiftRetainableBox :
    RetainableBoxBase<SwiftRetainableBox, HeapObject*> {
  static HeapObject *retain(HeapObject *obj) {
    if (isAtomic) {
      swift_retain(obj);
    } else {
      swift_nonatomic_retain(obj);
    }
    return obj;
  }

  static void release(HeapObject *obj) {
    if (isAtomic) {
      swift_release(obj);
    } else {
      swift_nonatomic_release(obj);
    }
  }
};

/// A box implementation class for Swift unowned object pointers.
struct SwiftUnownedRetainableBox :
    RetainableBoxBase<SwiftUnownedRetainableBox, HeapObject*> {
  static HeapObject *retain(HeapObject *obj) {
    if (isAtomic) {
      swift_unownedRetain(obj);
    } else {
      swift_nonatomic_unownedRetain(obj);
    }
    return obj;
  }

  static void release(HeapObject *obj) {
    if (isAtomic) {
      swift_unownedRelease(obj);
    } else {
      swift_nonatomic_unownedRelease(obj);
    }
  }

#if SWIFT_OBJC_INTEROP
  // The implementation from RetainableBoxBase is valid when interop is
  // disabled.
  static constexpr unsigned numExtraInhabitants = 1;

  static void storeExtraInhabitant(HeapObject **dest, unsigned index) {
    assert(index == 1);
    *dest = nullptr;
  }

  static unsigned getExtraInhabitantTag(const HeapObject * const *src) {
    return (*src == nullptr ? 1 : 0);
  }
#endif
};

/// CRTP base class for weak reference boxes.
template<typename Impl, typename T>
struct WeakRetainableBoxBase {
  using type = T;
  static constexpr size_t size = sizeof(type);
  static constexpr size_t alignment = alignof(type);
  static constexpr size_t stride = sizeof(type);
  static constexpr bool isPOD = false;
  static constexpr bool isBitwiseTakable = false;
  static constexpr unsigned numExtraInhabitants = 0;

  // The implementation must provide implementations of:
  //   static void destroy(T *);
  //   static T *initializeWithCopy(T *dest, T *src);
  //   static T *initializeWithTake(T *dest, T *src);
  //   static T *assignWithCopy(T *dest, T *src);
  //   static T *assignWithTake(T *dest, T *src);
};

/// A box implementation class for Swift weak object pointers.
struct SwiftWeakRetainableBox :
    WeakRetainableBoxBase<SwiftWeakRetainableBox, WeakReference> {
  static void destroy(WeakReference *ref) {
    swift_weakDestroy(ref);
  }
  static WeakReference *initializeWithCopy(WeakReference *dest,
                                           WeakReference *src) {
    swift_weakCopyInit(dest, src);
    return dest;
  }
  static WeakReference *initializeWithTake(WeakReference *dest,
                                           WeakReference *src) {
    swift_weakTakeInit(dest, src);
    return dest;
  }
  static WeakReference *assignWithCopy(WeakReference *dest,
                                       WeakReference *src) {
    swift_weakCopyAssign(dest, src);
    return dest;
  }
  static WeakReference *assignWithTake(WeakReference *dest,
                                       WeakReference *src) {
    swift_weakTakeAssign(dest, src);
    return dest;
  }
};

#if SWIFT_OBJC_INTEROP
/// A box implementation class for Objective-C object pointers.
struct ObjCRetainableBox : RetainableBoxBase<ObjCRetainableBox, void*> {
  static constexpr unsigned numExtraInhabitants =
    swift_getHeapObjectExtraInhabitantCount();

  static void *retain(void *obj) {
    return objc_retain((id)obj);
  }

  static void release(void *obj) {
    objc_release((id)obj);
  }
};

/// A box implementation class for unowned Objective-C object pointers.
struct ObjCUnownedRetainableBox
    : WeakRetainableBoxBase<ObjCUnownedRetainableBox, UnownedReference> {

  static constexpr unsigned numExtraInhabitants = 1;
  static void storeExtraInhabitantTag(UnownedReference *dest, unsigned index) {
    assert(index == 1);
    dest->Value = nullptr;
  }
  static unsigned getExtraInhabitantTag(const UnownedReference *src) {
    return (src->Value == nullptr ? 1 : 0);
  }

  static void destroy(UnownedReference *ref) {
    swift_unknownObjectUnownedDestroy(ref);
  }
  static UnownedReference *initializeWithCopy(UnownedReference *dest,
                                              UnownedReference *src) {
    swift_unknownObjectUnownedCopyInit(dest, src);
    return dest;
  }
  static UnownedReference *initializeWithTake(UnownedReference *dest,
                                              UnownedReference *src) {
    swift_unknownObjectUnownedTakeInit(dest, src);
    return dest;
  }
  static UnownedReference *assignWithCopy(UnownedReference *dest,
                                          UnownedReference *src) {
    swift_unknownObjectUnownedCopyAssign(dest, src);
    return dest;
  }
  static UnownedReference *assignWithTake(UnownedReference *dest,
                                          UnownedReference *src) {
    swift_unknownObjectUnownedTakeAssign(dest, src);
    return dest;
  }
};

/// A box implementation class for ObjC weak object pointers.
struct ObjCWeakRetainableBox :
    WeakRetainableBoxBase<ObjCWeakRetainableBox, WeakReference> {
  static void destroy(WeakReference *ref) {
    swift_unknownObjectWeakDestroy(ref);
  }
  static WeakReference *initializeWithCopy(WeakReference *dest,
                                           WeakReference *src) {
    swift_unknownObjectWeakCopyInit(dest, src);
    return dest;
  }
  static WeakReference *initializeWithTake(WeakReference *dest,
                                           WeakReference *src) {
    swift_unknownObjectWeakTakeInit(dest, src);
    return dest;
  }
  static WeakReference *assignWithCopy(WeakReference *dest,
                                       WeakReference *src) {
    swift_unknownObjectWeakCopyAssign(dest, src);
    return dest;
  }
  static WeakReference *assignWithTake(WeakReference *dest,
                                       WeakReference *src) {
    swift_unknownObjectWeakTakeAssign(dest, src);
    return dest;
  }
};

#endif

/// A box implementation class for unknown-retainable object pointers.
struct UnknownObjectRetainableBox
    : RetainableBoxBase<UnknownObjectRetainableBox, void *> {
  static void *retain(void *obj) {
#if SWIFT_OBJC_INTEROP
    swift_unknownObjectRetain(obj);
    return obj;
#else
    if (isAtomic) {
      swift_retain(static_cast<HeapObject *>(obj));
    } else {
      swift_nonatomic_retain(static_cast<HeapObject *>(obj));
    }
    return static_cast<HeapObject *>(obj);
#endif
  }

  static void release(void *obj) {
#if SWIFT_OBJC_INTEROP
    swift_unknownObjectRelease(obj);
#else
    if (isAtomic) {
      swift_release(static_cast<HeapObject *>(obj));
    } else {
      swift_nonatomic_release(static_cast<HeapObject *>(obj));
    }
#endif
  }
};

/// A box implementation class for BridgeObject.
struct BridgeObjectBox :
    RetainableBoxBase<BridgeObjectBox, void*> {
  static void *retain(void *obj) {
    return swift_bridgeObjectRetain(obj);
  }

  static void release(void *obj) {
    swift_bridgeObjectRelease(obj);
  }
};
  
/// A box implementation class for unmanaged, pointer-aligned pointers.
/// Metatype values have this layout.
struct PointerPointerBox : NativeBox<void**> {
  // TODO: we can do a lot better than this: we don't need to mask off
  // the ObjC reserved bits, and we have spare bits.
  static constexpr unsigned numExtraInhabitants =
    swift_getHeapObjectExtraInhabitantCount();

  static void storeExtraInhabitantTag(void ***dest, unsigned tag) {
    swift_storeHeapObjectExtraInhabitant((HeapObject**) dest, tag - 1);
  }

  static unsigned getExtraInhabitantTag(void ** const *src) {
    return swift_getHeapObjectExtraInhabitantIndex((HeapObject* const *) src)+1;
  }
};

/// A box implementation class for raw pointers.
///
/// Note that this is used for imported `void * _Nonnull`, which may include
/// reinterpret_cast-ed integers, so we only get NULL as an extra inhabitant.
struct RawPointerBox : NativeBox<void*> {
  static constexpr unsigned numExtraInhabitants = 1;

  static void storeExtraInhabitantTag(void **dest, unsigned tag) {
    assert(tag == 1);
    *dest = nullptr;
  }

  static unsigned getExtraInhabitantTag(void* const *src) {
    return *src == nullptr ? 1 : 0;
  }
};

/// A box implementation class for unmanaged function pointers.
/// @convention(thin) functions have this layout, as do the first elements of
/// Swift thick functions.
struct FunctionPointerBox : NativeBox<void*> {
  static constexpr unsigned numExtraInhabitants =
    swift_getFunctionPointerExtraInhabitantCount();

  static void storeExtraInhabitantTag(void **dest, unsigned tag) {
    swift_storeFunctionPointerExtraInhabitant(dest, tag - 1);
  }

  static unsigned getExtraInhabitantTag(void * const *src) {
    return swift_getFunctionPointerExtraInhabitantIndex(src) + 1;
  }
};

constexpr size_t roundUpToAlignment(size_t offset, size_t alignment) {
  return ((offset + alignment - 1) & ~(alignment - 1));
}

// A helper template for building an AggregateBox.  The more natural
// way to do this would be to left-recurse, but we have to
// right-recurse because C++ only lets you pattern-match things off
// the beginning of a pack.
template <size_t StartOffset, class... EltBoxes>
struct AggregateBoxHelper;

// Base case: empty list.
template <size_t StartOffset>
struct AggregateBoxHelper<StartOffset> {
public:
  static constexpr size_t endOffset = StartOffset;
  static constexpr size_t alignment = 1;
  static constexpr bool isPOD = true;
  static constexpr bool isBitwiseTakable = true;

public:
#define COPY_OP(OP)                        \
  static char *OP(char *dest, char *src) { \
    return dest;                           \
  }
  COPY_OP(initializeWithCopy)
  COPY_OP(initializeWithTake)
  COPY_OP(assignWithCopy)
  COPY_OP(assignWithTake)
#undef COPY_OP

  static void destroy(char *addr) {}
};

// Recursive case: add an element to the start.
template <size_t StartOffset, class EltBox, class... NextBoxes>
struct AggregateBoxHelper<StartOffset, EltBox, NextBoxes...> {
private:
  static constexpr size_t eltOffset =
    roundUpToAlignment(StartOffset, EltBox::alignment);
  static constexpr size_t startToEltOffset = (eltOffset - StartOffset);
  static constexpr size_t nextOffset = eltOffset + EltBox::size;
  using NextHelper = AggregateBoxHelper<nextOffset, NextBoxes...>;

public:
  static constexpr size_t endOffset = NextHelper::endOffset;
  static constexpr size_t alignment =
    (NextHelper::alignment > EltBox::alignment
   ? NextHelper::alignment : EltBox::alignment);
  static constexpr bool isPOD = EltBox::isPOD && NextHelper::isPOD;
  static constexpr bool isBitwiseTakable =
    EltBox::isBitwiseTakable && NextHelper::isBitwiseTakable;

private:
  static constexpr size_t eltToNextOffset = (nextOffset - eltOffset);
  static constexpr size_t startToNextOffset = (nextOffset - StartOffset);

public:
#define COPY_OP(OP)                                                       \
  static char *OP(char *dest, char *src) {                                \
    dest += startToEltOffset;                                             \
    src += startToEltOffset;                                              \
    dest = (char*) EltBox::OP((typename EltBox::type*) dest,              \
                              (typename EltBox::type*) src);              \
    dest = NextHelper::OP(dest + eltToNextOffset, src + eltToNextOffset); \
    return dest - startToNextOffset;                                      \
  }
  COPY_OP(initializeWithCopy)
  COPY_OP(initializeWithTake)
  COPY_OP(assignWithCopy)
  COPY_OP(assignWithTake)
#undef COPY_OP

  static void destroy(char *addr) {
    // We have no particular reason to destroy in either order.
    addr += startToEltOffset;
    EltBox::destroy((typename EltBox::type*) addr);
    NextHelper::destroy(addr + eltToNextOffset);
  }
};

/// A class which produces a tuple-like box (with Swift layout rules)
/// for a list of element boxes.
///
/// The aggregate box is monomorphic and has no extra inhabitants.
template <class... EltBoxes>
struct AggregateBox {
  using type = char;

  using Helper = AggregateBoxHelper<0, EltBoxes...>;
  static constexpr size_t size = Helper::endOffset;
  static constexpr size_t alignment = Helper::alignment;
  static constexpr size_t rawStride = roundUpToAlignment(size, alignment);
  static constexpr size_t stride = rawStride == 0 ? 1 : rawStride;

  static constexpr bool isPOD = Helper::isPOD;
  static constexpr bool isBitwiseTakable = Helper::isBitwiseTakable;

  /// Don't collect extra inhabitants from the members by default.
  static constexpr unsigned numExtraInhabitants = 0;

  static void destroy(char *value) {
    Helper::destroy(value);
  }

  static char *initializeWithCopy(char *dest, char *src) {
    return Helper::initializeWithCopy(dest, src);
  }

  static char *initializeWithTake(char *dest, char *src) {
    return Helper::initializeWithTake(dest, src);
  }
    
  static char *assignWithCopy(char *dest, char *src) {
    return Helper::assignWithCopy(dest, src);
  }

  static char *assignWithTake(char *dest, char *src) {
    return Helper::assignWithTake(dest, src);
  }
};
  
/// A template for using the Swift allocation APIs with a known size
/// and alignment.
template <size_t Size, size_t Alignment>
struct SwiftAllocator {
  static void *alloc() {
    return swift_slowAlloc(Size, Alignment-1);
  }

  static void dealloc(void *addr) {
    swift_slowDealloc(addr, Size, Alignment-1);
  }
};

/// A CRTP class which provides basic implementations for a number of
/// value witnesses relating to buffers.
template <class Impl>
struct BufferValueWitnessesBase {};

/// How should a type be packed into a fixed-size buffer?
enum class FixedPacking {
  Allocate,
  OffsetZero
};
constexpr FixedPacking getFixedPacking(bool isBitwiseTakable, size_t size,
                                       size_t alignment) {
  return (canBeInline(isBitwiseTakable, size, alignment)
              ? FixedPacking::OffsetZero
              : FixedPacking::Allocate);
}

/// A CRTP base class which provides default implementations of a
/// number of value witnesses.
template <class Impl, bool isBitwiseTakable, size_t Size, size_t Alignment,
          FixedPacking Packing =
              getFixedPacking(isBitwiseTakable, Size, Alignment)>
struct BufferValueWitnesses;

/// An implementation of ValueBase suitable for classes that can be
/// allocated inline.
template <class Impl, bool isBitwiseTakable, size_t Size, size_t Alignment>
struct BufferValueWitnesses<Impl, isBitwiseTakable, Size, Alignment,
                            FixedPacking::OffsetZero>
    : BufferValueWitnessesBase<Impl> {
  static constexpr bool isInline = true;

  static OpaqueValue *initializeBufferWithCopyOfBuffer(ValueBuffer *dest,
                                                       ValueBuffer *src,
                                                       const Metadata *self) {
    return Impl::initializeWithCopy(reinterpret_cast<OpaqueValue *>(dest),
                                    reinterpret_cast<OpaqueValue *>(src), self);
  }
};

/// An implementation of BufferValueWitnesses suitable for types that
/// cannot be allocated inline.
template <class Impl, bool isBitwiseTakable, size_t Size, size_t Alignment>
struct BufferValueWitnesses<Impl, isBitwiseTakable, Size, Alignment,
                            FixedPacking::Allocate>
    : BufferValueWitnessesBase<Impl> {
  static constexpr bool isInline = false;

  static OpaqueValue *initializeBufferWithCopyOfBuffer(ValueBuffer *dest,
                                                       ValueBuffer *src,
                                                       const Metadata *self) {
    auto wtable = self->getValueWitnesses();
    auto reference = src->PrivateData[0];
    dest->PrivateData[0] = reference;
    swift_retain(reinterpret_cast<HeapObject *>(reference));
    // Project the address of the value in the buffer.
    unsigned alignMask = wtable->getAlignmentMask();
    // Compute the byte offset of the object in the box.
    unsigned byteOffset = (sizeof(HeapObject) + alignMask) & ~alignMask;
    auto *bytePtr = reinterpret_cast<char *>(reference);
    return reinterpret_cast<OpaqueValue *>(bytePtr + byteOffset);
  }
};

/// A class which provides BufferValueWitnesses for types that are not
/// fixed in size.
template <class Impl, bool IsKnownAllocated>
struct NonFixedBufferValueWitnesses : BufferValueWitnessesBase<Impl> {

  static OpaqueValue *initializeBufferWithCopyOfBuffer(ValueBuffer *dest,
                                                       ValueBuffer *src,
                                                       const Metadata *self) {
    auto vwtable = self->getValueWitnesses();
    (void)vwtable;
    if (!IsKnownAllocated && vwtable->isValueInline()) {
      return Impl::initializeWithCopy(reinterpret_cast<OpaqueValue*>(dest),
                                      reinterpret_cast<OpaqueValue*>(src),
                                      self);
    } else {
      auto reference = src->PrivateData[0];
      dest->PrivateData[0] = reference;
      swift_retain(reinterpret_cast<HeapObject*>(reference));
      // Project the address of the value in the buffer.
      unsigned alignMask = vwtable->getAlignmentMask();
      // Compute the byte offset of the object in the box.
      unsigned byteOffset = (sizeof(HeapObject) + alignMask) & ~alignMask;
      auto *bytePtr = reinterpret_cast<char *>(reference);
      return reinterpret_cast<OpaqueValue *>(bytePtr + byteOffset);
    }
  }
};

/// Provides implementations for
/// getEnumTagSinglePayload/storeEnumTagSinglePayload.
template <class Impl, bool isBitwiseTakable, size_t Size, size_t Alignment,
          bool hasExtraInhabitants>
struct FixedSizeBufferValueWitnesses;

/// A fixed size buffer value witness that can rely on the presents of the extra
/// inhabitant functions.
template <class Impl, bool isBitwiseTakable, size_t Size, size_t Alignment>
struct FixedSizeBufferValueWitnesses<Impl, isBitwiseTakable, Size, Alignment,
                                     true /*hasExtraInhabitants*/>
    : BufferValueWitnesses<Impl, isBitwiseTakable, Size, Alignment> {

  static unsigned getEnumTagSinglePayload(const OpaqueValue *enumAddr,
                                          unsigned numEmptyCases,
                                          const Metadata *self) {
    return getEnumTagSinglePayloadImpl(enumAddr, numEmptyCases, self, Size,
                                       Impl::extraInhabitantCount,
                                       Impl::getExtraInhabitantTag);
  }

  static void storeEnumTagSinglePayload(OpaqueValue *enumAddr,
                                        unsigned whichCase,
                                        unsigned numEmptyCases,
                                        const Metadata *self) {
    return storeEnumTagSinglePayloadImpl(enumAddr, whichCase, numEmptyCases,
                                         self, Size, Impl::extraInhabitantCount,
                                         Impl::storeExtraInhabitantTag);
  }
};

/// A fixed size buffer value witness that cannot rely on the presents of the
/// extra inhabitant functions.
template <class Impl, bool isBitwiseTakable, size_t Size, size_t Alignment>
struct FixedSizeBufferValueWitnesses<Impl, isBitwiseTakable, Size, Alignment,
                                     false /*hasExtraInhabitants*/>
    : BufferValueWitnesses<Impl, isBitwiseTakable, Size, Alignment> {

  static unsigned getEnumTagSinglePayload(const OpaqueValue *enumAddr,
                                          unsigned numEmptyCases,
                                          const Metadata *self) {
    return getEnumTagSinglePayloadImpl(enumAddr, numEmptyCases, self, Size, 0,
                                       nullptr);
  }

  static void storeEnumTagSinglePayload(OpaqueValue *enumAddr,
                                        unsigned whichCase,
                                        unsigned numEmptyCases,
                                        const Metadata *self) {
    return storeEnumTagSinglePayloadImpl(enumAddr, whichCase, numEmptyCases,
                                         self, Size, 0, nullptr);
  }
};

static constexpr bool hasExtraInhabitants(unsigned numExtraInhabitants) {
  return numExtraInhabitants != 0;
}
/// A class which provides default implementations of various value
/// witnesses based on a box's value operations.
///
/// The box type has to provide a numExtraInhabitants member, but as
/// long as it's zero, the rest is fine.
template <class Box>
struct ValueWitnesses
    : FixedSizeBufferValueWitnesses<
          ValueWitnesses<Box>, Box::isBitwiseTakable, Box::size, Box::alignment,
          hasExtraInhabitants(Box::numExtraInhabitants)> {
  using Base = FixedSizeBufferValueWitnesses<
      ValueWitnesses<Box>, Box::isBitwiseTakable, Box::size, Box::alignment,
      hasExtraInhabitants(Box::numExtraInhabitants)>;

  static constexpr size_t size = Box::size;
  static constexpr size_t stride = Box::stride;
  static constexpr size_t alignment = Box::alignment;
  static constexpr bool isPOD = Box::isPOD;
  static constexpr bool isBitwiseTakable = Box::isBitwiseTakable;
  static constexpr unsigned extraInhabitantCount = Box::numExtraInhabitants;
  static constexpr bool hasExtraInhabitants = (extraInhabitantCount != 0);
  static constexpr ValueWitnessFlags flags =
    ValueWitnessFlags().withAlignmentMask(alignment - 1)
                       .withInlineStorage(Base::isInline && isBitwiseTakable)
                       .withPOD(isPOD)
                       .withBitwiseTakable(isBitwiseTakable);

  static void destroy(OpaqueValue *value, const Metadata *self) {
    return Box::destroy((typename Box::type*) value);
  }

  static OpaqueValue *initializeWithCopy(OpaqueValue *dest, OpaqueValue *src,
                                         const Metadata *self) {
    return (OpaqueValue*) Box::initializeWithCopy((typename Box::type*) dest,
                                                  (typename Box::type*) src);
  }

  static OpaqueValue *initializeWithTake(OpaqueValue *dest, OpaqueValue *src,
                                         const Metadata *self) {
    return (OpaqueValue*) Box::initializeWithTake((typename Box::type*) dest,
                                                  (typename Box::type*) src);
  }

  static OpaqueValue *assignWithCopy(OpaqueValue *dest, OpaqueValue *src,
                                     const Metadata *self) {
    return (OpaqueValue*) Box::assignWithCopy((typename Box::type*) dest,
                                              (typename Box::type*) src);
  }

  static OpaqueValue *assignWithTake(OpaqueValue *dest, OpaqueValue *src,
                                     const Metadata *self) {
    return (OpaqueValue*) Box::assignWithTake((typename Box::type*) dest,
                                              (typename Box::type*) src);
  }

  // These should not get instantiated if the type doesn't have extra
  // inhabitants.

  SWIFT_CC
  static void storeExtraInhabitantTag(OpaqueValue *dest, unsigned tag,
                                      unsigned xiCount, const Metadata *self) {
    Box::storeExtraInhabitantTag((typename Box::type*) dest, tag);
  }

  SWIFT_CC
  static unsigned getExtraInhabitantTag(const OpaqueValue *src,
                                        unsigned xiCount,
                                        const Metadata *self) {
    return Box::getExtraInhabitantTag((typename Box::type const *) src);
  }
};

/// A class which provides basic implementations of various function
/// value witnesses based on a type that is not fixed in size.
///
/// The 'Box' concept here is slightly different from the one for
/// fixed-size types: it does not need to provide size/alignment/isPOD
/// members, and its functions all take an extra 'const Metadata *self'
/// argument.
///
/// \tparam IsKnownAllocated - whether the type is known to not fit in
/// a fixed-size buffer
template <class Box, bool IsKnownAllocated>
struct NonFixedValueWitnesses :
  NonFixedBufferValueWitnesses<NonFixedValueWitnesses<Box, IsKnownAllocated>,
                               IsKnownAllocated>
{

  static constexpr unsigned numExtraInhabitants = Box::numExtraInhabitants;
  static constexpr bool hasExtraInhabitants = (numExtraInhabitants != 0);

  static void destroy(OpaqueValue *value, const Metadata *self) {
    return Box::destroy((typename Box::type*) value, self);
  }
  
  static OpaqueValue *initializeWithCopy(OpaqueValue *dest, OpaqueValue *src,
                                         const Metadata *self) {
    return (OpaqueValue*) Box::initializeWithCopy((typename Box::type*) dest,
                                                  (typename Box::type*) src,
                                                  self);
  }
  
  static OpaqueValue *initializeWithTake(OpaqueValue *dest, OpaqueValue *src,
                                         const Metadata *self) {
    return (OpaqueValue*) Box::initializeWithTake((typename Box::type*) dest,
                                                  (typename Box::type*) src,
                                                  self);
  }
  
  static OpaqueValue *assignWithCopy(OpaqueValue *dest, OpaqueValue *src,
                                     const Metadata *self) {
    return (OpaqueValue*) Box::assignWithCopy((typename Box::type*) dest,
                                              (typename Box::type*) src,
                                              self);
  }

  static OpaqueValue *assignWithTake(OpaqueValue *dest, OpaqueValue *src,
                                     const Metadata *self) {
    return (OpaqueValue*) Box::assignWithTake((typename Box::type*) dest,
                                              (typename Box::type*) src,
                                              self);
  }

  static unsigned getEnumTagSinglePayload(const OpaqueValue *enumAddr,
                                          unsigned numEmptyCases,
                                          const Metadata *self) {
    auto *payloadWitnesses = self->getValueWitnesses();
    auto size = payloadWitnesses->getSize();
    auto numExtraInhabitants = payloadWitnesses->getNumExtraInhabitants();

    return getEnumTagSinglePayloadImpl(enumAddr, numEmptyCases, self, size,
                                       numExtraInhabitants,
                                       getExtraInhabitantTag);
  }

  static void storeEnumTagSinglePayload(OpaqueValue *enumAddr,
                                        unsigned whichCase,
                                        unsigned numEmptyCases,
                                        const Metadata *self) {
    auto *payloadWitnesses = self->getValueWitnesses();
    auto size = payloadWitnesses->getSize();
    auto numExtraInhabitants = payloadWitnesses->getNumExtraInhabitants();

    storeEnumTagSinglePayloadImpl(enumAddr, whichCase, numEmptyCases, self,
                                  size, numExtraInhabitants,
                                  storeExtraInhabitantTag);
  }

  // These should not get instantiated if the type doesn't have extra
  // inhabitants.

  SWIFT_CC
  static void storeExtraInhabitantTag(OpaqueValue *dest, unsigned tag,
                                      unsigned xiCount, const Metadata *self) {
    Box::storeExtraInhabitantTag((typename Box::type*) dest, tag);
  }

  SWIFT_CC
  static unsigned getExtraInhabitantTag(const OpaqueValue *src,
                                        unsigned xiCount,
                                        const Metadata *self) {
    return Box::getExtraInhabitantTag((typename Box::type const *) src);
  }
};

/// A class which defines a ValueWitnessTable.
template <class Witnesses>
struct ValueWitnessTableGenerator {
  static constexpr const ValueWitnessTable table = {
#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define VALUE_WITNESS(LOWER_ID, UPPER_ID) Witnesses::LOWER_ID,
#include "swift/ABI/ValueWitness.def"
  };
};

/// A convenient way to get the value witness table for a box class.
template <class Box>
using ValueWitnessTableForBox = ValueWitnessTableGenerator<ValueWitnesses<Box>>;

} // end namespace metadataimpl
} // end namespace swift

#endif /* SWIFT_RUNTIME_METADATAIMPL_H */
