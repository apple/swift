//===--- Metadata.h - Swift Language ABI Metadata Support -------*- C++ -*-===//
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
// Swift ABI describing metadata.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ABI_METADATA_H
#define SWIFT_ABI_METADATA_H

#include <atomic>
#include <iterator>
#include <string>
#include <type_traits>
#include <utility>
#include <string.h>
#include "llvm/ADT/ArrayRef.h"
#include "swift/Strings.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/Once.h"
#include "swift/ABI/GenericContext.h"
#include "swift/ABI/MetadataRef.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/ABI/System.h"
#include "swift/ABI/TargetLayout.h"
#include "swift/ABI/TrailingObjects.h"
#include "swift/ABI/ValueWitnessTable.h"
#include "swift/Basic/Malloc.h"
#include "swift/Basic/FlaggedPointer.h"
#include "swift/Basic/RelativePointer.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/Basic/Unreachable.h"
#include "swift/shims/HeapObject.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Casting.h"

namespace swift {

template <typename Runtime> struct TargetGenericMetadataInstantiationCache;
template <typename Runtime> struct TargetAnyClassMetadata;
template <typename Runtime> struct TargetAnyClassMetadataObjCInterop;
template <typename Runtime, typename TargetAnyClassMetadataVariant>
struct TargetClassMetadata;
template <typename Runtime> struct TargetStructMetadata;
template <typename Runtime> struct TargetOpaqueMetadata;
template <typename Runtime> struct TargetValueMetadata;
template <typename Runtime> struct TargetForeignClassMetadata;
template <typename Runtime> struct TargetForeignReferenceTypeMetadata;
template <typename Runtime>
struct swift_ptrauth_struct_context_descriptor(ContextDescriptor)
    TargetContextDescriptor;
template <typename Runtime>
class swift_ptrauth_struct_context_descriptor(TypeContextDescriptor)
    TargetTypeContextDescriptor;
template <typename Runtime>
class swift_ptrauth_struct_context_descriptor(ClassDescriptor)
    TargetClassDescriptor;
template <typename Runtime>
class swift_ptrauth_struct_context_descriptor(ValueTypeDescriptor)
    TargetValueTypeDescriptor;
template <typename Runtime>
class swift_ptrauth_struct_context_descriptor(EnumDescriptor)
    TargetEnumDescriptor;
template <typename Runtime>
class swift_ptrauth_struct_context_descriptor(StructDescriptor)
    TargetStructDescriptor;
template <typename Runtime> struct TargetGenericMetadataPattern;
template <typename Runtime> struct TargetProtocolConformanceDescriptor;

struct HeapObject;
class WeakReference;
struct UnownedReference;

/// The result of requesting type metadata.  Generally the return value of
/// a function.
///
/// For performance and ABI matching across Swift/C++, functions returning
/// this type must use SWIFT_CC so that the components are returned as separate
/// values.
struct MetadataResponse {
  /// The requested metadata.
  const Metadata *Value;

  /// The current state of the metadata returned.  Always use this
  /// instead of trying to inspect the metadata directly to see if it
  /// satisfies the request.  An incomplete metadata may be getting
  /// initialized concurrently.  But this can generally be ignored if
  /// the metadata request was for abstract metadata or if the request
  /// is blocking.
  MetadataState State;
};

/// A dependency on the metadata progress of other type, indicating that
/// initialization of a metadata cannot progress until another metadata
/// reaches a particular state.
///
/// For performance, functions returning this type should use SWIFT_CC so
/// that the components are returned as separate values.
struct MetadataDependency {
  /// Either null, indicating that initialization was successful, or
  /// a metadata on which initialization depends for further progress.
  const Metadata *Value;

  /// The state that Metadata needs to be in before initialization
  /// can continue.
  MetadataState Requirement;

  MetadataDependency() : Value(nullptr) {}
  MetadataDependency(const Metadata *metadata, MetadataState requirement)
    : Value(metadata), Requirement(requirement) {}

  explicit operator bool() const { return Value != nullptr; }

  bool operator==(MetadataDependency other) const {
    assert(Value && other.Value);
    return Value == other.Value &&
           Requirement == other.Requirement;
  }
};

/// Prefix of a metadata header, containing a pointer to the
/// type layout string.
template <typename Runtime>
struct TargetTypeMetadataLayoutPrefix {
    TargetSignedPointer<Runtime, const uint8_t *
                                      __ptrauth_swift_type_layout_string>
        layoutString;
};

/// The header before a metadata object which appears on all type
/// metadata.  Note that heap metadata are not necessarily type
/// metadata, even for objects of a heap type: for example, objects of
/// Objective-C type possess a form of heap metadata (an Objective-C
/// Class pointer), but this metadata lacks the type metadata header.
/// This case can be distinguished using the isTypeMetadata() flag
/// on ClassMetadata.
template <typename Runtime>
struct TargetTypeMetadataHeaderBase {
  /// A pointer to the value-witnesses for this type.  This is only
  /// present for type metadata.
  TargetPointer<Runtime, const ValueWitnessTable> ValueWitnesses;
};

template <typename Runtime>
struct TargetTypeMetadataHeader
    : TargetTypeMetadataLayoutPrefix<Runtime>,
      TargetTypeMetadataHeaderBase<Runtime> {

  TargetTypeMetadataHeader() = default;
  constexpr TargetTypeMetadataHeader(
    const TargetTypeMetadataLayoutPrefix<Runtime> &layout,
    const TargetTypeMetadataHeaderBase<Runtime> &header)
      : TargetTypeMetadataLayoutPrefix<Runtime>(layout),
        TargetTypeMetadataHeaderBase<Runtime>(header) {}
};

using TypeMetadataHeader = TargetTypeMetadataHeader<InProcess>;

/// A "full" metadata pointer is simply an adjusted address point on a
/// metadata object; it points to the beginning of the metadata's
/// allocation, rather than to the canonical address point of the
/// metadata object.
template <class T> struct FullMetadata : T::HeaderType, T {
  typedef typename T::HeaderType HeaderType;

  FullMetadata() = default;
  constexpr FullMetadata(const HeaderType &header, const T &metadata)
    : HeaderType(header), T(metadata) {}

  template <class... Args>
  constexpr FullMetadata(const HeaderType &header, Args &&...metadataArgs)
    : HeaderType(header), T(std::forward<Args>(metadataArgs)...) {}
};

/// Given a canonical metadata pointer, produce the adjusted metadata pointer.
template <class T>
static inline FullMetadata<T> *asFullMetadata(T *metadata) {
  return (FullMetadata<T>*) (((typename T::HeaderType*) metadata) - 1);
}
template <class T>
static inline const FullMetadata<T> *asFullMetadata(const T *metadata) {
  return asFullMetadata(const_cast<T*>(metadata));
}

// std::result_of is busted in Xcode 5. This is a simplified reimplementation
// that isn't SFINAE-safe.
namespace {
  template<typename T> struct _ResultOf;
  
  template<typename R, typename...A>
  struct _ResultOf<R(*)(A...)> {
    using type = R;
  };
}

using TypeContextDescriptor = TargetTypeContextDescriptor<InProcess>;

template<template <typename Runtime> class ObjCInteropKind, unsigned PointerSize>
using ExternalTypeContextDescriptor = TargetTypeContextDescriptor<External<ObjCInteropKind<RuntimeTarget<PointerSize>>>>;

// FIXME: https://github.com/apple/swift/issues/43763
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"

/// Bounds for metadata objects.
template <typename Runtime>
struct TargetMetadataBounds {
  using StoredSize = typename Runtime::StoredSize;

  /// The negative extent of the metadata, in words.
  uint32_t NegativeSizeInWords;

  /// The positive extent of the metadata, in words.
  uint32_t PositiveSizeInWords;

  /// Return the total size of the metadata in bytes, including both
  /// negatively- and positively-offset members.
  StoredSize getTotalSizeInBytes() const {
    return (StoredSize(NegativeSizeInWords) + StoredSize(PositiveSizeInWords))
              * sizeof(void*);
  }

  /// Return the offset of the address point of the metadata from its
  /// start, in bytes.
  StoredSize getAddressPointInBytes() const {
    return StoredSize(NegativeSizeInWords) * sizeof(void*);
  }
};
using MetadataBounds = TargetMetadataBounds<InProcess>;

/// The common structure of all type metadata.
template <typename Runtime>
struct TargetMetadata {
  using StoredPointer = typename Runtime::StoredPointer;

  /// The basic header type.
  typedef TargetTypeMetadataHeader<Runtime> HeaderType;

  constexpr TargetMetadata()
    : Kind(static_cast<StoredPointer>(MetadataKind::Class)) {}
  constexpr TargetMetadata(MetadataKind Kind)
    : Kind(static_cast<StoredPointer>(Kind)) {}

#if SWIFT_OBJC_INTEROP
protected:
  constexpr TargetMetadata(TargetAnyClassMetadataObjCInterop<Runtime> *isa)
    : Kind(reinterpret_cast<StoredPointer>(isa)) {}
#endif

private:
  /// The kind. Only valid for non-class metadata; getKind() must be used to get
  /// the kind value.
  StoredPointer Kind;
public:
  /// Get the metadata kind.
  MetadataKind getKind() const {
    return getEnumeratedMetadataKind(Kind);
  }
  
  /// Set the metadata kind.
  void setKind(MetadataKind kind) {
    Kind = static_cast<StoredPointer>(kind);
  }

protected:
  const TargetAnyClassMetadata<Runtime> *getClassISA() const {
    return reinterpret_cast<const TargetAnyClassMetadata<Runtime> *>(Kind);
  }
  void setClassISA(const TargetAnyClassMetadata<Runtime> *isa) {
    Kind = reinterpret_cast<StoredPointer>(isa);
  }

public:
  /// Is this a class object--the metadata record for a Swift class (which also
  /// serves as the class object), or the class object for an ObjC class (which
  /// is not metadata)?
  bool isClassObject() const {
    return static_cast<MetadataKind>(getKind()) == MetadataKind::Class;
  }
  
  /// Does the given metadata kind represent metadata for some kind of class?
  static bool isAnyKindOfClass(MetadataKind k) {
    switch (k) {
    case MetadataKind::Class:
    case MetadataKind::ObjCClassWrapper:
    case MetadataKind::ForeignClass:
    case MetadataKind::ForeignReferenceType:
      return true;

    default:
      return false;
    }
  }
  
  /// Is this metadata for an existential type?
  bool isAnyExistentialType() const {
    switch (getKind()) {
    case MetadataKind::ExistentialMetatype:
    case MetadataKind::Existential:
      return true;

    default:
      return false;
    }
  }
  
  /// Is this either type metadata or a class object for any kind of class?
  bool isAnyClass() const {
    return isAnyKindOfClass(getKind());
  }

  const uint8_t *getLayoutString() const {
    assert(hasLayoutString());
    if (isAnyClass()) {
      return asFullMetadata(
                 reinterpret_cast<const TargetAnyClassMetadata<Runtime> *>(
                     this))
          ->layoutString;
    }
    return asFullMetadata(this)->layoutString;
  }

  const ValueWitnessTable *getValueWitnesses() const {
    return asFullMetadata(this)->ValueWitnesses;
  }

  const TypeLayout *getTypeLayout() const {
    return getValueWitnesses()->getTypeLayout();
  }

  void setValueWitnesses(const ValueWitnessTable *table) {
    asFullMetadata(this)->ValueWitnesses = table;
  }

  void setLayoutString(const uint8_t *layoutString) {
    if (isAnyClass()) {
      asFullMetadata(reinterpret_cast<TargetAnyClassMetadata<Runtime> *>(this))
          ->layoutString = layoutString;
    } else {
      asFullMetadata(this)->layoutString = layoutString;
    }
  }

  bool hasLayoutString() const {
    if (auto *contextDescriptor = getTypeContextDescriptor()) {
      return contextDescriptor->hasLayoutString();
    }

    return false;
  }
  
  // Define forwarders for value witnesses. These invoke this metadata's value
  // witness table with itself as the 'self' parameter.
  #define WANT_ONLY_REQUIRED_VALUE_WITNESSES
  #define FUNCTION_VALUE_WITNESS(WITNESS, UPPER, RET_TYPE, PARAM_TYPES)    \
    template<typename...A>                                                 \
    _ResultOf<ValueWitnessTypes::WITNESS ## Unsigned>::type                            \
    vw_##WITNESS(A &&...args) const {                                      \
      return getValueWitnesses()->WITNESS(std::forward<A>(args)..., this); \
    }
  #define DATA_VALUE_WITNESS(LOWER, UPPER, TYPE)
  #include "swift/ABI/ValueWitness.def"

  unsigned vw_getEnumTag(const OpaqueValue *value) const {
    return getValueWitnesses()->_asEVWT()->getEnumTag(const_cast<OpaqueValue*>(value), this);
  }
  void vw_destructiveProjectEnumData(OpaqueValue *value) const {
    getValueWitnesses()->_asEVWT()->destructiveProjectEnumData(value, this);
  }
  void vw_destructiveInjectEnumTag(OpaqueValue *value, unsigned tag) const {
    getValueWitnesses()->_asEVWT()->destructiveInjectEnumTag(value, tag, this);
  }

  size_t vw_size() const {
    return getValueWitnesses()->getSize();
  }

  size_t vw_alignment() const {
    return getValueWitnesses()->getAlignment();
  }

  size_t vw_stride() const {
    return getValueWitnesses()->getStride();
  }

  unsigned vw_getNumExtraInhabitants() const {
    return getValueWitnesses()->getNumExtraInhabitants();
  }

  /// Allocate an out-of-line buffer if values of this type don't fit in the
  /// ValueBuffer.
  /// NOTE: This is not a box for copy-on-write existentials.
  OpaqueValue *allocateBufferIn(ValueBuffer *buffer) const;

  /// Get the address of the memory previously allocated in the ValueBuffer.
  /// NOTE: This is not a box for copy-on-write existentials.
  OpaqueValue *projectBufferFrom(ValueBuffer *buffer) const;

  /// Deallocate an out-of-line buffer stored in 'buffer' if values of this type
  /// are not stored inline in the ValueBuffer.
  void deallocateBufferIn(ValueBuffer *buffer) const;

  // Allocate an out-of-line buffer box (reference counted) if values of this
  // type don't fit in the ValueBuffer.
  // NOTE: This *is* a box for copy-on-write existentials.
  OpaqueValue *allocateBoxForExistentialIn(ValueBuffer *Buffer) const;

  // Deallocate an out-of-line buffer box if one is present.
  void deallocateBoxForExistentialIn(ValueBuffer *Buffer) const;

  /// Get the nominal type descriptor if this metadata describes a nominal type,
  /// or return null if it does not.
  ConstTargetMetadataPointer<Runtime, TargetTypeContextDescriptor>
  getTypeContextDescriptor() const {
    switch (getKind()) {
    case MetadataKind::Class: {
      const auto cls =
        static_cast<const TargetClassMetadataType<Runtime> *>(this);
      if (!cls->isTypeMetadata())
        return nullptr;
      if (cls->isArtificialSubclass())
        return nullptr;
      return cls->getDescription();
    }
    case MetadataKind::Struct:
    case MetadataKind::Enum:
    case MetadataKind::Optional:
      return static_cast<const TargetValueMetadata<Runtime> *>(this)
          ->Description;
    case MetadataKind::ForeignClass:
      return static_cast<const TargetForeignClassMetadata<Runtime> *>(this)
          ->Description;
    case MetadataKind::ForeignReferenceType:
      return static_cast<const TargetForeignReferenceTypeMetadata<Runtime> *>(this)
          ->Description;
    default:
      return nullptr;
    }
  }

  /// Get the class object for this type if it has one, or return null if the
  /// type is not a class (or not a class with a class object).
  const TargetClassMetadataType<Runtime> *
  getClassObject() const;

  /// Retrieve the generic arguments of this type, if it has any.
  ConstTargetMetadataPointer<Runtime, swift::TargetMetadata> const *
  getGenericArgs() const {
    auto description = getTypeContextDescriptor();
    if (!description)
      return nullptr;

    auto generics = description->getGenericContext();
    if (!generics)
      return nullptr;

    auto asWords = reinterpret_cast<
      ConstTargetMetadataPointer<Runtime, swift::TargetMetadata> const *>(this);
    return asWords + description->getGenericArgumentOffset();
  }

  bool satisfiesClassConstraint() const;

  const TypeContextDescriptor *getDescription() const;

  bool isStaticallySpecializedGenericMetadata() const;

  bool isCanonicalStaticallySpecializedGenericMetadata() const;

#if SWIFT_OBJC_INTEROP
  /// Get the ObjC class object for this type if it has one, or return null if
  /// the type is not a class (or not a class with a class object).
  /// This is allowed for InProcess values only.
  template <typename R = Runtime>
  typename std::enable_if<std::is_same<R, InProcess>::value, Class>::type
  getObjCClassObject() const {
    return reinterpret_cast<Class>(
        const_cast<TargetClassMetadata<
            InProcess, TargetAnyClassMetadataObjCInterop<InProcess>> *>(
            getClassObject()));
  }
#endif

#ifndef NDEBUG
  [[deprecated("Only meant for use in the debugger")]] void dump() const;
#endif

protected:
  friend struct TargetOpaqueMetadata<Runtime>;
  
  /// Metadata should not be publicly copied or moved.
  constexpr TargetMetadata(const TargetMetadata &) = default;
  TargetMetadata &operator=(const TargetMetadata &) = default;
  constexpr TargetMetadata(TargetMetadata &&) = default;
  TargetMetadata &operator=(TargetMetadata &&) = default;
};

/// The common structure of opaque metadata.  Adds nothing.
template <typename Runtime>
struct TargetOpaqueMetadata {
  typedef TargetTypeMetadataHeaderBase<Runtime> HeaderType;

  // We have to represent this as a member so we can list-initialize it.
  TargetMetadata<Runtime> base;
};

using HeapObjectDestroyer =
  SWIFT_CC(swift) void(SWIFT_CONTEXT HeapObject *);

/// The prefix on a heap metadata.
template <typename Runtime>
struct TargetHeapMetadataHeaderPrefix {
  /// Destroy the object, returning the allocated size of the object
  /// or 0 if the object shouldn't be deallocated.
  TargetSignedPointer<Runtime, HeapObjectDestroyer *
                                   __ptrauth_swift_heap_object_destructor>
      destroy;
};
using HeapMetadataHeaderPrefix =
  TargetHeapMetadataHeaderPrefix<InProcess>;

/// The header present on all heap metadata.
template <typename Runtime>
struct TargetHeapMetadataHeader
    : TargetTypeMetadataLayoutPrefix<Runtime>,
      TargetHeapMetadataHeaderPrefix<Runtime>,
      TargetTypeMetadataHeaderBase<Runtime> {
  constexpr TargetHeapMetadataHeader(
      const TargetTypeMetadataLayoutPrefix<Runtime> &typeLayoutPrefix,
      const TargetHeapMetadataHeaderPrefix<Runtime> &heapPrefix,
      const TargetTypeMetadataHeaderBase<Runtime> &typePrefix)
    : TargetTypeMetadataLayoutPrefix<Runtime>(typeLayoutPrefix),
      TargetHeapMetadataHeaderPrefix<Runtime>(heapPrefix),
      TargetTypeMetadataHeaderBase<Runtime>(typePrefix) {}
};
using HeapMetadataHeader =
  TargetHeapMetadataHeader<InProcess>;

/// The common structure of all metadata for heap-allocated types.  A
/// pointer to one of these can be retrieved by loading the 'isa'
/// field of any heap object, whether it was managed by Swift or by
/// Objective-C.  However, when loading from an Objective-C object,
/// this metadata may not have the heap-metadata header, and it may
/// not be the Swift type metadata for the object's dynamic type.
template <typename Runtime>
struct TargetHeapMetadata : TargetMetadata<Runtime> {
  using HeaderType = TargetHeapMetadataHeader<Runtime>;

  TargetHeapMetadata() = default;
  constexpr TargetHeapMetadata(MetadataKind kind)
    : TargetMetadata<Runtime>(kind) {}
  constexpr TargetHeapMetadata(TargetAnyClassMetadataObjCInterop<Runtime> *isa)
    : TargetMetadata<Runtime>(isa) {}
};
using HeapMetadata = TargetHeapMetadata<InProcess>;

/// An opaque descriptor describing a class or protocol method. References to
/// these descriptors appear in the method override table of a class context
/// descriptor, or a resilient witness table pattern, respectively.
///
/// Clients should not assume anything about the contents of this descriptor
/// other than it having 4 byte alignment.
template <typename Runtime>
struct TargetMethodDescriptor {
  /// Flags describing the method.
  MethodDescriptorFlags Flags;

  /// The method implementation.
  union {
    TargetCompactFunctionPointer<Runtime, void> Impl;
    TargetRelativeDirectPointer<Runtime, void> AsyncImpl;
  };

  // TODO: add method types or anything else needed for reflection.

  void *getImpl() const {
    if (Flags.isAsync()) {
      return AsyncImpl.get();
    } else {
      return Impl.get();
    }
  }
};

using MethodDescriptor = TargetMethodDescriptor<InProcess>;

/// Header for a class vtable descriptor. This is a variable-sized
/// structure that describes how to find and parse a vtable
/// within the type metadata for a class.
template <typename Runtime>
struct TargetVTableDescriptorHeader {
  using StoredPointer = typename Runtime::StoredPointer;

private:
  /// The offset of the vtable for this class in its metadata, if any,
  /// in words.
  ///
  /// If this class has a resilient superclass, this offset is relative to the
  /// the start of the immediate class's metadata. Otherwise, it is relative
  /// to the metadata address point.
  uint32_t VTableOffset;

public:
  /// The number of vtable entries. This is the number of MethodDescriptor
  /// records following the vtable header in the class's nominal type
  /// descriptor, which is equal to the number of words this subclass's vtable
  /// entries occupy in instantiated class metadata.
  uint32_t VTableSize;

  uint32_t getVTableOffset(const TargetClassDescriptor<Runtime> *description) const {
    if (description->hasResilientSuperclass()) {
      auto bounds = description->getMetadataBounds();
      return (bounds.ImmediateMembersOffset / sizeof(StoredPointer)
              + VTableOffset);
    }

    return VTableOffset;
  }
};

template<typename Runtime> struct TargetMethodDescriptor;

template<typename Runtime>
using TargetRelativeMethodDescriptorPointer =
  RelativeIndirectablePointer<const TargetMethodDescriptor<Runtime>,
                              /*nullable*/ true>;

using RelativeMethodDescriptorPointer =
  TargetRelativeMethodDescriptorPointer<InProcess>;

template<typename Runtime> struct TargetProtocolRequirement;

template<typename Runtime>
using TargetRelativeProtocolRequirementPointer =
  RelativeIndirectablePointer<const TargetProtocolRequirement<Runtime>,
                              /*nullable*/ true>;

using RelativeProtocolRequirementPointer =
  TargetRelativeProtocolRequirementPointer<InProcess>;

/// An entry in the method override table, referencing a method from one of our
/// ancestor classes, together with an implementation.
template <typename Runtime>
struct TargetMethodOverrideDescriptor {
  /// The class containing the base method.
  TargetRelativeContextPointer<Runtime> Class;

  /// The base method.
  TargetRelativeMethodDescriptorPointer<Runtime> Method;

  /// The implementation of the override.
  union {
    TargetCompactFunctionPointer<Runtime, void, /*nullable*/ true> Impl;
    TargetRelativeDirectPointer<Runtime, void, /*nullable*/ true> AsyncImpl;
  };

  void *getImpl() const {
    auto *baseMethod = Method.get();
    assert(baseMethod && "no base method");
    if (baseMethod->Flags.isAsync()) {
      return AsyncImpl.get();
    } else {
      return Impl.get();
    }
  }
};

/// Header for a class vtable override descriptor. This is a variable-sized
/// structure that provides implementations for overrides of methods defined
/// in superclasses.
template <typename Runtime>
struct TargetOverrideTableHeader {
  /// The number of MethodOverrideDescriptor records following the vtable
  /// override header in the class's nominal type descriptor.
  uint32_t NumEntries;
};

/// The bounds of a class metadata object.
///
/// This type is a currency type and is not part of the ABI.
/// See TargetStoredClassMetadataBounds for the type of the class
/// metadata bounds variable.
template <typename Runtime>
struct TargetClassMetadataBounds : TargetMetadataBounds<Runtime> {
  using StoredPointer = typename Runtime::StoredPointer;
  using StoredSize = typename Runtime::StoredSize;
  using StoredPointerDifference = typename Runtime::StoredPointerDifference;

  using TargetMetadataBounds<Runtime>::NegativeSizeInWords;
  using TargetMetadataBounds<Runtime>::PositiveSizeInWords;
  using TargetClassMetadata = TargetClassMetadataType<Runtime>;

  /// The offset from the address point of the metadata to the immediate
  /// members.
  StoredPointerDifference ImmediateMembersOffset;

  constexpr TargetClassMetadataBounds() = default;
  constexpr TargetClassMetadataBounds(
              StoredPointerDifference immediateMembersOffset,
              uint32_t negativeSizeInWords, uint32_t positiveSizeInWords)
    : TargetMetadataBounds<Runtime>{negativeSizeInWords, positiveSizeInWords},
      ImmediateMembersOffset(immediateMembersOffset) {}

  /// Return the basic bounds of all Swift class metadata.
  /// The immediate members offset will not be meaningful.
  static constexpr TargetClassMetadataBounds<Runtime> forSwiftRootClass() {
    using MetadataTy = FullMetadata<TargetClassMetadataType<Runtime>>;
    return forAddressPointAndSize(sizeof(typename MetadataTy::HeaderType),
                                  sizeof(MetadataTy));
  }

  /// Return the bounds of a Swift class metadata with the given address
  /// point and size (both in bytes).
  /// The immediate members offset will not be meaningful.
  static constexpr TargetClassMetadataBounds<Runtime>
  forAddressPointAndSize(StoredSize addressPoint, StoredSize totalSize) {
    return {
      // Immediate offset in bytes.
      StoredPointerDifference(totalSize - addressPoint),
      // Negative size in words.
      uint32_t(addressPoint / sizeof(StoredPointer)),
      // Positive size in words.
      uint32_t((totalSize - addressPoint) / sizeof(StoredPointer))
    };
  }

  /// Adjust these bounds for a subclass with the given immediate-members
  /// section.
  void adjustForSubclass(bool areImmediateMembersNegative,
                         uint32_t numImmediateMembers) {
    if (areImmediateMembersNegative) {
      NegativeSizeInWords += numImmediateMembers;
      ImmediateMembersOffset =
        -StoredPointerDifference(NegativeSizeInWords) * sizeof(StoredPointer);
    } else {
      ImmediateMembersOffset = PositiveSizeInWords * sizeof(StoredPointer);
      PositiveSizeInWords += numImmediateMembers;
    }
  }
};
using ClassMetadataBounds =
  TargetClassMetadataBounds<InProcess>;

/// The portion of a class metadata object that is compatible with
/// all classes, even non-Swift ones.
template <typename Runtime>
struct TargetAnyClassMetadata : public TargetHeapMetadata<Runtime> {
  using StoredPointer = typename Runtime::StoredPointer;
  using StoredSize = typename Runtime::StoredSize;
  using TargetClassMetadata = TargetClassMetadataType<Runtime>;

protected:
  constexpr TargetAnyClassMetadata(
      TargetAnyClassMetadataObjCInterop<Runtime> *isa,
      TargetClassMetadata *superclass)
      : TargetHeapMetadata<Runtime>(isa), Superclass(superclass) {}
public:
  constexpr TargetAnyClassMetadata(TargetClassMetadata *superclass)
      : TargetHeapMetadata<Runtime>(MetadataKind::Class),
        Superclass(superclass) {}

  // Note that ObjC classes do not have a metadata header.

  /// The metadata for the superclass.  This is null for the root class.
  TargetSignedPointer<Runtime, const TargetClassMetadata *
                                   __ptrauth_swift_objc_superclass>
      Superclass;

  /// Is this object a valid swift type metadata?  That is, can it be
  /// safely downcast to ClassMetadata?
  bool isTypeMetadata() const {
    return true;
  }
  /// A different perspective on the same bit.
  bool isPureObjC() const {
    return !isTypeMetadata();
  }
};

/// This is the class metadata object for all classes (Swift and ObjC) in a
/// runtime that has Objective-C interoperability.
template <typename Runtime>
struct TargetAnyClassMetadataObjCInterop
    : public TargetAnyClassMetadata<Runtime> {
  using StoredPointer = typename Runtime::StoredPointer;
  using StoredSize = typename Runtime::StoredSize;
  using TargetClassMetadataObjCInterop =
    // swift:: qualifier works around an MSVC quirk
    swift::TargetClassMetadata<Runtime, TargetAnyClassMetadataObjCInterop<Runtime>>;

  constexpr TargetAnyClassMetadataObjCInterop(
      TargetAnyClassMetadataObjCInterop<Runtime> *isa,
      TargetClassMetadataObjCInterop *superclass)
      : TargetAnyClassMetadata<Runtime>(isa, superclass),
        CacheData{nullptr, nullptr},
        Data(SWIFT_CLASS_IS_SWIFT_MASK) {}

  constexpr TargetAnyClassMetadataObjCInterop(
      TargetClassMetadataObjCInterop *superclass)
      : TargetAnyClassMetadata<Runtime>(superclass), CacheData{nullptr,
                                                               nullptr},
        Data(SWIFT_CLASS_IS_SWIFT_MASK) {}

  // Allow setting the metadata kind to a class ISA on class metadata.
  using TargetMetadata<Runtime>::getClassISA;
  using TargetMetadata<Runtime>::setClassISA;

  /// The cache data is used for certain dynamic lookups; it is owned
  /// by the runtime and generally needs to interoperate with
  /// Objective-C's use.
  TargetPointer<Runtime, void> CacheData[2];

  /// The data pointer is used for out-of-line metadata and is
  /// generally opaque, except that the compiler sets the low bit in
  /// order to indicate that this is a Swift metatype and therefore
  /// that the type metadata header is present.
  StoredSize Data;
  
  static constexpr StoredPointer offsetToData() {
    return offsetof(TargetAnyClassMetadataObjCInterop, Data);
  }

  /// Is this object a valid swift type metadata?  That is, can it be
  /// safely downcast to ClassMetadata?
  bool isTypeMetadata() const {
    return (Data & SWIFT_CLASS_IS_SWIFT_MASK);
  }
  /// A different perspective on the same bit
  bool isPureObjC() const {
    return !isTypeMetadata();
  }
};

using AnyClassMetadata = TargetAnyClassMetadataType<InProcess>;

using ClassIVarDestroyer =
  SWIFT_CC(swift) void(SWIFT_CONTEXT HeapObject *);

/// The structure of all class metadata.  This structure is embedded
/// directly within the class's heap metadata structure and therefore
/// cannot be extended without an ABI break.
///
/// Note that the layout of this type is compatible with the layout of
/// an Objective-C class.
///
/// If the Runtime supports Objective-C interoperability, this class inherits
/// from TargetAnyClassMetadataObjCInterop, otherwise it inherits from
/// TargetAnyClassMetadata.
template <typename Runtime, typename TargetAnyClassMetadataVariant>
struct TargetClassMetadata : public TargetAnyClassMetadataVariant {
  using StoredPointer = typename Runtime::StoredPointer;
  using StoredSize = typename Runtime::StoredSize;

  TargetClassMetadata() = default;
  constexpr TargetClassMetadata(const TargetAnyClassMetadataVariant &base,
                                ClassFlags flags,
                                ClassIVarDestroyer *ivarDestroyer,
                                StoredPointer size, StoredPointer addressPoint,
                                StoredPointer alignMask,
                                StoredPointer classSize,
                                StoredPointer classAddressPoint)
      : TargetAnyClassMetadataVariant(base), Flags(flags),
        InstanceAddressPoint(addressPoint), InstanceSize(size),
        InstanceAlignMask(alignMask), Reserved(0), ClassSize(classSize),
        ClassAddressPoint(classAddressPoint), Description(nullptr),
        IVarDestroyer(ivarDestroyer) {}

  // The remaining fields are valid only when isTypeMetadata().
  // The Objective-C runtime knows the offsets to some of these fields.
  // Be careful when accessing them.

  /// Swift-specific class flags.
  ClassFlags Flags;

  /// The address point of instances of this type.
  uint32_t InstanceAddressPoint;

  /// The required size of instances of this type.
  /// 'InstanceAddressPoint' bytes go before the address point;
  /// 'InstanceSize - InstanceAddressPoint' bytes go after it.
  uint32_t InstanceSize;

  /// The alignment mask of the address point of instances of this type.
  uint16_t InstanceAlignMask;

  /// Reserved for runtime use.
  uint16_t Reserved;

  /// The total size of the class object, including prefix and suffix
  /// extents.
  uint32_t ClassSize;

  /// The offset of the address point within the class object.
  uint32_t ClassAddressPoint;

  // Description is by far the most likely field for a client to try
  // to access directly, so we force access to go through accessors.
private:
  /// An out-of-line Swift-specific description of the type, or null
  /// if this is an artificial subclass.  We currently provide no
  /// supported mechanism for making a non-artificial subclass
  /// dynamically.
  TargetSignedPointer<Runtime, const TargetClassDescriptor<Runtime> * __ptrauth_swift_type_descriptor> Description;

public:
  /// A function for destroying instance variables, used to clean up after an
  /// early return from a constructor. If null, no clean up will be performed
  /// and all ivars must be trivial.
  TargetSignedPointer<Runtime, ClassIVarDestroyer * __ptrauth_swift_heap_object_destructor> IVarDestroyer;

  // After this come the class members, laid out as follows:
  //   - class members for the superclass (recursively)
  //   - metadata reference for the parent, if applicable
  //   - generic parameters for this class
  //   - class variables (if we choose to support these)
  //   - "tabulated" virtual methods

  using TargetAnyClassMetadataVariant::isTypeMetadata;

  ConstTargetMetadataPointer<Runtime, TargetClassDescriptor>
  getDescription() const {
    assert(isTypeMetadata());
    return Description;
  }

  typename Runtime::StoredSignedPointer
  getDescriptionAsSignedPointer() const {
    assert(isTypeMetadata());
    return Description;
  }

  void setDescription(const TargetClassDescriptor<Runtime> *description) {
    Description = description;
  }

  // [NOTE: Dynamic-subclass-KVO]
  //
  // Using Objective-C runtime, KVO can modify object behavior without needing
  // to modify the object's code. This is done by dynamically creating an
  // artificial subclass of the object's type.
  //
  // The isa pointer of the observed object is swapped out to point to
  // the artificial subclass, which has the following properties:
  // - Setters for observed keys are overridden to additionally post
  // notifications.
  // - The `-class` method is overridden to return the original class type
  // instead of the artificial subclass type.
  //
  // For more details, see:
  // https://www.mikeash.com/pyblog/friday-qa-2009-01-23.html

  /// Is this class an artificial subclass, such as one dynamically
  /// created for various dynamic purposes like KVO?
  /// See [NOTE: Dynamic-subclass-KVO]
  bool isArtificialSubclass() const {
    assert(isTypeMetadata());
    return Description == nullptr;
  }
  void setArtificialSubclass() {
    assert(isTypeMetadata());
    Description = nullptr;
  }

  ClassFlags getFlags() const {
    assert(isTypeMetadata());
    return Flags;
  }
  void setFlags(ClassFlags flags) {
    assert(isTypeMetadata());
    Flags = flags;
  }

  StoredSize getInstanceSize() const {
    assert(isTypeMetadata());
    return InstanceSize;
  }
  void setInstanceSize(StoredSize size) {
    assert(isTypeMetadata());
    InstanceSize = size;
  }

  StoredPointer getInstanceAddressPoint() const {
    assert(isTypeMetadata());
    return InstanceAddressPoint;
  }
  void setInstanceAddressPoint(StoredSize size) {
    assert(isTypeMetadata());
    InstanceAddressPoint = size;
  }

  StoredPointer getInstanceAlignMask() const {
    assert(isTypeMetadata());
    return InstanceAlignMask;
  }
  void setInstanceAlignMask(StoredSize mask) {
    assert(isTypeMetadata());
    InstanceAlignMask = mask;
  }

  StoredPointer getClassSize() const {
    assert(isTypeMetadata());
    return ClassSize;
  }
  void setClassSize(StoredSize size) {
    assert(isTypeMetadata());
    ClassSize = size;
  }

  StoredPointer getClassAddressPoint() const {
    assert(isTypeMetadata());
    return ClassAddressPoint;
  }
  void setClassAddressPoint(StoredSize offset) {
    assert(isTypeMetadata());
    ClassAddressPoint = offset;
  }

  uint16_t getRuntimeReservedData() const {
    assert(isTypeMetadata());
    return Reserved;
  }
  void setRuntimeReservedData(uint16_t data) {
    assert(isTypeMetadata());
    Reserved = data;
  }

  /// Get a pointer to the field offset vector, if present, or null.
  const StoredPointer *getFieldOffsets() const {
    assert(isTypeMetadata());
    auto offset = getDescription()->getFieldOffsetVectorOffset();
    if (offset == 0)
      return nullptr;
    auto asWords = reinterpret_cast<const void * const*>(this);
    return reinterpret_cast<const StoredPointer *>(asWords + offset);
  }

  uint32_t getSizeInWords() const {
    assert(isTypeMetadata());
    uint32_t size = getClassSize() - getClassAddressPoint();
    assert(size % sizeof(StoredPointer) == 0);
    return size / sizeof(StoredPointer);
  }

  /// Given that this class is serving as the superclass of a Swift class,
  /// return its bounds as metadata.
  ///
  /// Note that the ImmediateMembersOffset member will not be meaningful.
  TargetClassMetadataBounds<Runtime>
  getClassBoundsAsSwiftSuperclass() const {
    using Bounds = TargetClassMetadataBounds<Runtime>;

    auto rootBounds = Bounds::forSwiftRootClass();

    // If the class is not type metadata, just use the root-class bounds.
    if (!isTypeMetadata())
      return rootBounds;

    // Otherwise, pull out the bounds from the metadata.
    auto bounds = Bounds::forAddressPointAndSize(getClassAddressPoint(),
                                                 getClassSize());

    // Round the bounds up to the required dimensions.
    if (bounds.NegativeSizeInWords < rootBounds.NegativeSizeInWords)
      bounds.NegativeSizeInWords = rootBounds.NegativeSizeInWords;
    if (bounds.PositiveSizeInWords < rootBounds.PositiveSizeInWords)
      bounds.PositiveSizeInWords = rootBounds.PositiveSizeInWords;

    return bounds;
  }

#if SWIFT_OBJC_INTEROP
  /// Given a statically-emitted metadata template, this sets the correct
  /// "is Swift" bit for the current runtime. Depending on the deployment
  /// target a binary was compiled for, statically emitted metadata templates
  /// may have a different bit set from the one that this runtime canonically
  /// considers the "is Swift" bit.
  void setAsTypeMetadata() {
    // If the wrong "is Swift" bit is set, set the correct one.
    //
    // Note that the only time we should see the "new" bit set while
    // expecting the "old" one is when running a binary built for a
    // new OS on an old OS, which is not supported, however we do
    // have tests that exercise this scenario.
    auto otherSwiftBit = (3ULL - SWIFT_CLASS_IS_SWIFT_MASK);
    assert(otherSwiftBit == 1ULL || otherSwiftBit == 2ULL);

    if ((this->Data & 3) == otherSwiftBit) {
      this->Data ^= 3;
    }

    // Otherwise there should be nothing to do, since only the old "is
    // Swift" bit is used for backward-deployed runtimes.
    
    assert(isTypeMetadata());
  }
#endif

  bool isStaticallySpecializedGenericMetadata() const {
    auto *description = getDescription();
    if (!description->isGeneric())
      return false;

    return this->Flags & ClassFlags::IsStaticSpecialization;
  }

  bool isCanonicalStaticallySpecializedGenericMetadata() const {
    auto *description = getDescription();
    if (!description->isGeneric())
      return false;

    return this->Flags & ClassFlags::IsCanonicalStaticSpecialization;
  }

  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::Class;
  }
};
using ClassMetadata = TargetClassMetadataType<InProcess>;

/// The structure of class metadata that's compatible with dispatch objects.
/// This includes Swift heap metadata, followed by the vtable entries that
/// dispatch expects to see, with padding to place them at the expected offsets.
template <typename Runtime>
struct TargetDispatchClassMetadata : public TargetHeapMetadata<Runtime> {
  using InvokeCall = void (*)(void *, void *, uint32_t);

  TargetDispatchClassMetadata(MetadataKind Kind, unsigned long VTableType,
                              InvokeCall Invoke)
      : TargetHeapMetadata<Runtime>(Kind), VTableType(VTableType),
        VTableInvoke(Invoke) {}

  TargetPointer<Runtime, void> Opaque;
#if SWIFT_OBJC_INTEROP
  TargetPointer<Runtime, void> OpaqueObjC[3];
#endif

  unsigned long VTableType;
  TargetSignedPointer<Runtime, InvokeCall __ptrauth_swift_dispatch_invoke_function> VTableInvoke;
};
using DispatchClassMetadata = TargetDispatchClassMetadata<InProcess>;

/// The structure of metadata for heap-allocated local variables.
/// This is non-type metadata.
template <typename Runtime>
struct TargetHeapLocalVariableMetadata
  : public TargetHeapMetadata<Runtime> {
  using StoredPointer = typename Runtime::StoredPointer;
  uint32_t OffsetToFirstCapture;
  TargetPointer<Runtime, const char> CaptureDescription;

  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::HeapLocalVariable;
  }
  constexpr TargetHeapLocalVariableMetadata()
      : TargetHeapMetadata<Runtime>(MetadataKind::HeapLocalVariable),
        OffsetToFirstCapture(0), CaptureDescription(nullptr) {}
};
using HeapLocalVariableMetadata
  = TargetHeapLocalVariableMetadata<InProcess>;

/// The structure of wrapper metadata for Objective-C classes.  This
/// is used as a type metadata pointer when the actual class isn't
/// Swift-compiled.
template <typename Runtime>
struct TargetObjCClassWrapperMetadata : public TargetMetadata<Runtime> {
  ConstTargetMetadataPointer<Runtime, TargetClassMetadataObjCInterop> Class;

  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::ObjCClassWrapper;
  }
};
using ObjCClassWrapperMetadata
  = TargetObjCClassWrapperMetadata<InProcess>;

/// The structure of metadata for foreign types where the source
/// language doesn't provide any sort of more interesting metadata for
/// us to use.
template <typename Runtime>
struct TargetForeignTypeMetadata : public TargetMetadata<Runtime> {
};
using ForeignTypeMetadata = TargetForeignTypeMetadata<InProcess>;

/// The structure of metadata objects for foreign class types.
/// A foreign class is a foreign type with reference semantics and
/// Swift-supported reference counting.  Generally this requires
/// special logic in the importer.
///
/// We assume for now that foreign classes are entirely opaque
/// to Swift introspection.
template <typename Runtime>
struct TargetForeignClassMetadata : public TargetForeignTypeMetadata<Runtime> {
  using StoredPointer = typename Runtime::StoredPointer;

  /// An out-of-line description of the type.
  TargetSignedPointer<Runtime, const TargetClassDescriptor<Runtime> * __ptrauth_swift_type_descriptor> Description;

  /// The superclass of the foreign class, if any.
  ConstTargetMetadataPointer<Runtime, swift::TargetForeignClassMetadata>
    Superclass;

  /// Reserved space.  For now, this should be zero-initialized.
  /// If this is used for anything in the future, at least some of these
  /// first bits should be flags.
  StoredPointer Reserved[1];

  ConstTargetMetadataPointer<Runtime, TargetClassDescriptor>
  getDescription() const {
    return Description;
  }

  typename Runtime::StoredSignedPointer
  getDescriptionAsSignedPointer() const {
    return Description;
  }

  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::ForeignClass;
  }
};
using ForeignClassMetadata = TargetForeignClassMetadata<InProcess>;

/// The structure of metadata objects for foreign reference types.
/// A foreign reference type is a non-Swift, non-Objective-C foreign type with
/// reference semantics. Foreign reference types are pointers/reference to
/// value types marked with the "import_as_ref" attribute.
///
/// Foreign reference types may have *custom* reference counting operations, or
/// they may be immortal (and therefore trivial).
///
/// We assume for now that foreign reference types are entirely opaque
/// to Swift introspection.
template <typename Runtime>
struct TargetForeignReferenceTypeMetadata : public TargetForeignTypeMetadata<Runtime> {
  using StoredPointer = typename Runtime::StoredPointer;

  /// An out-of-line description of the type.
  TargetSignedPointer<Runtime, const TargetClassDescriptor<Runtime> * __ptrauth_swift_type_descriptor> Description;

  /// Reserved space.  For now, this should be zero-initialized.
  /// If this is used for anything in the future, at least some of these
  /// first bits should be flags.
  StoredPointer Reserved[1];

  ConstTargetMetadataPointer<Runtime, TargetClassDescriptor>
  getDescription() const {
    return Description;
  }

  typename Runtime::StoredSignedPointer
  getDescriptionAsSignedPointer() const {
    return Description;
  }

  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::ForeignReferenceType;
  }
};
using ForeignReferenceTypeMetadata = TargetForeignReferenceTypeMetadata<InProcess>;

/// The common structure of metadata for structs and enums.
template <typename Runtime>
struct TargetValueMetadata : public TargetMetadata<Runtime> {
  using StoredPointer = typename Runtime::StoredPointer;
  TargetValueMetadata(MetadataKind Kind,
                      const TargetTypeContextDescriptor<Runtime> *description)
      : TargetMetadata<Runtime>(Kind), Description(description) {}

  /// An out-of-line description of the type.
  TargetSignedPointer<Runtime, const TargetValueTypeDescriptor<Runtime> * __ptrauth_swift_type_descriptor> Description;

  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::Struct
      || metadata->getKind() == MetadataKind::Enum
      || metadata->getKind() == MetadataKind::Optional;
  }

  ConstTargetMetadataPointer<Runtime, TargetValueTypeDescriptor>
  getDescription() const {
    return Description;
  }

  typename Runtime::StoredSignedPointer
  getDescriptionAsSignedPointer() const {
    return Description;
  }
};
using ValueMetadata = TargetValueMetadata<InProcess>;

/// The structure of type metadata for structs.
template <typename Runtime>
struct TargetStructMetadata : public TargetValueMetadata<Runtime> {
  using StoredPointer = typename Runtime::StoredPointer;
  using TargetValueMetadata<Runtime>::TargetValueMetadata;

  const TargetStructDescriptor<Runtime> *getDescription() const {
    return llvm::cast<TargetStructDescriptor<Runtime>>(this->Description);
  }

  // The first trailing field of struct metadata is always the generic
  // argument array.

  /// Get a pointer to the field offset vector, if present, or null.
  const uint32_t *getFieldOffsets() const {
    auto offset = getDescription()->FieldOffsetVectorOffset;
    if (offset == 0)
      return nullptr;
    auto asWords = reinterpret_cast<const void * const*>(this);
    return reinterpret_cast<const uint32_t *>(asWords + offset);
  }

  bool isStaticallySpecializedGenericMetadata() const {
    auto *description = getDescription();
    if (!description->isGeneric())
      return false;

    auto *trailingFlags = getTrailingFlags();
    if (trailingFlags == nullptr)
      return false;

    return trailingFlags->isStaticSpecialization();
  }

  bool isCanonicalStaticallySpecializedGenericMetadata() const {
    auto *description = getDescription();
    if (!description->isGeneric())
      return false;

    auto *trailingFlags = getTrailingFlags();
    if (trailingFlags == nullptr)
      return false;

    return trailingFlags->isCanonicalStaticSpecialization();
  }

  const MetadataTrailingFlags *getTrailingFlags() const {
    auto description = getDescription();
    auto flags = description->getFullGenericContextHeader()
                     .DefaultInstantiationPattern->PatternFlags;
    if (!flags.hasTrailingFlags())
      return nullptr;
    auto fieldOffset = description->FieldOffsetVectorOffset;
    auto offset =
        fieldOffset +
        // Pad to the nearest pointer.
        ((description->NumFields * sizeof(uint32_t) + sizeof(void *) - 1) /
         sizeof(void *));
    auto asWords = reinterpret_cast<const void *const *>(this);
    return reinterpret_cast<const MetadataTrailingFlags *>(asWords + offset);
  }

  static constexpr int32_t getGenericArgumentOffset() {
    return sizeof(TargetStructMetadata<Runtime>) / sizeof(StoredPointer);
  }

  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::Struct;
  }
};
using StructMetadata = TargetStructMetadata<InProcess>;

/// The structure of type metadata for enums.
template <typename Runtime>
struct TargetEnumMetadata : public TargetValueMetadata<Runtime> {
  using StoredPointer = typename Runtime::StoredPointer;
  using StoredSize = typename Runtime::StoredSize;
  using TargetValueMetadata<Runtime>::TargetValueMetadata;

  const TargetEnumDescriptor<Runtime> *getDescription() const {
    return llvm::cast<TargetEnumDescriptor<Runtime>>(this->Description);
  }

  // The first trailing field of enum metadata is always the generic
  // argument array.

  /// True if the metadata records the size of the payload area.
  bool hasPayloadSize() const {
    return getDescription()->hasPayloadSizeOffset();
  }

  /// Retrieve the size of the payload area.
  ///
  /// `hasPayloadSize` must be true for this to be valid.
  StoredSize getPayloadSize() const {
    assert(hasPayloadSize());
    auto offset = getDescription()->getPayloadSizeOffset();
    const StoredSize *asWords = reinterpret_cast<const StoredSize *>(this);
    asWords += offset;
    return *asWords;
  }

  StoredSize &getPayloadSize() {
    assert(hasPayloadSize());
    auto offset = getDescription()->getPayloadSizeOffset();
    StoredSize *asWords = reinterpret_cast<StoredSize *>(this);
    asWords += offset;
    return *asWords;
  }

  bool isStaticallySpecializedGenericMetadata() const {
    auto *description = getDescription();
    if (!description->isGeneric())
      return false;

    auto *trailingFlags = getTrailingFlags();
    if (trailingFlags == nullptr)
      return false;

    return trailingFlags->isStaticSpecialization();
  }

  bool isCanonicalStaticallySpecializedGenericMetadata() const {
    auto *description = getDescription();
    if (!description->isGeneric())
      return false;

    auto *trailingFlags = getTrailingFlags();
    if (trailingFlags == nullptr)
      return false;

    return trailingFlags->isCanonicalStaticSpecialization();
  }

  const MetadataTrailingFlags *getTrailingFlags() const {
    auto description = getDescription();
    auto flags = description->getFullGenericContextHeader()
                     .DefaultInstantiationPattern->PatternFlags;
    if (!flags.hasTrailingFlags())
      return nullptr;
    auto offset =
        getGenericArgumentOffset() +
        description->getFullGenericContextHeader().Base.getNumArguments() +
        (hasPayloadSize() ? 1 : 0);
    auto asWords = reinterpret_cast<const void *const *>(this);
    return reinterpret_cast<const MetadataTrailingFlags *>(asWords + offset);
  }

  static constexpr int32_t getGenericArgumentOffset() {
    return sizeof(TargetEnumMetadata<Runtime>) / sizeof(StoredPointer);
  }

  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::Enum
      || metadata->getKind() == MetadataKind::Optional;
  }
};
using EnumMetadata = TargetEnumMetadata<InProcess>;

/// The structure of function type metadata.
template <typename Runtime>
struct TargetFunctionTypeMetadata : public TargetMetadata<Runtime> {
  using StoredSize = typename Runtime::StoredSize;
  using Parameter = ConstTargetMetadataPointer<Runtime, swift::TargetMetadata>;

  TargetFunctionTypeFlags<StoredSize> Flags;

  /// The type metadata for the result type.
  ConstTargetMetadataPointer<Runtime, swift::TargetMetadata> ResultType;

  Parameter *getParameters() { return reinterpret_cast<Parameter *>(this + 1); }

  const Parameter *getParameters() const {
    return reinterpret_cast<const Parameter *>(this + 1);
  }

  Parameter getParameter(unsigned index) const {
    assert(index < getNumParameters());
    return getParameters()[index];
  }

  ParameterFlags getParameterFlags(unsigned index) const {
    assert(index < getNumParameters());
    auto flags = hasParameterFlags() ? getParameterFlags()[index] : 0;
    return ParameterFlags::fromIntValue(flags);
  }

  StoredSize getNumParameters() const {
    return Flags.getNumParameters();
  }
  FunctionMetadataConvention getConvention() const {
    return Flags.getConvention();
  }
  bool isAsync() const { return Flags.isAsync(); }
  bool isThrowing() const { return Flags.isThrowing(); }
  bool isSendable() const { return Flags.isSendable(); }
  bool isDifferentiable() const { return Flags.isDifferentiable(); }
  bool hasParameterFlags() const { return Flags.hasParameterFlags(); }
  bool isEscaping() const { return Flags.isEscaping(); }
  bool hasGlobalActor() const { return Flags.hasGlobalActor(); }

  static constexpr StoredSize OffsetToFlags = sizeof(TargetMetadata<Runtime>);

  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::Function;
  }

  uint32_t *getParameterFlags() {
    return reinterpret_cast<uint32_t *>(getParameters() + getNumParameters());
  }

  const uint32_t *getParameterFlags() const {
    return reinterpret_cast<const uint32_t *>(getParameters() +
                                              getNumParameters());
  }

  TargetFunctionMetadataDifferentiabilityKind<StoredSize> *
  getDifferentiabilityKindAddress() {
    assert(isDifferentiable());
    void *previousEndAddr = hasParameterFlags()
        ? reinterpret_cast<void *>(getParameterFlags() + getNumParameters())
        : reinterpret_cast<void *>(getParameters() + getNumParameters());
    return reinterpret_cast<
        TargetFunctionMetadataDifferentiabilityKind<StoredSize> *>(
        llvm::alignAddr(previousEndAddr,
                        llvm::Align(alignof(typename Runtime::StoredPointer))));
  }

  TargetFunctionMetadataDifferentiabilityKind<StoredSize>
  getDifferentiabilityKind() const {
    if (isDifferentiable()) {
      return *const_cast<TargetFunctionTypeMetadata<Runtime> *>(this)
          ->getDifferentiabilityKindAddress();
    }
    return TargetFunctionMetadataDifferentiabilityKind<StoredSize>
        ::NonDifferentiable;
  }

  ConstTargetMetadataPointer<Runtime, swift::TargetMetadata> *
  getGlobalActorAddr() {
    assert(hasGlobalActor());
    
    void *endAddr =
        isDifferentiable()
          ? reinterpret_cast<void *>(getDifferentiabilityKindAddress() + 1) :
        hasParameterFlags()
          ? reinterpret_cast<void *>(getParameterFlags() + getNumParameters()) :
        reinterpret_cast<void *>(getParameters() + getNumParameters());
    return reinterpret_cast<
        ConstTargetMetadataPointer<Runtime, swift::TargetMetadata> *>(
          llvm::alignAddr(
              endAddr, llvm::Align(alignof(typename Runtime::StoredPointer))));
  }

  ConstTargetMetadataPointer<Runtime, swift::TargetMetadata>
  getGlobalActor() const {
    if (!hasGlobalActor())
      return ConstTargetMetadataPointer<Runtime, swift::TargetMetadata>();

    return *const_cast<TargetFunctionTypeMetadata<Runtime> *>(this)
      ->getGlobalActorAddr();
  }
};
using FunctionTypeMetadata = TargetFunctionTypeMetadata<InProcess>;

/// The structure of metadata for metatypes.
template <typename Runtime>
struct TargetMetatypeMetadata : public TargetMetadata<Runtime> {
  /// The type metadata for the element.
  ConstTargetMetadataPointer<Runtime, swift::TargetMetadata> InstanceType;

  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::Metatype;
  }
};
using MetatypeMetadata = TargetMetatypeMetadata<InProcess>;

/// The structure of tuple type metadata.
template <typename Runtime>
struct TargetTupleTypeMetadata : public TargetMetadata<Runtime> {
  using StoredSize = typename Runtime::StoredSize;
  using HeaderType = TargetTypeMetadataHeaderBase<Runtime>;
  TargetTupleTypeMetadata() = default;
  constexpr TargetTupleTypeMetadata(const TargetMetadata<Runtime> &base,
                                    uint32_t numElements,
                                    TargetPointer<Runtime, const char> labels)
    : TargetMetadata<Runtime>(base),
      NumElements(numElements),
      Labels(labels) {}

  /// The number of elements.
  StoredSize NumElements;

  /// The labels string;  see swift_getTupleTypeMetadata.
  TargetPointer<Runtime, const char> Labels;

  struct Element {
    /// The type of the element.
    ConstTargetMetadataPointer<Runtime, swift::TargetMetadata> Type;

    /// The offset of the tuple element within the tuple.
#if __APPLE__
    StoredSize Offset;
#else
    uint32_t Offset;
#endif

    OpaqueValue *findIn(OpaqueValue *tuple) const {
      return (OpaqueValue*) (((char*) tuple) + Offset);
    }

    const TypeLayout *getTypeLayout() const {
      return Type->getTypeLayout();
    }
  };

  static_assert(sizeof(Element) == sizeof(StoredSize) * 2,
                "element size should be two words");

  Element *getElements() {
    return reinterpret_cast<Element*>(this + 1);
  }

  const Element *getElements() const {
    return reinterpret_cast<const Element*>(this + 1);
  }

  const Element &getElement(unsigned i) const {
    return getElements()[i];
  }

  Element &getElement(unsigned i) {
    return getElements()[i];
  }

  static constexpr StoredSize getOffsetToNumElements();
  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::Tuple;
  }
};
using TupleTypeMetadata = TargetTupleTypeMetadata<InProcess>;
  
template <typename Runtime>
constexpr inline auto
TargetTupleTypeMetadata<Runtime>::getOffsetToNumElements() -> StoredSize {
  return offsetof(TargetTupleTypeMetadata<Runtime>, NumElements);
}

template <typename Runtime>
struct swift_ptrauth_struct_context_descriptor(ProtocolDescriptor)
    TargetProtocolDescriptor;

/// A protocol requirement descriptor. This describes a single protocol
/// requirement in a protocol descriptor. The index of the requirement in
/// the descriptor determines the offset of the witness in a witness table
/// for this protocol.
template <typename Runtime>
struct TargetProtocolRequirement {
  ProtocolRequirementFlags Flags;
  // TODO: name, type

  /// The optional default implementation of the protocol.
  union {
    TargetCompactFunctionPointer<Runtime, void, /*nullable*/ true> DefaultFuncImplementation;
    TargetRelativeDirectPointer<Runtime, void, /*nullable*/ true> DefaultImplementation;
  };

  void *getDefaultImplementation() const {
    if (Flags.isFunctionImpl()) {
      return DefaultFuncImplementation.get();
    } else {
      return DefaultImplementation.get();
    }
  }
};

using ProtocolRequirement = TargetProtocolRequirement<InProcess>;

template <typename Runtime>
struct swift_ptrauth_struct_context_descriptor(ProtocolDescriptor)
    TargetProtocolDescriptor;
using ProtocolDescriptor = TargetProtocolDescriptor<InProcess>;

template<template <typename Runtime> class ObjCInteropKind, unsigned PointerSize>
using ExternalProtocolDescriptor = TargetProtocolDescriptor<External<ObjCInteropKind<RuntimeTarget<PointerSize>>>>;

/// A witness table for a protocol.
///
/// With the exception of the initial protocol conformance descriptor,
/// the layout of a witness table is dependent on the protocol being
/// represented.
template <typename Runtime>
class TargetWitnessTable {
  /// The protocol conformance descriptor from which this witness table
  /// was generated.
  ConstTargetMetadataPointer<Runtime, TargetProtocolConformanceDescriptor>
    Description;

public:
  const TargetProtocolConformanceDescriptor<Runtime> *getDescription() const {
    return Description;
  }
};

using WitnessTable = TargetWitnessTable<InProcess>;

template <typename Runtime>
using TargetWitnessTablePointer =
  ConstTargetMetadataPointer<Runtime, TargetWitnessTable>;

using WitnessTablePointer = TargetWitnessTablePointer<InProcess>;

using AssociatedWitnessTableAccessFunction =
  SWIFT_CC(swift) WitnessTable *(const Metadata *associatedType,
                                 const Metadata *self,
                                 const WitnessTable *selfConformance);

template<typename Runtime>
using TargetRelativeProtocolConformanceDescriptorPointer =
  RelativeIndirectablePointer<const TargetProtocolConformanceDescriptor<Runtime>,
                              /*nullable*/ false>;

/// A relative witness table for a protocol.
///
/// With the exception of the initial protocol conformance descriptor,
/// the layout of a witness table is dependent on the protocol being
/// represented.
/// Entries are relative pointers.
template <typename Runtime>
class TargetRelativeWitnessTable {
  /// The protocol conformance descriptor from which this witness table
  /// was generated.
  TargetRelativeProtocolConformanceDescriptorPointer<Runtime> Description;

public:
  const TargetProtocolConformanceDescriptor<Runtime> *getDescription() const {
    return Description;
  }
};
using RelativeWitnessTable = TargetRelativeWitnessTable<InProcess>;

using AssociatedRelativeWitnessTableAccessFunction =
  SWIFT_CC(swift) RelativeWitnessTable *(const Metadata *associatedType,
                                 const Metadata *self,
                                 const RelativeWitnessTable *selfConformance);

/// The possible physical representations of existential types.
enum class ExistentialTypeRepresentation {
  /// The type uses an opaque existential representation.
  Opaque,
  /// The type uses a class existential representation.
  Class,
  /// The type uses the Error boxed existential representation.
  Error,
};

/// The structure of type metadata for simple existential types which
/// don't require an extended existential descriptor:
///
/// - They are existential over a single type T.
/// - Their head type is that type T.
/// - Their existential constraints are a composition of protocol
///   requirements, superclass constraints, and possibly a class
///   layout constraint on T.
template <typename Runtime>
struct TargetExistentialTypeMetadata
  : TargetMetadata<Runtime>,
    swift::ABI::TrailingObjects<
      TargetExistentialTypeMetadata<Runtime>,
      ConstTargetMetadataPointer<Runtime, TargetMetadata>,
      TargetProtocolDescriptorRef<Runtime>> {
  using HeaderType = TargetTypeMetadataHeaderBase<Runtime>;

private:
  using ProtocolDescriptorRef = TargetProtocolDescriptorRef<Runtime>;
  using MetadataPointer =
      ConstTargetMetadataPointer<Runtime, swift::TargetMetadata>;
  using TrailingObjects =
          swift::ABI::TrailingObjects<
          TargetExistentialTypeMetadata<Runtime>,
          MetadataPointer,
          ProtocolDescriptorRef>;
  friend TrailingObjects;

  template<typename T>
  using OverloadToken = typename TrailingObjects::template OverloadToken<T>;

  size_t numTrailingObjects(OverloadToken<ProtocolDescriptorRef>) const {
    return NumProtocols;
  }

  size_t numTrailingObjects(OverloadToken<MetadataPointer>) const {
    return Flags.hasSuperclassConstraint() ? 1 : 0;
  }

public:
  using StoredPointer = typename Runtime::StoredPointer;
  /// The number of witness tables and class-constrained-ness of the type.
  ExistentialTypeFlags Flags;

  /// The number of protocols.
  uint32_t NumProtocols;

  constexpr TargetExistentialTypeMetadata()
    : TargetMetadata<Runtime>(MetadataKind::Existential),
      Flags(ExistentialTypeFlags()), NumProtocols(0) {}
  
  explicit constexpr TargetExistentialTypeMetadata(ExistentialTypeFlags Flags)
    : TargetMetadata<Runtime>(MetadataKind::Existential),
      Flags(Flags), NumProtocols(0) {}

  /// Get the representation form this existential type uses.
  ExistentialTypeRepresentation getRepresentation() const;

  /// True if it's valid to take ownership of the value in the existential
  /// container if we own the container.
  bool mayTakeValue(const OpaqueValue *container) const;
  
  /// Clean up an existential container whose value is uninitialized.
  void deinitExistentialContainer(OpaqueValue *container) const;
  
  /// Project the value pointer from an existential container of the type
  /// described by this metadata.
  const OpaqueValue *projectValue(const OpaqueValue *container) const;
  
  OpaqueValue *projectValue(OpaqueValue *container) const {
    return const_cast<OpaqueValue *>(projectValue((const OpaqueValue*)container));
  }

  /// Get the dynamic type from an existential container of the type described
  /// by this metadata.
  const TargetMetadata<Runtime> *
  getDynamicType(const OpaqueValue *container) const;
  
  /// Get a witness table from an existential container of the type described
  /// by this metadata.
  const TargetWitnessTable<Runtime> * getWitnessTable(
                                                  const OpaqueValue *container,
                                                  unsigned i) const;

  /// Return true iff all the protocol constraints are @objc.
  bool isObjC() const {
    return isClassBounded() && Flags.getNumWitnessTables() == 0;
  }

  bool isClassBounded() const {
    return Flags.getClassConstraint() == ProtocolClassConstraint::Class;
  }

  /// Retrieve the set of protocols required by the existential.
  llvm::ArrayRef<ProtocolDescriptorRef> getProtocols() const {
    return { this->template getTrailingObjects<ProtocolDescriptorRef>(),
             NumProtocols };
  }

  MetadataPointer getSuperclassConstraint() const {
    if (!Flags.hasSuperclassConstraint())
      return MetadataPointer();

    return this->template getTrailingObjects<MetadataPointer>()[0];
  }

  /// Retrieve the set of protocols required by the existential.
  llvm::MutableArrayRef<ProtocolDescriptorRef> getMutableProtocols() {
    return { this->template getTrailingObjects<ProtocolDescriptorRef>(),
             NumProtocols };
  }

  /// Set the superclass.
  void setSuperclassConstraint(MetadataPointer superclass) {
    assert(Flags.hasSuperclassConstraint());
    assert(superclass != nullptr);
    this->template getTrailingObjects<MetadataPointer>()[0] = superclass;
  }

  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::Existential;
  }
};
using ExistentialTypeMetadata
  = TargetExistentialTypeMetadata<InProcess>;

template<typename Runtime>
struct TargetExistentialTypeExpression {
  /// The type expression.
  TargetRelativeDirectPointer<Runtime, const char, /*nullable*/ false> name;
};

/// A description of the shape of an existential type.
///
/// An existential type has the general form:
///   \exists <signature> . <type>
///
/// The signature is called the requirement signature.  In the most
/// common case (the only one Swift currently supports), it has exactly
/// one type parameter.  The types and conformances required by this
/// signature must be stored in the existential container at runtime.
///
/// The type is called the type (sub)expression, and it is expressed
/// in terms of the type parameters introduced by the requirement
/// signature.  It is required to name each of the type parameters in
/// an identity position (outside of the base type of a member type
/// expression).  In the most common case, it is the unique type
/// parameter type.  Swift currently only supports existential
/// subexpressions that are either the (unique) type parameter or
/// a metatype thereof (possibly multiply-derived).
///
/// In order to efficiently support generic substitution of the
/// constraints of existential types, extended existential type
/// descriptors represent a generalizable form of the existential:
///   \forall <gen sig> . \exists <req sig> . <type>
///
/// The new signature is called the generalization signature of the
/// shape.  All concrete references to types within the requirement
/// signature and head type which are not expressed in terms of
/// the requirement signature are replaced with type parameters freshly
/// added to the generalization signature.  Any given existential type
/// is then a specialization of this generalization.
///
/// For example, the existential type
///   \exists <T: Collection where T.Element == Int> . T
/// is generalized as
///   \forall <U> . \exists <T: Collection where T.Element == U> . T
/// and is the application of this generalization at <Int>.
///
/// The soundness condition on this is that the generalization
/// signatures, requirement signatures, and head types must be
/// identical for any two existential types for which a generic
/// substitution can carry one to the other.
///
/// Only particularly complex existential types are represented with
/// an explicit existential shape.  Types which are a simple composition
/// of layout, superclass, and unconstrained protocol constraints on
/// an opaque or (metatype-of)+-opaque are represented with
/// ExistentialTypeMetadata or ExistentialMetatypeMetadata.
/// Types which *can* be represented with those classes *must*
/// be represented with them.
template <typename Runtime>
struct TargetExtendedExistentialTypeShape
  : swift::ABI::TrailingObjects<
      TargetExtendedExistentialTypeShape<Runtime>,
      // Optional generalization signature header
      TargetGenericContextDescriptorHeader<Runtime>,
      // Optional type subexpression
      TargetExistentialTypeExpression<Runtime>,
      // Optional suggested value witnesses
      TargetRelativeIndirectablePointer<Runtime, const TargetValueWitnessTable<Runtime>,
                                        /*nullable*/ false>,
      // Parameters for requirement signature, followed by parameters
      // for generalization signature
      GenericParamDescriptor,
      // Requirements for requirement signature, followed by requirements
      // for generalization signature
      TargetGenericRequirementDescriptor<Runtime>,
      // Optional header describing any type packs in the generalization
      // signature.
      GenericPackShapeHeader,
      // For each type pack in the generalization signature, a descriptor
      // storing the shape class.
      GenericPackShapeDescriptor> {
private:
  using RelativeValueWitnessTablePointer =
    TargetRelativeIndirectablePointer<Runtime,
                                      const TargetValueWitnessTable<Runtime>,
                                      /*nullable*/ false>;
  using TrailingObjects =
    swift::ABI::TrailingObjects<
      TargetExtendedExistentialTypeShape<Runtime>,
      TargetGenericContextDescriptorHeader<Runtime>,
      TargetExistentialTypeExpression<Runtime>,
      RelativeValueWitnessTablePointer,
      GenericParamDescriptor,
      TargetGenericRequirementDescriptor<Runtime>,
      GenericPackShapeHeader,
      GenericPackShapeDescriptor>;
  friend TrailingObjects;

  template<typename T>
  using OverloadToken = typename TrailingObjects::template OverloadToken<T>;

  size_t numTrailingObjects(OverloadToken<TargetGenericContextDescriptorHeader<Runtime>>) const {
    return Flags.hasGeneralizationSignature();
  }

  size_t numTrailingObjects(OverloadToken<TargetExistentialTypeExpression<Runtime>>) const {
    return Flags.hasTypeExpression();
  }

  size_t numTrailingObjects(OverloadToken<RelativeValueWitnessTablePointer>) const {
    return Flags.hasSuggestedValueWitnesses();
  }

  size_t numTrailingObjects(OverloadToken<GenericParamDescriptor>) const {
    return (Flags.hasImplicitReqSigParams() ? 0 : getNumReqSigParams())
         + (Flags.hasImplicitGenSigParams() ? 0 : getNumGenSigParams());
  }

  size_t numTrailingObjects(OverloadToken<TargetGenericRequirementDescriptor<Runtime>>) const {
    return getNumGenSigRequirements() + getNumReqSigRequirements();
  }

  size_t numTrailingObjects(OverloadToken<GenericPackShapeHeader>) const {
    return (Flags.hasTypePacks() ? 1 : 0);
  }

  size_t numTrailingObjects(OverloadToken<GenericPackShapeDescriptor>) const {
    if (!Flags.hasTypePacks())
      return 0;

    return getGenSigPackShapeHeader().NumTypePacks;
  }

  const TargetGenericContextDescriptorHeader<Runtime> *
  getGenSigHeader() const {
    assert(hasGeneralizationSignature());
    return this->template getTrailingObjects<
                   TargetGenericContextDescriptorHeader<Runtime>>();
  }

public:
  using SpecialKind = ExtendedExistentialTypeShapeFlags::SpecialKind;

  /// Flags for the existential shape.
  ExtendedExistentialTypeShapeFlags Flags;

  /// The mangling of the generalized existential type, expressed
  /// (if necessary) in terms of the type parameters of the
  /// generalization signature.
  ///
  /// If this shape is non-unique, this is always a flat string, not a
  /// "symbolic" mangling which can contain relative references.  This
  /// allows uniquing to simply compare the string content.
  ///
  /// In principle, the content of the requirement signature and type
  /// expression are derivable from this type.  We store them separately
  /// so that code which only needs to work with the logical content of
  /// the type doesn't have to break down the existential type string.
  /// This both (1) allows those operations to work substantially more
  /// efficiently (and without needing code to produce a requirement
  /// signature from an existential type to exist in the runtime) and
  /// (2) potentially allows old runtimes to support new existential
  /// types without as much invasive code.
  ///
  /// The content of this string is *not* necessarily derivable from
  /// the requirement signature.  This is because there may be multiple
  /// existential types that have equivalent logical content but which
  /// we nonetheless distinguish at compile time.  Storing this also
  /// allows us to far more easily produce a formal type from this
  /// shape reflectively.
  TargetRelativeDirectPointer<Runtime, const char, /*nullable*/ false>
      ExistentialType;

  /// The header describing the requirement signature of the existential.
  TargetGenericContextDescriptorHeader<Runtime> ReqSigHeader;

  RuntimeGenericSignature<Runtime> getRequirementSignature() const {
    return {ReqSigHeader, getReqSigParams(), getReqSigRequirements(),
            {0, 0}, nullptr};
  }

  unsigned getNumReqSigParams() const {
    return ReqSigHeader.NumParams;
  }

  const GenericParamDescriptor *getReqSigParams() const {
    return Flags.hasImplicitReqSigParams()
               ? swift::targetImplicitGenericParamDescriptors<Runtime>()
               : this->template getTrailingObjects<GenericParamDescriptor>();
  }

  unsigned getNumReqSigRequirements() const {
    return ReqSigHeader.NumRequirements;
  }

  const TargetGenericRequirementDescriptor<Runtime> *
  getReqSigRequirements() const {
    return this->template getTrailingObjects<
                              TargetGenericRequirementDescriptor<Runtime>>();
  }

  /// The type expression of the existential, as a symbolic mangled type
  /// string.  Must be null if the header is just the (single)
  /// requirement type parameter.
  const TargetExistentialTypeExpression<Runtime> *getTypeExpression() const {
    return Flags.hasTypeExpression()
               ? this->template getTrailingObjects<
                     TargetExistentialTypeExpression<Runtime>>()
               : nullptr;
  }

  bool isTypeExpressionOpaque() const {
    return !Flags.hasTypeExpression();
  }

  /// The suggested value witness table for the existential.
  /// Required if:
  /// - the special kind is SpecialKind::ExplicitLayout
  /// - the special kind is SpecialKind::None and the type head
  ///   isn't opaque
  TargetPointer<Runtime, const TargetValueWitnessTable<Runtime>>
  getSuggestedValueWitnesses() const {
    return Flags.hasSuggestedValueWitnesses()
             ? this->template getTrailingObjects<
                 RelativeValueWitnessTablePointer>()->get()
             : nullptr;
  }

  /// Return the amount of space used in the existential container
  /// for storing the existential arguments (including both the
  /// type metadata and the conformances).
  unsigned getContainerSignatureLayoutSizeInWords() const {
    unsigned rawSize = ReqSigHeader.getArgumentLayoutSizeInWords();
    switch (Flags.getSpecialKind()) {
    // The default and explicitly-sized-value-layout cases don't optimize
    // the storage of the signature.
    case SpecialKind::None:
    case SpecialKind::ExplicitLayout:
      return rawSize;

    // The class and metadata cases don't store type metadata.
    case SpecialKind::Class:
    case SpecialKind::Metatype:
      // Requirement signatures won't have non-key parameters.
      return rawSize - ReqSigHeader.NumParams;
    }

    // Assume any future cases don't optimize metadata storage.
    return rawSize;
  }

  bool hasGeneralizationSignature() const {
    return Flags.hasGeneralizationSignature();
  }

  RuntimeGenericSignature<Runtime> getGeneralizationSignature() const {
    if (!hasGeneralizationSignature()) return RuntimeGenericSignature<Runtime>();
    return {*getGenSigHeader(), getGenSigParams(), getGenSigRequirements(),
            getGenSigPackShapeHeader(), getGenSigPackShapeDescriptors()};
  }

  unsigned getNumGenSigParams() const {
    return hasGeneralizationSignature()
             ? getGenSigHeader()->NumParams : 0;
  }

  const GenericParamDescriptor *getGenSigParams() const {
    assert(hasGeneralizationSignature());
    if (Flags.hasImplicitGenSigParams())
      return swift::targetImplicitGenericParamDescriptors<Runtime>();
    auto base = this->template getTrailingObjects<GenericParamDescriptor>();
    if (!Flags.hasImplicitReqSigParams())
      base += getNumReqSigParams();
    return base;
  }

  unsigned getNumGenSigRequirements() const {
    return hasGeneralizationSignature()
             ? getGenSigHeader()->NumRequirements : 0;
  }

  const TargetGenericRequirementDescriptor<Runtime> *
  getGenSigRequirements() const {
    assert(hasGeneralizationSignature());
    return getReqSigRequirements() + ReqSigHeader.NumRequirements;
  }

  GenericPackShapeHeader getGenSigPackShapeHeader() const {
    assert(hasGeneralizationSignature());
    if (!Flags.hasTypePacks())
      return {0, 0};
    return *this->template getTrailingObjects<GenericPackShapeHeader>();
  }

  const GenericPackShapeDescriptor *getGenSigPackShapeDescriptors() const {
    assert(hasGeneralizationSignature());
    if (!Flags.hasTypePacks())
      return nullptr;
    return this->template getTrailingObjects<GenericPackShapeDescriptor>();
  }

  /// Return the amount of space used in ExtendedExistentialTypeMetadata
  /// for this shape to store the generalization arguments.
  unsigned getGenSigArgumentLayoutSizeInWords() const {
    if (!hasGeneralizationSignature()) return 0;
    return getGenSigHeader()->getArgumentLayoutSizeInWords();
  }
};
using ExtendedExistentialTypeShape
  = TargetExtendedExistentialTypeShape<InProcess>;

/// A hash which is guaranteed (ignoring a weakness in the
/// selected cryptographic hash algorithm) to be unique for the
/// source string.  Cryptographic hashing is reasonable to use
/// for certain kinds of complex uniquing performed by the
/// runtime when the representation being uniqued would otherwise
/// be prohibitively difficult to hash and compare, such as a
/// generic signature.
///
/// The hash is expected to be computed at compile time and simply
/// trusted at runtime.  We are therefore not concerned about
/// malicious collisions: an attacker would have to control the
/// program text, and there is nothing they can accomplish from a
/// hash collision that they couldn't do more easily by just changing
/// the program to do what they want.  We just want a hash with
/// minimal chance of a birthday-problem collision.  As long as
/// we use a cryptographic hash algorithm, even a 64-bit hash would
/// probably do the trick just fine, since the number of objects
/// being uniqued will be far less than 2^32.  Still, to stay on the
/// safe side, we use a 128-bit hash; the additional 8 bytes is
/// fairly marginal compared to the size of (e.g.) even the smallest
/// existential shape.
///
/// We'd like to use BLAKE3 for this, but pending acceptance of
/// that into LLVM, we're using SHA-256.  We simply truncate the
/// hash to the desired length.
struct UniqueHash {
  static_assert(NumBytes_UniqueHash % sizeof(uint32_t) == 0,
                "NumBytes_UniqueHash not a multiple of 4");
  enum { NumChunks = NumBytes_UniqueHash / sizeof(uint32_t) };

  uint32_t Data[NumChunks];

  friend bool operator==(const UniqueHash &lhs, const UniqueHash &rhs) {
    for (unsigned i = 0; i != NumChunks; ++i)
      if (lhs.Data[i] != rhs.Data[i])
        return false;
    return true;
  }

  friend uint32_t hash_value(const UniqueHash &hash) {
    // It's a cryptographic hash, so there's no point in merging
    // hash data from multiple chunks.
    return hash.Data[0];
  }
};

/// A descriptor for an extended existential type descriptor which
/// needs to be uniqued at runtime.
///
/// Uniquing is performed by comparing the existential type strings
/// of the shapes.
template <typename Runtime>
struct TargetNonUniqueExtendedExistentialTypeShape {
  /// A reference to memory that can be used to cache a globally-unique
  /// descriptor for this existential shape.
  TargetRelativeDirectPointer<Runtime,
    std::atomic<ConstTargetMetadataPointer<Runtime,
                  TargetExtendedExistentialTypeShape>>> UniqueCache;

  llvm::StringRef getExistentialTypeStringForUniquing() const {
    // When we have a non-unique shape, we're guaranteed that
    // ExistentialType contains no symbolic references, so we can just
    // recover it this way rather than having to parse it.
    return LocalCopy.ExistentialType.get();
  }

  /// The local copy of the existential shape descriptor.
  TargetExtendedExistentialTypeShape<Runtime> LocalCopy;
};
using NonUniqueExtendedExistentialTypeShape
  = TargetNonUniqueExtendedExistentialTypeShape<InProcess>;

/// The structure of type metadata for existential types which require
/// an extended existential descriptor.
///
/// An extended existential type metadata is a concrete application of
/// an extended existential descriptor to its generalization arguments,
/// which there may be none of.  See ExtendedExistentialDescriptor.
template <typename Runtime>
struct TargetExtendedExistentialTypeMetadata
  : TargetMetadata<Runtime>,
    swift::ABI::TrailingObjects<
      TargetExtendedExistentialTypeMetadata<Runtime>,
      ConstTargetPointer<Runtime, void>> {
  using StoredSize = typename Runtime::StoredSize;

private:
  using TrailingObjects =
    swift::ABI::TrailingObjects<
      TargetExtendedExistentialTypeMetadata<Runtime>,
      ConstTargetPointer<Runtime, void>>;
  friend TrailingObjects;

  template<typename T>
  using OverloadToken = typename TrailingObjects::template OverloadToken<T>;

  size_t numTrailingObjects(OverloadToken<ConstTargetPointer<Runtime, void>>) const {
    return Shape->getGenSigArgumentLayoutSizeInWords();
  }

public:
  static constexpr StoredSize OffsetToArguments = sizeof(TargetMetadata<Runtime>);

public:
  explicit constexpr
  TargetExtendedExistentialTypeMetadata(const ExtendedExistentialTypeShape *shape)
    : TargetMetadata<Runtime>(MetadataKind::ExtendedExistential),
      Shape(shape) {}

  TargetSignedPointer<Runtime, const ExtendedExistentialTypeShape *
                    __ptrauth_swift_nonunique_extended_existential_type_shape>
    Shape;

  ConstTargetPointer<Runtime, void> const *getGeneralizationArguments() const {
    return this->template getTrailingObjects<ConstTargetPointer<Runtime, void>>();
  }

public:
  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::ExtendedExistential;
  }
};
using ExtendedExistentialTypeMetadata
  = TargetExtendedExistentialTypeMetadata<InProcess>;

/// The basic layout of an existential metatype type.
template <typename Runtime>
struct TargetExistentialMetatypeContainer {
  ConstTargetMetadataPointer<Runtime, TargetMetadata> Value;

  TargetWitnessTablePointer<Runtime> *getWitnessTables() {
    return reinterpret_cast<TargetWitnessTablePointer<Runtime> *>(this + 1);
  }
  TargetWitnessTablePointer<Runtime> const *getWitnessTables() const {
    return reinterpret_cast<TargetWitnessTablePointer<Runtime> const *>(this+1);
  }

  void copyTypeInto(TargetExistentialMetatypeContainer *dest,
                    unsigned numTables) const {
    for (unsigned i = 0; i != numTables; ++i)
      dest->getWitnessTables()[i] = getWitnessTables()[i];
  }
};
using ExistentialMetatypeContainer
  = TargetExistentialMetatypeContainer<InProcess>;

/// The structure of metadata for existential metatypes.
template <typename Runtime>
struct TargetExistentialMetatypeMetadata
  : public TargetMetadata<Runtime> {
  /// The type metadata for the element.
  ConstTargetMetadataPointer<Runtime, swift::TargetMetadata> InstanceType;

  /// The number of witness tables and class-constrained-ness of the
  /// underlying type.
  ExistentialTypeFlags Flags;

  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::ExistentialMetatype;
  }

  /// Return true iff all the protocol constraints are @objc.
  bool isObjC() const {
    return isClassBounded() && Flags.getNumWitnessTables() == 0;
  }

  bool isClassBounded() const {
    return Flags.getClassConstraint() == ProtocolClassConstraint::Class;
  }
};
using ExistentialMetatypeMetadata
  = TargetExistentialMetatypeMetadata<InProcess>;

/// Heap metadata for a box, which may have been generated statically by the
/// compiler or by the runtime.
template <typename Runtime>
struct TargetBoxHeapMetadata : public TargetHeapMetadata<Runtime> {
  /// The offset from the beginning of a box to its value.
  unsigned Offset;

  constexpr TargetBoxHeapMetadata(MetadataKind kind, unsigned offset)
  : TargetHeapMetadata<Runtime>(kind), Offset(offset) {}
};
using BoxHeapMetadata = TargetBoxHeapMetadata<InProcess>;

/// Heap metadata for runtime-instantiated generic boxes.
template <typename Runtime>
struct TargetGenericBoxHeapMetadata : public TargetBoxHeapMetadata<Runtime> {
  using super = TargetBoxHeapMetadata<Runtime>;
  using super::Offset;

  /// The type inside the box.
  ConstTargetMetadataPointer<Runtime, swift::TargetMetadata> BoxedType;

  constexpr
  TargetGenericBoxHeapMetadata(MetadataKind kind, unsigned offset,
    ConstTargetMetadataPointer<Runtime, swift::TargetMetadata> boxedType)
  : TargetBoxHeapMetadata<Runtime>(kind, offset), BoxedType(boxedType)
  {}

  static unsigned getHeaderOffset(const Metadata *boxedType) {
    // Round up the header size to alignment.
    unsigned alignMask = boxedType->getValueWitnesses()->getAlignmentMask();
    return (sizeof(HeapObject) + alignMask) & ~alignMask;
  }

  /// Project the value out of a box of this type.
  OpaqueValue *project(HeapObject *box) const {
    auto bytes = reinterpret_cast<char*>(box);
    return reinterpret_cast<OpaqueValue *>(bytes + Offset);
  }

  /// Get the allocation size of this box.
  unsigned getAllocSize() const {
    return Offset + BoxedType->getValueWitnesses()->getSize();
  }

  /// Get the allocation alignment of this box.
  unsigned getAllocAlignMask() const {
    // Heap allocations are at least pointer aligned.
    return BoxedType->getValueWitnesses()->getAlignmentMask()
      | (alignof(void*) - 1);
  }

  static bool classof(const TargetMetadata<Runtime> *metadata) {
    return metadata->getKind() == MetadataKind::HeapGenericLocalVariable;
  }
};
using GenericBoxHeapMetadata = TargetGenericBoxHeapMetadata<InProcess>;

/// The control structure of a generic or resilient protocol
/// conformance witness.
///
/// Resilient conformances must use a pattern where new requirements
/// with default implementations can be added and the order of existing
/// requirements can be changed.
///
/// This is accomplished by emitting an order-independent series of
/// relative pointer pairs, consisting of a protocol requirement together
/// with a witness. The requirement is identified by an indirectable relative
/// pointer to the protocol requirement descriptor.
template <typename Runtime>
struct TargetResilientWitness {
  TargetRelativeProtocolRequirementPointer<Runtime> Requirement;
  union {
    TargetRelativeDirectPointer<Runtime, void> Impl;
    TargetCompactFunctionPointer<Runtime, void> FuncImpl;
  };

  void *getWitness(ProtocolRequirementFlags flags) const {
    if (flags.isFunctionImpl()) {
      return FuncImpl.get();
    } else {
      return Impl.get();
    }
  }
};
using ResilientWitness = TargetResilientWitness<InProcess>;

template <typename Runtime>
struct TargetResilientWitnessTable final
  : public swift::ABI::TrailingObjects<
             TargetResilientWitnessTable<Runtime>,
             TargetResilientWitness<Runtime>> {
  uint32_t NumWitnesses;

  using TrailingObjects = swift::ABI::TrailingObjects<
                             TargetResilientWitnessTable<Runtime>,
                             TargetResilientWitness<Runtime>>;
  friend TrailingObjects;

  template<typename T>
  using OverloadToken = typename TrailingObjects::template OverloadToken<T>;

  size_t numTrailingObjects(
                        OverloadToken<TargetResilientWitness<Runtime>>) const {
    return NumWitnesses;
  }

  llvm::ArrayRef<TargetResilientWitness<Runtime>>
  getWitnesses() const {
    return {this->template getTrailingObjects<TargetResilientWitness<Runtime>>(),
            NumWitnesses};
  }

  const TargetResilientWitness<Runtime> &
  getWitness(unsigned i) const {
    return getWitnesses()[i];
  }
};
using ResilientWitnessTable = TargetResilientWitnessTable<InProcess>;

/// The control structure of a generic or resilient protocol
/// conformance, which is embedded in the protocol conformance descriptor.
///
/// Witness tables need to be instantiated at runtime in these cases:
/// - For a generic conforming type, associated type requirements might be
///   dependent on the conforming type.
/// - For a type conforming to a resilient protocol, the runtime size of
///   the witness table is not known because default requirements can be
///   added resiliently.
///
/// One per conformance.
template <typename Runtime>
struct TargetGenericWitnessTable {
  /// The size of the witness table in words.  This amount is copied from
  /// the witness table template into the instantiated witness table.
  uint16_t WitnessTableSizeInWords;

  /// The amount of private storage to allocate before the address point,
  /// in words. This memory is zeroed out in the instantiated witness table
  /// template.
  ///
  /// The low bit is used to indicate whether this witness table is known
  /// to require instantiation.
  uint16_t WitnessTablePrivateSizeInWordsAndRequiresInstantiation;

  /// The instantiation function, which is called after the template is copied.
  TargetCompactFunctionPointer<
      Runtime,
      void(TargetWitnessTable<Runtime> *instantiatedTable,
           const TargetMetadata<Runtime> *type,
           const void *const *instantiationArgs),
      /*nullable*/ true>
      Instantiator;

  using PrivateDataType = void *[swift::NumGenericMetadataPrivateDataWords];

  /// Private data for the instantiator.  Out-of-line so that the rest
  /// of this structure can be constant. Might be null when building with
  /// -disable-preallocated-instantiation-caches.
  RelativeDirectPointer<PrivateDataType> PrivateData;

  uint16_t getWitnessTablePrivateSizeInWords() const {
    return WitnessTablePrivateSizeInWordsAndRequiresInstantiation >> 1;
  }

  /// This bit doesn't really mean anything. Currently, the compiler always
  /// sets it when emitting a generic witness table.
  uint16_t requiresInstantiation() const {
    return WitnessTablePrivateSizeInWordsAndRequiresInstantiation & 0x01;
  }
};
using GenericWitnessTable = TargetGenericWitnessTable<InProcess>;

/// The structure of a type metadata record.
///
/// This contains enough static information to recover type metadata from a
/// name.
template <typename Runtime>
struct TargetTypeMetadataRecord {
private:
  union {
    /// A direct reference to a nominal type descriptor.
    RelativeDirectPointerIntPair<TargetContextDescriptor<Runtime>,
                                 TypeReferenceKind>
      DirectNominalTypeDescriptor;

    /// An indirect reference to a nominal type descriptor.
    RelativeDirectPointerIntPair<TargetSignedPointer<Runtime, TargetContextDescriptor<Runtime> * __ptrauth_swift_type_descriptor>,
                                 TypeReferenceKind>
      IndirectNominalTypeDescriptor;

    // We only allow a subset of the TypeReferenceKinds here.
    // Should we just acknowledge that this is a different enum?
  };

public:
  TypeReferenceKind getTypeKind() const {
    return DirectNominalTypeDescriptor.getInt();
  }
  
  const TargetContextDescriptor<Runtime> *
  getContextDescriptor() const {
    switch (getTypeKind()) {
    case TypeReferenceKind::DirectTypeDescriptor:
      return DirectNominalTypeDescriptor.getPointer();

    case TypeReferenceKind::IndirectTypeDescriptor:
      return *IndirectNominalTypeDescriptor.getPointer();

    // These types (and any others we might add to TypeReferenceKind
    // in the future) are just never used in these lists.
    case TypeReferenceKind::DirectObjCClassName:
    case TypeReferenceKind::IndirectObjCClass:
      return nullptr;
    }
    
    return nullptr;
  }
};

using TypeMetadataRecord = TargetTypeMetadataRecord<InProcess>;

/// The structure of a protocol reference record.
template <typename Runtime>
struct TargetProtocolRecord {
  /// The protocol referenced.
  ///
  /// The remaining low bit is reserved for future use.
  RelativeContextPointerIntPair<Runtime, /*reserved=*/bool,
                                TargetProtocolDescriptor>
    Protocol;
};
using ProtocolRecord = TargetProtocolRecord<InProcess>;

template<typename Runtime> class TargetGenericRequirementDescriptor;

/// Header containing information about the resilient witnesses in a
/// protocol conformance descriptor.
template <typename Runtime>
struct TargetResilientWitnessesHeader {
  uint32_t NumWitnesses;
};
using ResilientWitnessesHeader = TargetResilientWitnessesHeader<InProcess>;

/// The structure of a protocol conformance.
///
/// This contains enough static information to recover the witness table for a
/// type's conformance to a protocol.
template <typename Runtime>
struct TargetProtocolConformanceDescriptor final
  : public swift::ABI::TrailingObjects<
             TargetProtocolConformanceDescriptor<Runtime>,
             TargetRelativeContextPointer<Runtime>,
             TargetGenericRequirementDescriptor<Runtime>,
             GenericPackShapeDescriptor,
             TargetResilientWitnessesHeader<Runtime>,
             TargetResilientWitness<Runtime>,
             TargetGenericWitnessTable<Runtime>> {

  using TrailingObjects = swift::ABI::TrailingObjects<
                             TargetProtocolConformanceDescriptor<Runtime>,
                             TargetRelativeContextPointer<Runtime>,
                             TargetGenericRequirementDescriptor<Runtime>,
                             GenericPackShapeDescriptor,
                             TargetResilientWitnessesHeader<Runtime>,
                             TargetResilientWitness<Runtime>,
                             TargetGenericWitnessTable<Runtime>>;
  friend TrailingObjects;

  template<typename T>
  using OverloadToken = typename TrailingObjects::template OverloadToken<T>;

public:
  using GenericRequirementDescriptor =
    TargetGenericRequirementDescriptor<Runtime>;

  using ResilientWitnessesHeader = TargetResilientWitnessesHeader<Runtime>;
  using ResilientWitness = TargetResilientWitness<Runtime>;
  using GenericWitnessTable = TargetGenericWitnessTable<Runtime>;

private:
  /// The protocol being conformed to.
  TargetRelativeContextPointer<Runtime, TargetProtocolDescriptor> Protocol;
  
  // Some description of the type that conforms to the protocol.
  TargetTypeReference<Runtime> TypeRef;

  /// The witness table pattern, which may also serve as the witness table.
  RelativeDirectPointer<const TargetWitnessTable<Runtime>> WitnessTablePattern;

  /// Various flags, including the kind of conformance.
  ConformanceFlags Flags;

public:
  ConstTargetPointer<Runtime, TargetProtocolDescriptor<Runtime>>
  getProtocol() const {
    return Protocol;
  }

  TypeReferenceKind getTypeKind() const {
    return Flags.getTypeReferenceKind();
  }

  const char *getDirectObjCClassName() const {
    return TypeRef.getDirectObjCClassName(getTypeKind());
  }

  const TargetClassMetadataObjCInterop<Runtime> *const *
  getIndirectObjCClass() const {
    return TypeRef.getIndirectObjCClass(getTypeKind());
  }

  const TargetContextDescriptor<Runtime> *getTypeDescriptor() const {
    return TypeRef.getTypeDescriptor(getTypeKind());
  }

  constexpr inline auto
  getTypeRefDescriptorOffset() const -> typename Runtime::StoredSize {
    return offsetof(typename std::remove_reference<decltype(*this)>::type, TypeRef);
  }

  constexpr inline auto
  getProtocolDescriptorOffset() const -> typename Runtime::StoredSize {
    return offsetof(typename std::remove_reference<decltype(*this)>::type, Protocol);
  }

  TargetContextDescriptor<Runtime> * __ptrauth_swift_type_descriptor *
  _getTypeDescriptorLocation() const {
    if (getTypeKind() != TypeReferenceKind::IndirectTypeDescriptor)
      return nullptr;
    return TypeRef.IndirectTypeDescriptor.get();
  }

  /// Retrieve the context of a retroactive conformance.
  const TargetContextDescriptor<Runtime> *getRetroactiveContext() const {
    if (!Flags.isRetroactive()) return nullptr;

    return this->template getTrailingObjects<
        TargetRelativeContextPointer<Runtime>>();
  }

  /// Whether this conformance is non-unique because it has been synthesized
  /// for a foreign type.
  bool isSynthesizedNonUnique() const {
    return Flags.isSynthesizedNonUnique();
  }

  /// Whether this conformance has any conditional requirements that need to
  /// be evaluated.
  bool hasConditionalRequirements() const {
    return Flags.getNumConditionalRequirements() > 0;
  }

  /// Retrieve the conditional requirements that must also be
  /// satisfied
  llvm::ArrayRef<TargetGenericRequirementDescriptor<Runtime>>
  getConditionalRequirements() const {
    return {this->template getTrailingObjects<TargetGenericRequirementDescriptor<Runtime>>(),
            Flags.getNumConditionalRequirements()};
  }

  /// Retrieve the pack shape descriptors for the conditional pack requirements.
  llvm::ArrayRef<GenericPackShapeDescriptor>
  getConditionalPackShapeDescriptors() const {
    return {this->template getTrailingObjects<GenericPackShapeDescriptor>(),
            Flags.getNumConditionalPackShapeDescriptors()};
  }

  /// Get the directly-referenced witness table pattern, which may also
  /// serve as the witness table.
  const swift::TargetWitnessTable<Runtime> *getWitnessTablePattern() const {
    return WitnessTablePattern;
  }

  /// Get the canonical metadata for the type referenced by this record, or
  /// return null if the record references a generic or universal type.
  const TargetMetadata<Runtime> *getCanonicalTypeMetadata() const;
  
  /// Get the witness table for the specified type, realizing it if
  /// necessary, or return null if the conformance does not apply to the
  /// type.
  const swift::TargetWitnessTable<Runtime> *
  getWitnessTable(const TargetMetadata<Runtime> *type) const;

  /// Retrieve the resilient witnesses.
  llvm::ArrayRef<ResilientWitness> getResilientWitnesses() const {
    if (!Flags.hasResilientWitnesses())
      return { };

    return llvm::ArrayRef<ResilientWitness>(
        this->template getTrailingObjects<ResilientWitness>(),
        numTrailingObjects(OverloadToken<ResilientWitness>()));
  }

  ConstTargetPointer<Runtime, GenericWitnessTable>
  getGenericWitnessTable() const {
    if (!Flags.hasGenericWitnessTable())
      return nullptr;

    return this->template getTrailingObjects<GenericWitnessTable>();
  }

#if !defined(NDEBUG) && SWIFT_OBJC_INTEROP
  void dump() const;
#endif

#ifndef NDEBUG
  /// Verify that the protocol descriptor obeys all invariants.
  ///
  /// We currently check that the descriptor:
  ///
  /// 1. Has a valid TypeReferenceKind.
  /// 2. Has a valid conformance kind.
  void verify() const;
#endif

private:
  size_t numTrailingObjects(
                        OverloadToken<TargetRelativeContextPointer<Runtime>>) const {
    return Flags.isRetroactive() ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<GenericRequirementDescriptor>) const {
    return Flags.getNumConditionalRequirements();
  }

  size_t numTrailingObjects(OverloadToken<GenericPackShapeDescriptor>) const {
    return Flags.getNumConditionalPackShapeDescriptors();
  }

  size_t numTrailingObjects(OverloadToken<ResilientWitnessesHeader>) const {
    return Flags.hasResilientWitnesses() ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<ResilientWitness>) const {
    return Flags.hasResilientWitnesses()
      ? this->template getTrailingObjects<ResilientWitnessesHeader>()
          ->NumWitnesses
      : 0;
  }

  size_t numTrailingObjects(OverloadToken<GenericWitnessTable>) const {
    return Flags.hasGenericWitnessTable() ? 1 : 0;
  }
};
using ProtocolConformanceDescriptor
  = TargetProtocolConformanceDescriptor<InProcess>;

template<typename Runtime>
using TargetProtocolConformanceRecord =
  RelativeDirectPointer<TargetProtocolConformanceDescriptor<Runtime>,
                        /*Nullable=*/false>;

using ProtocolConformanceRecord = TargetProtocolConformanceRecord<InProcess>;

template<template <typename Runtime> class ObjCInteropKind, unsigned PointerSize>
using ExternalProtocolConformanceDescriptor = TargetProtocolConformanceDescriptor<External<ObjCInteropKind<RuntimeTarget<PointerSize>>>>;

template<template <typename Runtime> class ObjCInteropKind, unsigned PointerSize>
using ExternalProtocolConformanceRecord = TargetProtocolConformanceRecord<External<ObjCInteropKind<RuntimeTarget<PointerSize>>>>;

template <typename Runtime>
struct swift_ptrauth_struct_context_descriptor(ModuleContextDescriptor)
    TargetModuleContextDescriptor;

/// Base class for all context descriptors.
template <typename Runtime>
struct swift_ptrauth_struct_context_descriptor(ContextDescriptor)
    TargetContextDescriptor {
  /// Flags describing the context, including its kind and format version.
  ContextDescriptorFlags Flags;
  
  /// The parent context, or null if this is a top-level context.
  TargetRelativeContextPointer<Runtime> Parent;

  bool isGeneric() const { return Flags.isGeneric(); }
  bool isUnique() const { return Flags.isUnique(); }
  ContextDescriptorKind getKind() const { return Flags.getKind(); }

  /// Get the generic context information for this context, or null if the
  /// context is not generic.
  const TargetGenericContext<Runtime> *getGenericContext() const;

  /// Get the module context for this context.
  const TargetModuleContextDescriptor<Runtime> *getModuleContext() const;

  /// Is this context part of a C-imported module?
  bool isCImportedContext() const;

  unsigned getNumGenericParams() const {
    auto *genericContext = getGenericContext();
    return genericContext
              ? genericContext->getGenericContextHeader().NumParams
              : 0;
  }

  constexpr inline auto
  getParentOffset() const -> typename Runtime::StoredSize {
    return offsetof(typename std::remove_reference<decltype(*this)>::type, Parent);
  }

#ifndef NDEBUG
  [[deprecated("Only meant for use in the debugger")]] void dump() const;
#endif

private:
  TargetContextDescriptor(const TargetContextDescriptor &) = delete;
  TargetContextDescriptor(TargetContextDescriptor &&) = delete;
  TargetContextDescriptor &operator=(const TargetContextDescriptor &) = delete;
  TargetContextDescriptor &operator=(TargetContextDescriptor &&) = delete;
};

using ContextDescriptor = TargetContextDescriptor<InProcess>;
template<template <typename Runtime> class ObjCInteropKind, unsigned PointerSize>
using ExternalContextDescriptor = TargetContextDescriptor<External<ObjCInteropKind<RuntimeTarget<PointerSize>>>>;

inline bool isCImportedModuleName(llvm::StringRef name) {
  // This does not include MANGLING_MODULE_CLANG_IMPORTER because that's
  // used only for synthesized declarations and not actual imported
  // declarations.
  return name == MANGLING_MODULE_OBJC;
}

/// Descriptor for a module context.
template <typename Runtime>
struct swift_ptrauth_struct_context_descriptor(ModuleContextDescriptor)
    TargetModuleContextDescriptor final : TargetContextDescriptor<Runtime> {
  /// The module name.
  RelativeDirectPointer<const char, /*nullable*/ false> Name;

  /// Is this module a special C-imported module?
  bool isCImportedContext() const {
    return isCImportedModuleName(Name.get());
  }

  constexpr inline auto
  getNameOffset() const -> typename Runtime::StoredSize {
    return offsetof(typename std::remove_reference<decltype(*this)>::type, Name);
  }

  static bool classof(const TargetContextDescriptor<Runtime> *cd) {
    return cd->getKind() == ContextDescriptorKind::Module;
  }
};

using ModuleContextDescriptor = TargetModuleContextDescriptor<InProcess>;

template<template <typename Runtime> class ObjCInteropKind, unsigned PointerSize>
using ExternalModuleContextDescriptor = TargetModuleContextDescriptor<External<ObjCInteropKind<RuntimeTarget<PointerSize>>>>;

template<typename Runtime>
inline bool TargetContextDescriptor<Runtime>::isCImportedContext() const {
  return getModuleContext()->isCImportedContext();
}

template<typename Runtime>
inline const TargetModuleContextDescriptor<Runtime> *
TargetContextDescriptor<Runtime>::getModuleContext() const {
  // All context chains should eventually find a module.
  for (auto cur = this; true; cur = cur->Parent.get()) {
    if (auto module = dyn_cast<TargetModuleContextDescriptor<Runtime>>(cur))
      return module;
  }
}

/// Descriptor for an extension context.
template <typename Runtime>
struct swift_ptrauth_struct_context_descriptor(ExtensionContextDescriptor)
    TargetExtensionContextDescriptor final
    : TargetContextDescriptor<Runtime>,
      TrailingGenericContextObjects<TargetExtensionContextDescriptor<Runtime>>
{
private:
  using TrailingGenericContextObjects =
      swift::TrailingGenericContextObjects<TargetExtensionContextDescriptor<Runtime>>;

public:
  /// A mangling of the `Self` type context that the extension extends.
  /// The mangled name represents the type in the generic context encoded by
  /// this descriptor. For example, a nongeneric nominal type extension will
  /// encode the nominal type name. A generic nominal type extension will encode
  /// the instance of the type with any generic arguments bound.
  ///
  /// Note that the Parent of the extension will be the module context the
  /// extension is declared inside.
  RelativeDirectPointer<const char> ExtendedContext;

  using TrailingGenericContextObjects::getGenericContext;

  llvm::StringRef getMangledExtendedContext() const {
    return Demangle::makeSymbolicMangledNameStringRef(ExtendedContext.get());
  }
  
  static bool classof(const TargetContextDescriptor<Runtime> *cd) {
    return cd->getKind() == ContextDescriptorKind::Extension;
  }
};

using ExtensionContextDescriptor = TargetExtensionContextDescriptor<InProcess>;

template<typename Runtime>
struct TargetMangledContextName {
  /// The mangled name of the context.
  TargetRelativeDirectPointer<Runtime, const char, /*nullable*/ false> name;
};

template <typename Runtime>
struct swift_ptrauth_struct_context_descriptor(AnonymousContextDescriptor)
    TargetAnonymousContextDescriptor final
    : TargetContextDescriptor<Runtime>,
      TrailingGenericContextObjects<TargetAnonymousContextDescriptor<Runtime>,
                                    TargetGenericContextDescriptorHeader,
                                    TargetMangledContextName<Runtime>>
{
private:
  using TrailingGenericContextObjects =
      swift::TrailingGenericContextObjects<TargetAnonymousContextDescriptor<Runtime>,
                                           TargetGenericContextDescriptorHeader,
                                           TargetMangledContextName<Runtime>>;
  using TrailingObjects =
    typename TrailingGenericContextObjects::TrailingObjects;
  friend TrailingObjects;

public:
  using MangledContextName = TargetMangledContextName<Runtime>;

  using TrailingGenericContextObjects::getGenericContext;
  using TrailingGenericContextObjects::getGenericContextHeader;
  using TrailingGenericContextObjects::getFullGenericContextHeader;
  using TrailingGenericContextObjects::getGenericParams;

  AnonymousContextDescriptorFlags getAnonymousContextDescriptorFlags() const {
    return AnonymousContextDescriptorFlags(this->Flags.getKindSpecificFlags());
  }

  /// Whether this anonymous context descriptor contains a full mangled name,
  /// which can be used to match the anonymous type to its textual form.
  bool hasMangledName() const {
    return getAnonymousContextDescriptorFlags().hasMangledName();
  }

  /// Retrieve the mangled name of this context, or NULL if it was not
  /// recorded in the metadata.
  ConstTargetPointer<Runtime, char> getMangledName() const {
    if (!hasMangledName())
      return ConstTargetPointer<Runtime, char>();

    return this->template getTrailingObjects<MangledContextName>()->name;
  }

  /// Retrieve a pointer to the mangled context name structure.
  const MangledContextName *getMangledContextName() const {
    if (!hasMangledName())
      return nullptr;

    return this->template getTrailingObjects<MangledContextName>();
  }

private:
  template<typename T>
  using OverloadToken =
    typename TrailingGenericContextObjects::template OverloadToken<T>;

  using TrailingGenericContextObjects::numTrailingObjects;

  size_t numTrailingObjects(OverloadToken<MangledContextName>) const {
    return this->hasMangledNam() ? 1 : 0;
  }

public:
  static bool classof(const TargetContextDescriptor<Runtime> *cd) {
    return cd->getKind() == ContextDescriptorKind::Anonymous;
  }
};
using AnonymousContextDescriptor = TargetAnonymousContextDescriptor<InProcess>;

template<template <typename Runtime> class ObjCInteropKind, unsigned PointerSize>
using ExternalAnonymousContextDescriptor = TargetAnonymousContextDescriptor<External<ObjCInteropKind<RuntimeTarget<PointerSize>>>>;

/// A protocol descriptor.
///
/// Protocol descriptors contain information about the contents of a protocol:
/// it's name, requirements, requirement signature, context, and so on. They
/// are used both to identify a protocol and to reason about its contents.
///
/// Only Swift protocols are defined by a protocol descriptor, whereas
/// Objective-C (including protocols defined in Swift as @objc) use the
/// Objective-C protocol layout.
template <typename Runtime>
struct swift_ptrauth_struct_context_descriptor(ProtocolDescriptor)
    TargetProtocolDescriptor final
    : TargetContextDescriptor<Runtime>,
      swift::ABI::TrailingObjects<
        TargetProtocolDescriptor<Runtime>,
        TargetGenericRequirementDescriptor<Runtime>,
        TargetProtocolRequirement<Runtime>>
{
private:
  using TrailingObjects
    = swift::ABI::TrailingObjects<
        TargetProtocolDescriptor<Runtime>,
        TargetGenericRequirementDescriptor<Runtime>,
        TargetProtocolRequirement<Runtime>>;

  friend TrailingObjects;

  template<typename T>
  using OverloadToken = typename TrailingObjects::template OverloadToken<T>;

public:
  size_t numTrailingObjects(
            OverloadToken<TargetGenericRequirementDescriptor<Runtime>>) const {
    return NumRequirementsInSignature;
  }

  size_t numTrailingObjects(
            OverloadToken<TargetProtocolRequirement<Runtime>>) const {
    return NumRequirements;
  }


  /// The name of the protocol.
  TargetRelativeDirectPointer<Runtime, const char, /*nullable*/ false> Name;

  /// The number of generic requirements in the requirement signature of the
  /// protocol.
  uint32_t NumRequirementsInSignature;

  /// The number of requirements in the protocol.
  /// If any requirements beyond MinimumWitnessTableSizeInWords are present
  /// in the witness table template, they will be not be overwritten with
  /// defaults.
  uint32_t NumRequirements;

  /// Associated type names, as a space-separated list in the same order
  /// as the requirements.
  RelativeDirectPointer<const char, /*Nullable=*/true> AssociatedTypeNames;

  ProtocolContextDescriptorFlags getProtocolContextDescriptorFlags() const {
    return ProtocolContextDescriptorFlags(this->Flags.getKindSpecificFlags());
  }

  /// Retrieve the requirements that make up the requirement signature of
  /// this protocol.
  llvm::ArrayRef<TargetGenericRequirementDescriptor<Runtime>>
  getRequirementSignature() const {
    return {this->template getTrailingObjects<
                             TargetGenericRequirementDescriptor<Runtime>>(),
            NumRequirementsInSignature};
  }

  /// Retrieve the requirements of this protocol.
  llvm::ArrayRef<TargetProtocolRequirement<Runtime>>
  getRequirements() const {
    return {this->template getTrailingObjects<
                             TargetProtocolRequirement<Runtime>>(),
            NumRequirements};
  }

  constexpr inline auto
  getNameOffset() const -> typename Runtime::StoredSize {
    return offsetof(typename std::remove_reference<decltype(*this)>::type, Name);
  }

  /// Retrieve the requirement base descriptor address.
  ConstTargetPointer<Runtime, TargetProtocolRequirement<Runtime>>
  getRequirementBaseDescriptor() const {
    return getRequirements().data() - WitnessTableFirstRequirementOffset;
  }

#ifndef NDEBUG
  [[deprecated("Only meant for use in the debugger")]] void dump() const;
#endif

  static bool classof(const TargetContextDescriptor<Runtime> *cd) {
    return cd->getKind() == ContextDescriptorKind::Protocol;
  }
};

/// The descriptor for an opaque type.
template <typename Runtime>
struct swift_ptrauth_struct_context_descriptor(OpaqueTypeDescriptor)
    TargetOpaqueTypeDescriptor final
    : TargetContextDescriptor<Runtime>,
    TrailingGenericContextObjects<TargetOpaqueTypeDescriptor<Runtime>,
                                  TargetGenericContextDescriptorHeader,
                                  RelativeDirectPointer<const char>>
{
private:
  using TrailingGenericContextObjects =
      swift::TrailingGenericContextObjects<TargetOpaqueTypeDescriptor<Runtime>,
                                           TargetGenericContextDescriptorHeader,
                                           RelativeDirectPointer<const char>>;
  using TrailingObjects =
    typename TrailingGenericContextObjects::TrailingObjects;
  friend TrailingObjects;

  template<typename T>
  using OverloadToken = typename TrailingObjects::template OverloadToken<T>;

public:
  using TrailingGenericContextObjects::getGenericContext;
  using TrailingGenericContextObjects::getGenericContextHeader;
  using TrailingGenericContextObjects::getFullGenericContextHeader;
  using TrailingGenericContextObjects::getGenericParams;

  // The kind-specific flags area is used to store the count of the generic
  // arguments for underlying type(s) encoded in the descriptor.
  unsigned getNumUnderlyingTypeArguments() const {
    return this->Flags.getKindSpecificFlags();
  }
  
  using TrailingGenericContextObjects::numTrailingObjects;
  size_t numTrailingObjects(OverloadToken<RelativeDirectPointer<const char>>) const {
    return getNumUnderlyingTypeArguments();
  }
  
  const RelativeDirectPointer<const char> &
  getUnderlyingTypeArgumentMangledName(unsigned i) const {
    assert(i < getNumUnderlyingTypeArguments());
    return (this
         ->template getTrailingObjects<RelativeDirectPointer<const char>>())[i];
  }

  llvm::StringRef getUnderlyingTypeArgument(unsigned i) const {
    assert(i < getNumUnderlyingTypeArguments());
    const char *ptr = getUnderlyingTypeArgumentMangledName(i);    
    return Demangle::makeSymbolicMangledNameStringRef(ptr);
  }

  static bool classof(const TargetContextDescriptor<Runtime> *cd) {
    return cd->getKind() == ContextDescriptorKind::OpaqueType;
  }
};

template <template <typename Runtime> class ObjCInteropKind,
          unsigned PointerSize>
using ExternalOpaqueTypeDescriptor = TargetOpaqueTypeDescriptor<
    External<ObjCInteropKind<RuntimeTarget<PointerSize>>>>;
using OpaqueTypeDescriptor = TargetOpaqueTypeDescriptor<InProcess>;

/// The instantiation cache for generic metadata.  This must be guaranteed
/// to zero-initialized before it is first accessed.  Its contents are private
/// to the runtime.
template <typename Runtime>
struct TargetGenericMetadataInstantiationCache {
  /// Data that the runtime can use for its own purposes.  It is guaranteed
  /// to be zero-filled by the compiler. Might be null when building with
  /// -disable-preallocated-instantiation-caches.
  TargetPointer<Runtime, void>
  PrivateData[swift::NumGenericMetadataPrivateDataWords];
};
using GenericMetadataInstantiationCache =
  TargetGenericMetadataInstantiationCache<InProcess>;

/// A function that instantiates metadata.  This function is required
/// to succeed.
///
/// In general, the metadata returned by this function should have all the
/// basic structure necessary to identify itself: that is, it must have a
/// type descriptor and generic arguments.  However, it does not need to be
/// fully functional as type metadata; for example, it does not need to have
/// a meaningful value witness table, v-table entries, or a superclass.
///
/// Operations which may fail (due to e.g. recursive dependencies) but which
/// must be performed in order to prepare the metadata object to be fully
/// functional as type metadata should be delayed until the completion
/// function.
using MetadataInstantiator =
  Metadata *(const TargetTypeContextDescriptor<InProcess> *type,
             const void *arguments,
             const TargetGenericMetadataPattern<InProcess> *pattern);

/// The opaque completion context of a metadata completion function.
/// A completion function that needs to report a completion dependency
/// can use this to figure out where it left off and thus avoid redundant
/// work when re-invoked.  It will be zero on first entry for a type, and
/// the runtime is free to copy it to a different location between
/// invocations.
struct MetadataCompletionContext {
  void *Data[NumWords_MetadataCompletionContext];
};

/// A function which attempts to complete the given metadata.
///
/// This function may fail due to a dependency on the completion of some
/// other metadata object.  It can indicate this by returning the metadata
/// on which it depends.  In this case, the function will be invoked again
/// when the dependency is resolved.  The function must be careful not to
/// indicate a completion dependency on a type that has always been
/// completed; the runtime cannot reliably distinguish this sort of
/// programming failure from a race in which the dependent type was
/// completed immediately after it was observed to be incomplete, and so
/// the function will be repeatedly re-invoked.
///
/// The function will never be called multiple times simultaneously, but
/// it may be called many times as successive dependencies are resolved.
/// If the function ever completes successfully (by returning null), it
/// will not be called again for the same type.
using MetadataCompleter =
  SWIFT_CC(swift)
  MetadataDependency(const Metadata *type,
                     MetadataCompletionContext *context,
                     const TargetGenericMetadataPattern<InProcess> *pattern);

/// An instantiation pattern for type metadata.
template <typename Runtime>
struct TargetGenericMetadataPattern {
  /// The function to call to instantiate the template.
  TargetCompactFunctionPointer<Runtime, MetadataInstantiator>
    InstantiationFunction;

  /// The function to call to complete the instantiation.  If this is null,
  /// the instantiation function must always generate complete metadata.
  TargetCompactFunctionPointer<Runtime, MetadataCompleter, /*nullable*/ true>
    CompletionFunction;

  /// Flags describing the layout of this instantiation pattern.
  GenericMetadataPatternFlags PatternFlags;

  bool hasExtraDataPattern() const {
    return PatternFlags.hasExtraDataPattern();
  }
};
using GenericMetadataPattern =
  TargetGenericMetadataPattern<InProcess>;

/// Part of a generic metadata instantiation pattern.
template <typename Runtime>
struct TargetGenericMetadataPartialPattern {
  /// A reference to the pattern.  The pattern must always be at least
  /// word-aligned.
  TargetRelativeDirectPointer<Runtime, typename Runtime::StoredPointer> Pattern;

  /// The offset into the section into which to copy this pattern, in words.
  uint16_t OffsetInWords;

  /// The size of the pattern, in words.
  uint16_t SizeInWords;
};
using GenericMetadataPartialPattern =
  TargetGenericMetadataPartialPattern<InProcess>;

/// A base class for conveniently adding trailing fields to a
/// generic metadata pattern.
template <typename Runtime,
          typename Self,
          typename... ExtraTrailingObjects>
class TargetGenericMetadataPatternTrailingObjects :
  protected swift::ABI::TrailingObjects<Self,
                                TargetGenericMetadataPartialPattern<Runtime>,
                                ExtraTrailingObjects...> {

  using TrailingObjects =
    swift::ABI::TrailingObjects<Self,
                                TargetGenericMetadataPartialPattern<Runtime>,
                                ExtraTrailingObjects...>;
  friend TrailingObjects;

  using GenericMetadataPartialPattern =
    TargetGenericMetadataPartialPattern<Runtime>;

  const Self *asSelf() const {
    return static_cast<const Self *>(this);
  }

  template<typename T>
  using OverloadToken = typename TrailingObjects::template OverloadToken<T>;

public:
  /// Return the extra-data pattern.
  ///
  /// For class metadata, the existence of this pattern creates the need
  /// for extra data to be allocated in the metadata.  The amount of extra
  /// data allocated is the sum of the offset and the size of this pattern.
  ///
  /// In value metadata, the size of the extra data section is passed to the
  /// allocation function; this is because it is currently not stored elsewhere
  /// and because the extra data is principally used for storing values that
  /// cannot be patterned anyway.
  ///
  /// In value metadata, this section is relative to the end of the
  /// metadata header (e.g. after the last members declared in StructMetadata).
  /// In class metadata, this section is relative to the end of the entire
  /// class metadata.
  ///
  /// See also: [pre-5.2-extra-data-zeroing]
  /// See also: [pre-5.3-extra-data-zeroing]
  const GenericMetadataPartialPattern *getExtraDataPattern() const {
    assert(asSelf()->hasExtraDataPattern());
    return this->template getTrailingObjects<GenericMetadataPartialPattern>();
  }

protected:
  /// Return the class immediate-members pattern.
  const GenericMetadataPartialPattern *class_getImmediateMembersPattern() const{
    assert(asSelf()->class_hasImmediateMembersPattern());
    return this->template getTrailingObjects<GenericMetadataPartialPattern>()
             + size_t(asSelf()->hasExtraDataPattern());
  }

  size_t numTrailingObjects(OverloadToken<GenericMetadataPartialPattern>) const{
    return size_t(asSelf()->hasExtraDataPattern())
         + size_t(asSelf()->class_hasImmediateMembersPattern());
  }
};

/// An instantiation pattern for generic class metadata.
template <typename Runtime>
struct TargetGenericClassMetadataPattern final :
       TargetGenericMetadataPattern<Runtime>,
       TargetGenericMetadataPatternTrailingObjects<Runtime,
         TargetGenericClassMetadataPattern<Runtime>> {
  using TrailingObjects =
       TargetGenericMetadataPatternTrailingObjects<Runtime,
         TargetGenericClassMetadataPattern<Runtime>>;
  friend TrailingObjects;

  using TargetGenericMetadataPattern<Runtime>::PatternFlags;

  /// The heap-destructor function.
  TargetCompactFunctionPointer<Runtime, HeapObjectDestroyer> Destroy;

  /// The ivar-destructor function.
  TargetCompactFunctionPointer<Runtime, ClassIVarDestroyer, /*nullable*/ true>
    IVarDestroyer;

  /// The class flags.
  ClassFlags Flags;

  // The following fields are only present in ObjC interop.

  /// The offset of the class RO-data within the extra data pattern,
  /// in words.
  uint16_t ClassRODataOffset;

  /// The offset of the metaclass object within the extra data pattern,
  /// in words.
  uint16_t MetaclassObjectOffset;

  /// The offset of the metaclass RO-data within the extra data pattern,
  /// in words.
  uint16_t MetaclassRODataOffset;

  uint16_t Reserved;

  bool hasImmediateMembersPattern() const {
    return PatternFlags.class_hasImmediateMembersPattern();
  }
  const GenericMetadataPartialPattern *getImmediateMembersPattern() const{
    return this->class_getImmediateMembersPattern();
  }

private:
  bool class_hasImmediateMembersPattern() const {
    return hasImmediateMembersPattern();
  }
};
using GenericClassMetadataPattern =
  TargetGenericClassMetadataPattern<InProcess>;

/// An instantiation pattern for generic value metadata.
template <typename Runtime>
struct TargetGenericValueMetadataPattern final :
       TargetGenericMetadataPattern<Runtime>,
       TargetGenericMetadataPatternTrailingObjects<Runtime,
         TargetGenericValueMetadataPattern<Runtime>> {
  using TrailingObjects =
       TargetGenericMetadataPatternTrailingObjects<Runtime,
         TargetGenericValueMetadataPattern<Runtime>>;
  friend TrailingObjects;

  using TargetGenericMetadataPattern<Runtime>::PatternFlags;

  /// The value-witness table.  Indirectable so that we can re-use tables
  /// from other libraries if that seems wise.
  TargetRelativeIndirectablePointer<Runtime, const ValueWitnessTable>
    ValueWitnesses;

  const ValueWitnessTable *getValueWitnessesPattern() const {
    return ValueWitnesses.get();
  }

  /// Return the metadata kind to use in the instantiation.
  MetadataKind getMetadataKind() const {
    return PatternFlags.value_getMetadataKind();
  }

private:
  bool class_hasImmediateMembersPattern() const {
    // It's important to not look at the flag because we use those
    // bits for other things.
    return false;
  }
};
using GenericValueMetadataPattern =
  TargetGenericValueMetadataPattern<InProcess>;

template <typename Runtime>
struct TargetTypeGenericContextDescriptorHeader {
  /// The metadata instantiation cache.
  TargetRelativeDirectPointer<Runtime,
                              TargetGenericMetadataInstantiationCache<Runtime>>
    InstantiationCache;

  GenericMetadataInstantiationCache *getInstantiationCache() const {
    return InstantiationCache.get();
  }

  /// The default instantiation pattern.
  TargetRelativeDirectPointer<Runtime, TargetGenericMetadataPattern<Runtime>>
    DefaultInstantiationPattern;

  /// The base header.  Must always be the final member.
  TargetGenericContextDescriptorHeader<Runtime> Base;
  
  operator const TargetGenericContextDescriptorHeader<Runtime> &() const {
    return Base;
  }
};
using TypeGenericContextDescriptorHeader =
  TargetTypeGenericContextDescriptorHeader<InProcess>;

/// Wrapper class for the pointer to a metadata access function that provides
/// operator() overloads to call it with the right calling convention.
class MetadataAccessFunction {
  MetadataResponse (*Function)(...);

  static_assert(NumDirectGenericTypeMetadataAccessFunctionArgs == 3,
                "Need to account for change in number of direct arguments");

public:
  explicit MetadataAccessFunction(MetadataResponse (*Function)(...))
    : Function(Function)
  {}
  
  explicit operator bool() const {
    return Function != nullptr;
  }

  /// For debugging purposes only.
  explicit operator void*() const {
    return reinterpret_cast<void *>(Function);
  }
  
  /// Invoke with an array of arguments of dynamic size.
  MetadataResponse operator()(MetadataRequest request,
                              llvm::ArrayRef<const void *> args) const {
    switch (args.size()) {
    case 0:
      return operator()(request);
    case 1:
      return operator()(request, args[0]);
    case 2:
      return operator()(request, args[0], args[1]);
    case 3:
      return operator()(request, args[0], args[1], args[2]);
    default:
      return applyMany(request, args.data());
    }
  }
  
  /// Invoke with exactly 0 arguments.
  MetadataResponse operator()(MetadataRequest request) const {
    using Fn0 = SWIFT_CC(swift) MetadataResponse(MetadataRequest request);
    return reinterpret_cast<Fn0*>(Function)(request);
  }

  /// Invoke with exactly 1 argument.
  MetadataResponse operator()(MetadataRequest request,
                              const void *arg0) const {
    using Fn1 = SWIFT_CC(swift) MetadataResponse(MetadataRequest request,
                                                 const void *arg0);
    return reinterpret_cast<Fn1*>(Function)(request, arg0);

  }

  /// Invoke with exactly 2 arguments.
  MetadataResponse operator()(MetadataRequest request,
                              const void *arg0,
                              const void *arg1) const {
    using Fn2 = SWIFT_CC(swift) MetadataResponse(MetadataRequest request,
                                                 const void *arg0,
                                                 const void *arg1);
    return reinterpret_cast<Fn2*>(Function)(request, arg0, arg1);
  }

  /// Invoke with exactly 3 arguments.
  MetadataResponse operator()(MetadataRequest request,
                              const void *arg0,
                              const void *arg1,
                              const void *arg2) const {
    using Fn3 = SWIFT_CC(swift) MetadataResponse(MetadataRequest request,
                                                 const void *arg0,
                                                 const void *arg1,
                                                 const void *arg2);
    return reinterpret_cast<Fn3*>(Function)(request, arg0, arg1, arg2);
  }
  
  /// Invoke with more than 3 arguments.
  template<typename...Args>
  MetadataResponse operator()(MetadataRequest request,
                              const void *arg0,
                              const void *arg1,
                              const void *arg2,
                              Args... argN) const {
    const void *args[] = { arg0, arg1, arg2, argN... };
    return applyMany(request, args);
  }

private:
  /// In the more-than-max case, just pass all the arguments as an array.
  MetadataResponse applyMany(MetadataRequest request,
                             const void * const *args) const {
    using FnN = SWIFT_CC(swift) MetadataResponse(MetadataRequest request,
                                                 const void * const *args);
    return reinterpret_cast<FnN*>(Function)(request, args);
  }
};

/// The control structure for performing non-trivial initialization of
/// singleton foreign metadata.
template <typename Runtime>
struct TargetForeignMetadataInitialization {
  /// The completion function.  The pattern will always be null.
  TargetCompactFunctionPointer<Runtime, MetadataCompleter, /*nullable*/ true>
    CompletionFunction;
};

/// The cache structure for non-trivial initialization of singleton value
/// metadata.
template <typename Runtime>
struct TargetSingletonMetadataCache {
  /// The metadata pointer.  Clients can do dependency-ordered loads
  /// from this, and if they see a non-zero value, it's a Complete
  /// metadata.
  std::atomic<TargetMetadataPointer<Runtime, TargetMetadata>> Metadata;

  /// The private cache data.
  std::atomic<TargetPointer<Runtime, void>> Private;
};
using SingletonMetadataCache =
  TargetSingletonMetadataCache<InProcess>;

template <typename Runtime>
struct TargetResilientClassMetadataPattern;

/// A function for allocating metadata for a resilient class, calculating
/// the correct metadata size at runtime.
using MetadataRelocator =
  Metadata *(const TargetTypeContextDescriptor<InProcess> *type,
             const TargetResilientClassMetadataPattern<InProcess> *pattern);

/// An instantiation pattern for non-generic resilient class metadata.
///
/// Used for classes with resilient ancestry, that is, where at least one
/// ancestor is defined in a different resilience domain.
///
/// The hasResilientSuperclass() flag in the class context descriptor is
/// set in this case, and hasSingletonMetadataInitialization() must be
/// set as well.
///
/// The pattern is referenced from the SingletonMetadataInitialization
/// record in the class context descriptor.
template <typename Runtime>
struct TargetResilientClassMetadataPattern {
  /// A function that allocates metadata with the correct size at runtime.
  ///
  /// If this is null, the runtime instead calls swift_relocateClassMetadata(),
  /// passing in the class descriptor and this pattern.
  TargetCompactFunctionPointer<Runtime, MetadataRelocator, /*nullable*/ true>
    RelocationFunction;

  /// The heap-destructor function.
  TargetCompactFunctionPointer<Runtime, HeapObjectDestroyer> Destroy;

  /// The ivar-destructor function.
  TargetCompactFunctionPointer<Runtime, ClassIVarDestroyer, /*nullable*/ true>
    IVarDestroyer;

  /// The class flags.
  ClassFlags Flags;

  // The following fields are only present in ObjC interop.

  /// Our ClassROData.
  TargetRelativeDirectPointer<Runtime, void> Data;

  /// Our metaclass.
  TargetRelativeDirectPointer<Runtime, TargetAnyClassMetadata<Runtime>> Metaclass;
};

using ResilientClassMetadataPattern =
  TargetResilientClassMetadataPattern<InProcess>;

/// The control structure for performing non-trivial initialization of
/// singleton value metadata, which is required when e.g. a non-generic
/// value type has a resilient component type.
template <typename Runtime>
struct TargetSingletonMetadataInitialization {
  /// The initialization cache.  Out-of-line because mutable.
  TargetRelativeDirectPointer<Runtime,
                              TargetSingletonMetadataCache<Runtime>>
    InitializationCache;

  union {
    /// The incomplete metadata, for structs, enums and classes without
    /// resilient ancestry.
    TargetRelativeDirectPointer<Runtime, TargetMetadata<Runtime>>
      IncompleteMetadata;

    /// If the class descriptor's hasResilientSuperclass() flag is set,
    /// this field instead points at a pattern used to allocate and
    /// initialize metadata for this class, since it's size and contents
    /// is not known at compile time.
    TargetRelativeDirectPointer<Runtime, TargetResilientClassMetadataPattern<Runtime>>
      ResilientPattern;
  };

  /// The completion function.  The pattern will always be null, even
  /// for a resilient class.
  TargetCompactFunctionPointer<Runtime, MetadataCompleter>
    CompletionFunction;

  bool hasResilientClassPattern(
      const TargetTypeContextDescriptor<Runtime> *description) const {
    auto *classDescription =
      dyn_cast<TargetClassDescriptor<Runtime>>(description);
    return (classDescription != nullptr &&
            classDescription->hasResilientSuperclass());
  }

  /// This method can only be called from the runtime itself. It is defined
  /// in Metadata.cpp.
  TargetMetadata<Runtime> *allocate(
      const TargetTypeContextDescriptor<Runtime> *description) const;
};

template <typename Runtime>
struct TargetCanonicalSpecializedMetadatasListCount {
  uint32_t count;
};

template <typename Runtime>
struct TargetCanonicalSpecializedMetadatasListEntry {
  TargetRelativeDirectPointer<Runtime, TargetMetadata<Runtime>, /*Nullable*/ false> metadata;
};

template <typename Runtime>
struct TargetCanonicalSpecializedMetadataAccessorsListEntry {
  TargetCompactFunctionPointer<Runtime, MetadataResponse(MetadataRequest), /*Nullable*/ false> accessor;
};

template <typename Runtime>
struct TargetCanonicalSpecializedMetadatasCachingOnceToken {
  TargetRelativeDirectPointer<Runtime, swift_once_t, /*Nullable*/ false> token;
};

template <typename Runtime>
class swift_ptrauth_struct_context_descriptor(TypeContextDescriptor)
    TargetTypeContextDescriptor : public TargetContextDescriptor<Runtime> {
public:
  /// The name of the type.
  TargetRelativeDirectPointer<Runtime, const char, /*nullable*/ false> Name;

  /// A pointer to the metadata access function for this type.
  ///
  /// The function type here is a stand-in. You should use getAccessFunction()
  /// to wrap the function pointer in an accessor that uses the proper calling
  /// convention for a given number of arguments.
  TargetCompactFunctionPointer<Runtime, MetadataResponse(...),
                              /*Nullable*/ true> AccessFunctionPtr;
  
  /// A pointer to the field descriptor for the type, if any.
  TargetRelativeDirectPointer<Runtime, const reflection::FieldDescriptor,
                              /*nullable*/ true> Fields;
      
  bool isReflectable() const { return (bool)Fields; }

  MetadataAccessFunction getAccessFunction() const {
    return MetadataAccessFunction(AccessFunctionPtr.get());
  }

  TypeContextDescriptorFlags getTypeContextDescriptorFlags() const {
    return TypeContextDescriptorFlags(this->Flags.getKindSpecificFlags());
  }

  /// Return the kind of metadata initialization required by this type.
  /// Note that this is only meaningful for non-generic types.
  TypeContextDescriptorFlags::MetadataInitializationKind
  getMetadataInitialization() const {
    return getTypeContextDescriptorFlags().getMetadataInitialization();
  }

  /// Does this type have non-trivial "singleton" metadata initialization?
  ///
  /// The type of the initialization-control structure differs by subclass,
  /// so it doesn't appear here.
  bool hasSingletonMetadataInitialization() const {
    return getTypeContextDescriptorFlags().hasSingletonMetadataInitialization();
  }

  /// Does this type have "foreign" metadata initialization?
  bool hasForeignMetadataInitialization() const {
    return getTypeContextDescriptorFlags().hasForeignMetadataInitialization();
  }

  bool hasCanonicalMetadataPrespecializations() const {
    return getTypeContextDescriptorFlags().hasCanonicalMetadataPrespecializations();
  }

  bool hasLayoutString() const {
    return getTypeContextDescriptorFlags().hasLayoutString();
  }

  /// Given that this type has foreign metadata initialization, return the
  /// control structure for it.
  const TargetForeignMetadataInitialization<Runtime> &
  getForeignMetadataInitialization() const;

  const TargetSingletonMetadataInitialization<Runtime> &
  getSingletonMetadataInitialization() const;

  const TargetTypeGenericContextDescriptorHeader<Runtime> &
  getFullGenericContextHeader() const;

  const TargetGenericContextDescriptorHeader<Runtime> &
  getGenericContextHeader() const {
    return getFullGenericContextHeader();
  }

  llvm::ArrayRef<GenericParamDescriptor> getGenericParams() const;

  /// Return the offset of the start of generic arguments in the nominal
  /// type's metadata. The returned value is measured in sizeof(StoredPointer).
  int32_t getGenericArgumentOffset() const;

  constexpr inline auto
  getNameOffset() const -> typename Runtime::StoredSize {
    return offsetof(typename std::remove_reference<decltype(*this)>::type, Name);
  }

  /// Return the start of the generic arguments array in the nominal
  /// type's metadata. The returned value is measured in sizeof(StoredPointer).
  const TargetMetadata<Runtime> * const *getGenericArguments(
                               const TargetMetadata<Runtime> *metadata) const {
    auto offset = getGenericArgumentOffset();
    auto words =
      reinterpret_cast<const TargetMetadata<Runtime> * const *>(metadata);
    return words + offset;
  }

  const llvm::ArrayRef<TargetRelativeDirectPointer<Runtime, TargetMetadata<Runtime>, /*Nullable*/ false>>
  getCanonicalMetadataPrespecializations() const;

  swift_once_t *getCanonicalMetadataPrespecializationCachingOnceToken() const;

  static bool classof(const TargetContextDescriptor<Runtime> *cd) {
    return cd->getKind() >= ContextDescriptorKind::Type_First
        && cd->getKind() <= ContextDescriptorKind::Type_Last;
  }
};

/// Storage for class metadata bounds.  This is the variable returned
/// by getAddrOfClassMetadataBounds in the compiler.
///
/// This storage is initialized before the allocation of any metadata
/// for the class to which it belongs.  In classes without resilient
/// superclasses, it is initialized statically with values derived
/// during compilation.  In classes with resilient superclasses, it
/// is initialized dynamically, generally during the allocation of
/// the first metadata of this class's type.  If metadata for this
/// class is available to you to use, you must have somehow synchronized
/// with the thread which allocated the metadata, and therefore the
/// complete initialization of this variable is also ordered before
/// your access.  That is why you can safely access this variable,
/// and moreover access it without further atomic accesses.  However,
/// since this variable may be accessed in a way that is not dependency-
/// ordered on the metadata pointer, it is important that you do a full
/// synchronization and not just a dependency-ordered (consume)
/// synchronization when sharing class metadata pointers between
/// threads.  (There are other reasons why this is true; for example,
/// field offset variables are also accessed without dependency-ordering.)
///
/// If you are accessing this storage without such a guarantee, you
/// should be aware that it may be lazily initialized, and moreover
/// it may be getting lazily initialized from another thread.  To ensure
/// correctness, the fields must be read in the correct order: the
/// immediate-members offset is initialized last with a store-release,
/// so it must be read first with a load-acquire, and if the result
/// is non-zero then the rest of the variable is known to be valid.
/// (No locking is required because racing initializations should always
/// assign the same values to the storage.)
template <typename Runtime>
struct TargetStoredClassMetadataBounds {
  using StoredPointerDifference =
    typename Runtime::StoredPointerDifference;

  /// The offset to the immediate members.  This value is in bytes so that
  /// clients don't have to sign-extend it.


  /// It is not necessary to use atomic-ordered loads when accessing this
  /// variable just to read the immediate-members offset when drilling to
  /// the immediate members of an already-allocated metadata object.
  /// The proper initialization of this variable is always ordered before
  /// any allocation of metadata for this class.
  std::atomic<StoredPointerDifference> ImmediateMembersOffset;

  /// The positive and negative bounds of the class metadata.
  TargetMetadataBounds<Runtime> Bounds;

  /// Attempt to read the cached immediate-members offset.
  ///
  /// \return true if the read was successful, or false if the cache hasn't
  ///   been filled yet
  bool tryGetImmediateMembersOffset(StoredPointerDifference &output) {
    output = ImmediateMembersOffset.load(std::memory_order_relaxed);
    return output != 0;
  }

  /// Attempt to read the full cached bounds.
  ///
  /// \return true if the read was successful, or false if the cache hasn't
  ///   been filled yet
  bool tryGet(TargetClassMetadataBounds<Runtime> &output) {
    auto offset = ImmediateMembersOffset.load(std::memory_order_acquire);
    if (offset == 0) return false;

    output.ImmediateMembersOffset = offset;
    output.NegativeSizeInWords = Bounds.NegativeSizeInWords;
    output.PositiveSizeInWords = Bounds.PositiveSizeInWords;
    return true;
  }

  void initialize(TargetClassMetadataBounds<Runtime> value) {
    assert(value.ImmediateMembersOffset != 0 &&
           "attempting to initialize metadata bounds cache to a zero state!");

    Bounds.NegativeSizeInWords = value.NegativeSizeInWords;
    Bounds.PositiveSizeInWords = value.PositiveSizeInWords;
    ImmediateMembersOffset.store(value.ImmediateMembersOffset,
                                 std::memory_order_release);
  }
};
using StoredClassMetadataBounds =
  TargetStoredClassMetadataBounds<InProcess>;

template <typename Runtime>
struct TargetResilientSuperclass {
  /// The superclass of this class.  This pointer can be interpreted
  /// using the superclass reference kind stored in the type context
  /// descriptor flags.  It is null if the class has no formal superclass.
  ///
  /// Note that SwiftObject, the implicit superclass of all Swift root
  /// classes when building with ObjC compatibility, does not appear here.
  TargetRelativeDirectPointer<Runtime, const void, /*nullable*/true> Superclass;
};

/// A structure that stores a reference to an Objective-C class stub.
///
/// This is not the class stub itself; it is part of a class context
/// descriptor.
template <typename Runtime>
struct TargetObjCResilientClassStubInfo {
  /// A relative pointer to an Objective-C resilient class stub.
  ///
  /// We do not declare a struct type for class stubs since the Swift runtime
  /// does not need to interpret them. The class stub struct is part of
  /// the Objective-C ABI, and is laid out as follows:
  /// - isa pointer, always 1
  /// - an update callback, of type 'Class (*)(Class *, objc_class_stub *)'
  ///
  /// Class stubs are used for two purposes:
  ///
  /// - Objective-C can reference class stubs when calling static methods.
  /// - Objective-C and Swift can reference class stubs when emitting
  ///   categories (in Swift, extensions with @objc members).
  TargetRelativeDirectPointer<Runtime, const void> Stub;
};

template <typename Runtime>
class swift_ptrauth_struct_context_descriptor(ClassDescriptor)
    TargetClassDescriptor final
    : public TargetTypeContextDescriptor<Runtime>,
      public TrailingGenericContextObjects<TargetClassDescriptor<Runtime>,
                              TargetTypeGenericContextDescriptorHeader,
                              /*additional trailing objects:*/
                              TargetResilientSuperclass<Runtime>,
                              TargetForeignMetadataInitialization<Runtime>,
                              TargetSingletonMetadataInitialization<Runtime>,
                              TargetVTableDescriptorHeader<Runtime>,
                              TargetMethodDescriptor<Runtime>,
                              TargetOverrideTableHeader<Runtime>,
                              TargetMethodOverrideDescriptor<Runtime>,
                              TargetObjCResilientClassStubInfo<Runtime>,
                              TargetCanonicalSpecializedMetadatasListCount<Runtime>,
                              TargetCanonicalSpecializedMetadatasListEntry<Runtime>,
                              TargetCanonicalSpecializedMetadataAccessorsListEntry<Runtime>,
                              TargetCanonicalSpecializedMetadatasCachingOnceToken<Runtime>> {
private:
  using TrailingGenericContextObjects =
    swift::TrailingGenericContextObjects<TargetClassDescriptor<Runtime>,
                                         TargetTypeGenericContextDescriptorHeader,
                                         TargetResilientSuperclass<Runtime>,
                                         TargetForeignMetadataInitialization<Runtime>,
                                         TargetSingletonMetadataInitialization<Runtime>,
                                         TargetVTableDescriptorHeader<Runtime>,
                                         TargetMethodDescriptor<Runtime>,
                                         TargetOverrideTableHeader<Runtime>,
                                         TargetMethodOverrideDescriptor<Runtime>,
                                         TargetObjCResilientClassStubInfo<Runtime>,
                                         TargetCanonicalSpecializedMetadatasListCount<Runtime>,
                                         TargetCanonicalSpecializedMetadatasListEntry<Runtime>,
                                         TargetCanonicalSpecializedMetadataAccessorsListEntry<Runtime>,
                                         TargetCanonicalSpecializedMetadatasCachingOnceToken<Runtime>>;

  using TrailingObjects =
    typename TrailingGenericContextObjects::TrailingObjects;
  friend TrailingObjects;

public:
  using MethodDescriptor = TargetMethodDescriptor<Runtime>;
  using VTableDescriptorHeader = TargetVTableDescriptorHeader<Runtime>;
  using OverrideTableHeader = TargetOverrideTableHeader<Runtime>;
  using MethodOverrideDescriptor = TargetMethodOverrideDescriptor<Runtime>;
  using ResilientSuperclass = TargetResilientSuperclass<Runtime>;
  using ForeignMetadataInitialization =
    TargetForeignMetadataInitialization<Runtime>;
  using SingletonMetadataInitialization =
    TargetSingletonMetadataInitialization<Runtime>;
  using ObjCResilientClassStubInfo =
    TargetObjCResilientClassStubInfo<Runtime>;
  using Metadata =
    TargetRelativeDirectPointer<Runtime, TargetMetadata<Runtime>, /*Nullable*/ false>;
  using MetadataListCount =
    TargetCanonicalSpecializedMetadatasListCount<Runtime>;
  using MetadataListEntry = 
    TargetCanonicalSpecializedMetadatasListEntry<Runtime>;
  using MetadataAccessor = 
    TargetCompactFunctionPointer<Runtime, MetadataResponse(MetadataRequest), /*Nullable*/ false>;
  using MetadataAccessorListEntry =
      TargetCanonicalSpecializedMetadataAccessorsListEntry<Runtime>;
  using MetadataCachingOnceToken =
      TargetCanonicalSpecializedMetadatasCachingOnceToken<Runtime>;

  using StoredPointer = typename Runtime::StoredPointer;
  using StoredPointerDifference = typename Runtime::StoredPointerDifference;
  using StoredSize = typename Runtime::StoredSize;

  using TrailingGenericContextObjects::getGenericContext;
  using TrailingGenericContextObjects::getGenericContextHeader;
  using TrailingGenericContextObjects::getFullGenericContextHeader;
  using TrailingGenericContextObjects::getGenericParams;
  using TargetTypeContextDescriptor<Runtime>::getTypeContextDescriptorFlags;

  TypeReferenceKind getResilientSuperclassReferenceKind() const {
    return getTypeContextDescriptorFlags()
      .class_getResilientSuperclassReferenceKind();
  }

  /// The type of the superclass, expressed as a mangled type name that can
  /// refer to the generic arguments of the subclass type.
  TargetRelativeDirectPointer<Runtime, const char> SuperclassType;

  union {
    /// If this descriptor does not have a resilient superclass, this is the
    /// negative size of metadata objects of this class (in words).
    uint32_t MetadataNegativeSizeInWords;

    /// If this descriptor has a resilient superclass, this is a reference
    /// to a cache holding the metadata's extents.
    TargetRelativeDirectPointer<Runtime,
                                TargetStoredClassMetadataBounds<Runtime>>
      ResilientMetadataBounds;
  };

  union {
    /// If this descriptor does not have a resilient superclass, this is the
    /// positive size of metadata objects of this class (in words).
    uint32_t MetadataPositiveSizeInWords;

    /// Otherwise, these flags are used to do things like indicating
    /// the presence of an Objective-C resilient class stub.
    ExtraClassDescriptorFlags ExtraClassFlags;
  };

  /// The number of additional members added by this class to the class
  /// metadata.  This data is opaque by default to the runtime, other than
  /// as exposed in other members; it's really just
  /// NumImmediateMembers * sizeof(void*) bytes of data.
  ///
  /// Whether those bytes are added before or after the address point
  /// depends on areImmediateMembersNegative().
  uint32_t NumImmediateMembers; // ABI: could be uint16_t?

  StoredSize getImmediateMembersSize() const {
    return StoredSize(NumImmediateMembers) * sizeof(StoredPointer);
  }

  /// Are the immediate members of the class metadata allocated at negative
  /// offsets instead of positive?
  bool areImmediateMembersNegative() const {
    return getTypeContextDescriptorFlags().class_areImmediateMembersNegative();
  }

  /// The number of stored properties in the class, not including its
  /// superclasses. If there is a field offset vector, this is its length.
  uint32_t NumFields;

private:
  /// The offset of the field offset vector for this class's stored
  /// properties in its metadata, in words. 0 means there is no field offset
  /// vector.
  ///
  /// If this class has a resilient superclass, this offset is relative to
  /// the size of the resilient superclass metadata. Otherwise, it is
  /// absolute.
  uint32_t FieldOffsetVectorOffset;

  template<typename T>
  using OverloadToken =
    typename TrailingGenericContextObjects::template OverloadToken<T>;
  
  using TrailingGenericContextObjects::numTrailingObjects;

  size_t numTrailingObjects(OverloadToken<ResilientSuperclass>) const {
    return this->hasResilientSuperclass() ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<ForeignMetadataInitialization>) const{
    return this->hasForeignMetadataInitialization() ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<SingletonMetadataInitialization>) const{
    return this->hasSingletonMetadataInitialization() ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<VTableDescriptorHeader>) const {
    return hasVTable() ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<MethodDescriptor>) const {
    if (!hasVTable())
      return 0;

    return getVTableDescriptor()->VTableSize;
  }

  size_t numTrailingObjects(OverloadToken<OverrideTableHeader>) const {
    return hasOverrideTable() ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<MethodOverrideDescriptor>) const {
    if (!hasOverrideTable())
      return 0;

    return getOverrideTable()->NumEntries;
  }

  size_t numTrailingObjects(OverloadToken<ObjCResilientClassStubInfo>) const {
    return hasObjCResilientClassStub() ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<MetadataListCount>) const {
    return this->hasCanonicalMetadataPrespecializations() ?
      1
      : 0;
  }

  size_t numTrailingObjects(OverloadToken<MetadataListEntry>) const {
    return this->hasCanonicalMetadataPrespecializations() ?
      this->template getTrailingObjects<MetadataListCount>()->count
      : 0;
  }

  size_t numTrailingObjects(OverloadToken<MetadataAccessorListEntry>) const {
    return this->hasCanonicalMetadataPrespecializations() ?
      this->template getTrailingObjects<MetadataListCount>()->count
      : 0;
  }

  size_t numTrailingObjects(OverloadToken<MetadataCachingOnceToken>) const {
    return this->hasCanonicalMetadataPrespecializations() ? 1 : 0;
  }

public:
  const TargetRelativeDirectPointer<Runtime, const void, /*nullable*/true> &
  getResilientSuperclass() const {
    assert(this->hasResilientSuperclass());
    return this->template getTrailingObjects<ResilientSuperclass>()->Superclass;
  }

  const ForeignMetadataInitialization &getForeignMetadataInitialization() const{
    assert(this->hasForeignMetadataInitialization());
    return *this->template getTrailingObjects<ForeignMetadataInitialization>();
  }

  const SingletonMetadataInitialization &getSingletonMetadataInitialization() const{
    assert(this->hasSingletonMetadataInitialization());
    return *this->template getTrailingObjects<SingletonMetadataInitialization>();
  }

  /// True if metadata records for this type have a field offset vector for
  /// its stored properties.
  bool hasFieldOffsetVector() const { return FieldOffsetVectorOffset != 0; }

  unsigned getFieldOffsetVectorOffset() const {
    if (hasResilientSuperclass()) {
      auto bounds = getMetadataBounds();
      return (bounds.ImmediateMembersOffset / sizeof(StoredPointer)
              + FieldOffsetVectorOffset);
    }

    return FieldOffsetVectorOffset;
  }

  bool isActor() const {
    return this->getTypeContextDescriptorFlags().class_isActor();
  }

  bool isDefaultActor() const {
    return this->getTypeContextDescriptorFlags().class_isDefaultActor();
  }

  bool hasVTable() const {
    return this->getTypeContextDescriptorFlags().class_hasVTable();
  }

  bool hasOverrideTable() const {
    return this->getTypeContextDescriptorFlags().class_hasOverrideTable();
  }

  bool hasResilientSuperclass() const {
    return this->getTypeContextDescriptorFlags().class_hasResilientSuperclass();
  }
  
  const VTableDescriptorHeader *getVTableDescriptor() const {
    if (!hasVTable())
      return nullptr;
    return this->template getTrailingObjects<VTableDescriptorHeader>();
  }

  llvm::ArrayRef<MethodDescriptor> getMethodDescriptors() const {
    if (!hasVTable())
      return {};
    return {this->template getTrailingObjects<MethodDescriptor>(),
            numTrailingObjects(OverloadToken<MethodDescriptor>{})};
  }

  const OverrideTableHeader *getOverrideTable() const {
    if (!hasOverrideTable())
      return nullptr;
    return this->template getTrailingObjects<OverrideTableHeader>();
  }

  llvm::ArrayRef<MethodOverrideDescriptor> getMethodOverrideDescriptors() const {
    if (!hasOverrideTable())
      return {};
    return {this->template getTrailingObjects<MethodOverrideDescriptor>(),
            numTrailingObjects(OverloadToken<MethodOverrideDescriptor>{})};
  }

  /// Return the bounds of this class's metadata.
  TargetClassMetadataBounds<Runtime> getMetadataBounds() const {
    if (!hasResilientSuperclass())
      return getNonResilientMetadataBounds();

    // This lookup works by ADL and will intentionally fail for
    // non-InProcess instantiations.
    return getResilientMetadataBounds(this);
  }

  /// Given that this class is known to not have a resilient superclass
  /// return its metadata bounds.
  TargetClassMetadataBounds<Runtime> getNonResilientMetadataBounds() const {
    return { getNonResilientImmediateMembersOffset()
               * StoredPointerDifference(sizeof(void*)),
             MetadataNegativeSizeInWords,
             MetadataPositiveSizeInWords };
  }

  /// Return the offset of the start of generic arguments in the nominal
  /// type's metadata. The returned value is measured in words.
  int32_t getGenericArgumentOffset() const {
    if (!hasResilientSuperclass())
      return getNonResilientGenericArgumentOffset();

    // This lookup works by ADL and will intentionally fail for
    // non-InProcess instantiations.
    return getResilientImmediateMembersOffset(this);
  }

  /// Given that this class is known to not have a resilient superclass,
  /// return the offset of its generic arguments in words.
  int32_t getNonResilientGenericArgumentOffset() const {
    return getNonResilientImmediateMembersOffset();
  }

  /// Given that this class is known to not have a resilient superclass,
  /// return the offset of its immediate members in words.
  int32_t getNonResilientImmediateMembersOffset() const {
    assert(!hasResilientSuperclass());
    return areImmediateMembersNegative()
             ? -int32_t(MetadataNegativeSizeInWords)
             : int32_t(MetadataPositiveSizeInWords - NumImmediateMembers);
  }

  void *getMethod(unsigned i) const {
    assert(hasVTable()
           && i < numTrailingObjects(OverloadToken<MethodDescriptor>{}));
    return getMethodDescriptors()[i].Impl.get();
  }

  /// Whether this context descriptor references an Objective-C resilient
  /// class stub. See the above description of TargetObjCResilientClassStubInfo
  /// for details.
  bool hasObjCResilientClassStub() const {
    if (!hasResilientSuperclass())
      return false;
    return ExtraClassFlags.hasObjCResilientClassStub();
  }

  const void *getObjCResilientClassStub() const {
    if (!hasObjCResilientClassStub())
      return nullptr;

    return this->template getTrailingObjects<ObjCResilientClassStubInfo>()
      ->Stub.get();
  }

  llvm::ArrayRef<Metadata> getCanonicalMetadataPrespecializations() const {
    if (!this->hasCanonicalMetadataPrespecializations()) {
      return {};
    }

    auto *listCount = this->template getTrailingObjects<MetadataListCount>();
    auto *list = this->template getTrailingObjects<MetadataListEntry>();
    return llvm::ArrayRef<Metadata>(
        reinterpret_cast<const Metadata *>(list),
        listCount->count
        );
  }

  llvm::ArrayRef<MetadataAccessor> getCanonicalMetadataPrespecializationAccessors() const {
    if (!this->hasCanonicalMetadataPrespecializations()) {
      return {};
    }

    auto *listCount = this->template getTrailingObjects<MetadataListCount>();
    auto *list = this->template getTrailingObjects<MetadataAccessorListEntry>();
    return llvm::ArrayRef<MetadataAccessor>(
        reinterpret_cast<const MetadataAccessor *>(list),
        listCount->count
        );
  }

  swift_once_t *getCanonicalMetadataPrespecializationCachingOnceToken() const {
    if (!this->hasCanonicalMetadataPrespecializations()) {
      return nullptr;
    }
    auto box = this->template getTrailingObjects<MetadataCachingOnceToken>();
    return box->token.get();
  }

  static bool classof(const TargetContextDescriptor<Runtime> *cd) {
    return cd->getKind() == ContextDescriptorKind::Class;
  }
};

using ClassDescriptor = TargetClassDescriptor<InProcess>;

template <typename Runtime>
class swift_ptrauth_struct_context_descriptor(ValueTypeDescriptor)
    TargetValueTypeDescriptor : public TargetTypeContextDescriptor<Runtime>{
public:
  static bool classof(const TargetContextDescriptor<Runtime> *cd) {
    return cd->getKind() == ContextDescriptorKind::Struct ||
           cd->getKind() == ContextDescriptorKind::Enum;
  }
};
using ValueTypeDescriptor = TargetValueTypeDescriptor<InProcess>;

template <typename Runtime>
class swift_ptrauth_struct_context_descriptor(StructDescriptor)
    TargetStructDescriptor final
    : public TargetValueTypeDescriptor<Runtime>,
      public TrailingGenericContextObjects<TargetStructDescriptor<Runtime>,
                            TargetTypeGenericContextDescriptorHeader,
                            /*additional trailing objects*/
                            TargetForeignMetadataInitialization<Runtime>,
                            TargetSingletonMetadataInitialization<Runtime>,
                            TargetCanonicalSpecializedMetadatasListCount<Runtime>,
                            TargetCanonicalSpecializedMetadatasListEntry<Runtime>,
                            TargetCanonicalSpecializedMetadatasCachingOnceToken<Runtime>> {
public:
  using ForeignMetadataInitialization =
    TargetForeignMetadataInitialization<Runtime>;
  using SingletonMetadataInitialization =
    TargetSingletonMetadataInitialization<Runtime>;
  using Metadata =
    TargetRelativeDirectPointer<Runtime, TargetMetadata<Runtime>, /*Nullable*/ false>;
  using MetadataListCount =
    TargetCanonicalSpecializedMetadatasListCount<Runtime>;
  using MetadataListEntry =
    TargetCanonicalSpecializedMetadatasListEntry<Runtime>;
  using MetadataCachingOnceToken =
      TargetCanonicalSpecializedMetadatasCachingOnceToken<Runtime>;

private:
  using TrailingGenericContextObjects =
      swift::TrailingGenericContextObjects<TargetStructDescriptor<Runtime>,
                                           TargetTypeGenericContextDescriptorHeader,
                                           ForeignMetadataInitialization,
                                           SingletonMetadataInitialization,
                                           MetadataListCount,
                                           MetadataListEntry,
                                           MetadataCachingOnceToken>;

  using TrailingObjects =
    typename TrailingGenericContextObjects::TrailingObjects;
  friend TrailingObjects;

  template<typename T>
  using OverloadToken = typename TrailingObjects::template OverloadToken<T>;
  using TrailingGenericContextObjects::numTrailingObjects;

  size_t numTrailingObjects(OverloadToken<ForeignMetadataInitialization>) const{
    return this->hasForeignMetadataInitialization() ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<SingletonMetadataInitialization>) const{
    return this->hasSingletonMetadataInitialization() ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<MetadataListCount>) const {
    return this->hasCanonicalMetadataPrespecializations() ?
      1
      : 0;
  }

  size_t numTrailingObjects(OverloadToken<MetadataListEntry>) const {
    return this->hasCanonicalMetadataPrespecializations() ?
      this->template getTrailingObjects<MetadataListCount>()->count
      : 0;
  }

  size_t numTrailingObjects(OverloadToken<MetadataCachingOnceToken>) const {
    return this->hasCanonicalMetadataPrespecializations() ? 1 : 0;
  }

public:
  using TrailingGenericContextObjects::getGenericContext;
  using TrailingGenericContextObjects::getGenericContextHeader;
  using TrailingGenericContextObjects::getFullGenericContextHeader;
  using TrailingGenericContextObjects::getGenericParams;

  /// The number of stored properties in the struct.
  /// If there is a field offset vector, this is its length.
  uint32_t NumFields;
  /// The offset of the field offset vector for this struct's stored
  /// properties in its metadata, if any. 0 means there is no field offset
  /// vector.
  uint32_t FieldOffsetVectorOffset;
  
  /// True if metadata records for this type have a field offset vector for
  /// its stored properties.
  bool hasFieldOffsetVector() const { return FieldOffsetVectorOffset != 0; }

  const ForeignMetadataInitialization &getForeignMetadataInitialization() const{
    assert(this->hasForeignMetadataInitialization());
    return *this->template getTrailingObjects<ForeignMetadataInitialization>();
  }

  const SingletonMetadataInitialization &getSingletonMetadataInitialization() const{
    assert(this->hasSingletonMetadataInitialization());
    return *this->template getTrailingObjects<SingletonMetadataInitialization>();
  }

  static constexpr int32_t getGenericArgumentOffset() {
    return TargetStructMetadata<Runtime>::getGenericArgumentOffset();
  }

  llvm::ArrayRef<Metadata> getCanonicalMetadataPrespecializations() const {
    if (!this->hasCanonicalMetadataPrespecializations()) {
      return {};
    }

    auto *listCount = this->template getTrailingObjects<MetadataListCount>();
    auto *list = this->template getTrailingObjects<MetadataListEntry>();
    return llvm::ArrayRef<Metadata>(
        reinterpret_cast<const Metadata *>(list),
        listCount->count
        );
  }

  swift_once_t *getCanonicalMetadataPrespecializationCachingOnceToken() const {
    if (!this->hasCanonicalMetadataPrespecializations()) {
      return nullptr;
    }
    auto box = this->template getTrailingObjects<MetadataCachingOnceToken>();
    return box->token.get();
  }

  static bool classof(const TargetContextDescriptor<Runtime> *cd) {
    return cd->getKind() == ContextDescriptorKind::Struct;
  }
};

using StructDescriptor = TargetStructDescriptor<InProcess>;

template <typename Runtime>
class swift_ptrauth_struct_context_descriptor(EnumDescriptor)
    TargetEnumDescriptor final
    : public TargetValueTypeDescriptor<Runtime>,
      public TrailingGenericContextObjects<TargetEnumDescriptor<Runtime>,
                            TargetTypeGenericContextDescriptorHeader,
                            /*additional trailing objects*/
                            TargetForeignMetadataInitialization<Runtime>,
                            TargetSingletonMetadataInitialization<Runtime>,
                            TargetCanonicalSpecializedMetadatasListCount<Runtime>,
                            TargetCanonicalSpecializedMetadatasListEntry<Runtime>,
                            TargetCanonicalSpecializedMetadatasCachingOnceToken<Runtime>> {
public:
  using SingletonMetadataInitialization =
    TargetSingletonMetadataInitialization<Runtime>;
  using ForeignMetadataInitialization =
    TargetForeignMetadataInitialization<Runtime>;
  using Metadata =
    TargetRelativeDirectPointer<Runtime, TargetMetadata<Runtime>, /*Nullable*/ false>;
  using MetadataListCount =
    TargetCanonicalSpecializedMetadatasListCount<Runtime>;
  using MetadataListEntry =
    TargetCanonicalSpecializedMetadatasListEntry<Runtime>;
  using MetadataCachingOnceToken =
      TargetCanonicalSpecializedMetadatasCachingOnceToken<Runtime>;

private:
  using TrailingGenericContextObjects =
    swift::TrailingGenericContextObjects<TargetEnumDescriptor<Runtime>,
                                        TargetTypeGenericContextDescriptorHeader,
                                        ForeignMetadataInitialization,
                                        SingletonMetadataInitialization,
                                        MetadataListCount,
                                        MetadataListEntry, 
                                        MetadataCachingOnceToken>;

  using TrailingObjects =
    typename TrailingGenericContextObjects::TrailingObjects;
  friend TrailingObjects;

  template<typename T>
  using OverloadToken = typename TrailingObjects::template OverloadToken<T>;
  using TrailingGenericContextObjects::numTrailingObjects;

  size_t numTrailingObjects(OverloadToken<ForeignMetadataInitialization>) const{
    return this->hasForeignMetadataInitialization() ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<SingletonMetadataInitialization>) const{
    return this->hasSingletonMetadataInitialization() ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<MetadataListCount>) const {
    return this->hasCanonicalMetadataPrespecializations() ?
      1
      : 0;
  }

  size_t numTrailingObjects(OverloadToken<MetadataListEntry>) const {
    return this->hasCanonicalMetadataPrespecializations() ?
      this->template getTrailingObjects<MetadataListCount>()->count
      : 0;
  }

  size_t numTrailingObjects(OverloadToken<MetadataCachingOnceToken>) const {
    return this->hasCanonicalMetadataPrespecializations() ? 1 : 0;
  }

public:
  using TrailingGenericContextObjects::getGenericContext;
  using TrailingGenericContextObjects::getGenericContextHeader;
  using TrailingGenericContextObjects::getFullGenericContextHeader;
  using TrailingGenericContextObjects::getGenericParams;

  /// The number of non-empty cases in the enum are in the low 24 bits;
  /// the offset of the payload size in the metadata record in words,
  /// if any, is stored in the high 8 bits.
  uint32_t NumPayloadCasesAndPayloadSizeOffset;

  /// The number of empty cases in the enum.
  uint32_t NumEmptyCases;

  uint32_t getNumPayloadCases() const {
    return NumPayloadCasesAndPayloadSizeOffset & 0x00FFFFFFU;
  }

  uint32_t getNumEmptyCases() const {
    return NumEmptyCases;
  }
  uint32_t getNumCases() const {
    return getNumPayloadCases() + NumEmptyCases;
  }
  size_t getPayloadSizeOffset() const {
    return ((NumPayloadCasesAndPayloadSizeOffset & 0xFF000000U) >> 24);
  }
  
  bool hasPayloadSizeOffset() const {
    return getPayloadSizeOffset() != 0;
  }

  static constexpr int32_t getGenericArgumentOffset() {
    return TargetEnumMetadata<Runtime>::getGenericArgumentOffset();
  }

  const ForeignMetadataInitialization &getForeignMetadataInitialization() const{
    assert(this->hasForeignMetadataInitialization());
    return *this->template getTrailingObjects<ForeignMetadataInitialization>();
  }

  const SingletonMetadataInitialization &getSingletonMetadataInitialization() const{
    assert(this->hasSingletonMetadataInitialization());
    return *this->template getTrailingObjects<SingletonMetadataInitialization>();
  }

  llvm::ArrayRef<Metadata> getCanonicalMetadataPrespecializations() const {
    if (!this->hasCanonicalMetadataPrespecializations()) {
      return {};
    }

    auto *listCount = this->template getTrailingObjects<MetadataListCount>();
    auto *list = this->template getTrailingObjects<MetadataListEntry>();
    return llvm::ArrayRef<Metadata>(
        reinterpret_cast<const Metadata *>(list),
        listCount->count
        );
  }

  swift_once_t *getCanonicalMetadataPrespecializationCachingOnceToken() const {
    if (!this->hasCanonicalMetadataPrespecializations()) {
      return nullptr;
    }
    auto box = this->template getTrailingObjects<MetadataCachingOnceToken>();
    return box->token.get();
  }

  static bool classof(const TargetContextDescriptor<Runtime> *cd) {
    return cd->getKind() == ContextDescriptorKind::Enum;
  }

#ifndef NDEBUG
  [[deprecated("Only meant for use in the debugger")]] void dump() const;
#endif
};

using EnumDescriptor = TargetEnumDescriptor<InProcess>;

template<typename Runtime>
inline const TargetGenericContext<Runtime> *
TargetContextDescriptor<Runtime>::getGenericContext() const {
  if (!isGeneric())
    return nullptr;

  switch (getKind()) {
  case ContextDescriptorKind::Module:
    // Never generic.
    return nullptr;
  case ContextDescriptorKind::Extension:
    return llvm::cast<TargetExtensionContextDescriptor<Runtime>>(this)
      ->getGenericContext();
  case ContextDescriptorKind::Anonymous:
    return llvm::cast<TargetAnonymousContextDescriptor<Runtime>>(this)
      ->getGenericContext();
  case ContextDescriptorKind::Class:
    return llvm::cast<TargetClassDescriptor<Runtime>>(this)
        ->getGenericContext();
  case ContextDescriptorKind::Enum:
    return llvm::cast<TargetEnumDescriptor<Runtime>>(this)
        ->getGenericContext();
  case ContextDescriptorKind::Struct:
    return llvm::cast<TargetStructDescriptor<Runtime>>(this)
        ->getGenericContext();
  case ContextDescriptorKind::OpaqueType:
    return llvm::cast<TargetOpaqueTypeDescriptor<Runtime>>(this)
        ->getGenericContext();
  default:    
    // We don't know about this kind of descriptor.
    return nullptr;
  }
}

template <typename Runtime>
int32_t TargetTypeContextDescriptor<Runtime>::getGenericArgumentOffset() const {
  switch (this->getKind()) {
  case ContextDescriptorKind::Class:
    return llvm::cast<TargetClassDescriptor<Runtime>>(this)
        ->getGenericArgumentOffset();
  case ContextDescriptorKind::Enum:
    return llvm::cast<TargetEnumDescriptor<Runtime>>(this)
        ->getGenericArgumentOffset();
  case ContextDescriptorKind::Struct:
    return llvm::cast<TargetStructDescriptor<Runtime>>(this)
        ->getGenericArgumentOffset();
  default:
    swift_unreachable("Not a type context descriptor.");
  }
}

template <typename Runtime>
const TargetTypeGenericContextDescriptorHeader<Runtime> &
TargetTypeContextDescriptor<Runtime>::getFullGenericContextHeader() const {
  switch (this->getKind()) {
  case ContextDescriptorKind::Class:
    return llvm::cast<TargetClassDescriptor<Runtime>>(this)
        ->getFullGenericContextHeader();
  case ContextDescriptorKind::Enum:
    return llvm::cast<TargetEnumDescriptor<Runtime>>(this)
        ->getFullGenericContextHeader();
  case ContextDescriptorKind::Struct:
    return llvm::cast<TargetStructDescriptor<Runtime>>(this)
        ->getFullGenericContextHeader();
  default:
    swift_unreachable("Not a type context descriptor.");
  }
}

template <typename Runtime>
llvm::ArrayRef<GenericParamDescriptor> 
TargetTypeContextDescriptor<Runtime>::getGenericParams() const {
  switch (this->getKind()) {
  case ContextDescriptorKind::Class:
    return llvm::cast<TargetClassDescriptor<Runtime>>(this)->getGenericParams();
  case ContextDescriptorKind::Enum:
    return llvm::cast<TargetEnumDescriptor<Runtime>>(this)->getGenericParams();
  case ContextDescriptorKind::Struct:
    return llvm::cast<TargetStructDescriptor<Runtime>>(this)->getGenericParams();
  case ContextDescriptorKind::OpaqueType:
    return llvm::cast<TargetOpaqueTypeDescriptor<Runtime>>(this)->getGenericParams();
  default:
    swift_unreachable("Not a type context descriptor.");
  }
}

template <typename Runtime>
const TargetForeignMetadataInitialization<Runtime> &
TargetTypeContextDescriptor<Runtime>::getForeignMetadataInitialization() const {
  switch (this->getKind()) {
  case ContextDescriptorKind::Class:
    return llvm::cast<TargetClassDescriptor<Runtime>>(this)
        ->getForeignMetadataInitialization();
  case ContextDescriptorKind::Enum:
    return llvm::cast<TargetEnumDescriptor<Runtime>>(this)
        ->getForeignMetadataInitialization();
  case ContextDescriptorKind::Struct:
    return llvm::cast<TargetStructDescriptor<Runtime>>(this)
        ->getForeignMetadataInitialization();
  default:
    swift_unreachable("Not a type context descriptor.");
  }
}

template<typename Runtime>
inline const TargetSingletonMetadataInitialization<Runtime> &
TargetTypeContextDescriptor<Runtime>::getSingletonMetadataInitialization() const {
  switch (this->getKind()) {
  case ContextDescriptorKind::Enum:
    return llvm::cast<TargetEnumDescriptor<Runtime>>(this)
        ->getSingletonMetadataInitialization();
  case ContextDescriptorKind::Struct:
    return llvm::cast<TargetStructDescriptor<Runtime>>(this)
        ->getSingletonMetadataInitialization();
  case ContextDescriptorKind::Class:
    return llvm::cast<TargetClassDescriptor<Runtime>>(this)
        ->getSingletonMetadataInitialization();
  default:
    swift_unreachable("Not a enum, struct or class type descriptor.");
  }
}

template<typename Runtime>
inline const llvm::ArrayRef<TargetRelativeDirectPointer<Runtime, TargetMetadata<Runtime>, /*Nullable*/ false>>
TargetTypeContextDescriptor<Runtime>::getCanonicalMetadataPrespecializations() const {
  switch (this->getKind()) {
  case ContextDescriptorKind::Enum:
    return llvm::cast<TargetEnumDescriptor<Runtime>>(this)
        ->getCanonicalMetadataPrespecializations();
  case ContextDescriptorKind::Struct:
    return llvm::cast<TargetStructDescriptor<Runtime>>(this)
        ->getCanonicalMetadataPrespecializations();
  case ContextDescriptorKind::Class:
    return llvm::cast<TargetClassDescriptor<Runtime>>(this)
        ->getCanonicalMetadataPrespecializations();
  default:
    swift_unreachable("Not a type context descriptor.");
  }
}

template <typename Runtime>
inline swift_once_t *TargetTypeContextDescriptor<
    Runtime>::getCanonicalMetadataPrespecializationCachingOnceToken() const {
  switch (this->getKind()) {
  case ContextDescriptorKind::Enum:
    return llvm::cast<TargetEnumDescriptor<Runtime>>(this)
        ->getCanonicalMetadataPrespecializationCachingOnceToken();
  case ContextDescriptorKind::Struct:
    return llvm::cast<TargetStructDescriptor<Runtime>>(this)
        ->getCanonicalMetadataPrespecializationCachingOnceToken();
  case ContextDescriptorKind::Class:
    return llvm::cast<TargetClassDescriptor<Runtime>>(this)
        ->getCanonicalMetadataPrespecializationCachingOnceToken();
  default:
    swift_unreachable("Not a type context descriptor.");
  }
}

/// An entry in the chain of dynamic replacement functions.
struct DynamicReplacementChainEntry {
  void *implementationFunction;
  DynamicReplacementChainEntry *next;
};

/// A record describing the root of dynamic replacements for a function.
struct DynamicReplacementKey {
  RelativeDirectPointer<DynamicReplacementChainEntry, false> root;
  uint32_t flags;

  uint16_t getExtraDiscriminator() const {
    return flags & 0x0000FFFF;
  }
  bool isAsync() const {
    return ((flags >> 16 ) & 0x1);
  }
};

/// A record describing a dynamic function replacement.
class DynamicReplacementDescriptor {
  RelativeIndirectablePointer<
      const DynamicReplacementKey, false, int32_t,
      TargetSignedPointer<InProcess,
                          DynamicReplacementKey *
                              __ptrauth_swift_dynamic_replacement_key>>
      replacedFunctionKey;
  union {
    TargetCompactFunctionPointer<InProcess, void, false> replacementFunction;
    TargetRelativeDirectPointer<InProcess, void, false> replacementAsyncFunction;
  };
  RelativeDirectPointer<DynamicReplacementChainEntry, false> chainEntry;
  uint32_t flags;

  enum : uint32_t { EnableChainingMask = 0x1 };

  void *getReplacementFunction() const {
    if (replacedFunctionKey->isAsync()) {
      return replacementAsyncFunction.get();
    } else {
      return replacementFunction.get();
    }
  }

public:
  /// Enable this replacement by changing the function's replacement chain's
  /// root entry.
  /// This replacement must be done while holding a global lock that guards this
  /// function's chain. Currently this is done by holding the
  /// \p DynamicReplacementLock.
  void enableReplacement() const;

  /// Disable this replacement by changing the function's replacement chain's
  /// root entry.
  /// This replacement must be done while holding a global lock that guards this
  /// function's chain. Currently this is done by holding the
  /// \p DynamicReplacementLock.
  void disableReplacement() const;

  uint32_t getFlags() const { return flags; }

  bool shouldChain() const { return (flags & EnableChainingMask); }
};

/// A collection of dynamic replacement records.
class DynamicReplacementScope
    : private swift::ABI::TrailingObjects<DynamicReplacementScope,
                                          DynamicReplacementDescriptor> {

  uint32_t flags;
  uint32_t numReplacements;

  using TrailingObjects =
      swift::ABI::TrailingObjects<DynamicReplacementScope,
                                  DynamicReplacementDescriptor>;
  friend TrailingObjects;

  llvm::ArrayRef<DynamicReplacementDescriptor>
  getReplacementDescriptors() const {
    return {this->template getTrailingObjects<DynamicReplacementDescriptor>(),
            numReplacements};
  }

public:
  void enable() const {
    for (auto &descriptor : getReplacementDescriptors()) {
      descriptor.enableReplacement();
    }
  }

  void disable() const {
    for (auto &descriptor : getReplacementDescriptors()) {
      descriptor.disableReplacement();
    }
  }
  uint32_t getFlags() { return flags; }
};

/// An "accessible" function that can be looked up based on a string key,
/// and then called through a fully-abstracted entry point whose arguments
/// can be constructed in code.
template <typename Runtime>
struct TargetAccessibleFunctionRecord final {
public:
  /// The name of the function, which is a unique string assigned to the
  /// function so it can be looked up later.
  RelativeDirectPointer<const char, /*nullable*/ false> Name;

  /// The generic environment associated with this accessor function.
  RelativeDirectPointer<GenericEnvironmentDescriptor, /*nullable*/ true>
      GenericEnvironment;

  /// The Swift function type, encoded as a mangled name.
  RelativeDirectPointer<const char, /*nullable*/ false> FunctionType;

  /// The fully-abstracted function to call.
  ///
  /// Could be a sync or async function pointer depending on flags.
  RelativeDirectPointer<void *, /*nullable*/ false> Function;

  /// Flags providing more information about the function.
  AccessibleFunctionFlags Flags;
};

using AccessibleFunctionRecord = TargetAccessibleFunctionRecord<InProcess>;

/// A single entry in an runtine discoverable attribute record
/// that relates a type attribute is attached to a generator function.
template <typename Runtime>
struct TargetRuntimeDiscoverableAttributeEntry {
  RelativeDirectPointer<const char, /*nullable*/ false> Type;
  RelativeDirectPointer<TargetAccessibleFunctionRecord<Runtime>> Generator;
};

/// A record that relates a runtime discoverable attribute to all of the
/// types (i.e. a nominal type, method, property etc.) it's attached to.
template <typename Runtime>
class RuntimeDiscoverableAttributeRecord
    : private swift::ABI::TrailingObjects<
          RuntimeDiscoverableAttributeRecord<Runtime>,
          TargetRuntimeDiscoverableAttributeEntry<Runtime>> {
  using TrailingObjects = swift::ABI::TrailingObjects<
      RuntimeDiscoverableAttributeRecord<Runtime>,
      ConstTargetMetadataPointer<Runtime, TargetMetadata>>;
  friend TrailingObjects;

  uint32_t flags;

  /// The nominal type that describes the attribute.
  TargetRelativeIndirectablePointer<Runtime,
                                    TargetTypeContextDescriptor<Runtime>,
                                    /*nullable*/ false>
      Attribute;

  /// The number of types this attribute is associated with.
  uint32_t numEntries;

public:
  uint32_t getFlags() { return flags; }

  llvm::ArrayRef<TargetRuntimeDiscoverableAttributeEntry<Runtime>>
  getEntries() const {
    return {this->template getTrailingObjects<
                TargetRuntimeDiscoverableAttributeEntry<Runtime>>(),
            numEntries};
  }
};

enum class PackLifetime : uint8_t {
  OnStack = 0,
  OnHeap = 1
};

/// A pointer to a metadata or witness table pack. If the LSB is set,
/// the pack is allocated on the heap; otherwise, it is allocated on
/// the stack.
template<typename Runtime, template <typename> class Pointee>
class TargetPackPointer {
  typename Runtime::StoredSize Ptr;

  using PointerType = typename Runtime::template Pointer<const Pointee<Runtime>>;

public:
  explicit TargetPackPointer() : Ptr(0) {}

  explicit TargetPackPointer(typename Runtime::StoredSize rawPtr) : Ptr(rawPtr) {}

  explicit TargetPackPointer(const void *rawPtr)
    : Ptr(reinterpret_cast<typename Runtime::StoredSize>(rawPtr)) {}

  explicit TargetPackPointer(PointerType const *ptr, PackLifetime lifetime)
    : Ptr(reinterpret_cast<typename Runtime::StoredSize>(ptr) |
          (lifetime == PackLifetime::OnHeap ? 1 : 0)) {}

  explicit operator bool() const {
    return Ptr != 0;
  }

  // Strips off the LSB.
  const PointerType *getElements() const {
    return reinterpret_cast<const PointerType *>(Ptr & ~1);
  }

  // Strips off the LSB.
  PointerType *getElements() {
    return reinterpret_cast<PointerType *>(Ptr & ~1);
  }

  // Leaves the LSB.
  const PointerType *getPointer() const {
    return reinterpret_cast<const PointerType *>(Ptr);
  }

  PackLifetime getLifetime() const {
    return (bool)(Ptr & 1) ? PackLifetime::OnHeap : PackLifetime::OnStack;
  }

  // Get the number of elements in the pack, only valid for on-heap packs.
  size_t getNumElements() const {
    if (getLifetime() == PackLifetime::OnHeap)
      return *(reinterpret_cast<const size_t *>(Ptr & ~1) - 1);

    fatalError(0, "Cannot get length of on-stack pack");
  }
};

/// A pointer to a metadata pack.
template<typename Runtime>
using TargetMetadataPackPointer = TargetPackPointer<Runtime, TargetMetadata>;

using MetadataPackPointer = TargetMetadataPackPointer<InProcess>;

/// A pointer to a witness table pack.
template<typename Runtime>
using TargetWitnessTablePackPointer = TargetPackPointer<Runtime, TargetWitnessTable>;

using WitnessTablePackPointer = TargetWitnessTablePackPointer<InProcess>;

} // end namespace swift

#pragma clang diagnostic pop

#endif // SWIFT_ABI_METADATA_H
