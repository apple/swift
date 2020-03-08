//===--- Private.h - Private runtime declarations ---------------*- C++ -*-===//
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
// Private declarations of the Swift runtime.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_RUNTIME_PRIVATE_H
#define SWIFT_RUNTIME_PRIVATE_H

#include "swift/Demangling/Demangler.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/Metadata.h"
#include "llvm/Support/Compiler.h"

#if defined(__APPLE__) && defined(__MACH__)
#include <TargetConditionals.h>
#endif

// Opaque ISAs need to use object_getClass which is in runtime.h
#if SWIFT_HAS_OPAQUE_ISAS
#include <objc/runtime.h>
#endif

namespace swift {
class ParsedTypeIdentity;

class TypeReferenceOwnership {
  enum : uint8_t {
    Weak = 1 << 0,
    Unowned = 1 << 1,
    Unmanaged = 1 << 2,
  };

  uint8_t Data;

  constexpr TypeReferenceOwnership(uint8_t Data) : Data(Data) {}

public:
  constexpr TypeReferenceOwnership() : Data(0) {}

#define REF_STORAGE(Name, ...) \
  void set##Name() { Data |= Name; } \
  bool is##Name() const { return Data == Name; }
#include "swift/AST/ReferenceStorage.def"

  bool isStrong() const { return Data == 0; }
};

/// Type information consists of metadata and its ownership info,
/// such information is used by `_typeByMangledName` accessor
/// since we don't represent ownership attributes in the metadata
/// itself related info has to be bundled with it.
class TypeInfo {
  MetadataResponse Response;
  TypeReferenceOwnership ReferenceOwnership;

public:
  TypeInfo()
    : Response{nullptr, MetadataState::Abstract}, ReferenceOwnership() {}

  TypeInfo(MetadataResponse response, TypeReferenceOwnership ownership)
    : Response(response), ReferenceOwnership(ownership) {}

  // FIXME: remove this constructor and require a response in all cases.
  TypeInfo(const Metadata *type, TypeReferenceOwnership ownership)
    : Response{type, MetadataState::Abstract}, ReferenceOwnership(ownership) {}

  const Metadata *getMetadata() const { return Response.Value; }
  MetadataResponse getResponse() const { return Response; }

#define REF_STORAGE(Name, ...) \
  bool is##Name() const { return ReferenceOwnership.is##Name(); }
#include "swift/AST/ReferenceStorage.def"

  bool isStrong() const { return ReferenceOwnership.isStrong(); }

  TypeReferenceOwnership getReferenceOwnership() const {
    return ReferenceOwnership;
  }
};

#if SWIFT_HAS_ISA_MASKING
  SWIFT_RUNTIME_EXPORT
  uintptr_t swift_isaMask;

// Hardcode the mask. We have our own copy of the value, as it's hard to work
// out the proper includes from libobjc. The values MUST match the ones from
// libobjc. Debug builds check these values against objc_debug_isa_class_mask
// from libobjc.
#  if TARGET_OS_SIMULATOR
// Simulators don't currently use isa masking, but we still want to emit
// swift_isaMask and the corresponding code in case that changes. libobjc's
// mask has the bottom bits clear to include pointer alignment, match that
// value here.
#    define SWIFT_ISA_MASK 0xfffffffffffffff8ULL
#  elif __arm64__
#    define SWIFT_ISA_MASK 0x0000000ffffffff8ULL
#  elif __x86_64__
#    define SWIFT_ISA_MASK 0x00007ffffffffff8ULL
#  else
#    error Unknown architecture for masked isa.
#  endif
#endif

#if SWIFT_OBJC_INTEROP
  bool objectConformsToObjCProtocol(const void *theObject,
                                    ProtocolDescriptorRef protocol);
  
  bool classConformsToObjCProtocol(const void *theClass,
                                   ProtocolDescriptorRef protocol);
#endif

  /// Is the given value a valid alignment mask?
  static inline bool isAlignmentMask(size_t mask) {
    // mask          == xyz01111...
    // mask+1        == xyz10000...
    // mask&(mask+1) == xyz00000...
    // So this is nonzero if and only if there any bits set
    // other than an arbitrarily long sequence of low bits.
    return (mask & (mask + 1)) == 0;
  }

  /// Is the given value an Objective-C tagged pointer?
  static inline bool isObjCTaggedPointer(const void *object) {
#if SWIFT_OBJC_INTEROP
    return (((uintptr_t) object) & heap_object_abi::ObjCReservedBitsMask);
#else
    assert(!(((uintptr_t) object) & heap_object_abi::ObjCReservedBitsMask));
    return false;
#endif
  }

  static inline bool isObjCTaggedPointerOrNull(const void *object) {
    return object == nullptr || isObjCTaggedPointer(object);
  }

  /// Return the class of an object which is known to be an allocated
  /// heap object.
  /// Note, in this case, the object may or may not have a non-pointer ISA.
  /// Masking, or otherwise, may be required to get a class pointer.
  static inline const ClassMetadata *_swift_getClassOfAllocated(const void *object) {
#if SWIFT_HAS_OPAQUE_ISAS
    // The ISA is opaque so masking it will not return a pointer.  We instead
    // need to call the objc runtime to get the class.
    id idObject = reinterpret_cast<id>(const_cast<void *>(object));
    return reinterpret_cast<const ClassMetadata*>(object_getClass(idObject));
#else
    // Load the isa field.
    uintptr_t bits = *reinterpret_cast<const uintptr_t*>(object);

#if SWIFT_HAS_ISA_MASKING
    // Apply the mask.
    bits &= SWIFT_ISA_MASK;
#endif

    // The result is a class pointer.
    return reinterpret_cast<const ClassMetadata *>(bits);
#endif
  }

  /// Return the class of an object which is known to be an allocated
  /// heap object.
  /// Note, in this case, the object is known to have a pointer ISA, and no
  /// masking is required to convert from non-pointer to pointer ISA.
  static inline const ClassMetadata *
  _swift_getClassOfAllocatedFromPointer(const void *object) {
    // Load the isa field.
    uintptr_t bits = *reinterpret_cast<const uintptr_t*>(object);

    // The result is a class pointer.
    return reinterpret_cast<const ClassMetadata *>(bits);
  }

#if SWIFT_OBJC_INTEROP && SWIFT_HAS_OPAQUE_ISAS
  /// Return whether this object is of a class which uses non-pointer ISAs.
  static inline bool _swift_isNonPointerIsaObjCClass(const void *object) {
    // Load the isa field.
    uintptr_t bits = *reinterpret_cast<const uintptr_t*>(object);
    // If the low bit is set, then we are definitely an objc object.
    // FIXME: Use a variable for this.
    return bits & 1;
  }
#endif

  LLVM_LIBRARY_VISIBILITY
  const ClassMetadata *_swift_getClass(const void *object);

  LLVM_LIBRARY_VISIBILITY
  bool usesNativeSwiftReferenceCounting(const ClassMetadata *theClass);

  static inline
  bool objectUsesNativeSwiftReferenceCounting(const void *object) {
    assert(!isObjCTaggedPointerOrNull(object));
#if SWIFT_HAS_OPAQUE_ISAS
    // Fast path for opaque ISAs.  We don't want to call
    // _swift_getClassOfAllocated as that will call object_getClass.
    // Instead we can look at the bits in the ISA and tell if its a
    // non-pointer opaque ISA which means it is definitely an ObjC
    // object and doesn't use native swift reference counting.
    if (_swift_isNonPointerIsaObjCClass(object))
      return false;
    return usesNativeSwiftReferenceCounting(_swift_getClassOfAllocatedFromPointer(object));
#else
    return usesNativeSwiftReferenceCounting(_swift_getClassOfAllocated(object));
#endif
  }

  /// Get the superclass pointer value used for Swift root classes.
  /// Note that this function may return a nullptr on non-objc platforms,
  /// where there is no common root class. rdar://problem/18987058
  const ClassMetadata *getRootSuperclass();

  /// Check if a class has a formal superclass in the AST.
  static inline
  bool classHasSuperclass(const ClassMetadata *c) {
    return  (c->Superclass && c->Superclass != getRootSuperclass());
  }

  /// Replace entries of a freshly-instantiated value witness table with more
  /// efficient common implementations where applicable.
  ///
  /// All information is taken from the passed-in layout rather than the VWT.
  /// This is so that we can delay "publishing" the flags in the actual
  /// value witness table until all required changes have been made.
  ///
  /// For instance, if the value witness table represents a POD type, this will
  /// insert POD value witnesses into the table. The vwtable's flags must have
  /// been initialized before calling this function.
  ///
  /// Returns true if common value witnesses were used, false otherwise.
  void installCommonValueWitnesses(const TypeLayout &layout,
                                   ValueWitnessTable *vwtable);

  const Metadata *
  _matchMetadataByMangledTypeName(const llvm::StringRef metadataNameRef,
                                  const Metadata *metadata,
                                  const TypeContextDescriptor *ntd);

  bool
  _contextDescriptorMatchesMangling(const ContextDescriptor *context,
                                    Demangle::NodePointer node);
  
  const ContextDescriptor *
  _searchConformancesByMangledTypeName(Demangle::NodePointer node);

  Demangle::NodePointer _swift_buildDemanglingForMetadata(const Metadata *type,
                                                      Demangle::Demangler &Dem);

  /// Callback used to provide the substitution of a generic parameter
  /// (described by depth/index) to its metadata.
  using SubstGenericParameterFn =
    std::function<const Metadata *(unsigned depth, unsigned index)>;

  /// Callback used to provide the substitution of a witness table based on
  /// its index into the enclosing generic environment.
  using SubstDependentWitnessTableFn =
    std::function<const WitnessTable *(const Metadata *type, unsigned index)>;

  /// Function object that produces substitutions for the generic parameters
  /// that occur within a mangled name, using the generic arguments from
  /// the given metadata.
  ///
  /// Use with \c _getTypeByMangledName to decode potentially-generic
  /// types.
  class SWIFT_RUNTIME_LIBRARY_VISIBILITY SubstGenericParametersFromMetadata {
    /// Whether the source is metadata (vs. a generic environment);
    const bool sourceIsMetadata;

    union {
      const TargetContextDescriptor<InProcess> *baseContext;
      const TargetGenericEnvironment<InProcess> *environment;
    };

    /// The generic arguments.
    const void * const *genericArgs;

    /// An element in the descriptor path.
    struct PathElement {
      /// The generic parameters local to this element.
      ArrayRef<GenericParamDescriptor> localGenericParams;

      /// The total number of generic parameters.
      unsigned numTotalGenericParams;

      /// The number of key parameters in the parent.
      unsigned numKeyGenericParamsInParent;

      /// The number of key parameters locally introduced here.
      unsigned numKeyGenericParamsHere;

      /// Whether this context has any non-key generic parameters.
      bool hasNonKeyGenericParams;
    };

    /// Information about the generic context descriptors that make up \c
    /// descriptor, from the outermost to the innermost.
    mutable llvm::SmallVector<PathElement, 8> descriptorPath;

    /// The number of key generic parameters.
    mutable unsigned numKeyGenericParameters = 0;

    /// Builds the descriptor path.
    ///
    /// \returns a pair containing the number of key generic parameters in
    /// the path up to this point.
    unsigned buildDescriptorPath(const ContextDescriptor *context,
                                 Demangler &demangler) const;

    /// Builds a path from the generic environment.
    unsigned buildEnvironmentPath(
               const TargetGenericEnvironment<InProcess> *environment) const;

    // Set up the state we need to compute substitutions.
    void setup() const;

  public:
    /// Produce substitutions entirely from the given metadata.
    explicit SubstGenericParametersFromMetadata(const Metadata *base)
      : sourceIsMetadata(true), baseContext(base->getTypeContextDescriptor()),
        genericArgs(base ? (const void * const *)base->getGenericArgs()
                         : nullptr) { }
    
    /// Produce substitutions from the given instantiation arguments for the
    /// given context.
    explicit SubstGenericParametersFromMetadata(const ContextDescriptor *base,
                                                const void * const *args)
      : sourceIsMetadata(true), baseContext(base), genericArgs(args)
    {}

    /// Produce substitutions from the given instantiation arguments for the
    /// given generic environment.
    explicit SubstGenericParametersFromMetadata(
               const TargetGenericEnvironment<InProcess> *environment,
               const void * const *arguments)
      : sourceIsMetadata(false), environment(environment),
        genericArgs(arguments) { }
    
    const void * const *getGenericArgs() const { return genericArgs; }

    const Metadata *getMetadata(unsigned depth, unsigned index) const;
    const WitnessTable *getWitnessTable(const Metadata *type,
                                        unsigned index) const;
  };

  /// Retrieve the type metadata described by the given demangled type name.
  ///
  /// \p substGenericParam Function that provides generic argument metadata
  /// given a particular generic parameter specified by depth/index.
  /// \p substWitnessTable Function that provides witness tables given a
  /// particular dependent conformance index.
  SWIFT_CC
  TypeInfo swift_getTypeByMangledNode(
                               MetadataRequest request,
                               Demangler &demangler,
                               Demangle::NodePointer node,
                               const void * const *arguments,
                               SubstGenericParameterFn substGenericParam,
                               SubstDependentWitnessTableFn substWitnessTable);

  /// Retrieve the type metadata described by the given type name.
  ///
  /// \p substGenericParam Function that provides generic argument metadata
  /// given a particular generic parameter specified by depth/index.
  /// \p substWitnessTable Function that provides witness tables given a
  /// particular dependent conformance index.
  SWIFT_CC
  TypeInfo swift_getTypeByMangledName(
                               MetadataRequest request,
                               StringRef typeName,
                               const void * const *arguments,
                               SubstGenericParameterFn substGenericParam,
                               SubstDependentWitnessTableFn substWitnessTable);

  /// Function object that produces substitutions for the generic parameters
  /// that occur within a mangled name, using the complete set of generic
  /// arguments "as written".
  ///
  /// Use with \c _getTypeByMangledName to decode potentially-generic types.
  class SWIFT_RUNTIME_LIBRARY_VISIBILITY SubstGenericParametersFromWrittenArgs {
    /// The complete set of generic arguments.
    const SmallVectorImpl<const Metadata *> &allGenericArgs;

    /// The counts of generic parameters at each level.
    const SmallVectorImpl<unsigned> &genericParamCounts;

  public:
    /// Initialize a new function object to handle substitutions. Both
    /// parameters are references to vectors that must live longer than
    /// this function object.
    ///
    /// \param allGenericArgs The complete set of generic arguments, as written.
    /// This could come directly from "source" (where all generic arguments are
    /// encoded) or from metadata via gatherWrittenGenericArgs().
    ///
    /// \param genericParamCounts The count of generic parameters at each
    /// generic level, typically gathered by _gatherGenericParameterCounts.
    explicit SubstGenericParametersFromWrittenArgs(
        const SmallVectorImpl<const Metadata *> &allGenericArgs,
        const SmallVectorImpl<unsigned> &genericParamCounts)
      : allGenericArgs(allGenericArgs), genericParamCounts(genericParamCounts) {
    }

    const Metadata *getMetadata(unsigned depth, unsigned index) const;
    const WitnessTable *getWitnessTable(const Metadata *type,
                                        unsigned index) const;
  };

  /// Gather generic parameter counts from a context descriptor.
  ///
  /// \returns true if the innermost descriptor is generic.
  bool _gatherGenericParameterCounts(const ContextDescriptor *descriptor,
                                     llvm::SmallVectorImpl<unsigned> &genericParamCounts,
                                     Demangler &BorrowFrom);

  /// Map depth/index to a flat index.
  llvm::Optional<unsigned> _depthIndexToFlatIndex(
                                          unsigned depth, unsigned index,
                                          llvm::ArrayRef<unsigned> paramCounts);

  /// Check the given generic requirements using the given set of generic
  /// arguments, collecting the key arguments (e.g., witness tables) for
  /// the caller.
  ///
  /// \param requirements The set of requirements to evaluate.
  ///
  /// \param extraArguments The extra arguments determined while checking
  /// generic requirements (e.g., those that need to be
  /// passed to an instantiation function) will be added to this vector.
  ///
  /// \returns true if an error occurred, false otherwise.
  bool _checkGenericRequirements(
                    llvm::ArrayRef<GenericRequirementDescriptor> requirements,
                    llvm::SmallVectorImpl<const void *> &extraArguments,
                    SubstGenericParameterFn substGenericParam,
                    SubstDependentWitnessTableFn substWitnessTable);

  /// A helper function which avoids performing a store if the destination
  /// address already contains the source value.  This is useful when
  /// "initializing" memory that might have been initialized to the correct
  /// value statically.  In such a case, the compiler might have gone so far
  /// as to map the entire object readonly, or we might just want to avoid
  /// dirtying memory unnecessarily.
  template <class T>
  static void assignUnlessEqual(T &dest, T newValue) {
    if (dest != newValue)
      dest = newValue;
  }

#if defined(__CYGWIN__)
  void _swift_once_f(uintptr_t *predicate, void *context,
                     void (*function)(void *));
#endif

  static inline const Metadata *getMetadataForClass(const ClassMetadata *c) {
#if SWIFT_OBJC_INTEROP
    return swift_getObjCClassMetadata(c);
#else
    return c;
#endif
  }
  
  template<> inline const ClassMetadata *
  Metadata::getClassObject() const {
    switch (getKind()) {
    case MetadataKind::Class: {
      // Native Swift class metadata is also the class object.
      return static_cast<const ClassMetadata *>(this);
    }
    case MetadataKind::ObjCClassWrapper: {
      // Objective-C class objects are referenced by their Swift metadata wrapper.
      auto wrapper = static_cast<const ObjCClassWrapperMetadata *>(this);
      return wrapper->Class;
    }
    // Other kinds of types don't have class objects.
    default:
      return nullptr;
    }
  }

  void *allocateMetadata(size_t size, size_t align);

  /// Gather the set of generic arguments that would be written in the
  /// source, as a f
  ///
  /// This function computes generic arguments even when they are not
  /// directly represented in the metadata, e.g., generic parameters that
  /// are canonicalized away by same-type constraints and are therefore not
  /// "key" parameters.
  ///
  /// \code
  ///   extension Array where Element == String { }
  ///   extension Dictionary where Key == Value { }
  /// \endcode
  void gatherWrittenGenericArgs(const Metadata *metadata,
                                const TypeContextDescriptor *description,
                                llvm::SmallVectorImpl<const Metadata *> &allGenericArgs,
                                Demangler &BorrowFrom);

  Demangle::NodePointer
  _buildDemanglingForContext(const ContextDescriptor *context,
                             llvm::ArrayRef<NodePointer> demangledGenerics,
                             Demangle::Demangler &Dem);
  
  /// Symbolic reference resolver that produces the demangling tree for the
  /// referenced context.
  class ResolveToDemanglingForContext {
    Demangle::Demangler &Dem;
  public:
    explicit ResolveToDemanglingForContext(Demangle::Demangler &Dem)
      : Dem(Dem) {}
    
    Demangle::NodePointer operator()(Demangle::SymbolicReferenceKind kind,
                                     Demangle::Directness isIndirect,
                                     int32_t offset,
                                     const void *base);
  };

  /// Symbolic reference resolver that resolves the absolute addresses of
  /// symbolic references but leaves them as references.
  class ResolveAsSymbolicReference {
    Demangle::Demangler &Dem;
  public:
    explicit ResolveAsSymbolicReference(Demangle::Demangler &Dem)
      : Dem(Dem) {}
    
    Demangle::NodePointer operator()(Demangle::SymbolicReferenceKind kind,
                                     Demangle::Directness isIndirect,
                                     int32_t offset,
                                     const void *base);
  };
  
  /// Demangler resolver that turns resolved symbolic references into their
  /// demangling trees.
  class ExpandResolvedSymbolicReferences {
    Demangle::Demangler &Dem;
  public:
    explicit ExpandResolvedSymbolicReferences(Demangle::Demangler &Dem)
      : Dem(Dem) {}
    
    Demangle::NodePointer operator()(Demangle::SymbolicReferenceKind kind,
                                     const void *resolvedReference);
  };

  /// Is the given type imported from a C tag type?
  bool _isCImportedTagType(const TypeContextDescriptor *type,
                           const ParsedTypeIdentity &identity);

  /// Check whether a type conforms to a protocol.
  ///
  /// \param value - can be null, in which case the question should
  ///   be answered abstractly if possible
  /// \param conformance - if non-null, and the protocol requires a
  ///   witness table, and the type implements the protocol, the witness
  ///   table will be placed here
  bool _conformsToProtocol(const OpaqueValue *value,
                           const Metadata *type,
                           ProtocolDescriptorRef protocol,
                           const WitnessTable **conformance);

  /// Construct type metadata for the given protocol.
  const Metadata *
  _getSimpleProtocolTypeMetadata(const ProtocolDescriptor *protocol);

  /// Given a type that we know can be used with the given conformance, find
  /// the superclass that introduced the conformance.
  const Metadata *findConformingSuperclass(
                             const Metadata *type,
                             const ProtocolConformanceDescriptor *conformance);

  /// Determine whether the given type conforms to the given Swift protocol,
  /// returning the appropriate protocol conformance descriptor when it does.
  const ProtocolConformanceDescriptor *
  swift_conformsToSwiftProtocol(const Metadata * const type,
                                const ProtocolDescriptor *protocol,
                                StringRef module);

  /// Retrieve an associated type witness from the given witness table.
  ///
  /// \param wtable The witness table.
  /// \param conformingType Metadata for the conforming type.
  /// \param reqBase "Base" requirement used to compute the witness index
  /// \param assocType Associated type descriptor.
  ///
  /// \returns metadata for the associated type witness.
  SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
  MetadataResponse swift_getAssociatedTypeWitnessSlow(
                                        MetadataRequest request,
                                        WitnessTable *wtable,
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
  SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
  const WitnessTable *swift_getAssociatedConformanceWitnessSlow(
                                  WitnessTable *wtable,
                                  const Metadata *conformingType,
                                  const Metadata *assocType,
                                  const ProtocolRequirement *reqBase,
                                  const ProtocolRequirement *assocConformance);
} // end namespace swift

#endif /* SWIFT_RUNTIME_PRIVATE_H */
