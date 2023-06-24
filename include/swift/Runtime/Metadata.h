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
// Swift runtime support for generating and uniquing metadata.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_RUNTIME_METADATA_H
#define SWIFT_RUNTIME_METADATA_H

#include "swift/ABI/Metadata.h"
#include "swift/RemoteInspection/Records.h"
#include "swift/Runtime/Once.h"
#include "swift/shims/Visibility.h"

namespace swift {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"

// Tags used to denote different kinds of allocations made with the metadata
// allocator. This is encoded in a header on each allocation when metadata
// iteration is enabled, and allows tools to know where each allocation came
// from.
enum MetadataAllocatorTags : uint16_t {
#define TAG(name, value) name##Tag = value,
#include "../../../stdlib/public/runtime/MetadataAllocatorTags.def"
};

template <typename Runtime> struct MetadataAllocationBacktraceHeader {
  TargetPointer<Runtime, const void> Next;
  TargetPointer<Runtime, void> Allocation;
  uint32_t Count;
  // Count backtrace pointers immediately follow.
};

/// The buffer used by a yield-once coroutine (such as the generalized
/// accessors `read` and `modify`).
struct YieldOnceBuffer {
  void *Data[NumWords_YieldOnceBuffer];
};
using YieldOnceContinuation =
  SWIFT_CC(swift) void (YieldOnceBuffer *buffer, bool forUnwind);

/// The return type of a call to a yield-once coroutine.  The function
/// must be declared with the swiftcall calling convention.
template <class ResultTy>
struct YieldOnceResult {
  YieldOnceContinuation *Continuation;
  ResultTy YieldValue;
};

template <class FnTy>
struct YieldOnceCoroutine;

/// A template which generates the type of the ramp function of a
/// yield-once coroutine.
template <class ResultTy, class... ArgTys>
struct YieldOnceCoroutine<ResultTy(ArgTys...)> {
  using type =
    SWIFT_CC(swift) YieldOnceResult<ResultTy> (YieldOnceBuffer *buffer,
                                               ArgTys...);
};

#if SWIFT_OBJC_INTEROP

  // Const cast shorthands for ObjC types.

  /// Cast to id, discarding const if necessary.
  template <typename T>
  static inline id id_const_cast(const T* value) {
    return reinterpret_cast<id>(const_cast<T*>(value));
  }

  /// Cast to Class, discarding const if necessary.
  template <typename T>
  static inline Class class_const_cast(const T* value) {
    return reinterpret_cast<Class>(const_cast<T*>(value));
  }

  /// Cast to Protocol*, discarding const if necessary.
  template <typename T>
  static inline Protocol* protocol_const_cast(const T* value) {
    return reinterpret_cast<Protocol *>(const_cast<T*>(value));
  }

  /// Cast from a CF type, discarding const if necessary.
  template <typename T>
  static inline T cf_const_cast(const void* value) {
    return reinterpret_cast<T>(const_cast<void *>(value));
  }

#endif

/// A standard routine, suitable for placement in the value witness
/// table, for copying an opaque POD object.
SWIFT_RUNTIME_EXPORT
OpaqueValue *swift_copyPOD(OpaqueValue *dest,
                           OpaqueValue *src,
                           const Metadata *self);

template <>
inline void ValueWitnessTable::publishLayout(const TypeLayout &layout) {
  size = layout.size;
  stride = layout.stride;
  extraInhabitantCount = layout.extraInhabitantCount;

  // Currently there is nothing in the runtime or ABI which tries to
  // asynchronously check completion, so we can just do a normal store here.
  //
  // If we decide to start allowing that (to speed up checkMetadataState,
  // maybe), we'll have to:
  //   - turn this into an store-release,
  //   - turn the load in checkIsComplete() into a load-acquire, and
  //   - do something about getMutableVWTableForInit.
  flags = layout.flags;
}

template <> inline bool ValueWitnessTable::checkIsComplete() const {
  return !flags.isIncomplete();
}

// Standard value-witness tables.

#define BUILTIN_TYPE(Symbol, _) \
  SWIFT_RUNTIME_EXPORT const ValueWitnessTable VALUE_WITNESS_SYM(Symbol);
#define BUILTIN_POINTER_TYPE(Symbol, _) \
  SWIFT_RUNTIME_EXPORT const ValueWitnessTable VALUE_WITNESS_SYM(Symbol);
#include "swift/Runtime/BuiltinTypes.def"

// The () -> () table can be used for arbitrary function types.
SWIFT_RUNTIME_EXPORT
const ValueWitnessTable
  VALUE_WITNESS_SYM(FUNCTION_MANGLING);     // () -> ()

// The @differentiable(reverse) () -> () table can be used for differentiable
// function types.
SWIFT_RUNTIME_EXPORT
const ValueWitnessTable
  VALUE_WITNESS_SYM(DIFF_FUNCTION_MANGLING); // @differentiable(reverse) () -> ()

// The @noescape () -> () table can be used for arbitrary noescaping function types.
SWIFT_RUNTIME_EXPORT
const ValueWitnessTable
  VALUE_WITNESS_SYM(NOESCAPE_FUNCTION_MANGLING);     // @noescape () -> ()

// The @convention(thin) () -> () table can be used for arbitrary thin function types.
SWIFT_RUNTIME_EXPORT
const ValueWitnessTable
  VALUE_WITNESS_SYM(THIN_FUNCTION_MANGLING);    // @convention(thin) () -> ()

// The () table can be used for arbitrary empty types.
SWIFT_RUNTIME_EXPORT
const ValueWitnessTable VALUE_WITNESS_SYM(EMPTY_TUPLE_MANGLING);        // ()

// The table for aligned-pointer-to-pointer types.
SWIFT_RUNTIME_EXPORT
const ValueWitnessTable METATYPE_VALUE_WITNESS_SYM(Bo); // Builtin.NativeObject.Type

/// Return the value witnesses for unmanaged pointers.
static inline const ValueWitnessTable &getUnmanagedPointerValueWitnesses() {
#if __POINTER_WIDTH__ == 64
  return VALUE_WITNESS_SYM(Bi64_);
#else
  return VALUE_WITNESS_SYM(Bi32_);
#endif
}

/// Return value witnesses for a pointer-aligned pointer type.
static inline
const ValueWitnessTable &
getUnmanagedPointerPointerValueWitnesses() {
  return METATYPE_VALUE_WITNESS_SYM(Bo);
}

using OpaqueMetadata = TargetOpaqueMetadata<InProcess>;

// Standard POD opaque metadata.
// The "Int" metadata are used for arbitrary POD data with the
// matching characteristics.
using FullOpaqueMetadata = FullMetadata<OpaqueMetadata>;
#define BUILTIN_TYPE(Symbol, Name) \
    SWIFT_RUNTIME_EXPORT \
    const FullOpaqueMetadata METADATA_SYM(Symbol);
#include "swift/Runtime/BuiltinTypes.def"

/// The standard metadata for the empty tuple type.
SWIFT_RUNTIME_EXPORT
const
  FullMetadata<TupleTypeMetadata> METADATA_SYM(EMPTY_TUPLE_MANGLING);

/// The standard metadata for the empty protocol composition type, Any.
SWIFT_RUNTIME_EXPORT
const
  FullMetadata<ExistentialTypeMetadata> METADATA_SYM(ANY_MANGLING);

/// The standard metadata for the empty class-constrained protocol composition
/// type, AnyObject.
SWIFT_RUNTIME_EXPORT
const
  FullMetadata<ExistentialTypeMetadata> METADATA_SYM(ANYOBJECT_MANGLING);


/// True if two context descriptors in the currently running program describe
/// the same context.
bool equalContexts(const ContextDescriptor *a, const ContextDescriptor *b);

/// Determines whether two type context descriptors describe the same type
/// context.
///
/// Runtime availability: Swift 5.4.
///
/// \param lhs The first type context descriptor to compare.
/// \param rhs The second type context descriptor to compare.
///
/// \returns true if both describe the same type context, false otherwise.
SWIFT_RUNTIME_EXPORT
SWIFT_CC(swift)
bool swift_compareTypeContextDescriptors(const TypeContextDescriptor *lhs,
                                         const TypeContextDescriptor *rhs);

/// Compute the bounds of class metadata with a resilient superclass.
ClassMetadataBounds getResilientMetadataBounds(
                                           const ClassDescriptor *descriptor);
int32_t getResilientImmediateMembersOffset(const ClassDescriptor *descriptor);

/// Fetch a uniqued metadata object for a nominal type which requires
/// singleton metadata initialization.
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
MetadataResponse
swift_getSingletonMetadata(MetadataRequest request,
                           const TypeContextDescriptor *description);

/// Fetch a uniqued metadata object for the generic nominal type described by
/// the provided candidate metadata, using that candidate metadata if there is
/// not already a canonical metadata.
///
/// Runtime availability: Swift 5.4
///
/// \param candidate A prespecialized metadata record for a type which is not
///                  statically made to be canonical which will be canonicalized
///                  if no other canonical metadata exists for the type.
/// \param cache A pointer to a cache which will be set to the canonical 
///              metadata record for the type described by the candidate 
///              metadata record.  If the cache has already been populated, its
///              contents will be returned.
/// \returns The canonical metadata for the specialized generic type described
///          by the provided candidate metadata.
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift) MetadataResponse
    swift_getCanonicalSpecializedMetadata(MetadataRequest request,
                                          const Metadata *candidate,
                                          const Metadata **cache);

/// Fetch a uniqued metadata object for the generic nominal type described by
/// the provided description and arguments, adding the canonical
/// prespecializations attached to the type descriptor to the metadata cache on
/// first run.
///
/// In contrast to swift_getGenericMetadata, this function is for use by
/// metadata accessors for which canonical generic metadata has been specialized
/// at compile time.
///
/// Runtime availability: Swift 5.4
///
/// \param request A specification of the metadata to be returned.
/// \param arguments The generic arguments--metadata and witness tables--which
///                  the returned metadata is to have been instantiated with.
/// \param description The type descriptor for the generic type whose
///                    generic metadata is to have been instantiated.
/// \param token The token that ensures that prespecialized records are added to
///              the metadata cache only once.
/// \returns The canonical metadata for the specialized generic type described
///          by the provided candidate metadata.
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift) MetadataResponse
    swift_getCanonicalPrespecializedGenericMetadata(
        MetadataRequest request, const void *const *arguments,
        const TypeContextDescriptor *description, swift_once_t *token);

/// Fetch a uniqued metadata object for a generic nominal type.
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
MetadataResponse
swift_getGenericMetadata(MetadataRequest request,
                         const void * const *arguments,
                         const TypeContextDescriptor *description);

/// Allocate a generic class metadata object.  This is intended to be
/// called by the metadata instantiation function of a generic class.
///
/// This function:
///   - computes the required size of the metadata object based on the
///     class hierarchy;
///   - allocates memory for the metadata object based on the computed
///     size and the additional requirements imposed by the pattern;
///   - copies information from the pattern into the allocated metadata; and
///   - fully initializes the ClassMetadata header, except that the
///     superclass pointer will be null (or SwiftObject under ObjC interop
///     if there is no formal superclass).
///
/// The instantiation function is responsible for completing the
/// initialization, including:
///   - setting the superclass pointer;
///   - copying class data from the superclass;
///   - installing the generic arguments;
///   - installing new v-table entries and overrides; and
///   - registering the class with the runtime under ObjC interop.
/// Most of this work can be achieved by calling swift_initClassMetadata.
SWIFT_EXTERN_C SWIFT_RETURNS_NONNULL SWIFT_NODISCARD SWIFT_RUNTIME_EXPORT_ATTRIBUTE
ClassMetadata *
swift_allocateGenericClassMetadata(const ClassDescriptor *description,
                                   const void *arguments,
                                   const GenericClassMetadataPattern *pattern);

SWIFT_EXTERN_C SWIFT_RETURNS_NONNULL SWIFT_NODISCARD SWIFT_RUNTIME_EXPORT_ATTRIBUTE
ClassMetadata *
swift_allocateGenericClassMetadataWithLayoutString(
    const ClassDescriptor *description,
    const void *arguments,
    const GenericClassMetadataPattern *pattern);

/// Allocate a generic value metadata object.  This is intended to be
/// called by the metadata instantiation function of a generic struct or
/// enum.
SWIFT_EXTERN_C SWIFT_RETURNS_NONNULL SWIFT_NODISCARD SWIFT_RUNTIME_EXPORT_ATTRIBUTE
ValueMetadata *
swift_allocateGenericValueMetadata(const ValueTypeDescriptor *description,
                                   const void *arguments,
                                   const GenericValueMetadataPattern *pattern,
                                   size_t extraDataSize);

SWIFT_EXTERN_C SWIFT_RETURNS_NONNULL SWIFT_NODISCARD SWIFT_RUNTIME_EXPORT_ATTRIBUTE
ValueMetadata *
swift_allocateGenericValueMetadataWithLayoutString(
    const ValueTypeDescriptor *description,
    const void *arguments,
    const GenericValueMetadataPattern *pattern,
    size_t extraDataSize);

/// Check that the given metadata has the right state.
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
MetadataResponse swift_checkMetadataState(MetadataRequest request,
                                          const Metadata *type);

/// Retrieve a witness table based on a given conformance.
///
/// \param conformance - The protocol conformance descriptor, which
///   contains any information required to form the witness table.
///
/// \param type - The conforming type, used to form a uniquing key
///   for the conformance.
///
/// \param instantiationArgs - An opaque pointer that's forwarded to
///   the instantiation function, used for conditional conformances.
///   This API implicitly embeds an assumption that these arguments
///   never form part of the uniquing key for the conformance, which
///   is ultimately a statement about the user model of overlapping
///   conformances.
SWIFT_RUNTIME_EXPORT
const WitnessTable *
swift_getWitnessTable(const ProtocolConformanceDescriptor *conformance,
                      const Metadata *type,
                      const void * const *instantiationArgs);

SWIFT_RUNTIME_EXPORT
const RelativeWitnessTable *
swift_getWitnessTableRelative(const ProtocolConformanceDescriptor *conformance,
                      const Metadata *type,
                      const void * const *instantiationArgs);

/// Retrieve an associated type witness from the given witness table.
///
/// \param wtable The witness table.
/// \param conformingType Metadata for the conforming type.
/// \param reqBase "Base" requirement used to compute the witness index
/// \param assocType Associated type descriptor.
///
/// \returns metadata for the associated type witness.
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
MetadataResponse swift_getAssociatedTypeWitness(
                                          MetadataRequest request,
                                          WitnessTable *wtable,
                                          const Metadata *conformingType,
                                          const ProtocolRequirement *reqBase,
                                          const ProtocolRequirement *assocType);
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
MetadataResponse swift_getAssociatedTypeWitnessRelative(
                                          MetadataRequest request,
                                          RelativeWitnessTable *wtable,
                                          const Metadata *conformingType,
                                          const ProtocolRequirement *reqBase,
                                          const ProtocolRequirement *assocType);
/// Retrieve an associated conformance witness table from the given witness
/// table.
///
/// \param wtable The witness table.
/// \param conformingType Metadata for the conforming type.
/// \param assocType Metadata for the associated type.
/// \param reqBase "Base" requirement used to compute the witness index
/// \param assocConformance Associated conformance descriptor.
///
/// \returns corresponding witness table.
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
const WitnessTable *swift_getAssociatedConformanceWitness(
                                  WitnessTable *wtable,
                                  const Metadata *conformingType,
                                  const Metadata *assocType,
                                  const ProtocolRequirement *reqBase,
                                  const ProtocolRequirement *assocConformance);

SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
const RelativeWitnessTable *swift_getAssociatedConformanceWitnessRelative(
                                  RelativeWitnessTable *wtable,
                                  const Metadata *conformingType,
                                  const Metadata *assocType,
                                  const ProtocolRequirement *reqBase,
                                  const ProtocolRequirement *assocConformance);

/// Compare two witness tables, which may involving checking the
/// contents of their conformance descriptors.
///
/// Runtime availability: Swift 5.4
///
/// \param lhs The first protocol witness table to compare.
/// \param rhs The second protocol witness table to compare.
///
/// \returns true if both witness tables describe the same conformance, false otherwise.
SWIFT_RUNTIME_EXPORT
SWIFT_CC(swift)
bool swift_compareWitnessTables(const WitnessTable *lhs,
                                const WitnessTable *rhs);

/// Determine whether two protocol conformance descriptors describe the same
/// conformance of a type to a protocol.
///
/// Runtime availability: Swift 5.4
///
/// \param lhs The first protocol conformance descriptor to compare.
/// \param rhs The second protocol conformance descriptor to compare.
///
/// \returns true if both describe the same conformance, false otherwise.
SWIFT_RUNTIME_EXPORT
SWIFT_CC(swift)
bool swift_compareProtocolConformanceDescriptors(
    const ProtocolConformanceDescriptor *lhs,
    const ProtocolConformanceDescriptor *rhs);

/// Allocate a metadata pack on the heap, unless this pack is already on the
/// heap.
///
/// Metadata packs are uniqued by pointer equality on their elements.
///
/// \param ptr A pack pointer, where the least significant bit indicates if
/// it is already on the heap.
/// \param count The number of metadata pointers in the pack.
///
/// \returns a metadata pack allocated on the heap, with the least significant
/// bit set to true.
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
const Metadata * const *
swift_allocateMetadataPack(const Metadata * const *ptr, size_t count);

/// Allocate a witness table pack on the heap, unless this pack is already on
/// the heap.
///
/// Witness table packs are not uniqued.
///
/// \param ptr A pack pointer, where the least significant bit indicates if
/// it is already on the heap.
/// \param count The number of witness table pointers in the pack.
///
/// \returns a witness table pack allocated on the heap, with the least
/// significant bit set to true.
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
const WitnessTable * const *
swift_allocateWitnessTablePack(const WitnessTable * const *ptr, size_t count);

/// Fetch a uniqued metadata for a function type.
SWIFT_RUNTIME_EXPORT
const FunctionTypeMetadata *
swift_getFunctionTypeMetadata(FunctionTypeFlags flags,
                              const Metadata *const *parameters,
                              const uint32_t *parameterFlags,
                              const Metadata *result);

SWIFT_RUNTIME_EXPORT
const FunctionTypeMetadata *
swift_getFunctionTypeMetadataDifferentiable(
    FunctionTypeFlags flags, FunctionMetadataDifferentiabilityKind diffKind,
    const Metadata *const *parameters, const uint32_t *parameterFlags,
    const Metadata *result);

SWIFT_RUNTIME_EXPORT
const FunctionTypeMetadata *
swift_getFunctionTypeMetadataGlobalActor(
    FunctionTypeFlags flags, FunctionMetadataDifferentiabilityKind diffKind,
    const Metadata *const *parameters, const uint32_t *parameterFlags,
    const Metadata *result, const Metadata *globalActor);

SWIFT_RUNTIME_EXPORT
const FunctionTypeMetadata *
swift_getFunctionTypeMetadata0(FunctionTypeFlags flags,
                               const Metadata *result);

SWIFT_RUNTIME_EXPORT
const FunctionTypeMetadata *
swift_getFunctionTypeMetadata1(FunctionTypeFlags flags,
                               const Metadata *arg0,
                               const Metadata *result);

SWIFT_RUNTIME_EXPORT
const FunctionTypeMetadata *
swift_getFunctionTypeMetadata2(FunctionTypeFlags flags,
                               const Metadata *arg0,
                               const Metadata *arg1,
                               const Metadata *result);

SWIFT_RUNTIME_EXPORT
const FunctionTypeMetadata *swift_getFunctionTypeMetadata3(
                                                FunctionTypeFlags flags,
                                                const Metadata *arg0,
                                                const Metadata *arg1,
                                                const Metadata *arg2,
                                                const Metadata *result);

#if SWIFT_OBJC_INTEROP
SWIFT_RUNTIME_EXPORT
void
swift_instantiateObjCClass(const ClassMetadata *theClass);

SWIFT_RUNTIME_EXPORT
Class
swift_getInitializedObjCClass(Class c);

/// Fetch a uniqued type metadata for an ObjC class.
SWIFT_RUNTIME_EXPORT
const Metadata *
swift_getObjCClassMetadata(const ClassMetadata *theClass);

/// Get the ObjC class object from class type metadata.
SWIFT_RUNTIME_EXPORT
const ClassMetadata *
swift_getObjCClassFromMetadata(const Metadata *theClass);

// Get the ObjC class object from class type metadata,
// or nullptr if the type isn't an ObjC class.
const ClassMetadata *
swift_getObjCClassFromMetadataConditional(const Metadata *theClass);

SWIFT_RUNTIME_EXPORT
const ClassMetadata *
swift_getObjCClassFromObject(HeapObject *object);
#endif

/// Fetch a unique type metadata object for a foreign type.
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
MetadataResponse
swift_getForeignTypeMetadata(MetadataRequest request,
                             ForeignTypeMetadata *nonUnique);

/// Fetch a uniqued metadata for a tuple type.
///
/// The labels argument is null if and only if there are no element
/// labels in the tuple.  Otherwise, it is a null-terminated
/// concatenation of space-terminated NFC-normalized UTF-8 strings,
/// assumed to point to constant global memory.
///
/// That is, for the tuple type (a : Int, Int, c : Int), this
/// argument should be:
///   "a  c \0"
///
/// This representation allows label strings to be efficiently
/// (1) uniqued within a linkage unit and (2) compared with strcmp.
/// In other words, it's optimized for code size and uniquing
/// efficiency, not for the convenience of actually consuming
/// these strings.
///
/// \param elements - potentially invalid if numElements is zero;
///   otherwise, an array of metadata pointers.
/// \param labels - the labels string
/// \param proposedWitnesses - an optional proposed set of value witnesses.
///   This is useful when working with a non-dependent tuple type
///   where the entrypoint is just being used to unique the metadata.
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
MetadataResponse
swift_getTupleTypeMetadata(MetadataRequest request,
                           TupleTypeFlags flags,
                           const Metadata * const *elements,
                           const char *labels,
                           const ValueWitnessTable *proposedWitnesses);

SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
MetadataResponse
swift_getTupleTypeMetadata2(MetadataRequest request,
                            const Metadata *elt0, const Metadata *elt1,
                            const char *labels,
                            const ValueWitnessTable *proposedWitnesses);
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
MetadataResponse
swift_getTupleTypeMetadata3(MetadataRequest request,
                            const Metadata *elt0, const Metadata *elt1,
                            const Metadata *elt2, const char *labels,
                            const ValueWitnessTable *proposedWitnesses);

/// Perform layout as if for a tuple whose elements have the given layouts.
///
/// \param tupleLayout - A structure into which to write the tuple layout.
///   Must be non-null.
/// \param elementOffsets - An array into which to write the offsets of
///   the elements.  May be null.  Must have space for all elements,
///   including element 0 (which will always have offset 0).
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
void swift_getTupleTypeLayout(TypeLayout *tupleLayout,
                              uint32_t *elementOffsets,
                              TupleTypeFlags flags,
                              const TypeLayout * const *elements);

/// Perform layout as if for a two-element tuple whose elements have
/// the given layouts.
///
/// \param tupleLayout - A structure into which to write the tuple layout.
///   Must be non-null.
/// \returns The offset of the second element.
///   The first element always has offset 0.
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
size_t swift_getTupleTypeLayout2(TypeLayout *tupleLayout,
                                 const TypeLayout *elt0,
                                 const TypeLayout *elt1);

struct OffsetPair { size_t First; size_t Second; };

/// Perform layout as if for a three-element tuple whose elements have
/// the given layouts.
///
/// \param tupleLayout - A structure into which to write the tuple layout.
///   Must be non-null.
/// \returns The offsets of the second and third elements.
///   The first element always has offset 0.
SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
OffsetPair swift_getTupleTypeLayout3(TypeLayout *tupleLayout,
                                     const TypeLayout *elt0Layout,
                                     const TypeLayout *elt1Layout,
                                     const TypeLayout *elt2Layout);

/// Initialize the value witness table and struct field offset vector for a
/// struct, using the "Universal" layout strategy.
SWIFT_RUNTIME_EXPORT
void swift_initStructMetadata(StructMetadata *self,
                              StructLayoutFlags flags,
                              size_t numFields,
                              const TypeLayout * const *fieldTypes,
                              uint32_t *fieldOffsets);

SWIFT_RUNTIME_EXPORT
void swift_initStructMetadataWithLayoutString(StructMetadata *self,
                                              StructLayoutFlags flags,
                                              size_t numFields,
                                              const uint8_t *const *fieldTypes,
                                              const uint8_t *fieldTags,
                                              uint32_t *fieldOffsets);

enum LayoutStringFlags : uint64_t {
  Empty = 0,
  // TODO: Track other useful information tha can be used to optimize layout
  //       strings, like different reference kinds contained in the string
  //       number of ref counting operations (maybe up to 4), so we can
  //       use witness functions optimized for these cases.
  HasRelativePointers = (1ULL << 63),
};

inline bool operator&(LayoutStringFlags a, LayoutStringFlags b) {
  return (uint64_t(a) & uint64_t(b)) != 0;
}
inline LayoutStringFlags operator|(LayoutStringFlags a, LayoutStringFlags b) {
  return LayoutStringFlags(uint64_t(a) | uint64_t(b));
}
inline LayoutStringFlags &operator|=(LayoutStringFlags &a, LayoutStringFlags b) {
  return a = (a | b);
}

SWIFT_RUNTIME_STDLIB_INTERNAL
size_t _swift_refCountBytesForMetatype(const Metadata *type);

struct LayoutStringWriter;

SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_addRefCountStringForMetatype(LayoutStringWriter &writer,
                                         LayoutStringFlags &flags,
                                         const Metadata *fieldType,
                                         size_t &fullOffset,
                                         size_t &previousFieldOffset);

/// Allocate the metadata for a class and copy fields from the given pattern.
/// The final size of the metadata is calculated at runtime from the metadata
/// bounds in the class descriptor.
///
/// This function is only intended to be called from the relocation function
/// of a resilient class pattern.
///
/// The metadata completion function must complete the metadata by calling
/// swift_initClassMetadata().
SWIFT_RUNTIME_EXPORT
ClassMetadata *
swift_relocateClassMetadata(const ClassDescriptor *descriptor,
                            const ResilientClassMetadataPattern *pattern);

/// Initialize various fields of the class metadata.
///
/// Namely:
/// - The superclass field is set to \p super.
/// - If the class metadata was allocated at runtime, copies the
///   vtable entries from the superclass and installs the class's
///   own vtable entries and overrides of superclass vtable entries.
/// - Copies the field offsets and generic parameters and conformances
///   from the superclass.
/// - Initializes the field offsets using the runtime type layouts
///   passed in \p fieldTypes.
///
/// This initialization pattern in the following cases:
/// - The class has generic ancestry, or resiliently-sized fields.
///   In this case the metadata was emitted statically but is incomplete,
///   because, the superclass field, generic parameters and conformances,
///   and field offset vector entries require runtime completion.
///
/// - The class is not generic, and has resilient ancestry.
///   In this case the class metadata was allocated from a resilient
///   class metadata pattern by swift_relocateClassMetadata().
///
/// - The class is generic.
///   In this case the class metadata was allocated from a generic
///   class metadata pattern by swift_allocateGenericClassMetadata().
SWIFT_RUNTIME_EXPORT
void swift_initClassMetadata(ClassMetadata *self,
                             ClassLayoutFlags flags,
                             size_t numFields,
                             const TypeLayout * const *fieldTypes,
                             size_t *fieldOffsets);

SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
MetadataDependency
swift_initClassMetadata2(ClassMetadata *self,
                         ClassLayoutFlags flags,
                         size_t numFields,
                         const TypeLayout * const *fieldTypes,
                         size_t *fieldOffsets);

#if SWIFT_OBJC_INTEROP
/// Initialize various fields of the class metadata.
///
/// This is a special function only used to re-initialize metadata of
/// classes that are visible to Objective-C and have resilient fields.
///
/// This means the class does not have generic or resilient ancestry,
/// and is itself not generic. However, it might have fields whose
/// size is not known at compile time.
SWIFT_RUNTIME_EXPORT
void swift_updateClassMetadata(ClassMetadata *self,
                               ClassLayoutFlags flags,
                               size_t numFields,
                               const TypeLayout * const *fieldTypes,
                               size_t *fieldOffsets);

SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
MetadataDependency
swift_updateClassMetadata2(ClassMetadata *self,
                           ClassLayoutFlags flags,
                           size_t numFields,
                           const TypeLayout * const *fieldTypes,
                           size_t *fieldOffsets);
#endif

/// Given class metadata, a class descriptor and a method descriptor, look up
/// and load the vtable entry from the given metadata. The metadata must be of
/// the same class or a subclass of the descriptor.
SWIFT_RUNTIME_EXPORT
void *
swift_lookUpClassMethod(const ClassMetadata *metadata,
                        const MethodDescriptor *method,
                        const ClassDescriptor *description);

/// Fetch a uniqued metadata for a metatype type.
SWIFT_RUNTIME_EXPORT
const MetatypeMetadata *
swift_getMetatypeMetadata(const Metadata *instanceType);

/// Fetch a uniqued metadata for an existential metatype type.
SWIFT_RUNTIME_EXPORT
const ExistentialMetatypeMetadata *
swift_getExistentialMetatypeMetadata(const Metadata *instanceType);

/// Fetch a uniqued metadata for an existential type.
///
/// The array referenced by \c protocols will be sorted in-place.
SWIFT_RUNTIME_EXPORT
const ExistentialTypeMetadata *
swift_getExistentialTypeMetadata(ProtocolClassConstraint classConstraint,
                                 const Metadata *superclassConstraint,
                                 size_t numProtocols,
                                 const ProtocolDescriptorRef *protocols);

/// Fetch unique metadata for an extended existential type.
///
/// The shape must not correspond to an existential that could be
/// represented with ExistentialTypeMetadata.  Its uniquing cache
/// pointer is guaranteed to be filled after this call.
SWIFT_RUNTIME_EXPORT
const ExtendedExistentialTypeMetadata *
swift_getExtendedExistentialTypeMetadata(
            const NonUniqueExtendedExistentialTypeShape *shape,
            const void * const *generalizationArguments);

/// Fetch unique metadata for an extended existential type, given its
/// known-unique existential shape.  The shape must not correspond to
/// an existential that could be represented with ExistentialTypeMetadata.
SWIFT_RUNTIME_EXPORT
const ExtendedExistentialTypeMetadata *
swift_getExtendedExistentialTypeMetadata_unique(
            const ExtendedExistentialTypeShape *shape,
            const void * const *generalizationArguments);

/// Fetch the unique existential shape for the given non-unique shape.
/// The shape's uniquing cache pointer is guaranteed to be filled after
/// this call.
SWIFT_RUNTIME_EXPORT
const ExtendedExistentialTypeShape *
swift_getExtendedExistentialTypeShape(
            const NonUniqueExtendedExistentialTypeShape *shape);

/// Perform a copy-assignment from one existential container to another.
/// Both containers must be of the same existential type representable with the
/// same number of witness tables.
SWIFT_RUNTIME_EXPORT
OpaqueValue *swift_assignExistentialWithCopy(OpaqueValue *dest,
                                             const OpaqueValue *src,
                                             const Metadata *type);

/// Perform a copy-assignment from one existential container to another.
/// Both containers must be of the same existential type representable with no
/// witness tables.
OpaqueValue *swift_assignExistentialWithCopy0(OpaqueValue *dest,
                                              const OpaqueValue *src,
                                              const Metadata *type);

/// Perform a copy-assignment from one existential container to another.
/// Both containers must be of the same existential type representable with one
/// witness table.
OpaqueValue *swift_assignExistentialWithCopy1(OpaqueValue *dest,
                                              const OpaqueValue *src,
                                              const Metadata *type);

/// Calculate the numeric index of an extra inhabitant of a heap object
/// pointer in memory.
inline int swift_getHeapObjectExtraInhabitantIndex(HeapObject * const* src) {
  // This must be consistent with the getHeapObjectExtraInhabitantIndex
  // implementation in IRGen's ExtraInhabitants.cpp.

  using namespace heap_object_abi;

  uintptr_t value = reinterpret_cast<uintptr_t>(*src);
  if (value >= LeastValidPointerValue)
    return -1;

  // Check for tagged pointers on appropriate platforms.  Knowing that
  // value < LeastValidPointerValue tells us a lot.
#if SWIFT_OBJC_INTEROP
  if (value & ((uintptr_t(1) << ObjCReservedLowBits) - 1))
    return -1;
  return int(value >> ObjCReservedLowBits);
#else
  return int(value);
#endif
}
  
/// Store an extra inhabitant of a heap object pointer to memory,
/// in the style of a value witness.
inline void swift_storeHeapObjectExtraInhabitant(HeapObject **dest, int index) {
  // This must be consistent with the storeHeapObjectExtraInhabitant
  // implementation in IRGen's ExtraInhabitants.cpp.

#if SWIFT_OBJC_INTEROP
  auto value = uintptr_t(index) << heap_object_abi::ObjCReservedLowBits;
#else
  auto value = uintptr_t(index);
#endif
  *dest = reinterpret_cast<HeapObject*>(value);
}

/// Return the number of extra inhabitants in a heap object pointer.
inline constexpr unsigned swift_getHeapObjectExtraInhabitantCount() {
  // This must be consistent with the getHeapObjectExtraInhabitantCount
  // implementation in IRGen's ExtraInhabitants.cpp.

  using namespace heap_object_abi;

  // The runtime needs no more than INT_MAX inhabitants.
#if SWIFT_OBJC_INTEROP
  return (LeastValidPointerValue >> ObjCReservedLowBits) > INT_MAX
    ? (unsigned)INT_MAX
    : (unsigned)(LeastValidPointerValue >> ObjCReservedLowBits);
#else
  return (LeastValidPointerValue) > INT_MAX
    ? unsigned(INT_MAX)
    : unsigned(LeastValidPointerValue);
#endif
}  

/// Calculate the numeric index of an extra inhabitant of a function
/// pointer in memory.
inline int swift_getFunctionPointerExtraInhabitantIndex(void * const* src) {
  // This must be consistent with the getFunctionPointerExtraInhabitantIndex
  // implementation in IRGen's ExtraInhabitants.cpp.
  uintptr_t value = reinterpret_cast<uintptr_t>(*src);
  return (value < heap_object_abi::LeastValidPointerValue
            ? (int) value : -1);
}
  
/// Store an extra inhabitant of a function pointer to memory, in the
/// style of a value witness.
inline void swift_storeFunctionPointerExtraInhabitant(void **dest, int index) {
  // This must be consistent with the storeFunctionPointerExtraInhabitantIndex
  // implementation in IRGen's ExtraInhabitants.cpp.
  *dest = reinterpret_cast<void*>(static_cast<uintptr_t>(index));
}

/// Return the number of extra inhabitants in a function pointer.
inline constexpr unsigned swift_getFunctionPointerExtraInhabitantCount() {
  // This must be consistent with the getFunctionPointerExtraInhabitantCount
  // implementation in IRGen's ExtraInhabitants.cpp.

  using namespace heap_object_abi;

  // The runtime needs no more than INT_MAX inhabitants.
  return (LeastValidPointerValue) > INT_MAX
    ? (unsigned)INT_MAX
    : (unsigned)(LeastValidPointerValue);
}

/// Return the type name for a given type metadata.
std::string nameForMetadata(const Metadata *type,
                            bool qualified = true);

/// Register a block of protocol records for dynamic lookup.
SWIFT_RUNTIME_EXPORT
void swift_registerProtocols(const ProtocolRecord *begin,
                             const ProtocolRecord *end);

/// Register a block of protocol conformance records for dynamic lookup.
SWIFT_RUNTIME_EXPORT
void swift_registerProtocolConformances(const ProtocolConformanceRecord *begin,
                                        const ProtocolConformanceRecord *end);

/// Register a block of type context descriptors for dynamic lookup.
SWIFT_RUNTIME_EXPORT
void swift_registerTypeMetadataRecords(const TypeMetadataRecord *begin,
                                       const TypeMetadataRecord *end);

/// Return the superclass, if any.  The result is nullptr for root
/// classes and class protocol types.
SWIFT_CC(swift)
SWIFT_RUNTIME_STDLIB_INTERNAL
const Metadata *_swift_class_getSuperclass(const Metadata *theClass);

SWIFT_CC(swift)
SWIFT_RUNTIME_STDLIB_INTERNAL MetadataResponse
getSuperclassMetadata(MetadataRequest request, const ClassMetadata *self);

SWIFT_CC(swift)
SWIFT_RUNTIME_STDLIB_SPI
bool _swift_class_isSubclass(const Metadata *subclass,
                             const Metadata *superclass);

#if !NDEBUG
/// Verify that the given metadata pointer correctly roundtrips its
/// mangled name through the demangler.
void verifyMangledNameRoundtrip(const Metadata *metadata);
#endif

SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
const TypeContextDescriptor *swift_getTypeContextDescriptor(const Metadata *type);

// Defined in KeyPath.swift in the standard library.
SWIFT_RUNTIME_EXPORT
const HeapObject *swift_getKeyPath(const void *pattern, const void *arguments);

// For some reason, MSVC doesn't accept these declarations outside of
// swiftCore.  TODO: figure out a reasonable way to declare them.
#if defined(swiftCore_EXPORTS)

/// Given a pointer to a borrowed value of type `Root` and a
/// `KeyPath<Root, Value>`, project a pointer to a borrowed value of type
/// `Value`.
SWIFT_RUNTIME_EXPORT
YieldOnceCoroutine<const OpaqueValue* (const OpaqueValue *root,
                                       void *keyPath)>::type
swift_readAtKeyPath;

/// Given a pointer to a mutable value of type `Root` and a
/// `WritableKeyPath<Root, Value>`, project a pointer to a mutable value
/// of type `Value`.
SWIFT_RUNTIME_EXPORT
YieldOnceCoroutine<OpaqueValue* (OpaqueValue *root, void *keyPath)>::type
swift_modifyAtWritableKeyPath;

/// Given a pointer to a borrowed value of type `Root` and a
/// `ReferenceWritableKeyPath<Root, Value>`, project a pointer to a
/// mutable value of type `Value`.
SWIFT_RUNTIME_EXPORT
YieldOnceCoroutine<OpaqueValue* (const OpaqueValue *root, void *keyPath)>::type
swift_modifyAtReferenceWritableKeyPath;

#endif // swiftCore_EXPORTS

SWIFT_RUNTIME_EXPORT
void swift_enableDynamicReplacementScope(const DynamicReplacementScope *scope);

SWIFT_RUNTIME_EXPORT
void swift_disableDynamicReplacementScope(const DynamicReplacementScope *scope);

/// A struct containing pointers to all of the type descriptors in the
/// Concurrency runtime which have standard manglings.
struct ConcurrencyStandardTypeDescriptors {
#define STANDARD_TYPE(KIND, MANGLING, TYPENAME)
#define STANDARD_TYPE_CONCURRENCY(KIND, MANGLING, TYPENAME)                    \
  const ContextDescriptor * __ptrauth_swift_type_descriptor TYPENAME;
#include "swift/Demangling/StandardTypesMangling.def"
};

/// Register the type descriptors with standard manglings from the Concurrency
/// runtime. The passed-in struct must be immortal.
SWIFT_RUNTIME_STDLIB_SPI
void _swift_registerConcurrencyStandardTypeDescriptors(
    const ConcurrencyStandardTypeDescriptors *descriptors);

#pragma clang diagnostic pop

} // end namespace swift

#endif // SWIFT_RUNTIME_METADATA_H
