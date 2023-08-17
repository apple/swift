//===--- Linking.h - Named declarations and how to link to them -*- C++ -*-===//
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

#ifndef SWIFT_IRGEN_LINKING_H
#define SWIFT_IRGEN_LINKING_H

#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/AST/ProtocolAssociations.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/RequirementSignature.h"
#include "swift/AST/Types.h"
#include "swift/IRGen/ValueWitness.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "swift/SIL/SILModule.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalObject.h"
#include "llvm/IR/Module.h"

namespace llvm {
class Triple;
}

namespace swift {
class AvailabilityContext;

namespace irgen {
class IRGenModule;
class Alignment;

/// Determine if the triple uses the DLL storage.
bool useDllStorage(const llvm::Triple &triple);

class UniversalLinkageInfo {
public:
  bool IsELFObject;
  bool IsMSVCEnvironment;
  bool UseDLLStorage;
  bool Internalize;

  /// True iff are multiple llvm modules.
  bool HasMultipleIGMs;

  /// When this is true, the linkage for forward-declared private symbols will
  /// be promoted to public external. Used by the LLDB expression evaluator.
  bool ForcePublicDecls;

  explicit UniversalLinkageInfo(IRGenModule &IGM);

  UniversalLinkageInfo(const llvm::Triple &triple, bool hasMultipleIGMs,
                       bool forcePublicDecls, bool isStaticLibrary);

  /// In case of multiple llvm modules (in multi-threaded compilation) all
  /// private decls must be visible from other files.
  bool shouldAllPrivateDeclsBeVisibleFromOtherFiles() const {
    return HasMultipleIGMs;
  }
  /// In case of multiple llvm modules, private lazy protocol
  /// witness table accessors could be emitted by two different IGMs during
  /// IRGen into different object files and the linker would complain about
  /// duplicate symbols.
  bool needLinkerToMergeDuplicateSymbols() const { return HasMultipleIGMs; }

  /// This is used by the LLDB expression evaluator since an expression's
  /// llvm::Module may need to access private symbols defined in the
  /// expression's context. This flag ensures that private accessors are
  /// forward-declared as public external in the expression's module.
  bool forcePublicDecls() const { return ForcePublicDecls; }
};

/// Selector for type metadata symbol kinds.
enum class TypeMetadataAddress {
  AddressPoint,
  FullMetadata,
};

/// A link entity is some sort of named declaration, combined with all
/// the information necessary to distinguish specific implementations
/// of the declaration from each other.
///
/// For example, functions may be uncurried at different levels, each of
/// which potentially creates a different top-level function.
class LinkEntity {
  /// ValueDecl*, SILFunction*, or TypeBase*, depending on Kind.
  void *Pointer;

  /// ProtocolConformance*, depending on Kind.
  void *SecondaryPointer;

  /// A hand-rolled bitfield with the following layout:
  unsigned Data;

  enum : unsigned {
    KindShift = 0, KindMask = 0xFF,

    // This field appears in the ValueWitness kind.
    ValueWitnessShift = 8, ValueWitnessMask = 0xFF00,

    // This field appears in the TypeMetadata and ObjCResilientClassStub kinds.
    MetadataAddressShift = 8, MetadataAddressMask = 0x0300,

    // This field appears in associated type access functions.
    AssociatedTypeIndexShift = 8, AssociatedTypeIndexMask = ~KindMask,

    // This field appears in associated conformance access functions.
    AssociatedConformanceIndexShift = 8,
    AssociatedConformanceIndexMask = ~KindMask,

    // This field appears in SILFunction.
    IsDynamicallyReplaceableImplShift = 8,
    IsDynamicallyReplaceableImplMask = ~KindMask,

    // These fields appear in ExtendedExistentialTypeShape.
    ExtendedExistentialIsUniqueShift = 8,
    ExtendedExistentialIsUniqueMask = 0x100,
    ExtendedExistentialIsSharedShift = 9,
    ExtendedExistentialIsSharedMask = 0x200,
  };
#define LINKENTITY_SET_FIELD(field, value) (value << field##Shift)
#define LINKENTITY_GET_FIELD(value, field) ((value & field##Mask) >> field##Shift)

  enum class Kind {
    /// A method dispatch thunk.  The pointer is a FuncDecl* inside a protocol
    /// or a class.
    DispatchThunk,

    /// A derivative method dispatch thunk.  The pointer is a
    /// AbstractFunctionDecl* inside a protocol or a class, and the secondary
    /// pointer is an AutoDiffDerivativeFunctionIdentifier*.
    DispatchThunkDerivative,

    /// A method dispatch thunk for an initializing constructor.  The pointer
    /// is a ConstructorDecl* inside a class.
    DispatchThunkInitializer,

    /// A method dispatch thunk for an allocating constructor.  The pointer is
    /// a ConstructorDecl* inside a protocol or a class.
    DispatchThunkAllocator,

    /// An async function pointer for a method dispatch thunk.  The pointer is
    /// a FuncDecl* inside a protocol or a class.
    DispatchThunkAsyncFunctionPointer,

    /// An async function pointer for a method dispatch thunk for an
    /// initializing constructor.  The pointer is a ConstructorDecl* inside a
    /// class.
    DispatchThunkInitializerAsyncFunctionPointer,

    /// An async function pointer for a method dispatch thunk for an allocating
    /// constructor.  The pointer is a ConstructorDecl* inside a protocol or
    /// a class.
    DispatchThunkAllocatorAsyncFunctionPointer,

    /// An async function pointer for a distributed thunk.
    /// The pointer is a FuncDecl* inside an actor (class).
    DistributedThunkAsyncFunctionPointer,

    /// A method descriptor.  The pointer is a FuncDecl* inside a protocol
    /// or a class.
    MethodDescriptor,

    /// A derivative method descriptor.  The pointer is a AbstractFunctionDecl*
    /// inside a protocol or a class, and the secondary pointer is an
    /// AutoDiffDerivativeFunctionIdentifier*.
    MethodDescriptorDerivative,

    /// A method descriptor for an initializing constructor.  The pointer
    /// is a ConstructorDecl* inside a class.
    MethodDescriptorInitializer,

    /// A method descriptor for an allocating constructor.  The pointer is a
    /// ConstructorDecl* inside a protocol or a class.
    MethodDescriptorAllocator,

    /// A method lookup function for a class.  The pointer is a ClassDecl*.
    MethodLookupFunction,

    /// A resilient enum tag index. The pointer is a EnumElementDecl*.
    EnumCase,

    /// A field offset.  The pointer is a VarDecl*.
    FieldOffset,

    /// An Objective-C class reference.  The pointer is a ClassDecl*.
    ObjCClass,

    /// An Objective-C class reference reference.  The pointer is a ClassDecl*.
    ObjCClassRef,

    /// An Objective-C metaclass reference.  The pointer is a ClassDecl*.
    ObjCMetaclass,

    /// A swift metaclass-stub reference.  The pointer is a ClassDecl*.
    SwiftMetaclassStub,

    /// A callback used by newer Objective-C runtimes to initialize class
    /// metadata for classes where getClassMetadataStrategy() is equal to
    /// ClassMetadataStrategy::Update or ::FixedOrUpdate.
    ObjCMetadataUpdateFunction,

    /// A stub that we emit to allow Clang-generated code to statically refer
    /// to Swift classes with resiliently-sized metadata, since the metadata
    /// is not statically-emitted. Used when getClassMetadataStrategy() is
    /// equal to ClassMetadataStrategy::Resilient.
    ObjCResilientClassStub,

    /// A class metadata base offset global variable.  This stores the offset
    /// of the immediate members of a class (generic parameters, field offsets,
    /// vtable offsets) in the class's metadata.  The immediate members begin
    /// immediately after the superclass members end.
    ///
    /// The pointer is a ClassDecl*.
    ClassMetadataBaseOffset,

    /// The property descriptor for a public property or subscript.
    /// The pointer is an AbstractStorageDecl*.
    PropertyDescriptor,

    /// The nominal type descriptor for a nominal type.
    /// The pointer is a NominalTypeDecl*.
    NominalTypeDescriptor,

    /// The nominal type descriptor runtime record for a nominal type.
    /// The pointer is a NominalTypeDecl*.
    NominalTypeDescriptorRecord,

    /// The descriptor for an opaque type.
    /// The pointer is an OpaqueTypeDecl*.
    OpaqueTypeDescriptor,

    /// The runtime record for a descriptor for an opaque type.
    /// The pointer is an OpaqueTypeDecl*.
    OpaqueTypeDescriptorRecord,

    /// The descriptor accessor for an opaque type used for dynamic functions.
    /// The pointer is an OpaqueTypeDecl*.
    OpaqueTypeDescriptorAccessor,

    /// The descriptor accessor implementation for an opaque type used for
    /// dynamic functions.
    /// The pointer is an OpaqueTypeDecl*.
    OpaqueTypeDescriptorAccessorImpl,

    /// The descriptor accessor key of dynamic replacements for an opaque type.
    /// The pointer is an OpaqueTypeDecl*.
    OpaqueTypeDescriptorAccessorKey,

    /// The descriptor accessor variable of dynamic replacements for an opaque
    /// type.
    /// The pointer is an OpaqueTypeDecl*.
    OpaqueTypeDescriptorAccessorVar,

    /// The metadata pattern for a generic nominal type.
    /// The pointer is a NominalTypeDecl*.
    TypeMetadataPattern,

    /// The instantiation cache for a generic nominal type.
    /// The pointer is a NominalTypeDecl*.
    TypeMetadataInstantiationCache,

    /// The instantiation function for a generic nominal type.
    /// The pointer is a NominalTypeDecl*.
    TypeMetadataInstantiationFunction,

    /// The in-place initialization cache for a generic nominal type.
    /// The pointer is a NominalTypeDecl*.
    TypeMetadataSingletonInitializationCache,

    /// The completion function for a generic or resilient nominal type.
    /// The pointer is a NominalTypeDecl*.
    TypeMetadataCompletionFunction,

    /// The module descriptor for a module.
    /// The pointer is a ModuleDecl*.
    ModuleDescriptor,

    /// The protocol descriptor for a protocol type.
    /// The pointer is a ProtocolDecl*.
    ProtocolDescriptor,

    /// The protocol descriptor runtime record for a protocol type.
    /// The pointer is a ProtocolDecl*.
    ProtocolDescriptorRecord,

    /// The alias referring to the base of the requirements within the
    /// protocol descriptor, which is used to determine the offset of a
    /// particular requirement in the witness table.
    /// The pointer is a ProtocolDecl*.
    ProtocolRequirementsBaseDescriptor,

    /// An descriptor for an associated type within a protocol, which
    /// will alias the TargetProtocolRequirement descripting this
    /// particular associated type.
    /// The pointer is an AssociatedTypeDecl*.
    AssociatedTypeDescriptor,

    /// An descriptor for an associated conformance within a protocol, which
    /// will alias the TargetProtocolRequirement descripting this
    /// particular associated conformance.
    /// The pointer is a ProtocolDecl*; the index of the associated conformance
    /// is stored in the data.
    AssociatedConformanceDescriptor,

    /// A default accessor for an associated conformance of a protocol.
    /// The pointer is a ProtocolDecl*; the index of the associated conformance
    /// is stored in the data.
    DefaultAssociatedConformanceAccessor,

    /// An descriptor for an base conformance within a protocol, which
    /// will alias the TargetProtocolRequirement descripting this
    /// particular base conformance.
    /// The pointer is a ProtocolDecl*; the index of the base conformance
    /// is stored in the data.
    BaseConformanceDescriptor,

    /// A global function pointer for dynamically replaceable functions.
    /// The pointer is a AbstractStorageDecl*.
    DynamicallyReplaceableFunctionVariableAST,

    /// The pointer is a AbstractStorageDecl*.
    DynamicallyReplaceableFunctionKeyAST,

    /// The original implementation of a dynamically replaceable function.
    /// The pointer is a AbstractStorageDecl*.
    DynamicallyReplaceableFunctionImpl,

    /// The once token used by cacheCanonicalSpecializedMetadata, by way of
    /// swift_getCanonicalSpecializedMetadata and
    /// swift_getCanonicalPrespecializedGenericMetadata, to
    /// ensure that canonical prespecialized generic records are only added to
    /// the metadata cache once.
    CanonicalPrespecializedGenericTypeCachingOnceToken,

    /// The same as AsyncFunctionPointer but with a different stored value, for
    /// use by TBDGen.
    /// The pointer is an AbstractFunctionDecl*.
    AsyncFunctionPointerAST,

    /// The pointer is a SILFunction*.
    DynamicallyReplaceableFunctionKey,

    /// A SIL function. The pointer is a SILFunction*.
    SILFunction,

    /// The descriptor for an extension.
    /// The pointer is an ExtensionDecl*.
    ExtensionDescriptor,

    /// The descriptor for a runtime-anonymous context.
    /// The pointer is the DeclContext* of a child of the context that should
    /// be considered private.
    AnonymousDescriptor,

    /// A SIL global variable. The pointer is a SILGlobalVariable*.
    SILGlobalVariable,

    /// An outlined read-only global object. The pointer is a SILGlobalVariable*.
    ReadOnlyGlobalObject,

    // These next few are protocol-conformance kinds.

    /// A direct protocol witness table. The secondary pointer is a
    /// RootProtocolConformance*.
    ProtocolWitnessTable,

    /// A protocol witness table pattern. The secondary pointer is a
    /// ProtocolConformance*.
    ProtocolWitnessTablePattern,

    /// The instantiation function for a generic protocol witness table.
    /// The secondary pointer is a ProtocolConformance*.
    GenericProtocolWitnessTableInstantiationFunction,

    /// A function which returns the witness table for a protocol-constrained
    /// associated type of a protocol.  The secondary pointer is a
    /// ProtocolConformance*.  The index of the associated conformance
    /// requirement is stored in the data.
    AssociatedTypeWitnessTableAccessFunction,

    /// A reflection metadata descriptor for the associated type witnesses of a
    /// nominal type in a protocol conformance.
    ReflectionAssociatedTypeDescriptor,

    /// The protocol conformance descriptor for a conformance.
    /// The pointer is a RootProtocolConformance*.
    ProtocolConformanceDescriptor,

    /// The protocol conformance descriptor runtime record for a conformance.
    /// The pointer is a RootProtocolConformance*.
    ProtocolConformanceDescriptorRecord,

    // These are both type kinds and protocol-conformance kinds.

    /// A lazy protocol witness accessor function. The pointer is a
    /// canonical TypeBase*, and the secondary pointer is a
    /// ProtocolConformance*.
    ProtocolWitnessTableLazyAccessFunction,

    /// A lazy protocol witness cache variable. The pointer is a
    /// canonical TypeBase*, and the secondary pointer is a
    /// ProtocolConformance*.
    ProtocolWitnessTableLazyCacheVariable,

    /// A SIL differentiability witness. The pointer is a
    /// SILDifferentiabilityWitness*.
    DifferentiabilityWitness,

    // Everything following this is a type kind.

    /// A value witness for a type.
    /// The pointer is a canonical TypeBase*.
    ValueWitness,

    /// The value witness table for a type.
    /// The pointer is a canonical TypeBase*.
    ValueWitnessTable,

    /// The metadata or metadata template for a type.
    /// The pointer is a canonical TypeBase*.
    TypeMetadata,

    /// An access function for type metadata.
    /// The pointer is a canonical TypeBase*.
    TypeMetadataAccessFunction,

    /// A lazy cache variable for type metadata.
    /// The pointer is a canonical TypeBase*.
    TypeMetadataLazyCacheVariable,

    /// A lazy cache variable for fetching type metadata from a mangled name.
    /// The pointer is a canonical TypeBase*.
    TypeMetadataDemanglingCacheVariable,

    /// A reflection metadata descriptor for a builtin or imported type.
    ReflectionBuiltinDescriptor,

    /// A reflection metadata descriptor for a struct, enum, class or protocol.
    ReflectionFieldDescriptor,

    /// A coroutine continuation prototype function.
    CoroutineContinuationPrototype,

    /// A global function pointer for dynamically replaceable functions.
    DynamicallyReplaceableFunctionVariable,

    /// A reference to a metaclass-stub for a statically specialized generic
    /// class.
    /// The pointer is a canonical TypeBase*.
    CanonicalSpecializedGenericSwiftMetaclassStub,

    /// An access function for prespecialized type metadata.
    /// The pointer is a canonical TypeBase*.
    CanonicalSpecializedGenericTypeMetadataAccessFunction,

    /// Metadata for a specialized generic type which cannot be statically
    /// guaranteed to be canonical and so must be canonicalized.
    /// The pointer is a canonical TypeBase*.
    NoncanonicalSpecializedGenericTypeMetadata,

    /// A cache variable for noncanonical specialized type metadata, to be
    /// passed to swift_getCanonicalSpecializedMetadata.
    /// The pointer is a canonical TypeBase*.
    NoncanonicalSpecializedGenericTypeMetadataCacheVariable,

    /// Provides the data required to invoke an async function using the async
    /// calling convention in the form of the size of the context to allocate
    /// and the relative address of the function to call with that allocated
    /// context.
    /// The pointer is a SILFunction*.
    AsyncFunctionPointer,

    /// The thunk provided for partially applying a function at some values
    /// which are captured.
    /// The pointer is an llvm::Function*.
    PartialApplyForwarder,

    /// An async function pointer to a partial apply forwarder.
    /// The pointer is the llvm::Function* for a partial apply forwarder.
    PartialApplyForwarderAsyncFunctionPointer,

    /// An async function pointer to a function which is known to exist whose
    /// name is known.
    /// The pointer is a const char* of the name.
    KnownAsyncFunctionPointer,

    /// The pointer is SILFunction*
    DistributedAccessor,
    /// An async function pointer for a distributed accessor (method or property).
    /// The pointer is a SILFunction*.
    DistributedAccessorAsyncPointer,

    /// Accessible function record, which describes a function that can be
    /// looked up by name by the runtime.
    AccessibleFunctionRecord,

    /// Extended existential type shape.
    /// Pointer is the (generalized) existential type.
    /// SecondaryPointer is the GenericSignatureImpl*.
    ExtendedExistentialTypeShape,
  };
  friend struct llvm::DenseMapInfo<LinkEntity>;

  Kind getKind() const {
    return Kind(LINKENTITY_GET_FIELD(Data, Kind));
  }

  static bool isDeclKind(Kind k) {
    return k <= Kind::AsyncFunctionPointerAST;
  }
  static bool isTypeKind(Kind k) {
    return k >= Kind::ProtocolWitnessTableLazyAccessFunction;
  }

  static bool isRootProtocolConformanceKind(Kind k) {
    return (k == Kind::ProtocolConformanceDescriptor ||
            k == Kind::ProtocolConformanceDescriptorRecord ||
            k == Kind::ProtocolWitnessTable);
  }

  static bool isProtocolConformanceKind(Kind k) {
    return (k >= Kind::ProtocolWitnessTable &&
            k <= Kind::ProtocolWitnessTableLazyCacheVariable);
  }

  void setForDecl(Kind kind, const ValueDecl *decl) {
    assert(isDeclKind(kind));
    Pointer = const_cast<void*>(static_cast<const void*>(decl));
    SecondaryPointer = nullptr;
    Data = LINKENTITY_SET_FIELD(Kind, unsigned(kind));
  }

  void setForProtocolAndAssociatedConformance(Kind kind,
                                              const ProtocolDecl *proto,
                                              CanType associatedType,
                                              ProtocolDecl *associatedProtocol){
    assert(isDeclKind(kind));
    Pointer = static_cast<ValueDecl *>(const_cast<ProtocolDecl *>(proto));
    SecondaryPointer = nullptr;
    Data = LINKENTITY_SET_FIELD(Kind, unsigned(kind)) |
           LINKENTITY_SET_FIELD(AssociatedConformanceIndex,
                                getAssociatedConformanceIndex(
                                                          proto,
                                                          associatedType,
                                                          associatedProtocol));
  }

  void setForProtocolConformance(Kind kind, const ProtocolConformance *c) {
    assert(isProtocolConformanceKind(kind) && !isTypeKind(kind));
    Pointer = nullptr;
    SecondaryPointer = const_cast<void*>(static_cast<const void*>(c));
    Data = LINKENTITY_SET_FIELD(Kind, unsigned(kind));
  }

  void setForProtocolConformanceAndType(Kind kind, const ProtocolConformance *c,
                                        CanType type) {
    assert(isProtocolConformanceKind(kind) && isTypeKind(kind));
    Pointer = type.getPointer();
    SecondaryPointer = const_cast<void*>(static_cast<const void*>(c));
    Data = LINKENTITY_SET_FIELD(Kind, unsigned(kind));
  }

  void setForProtocolConformanceAndAssociatedType(Kind kind,
                                                  const ProtocolConformance *c,
                                                  AssociatedTypeDecl *associate) {
    assert(isProtocolConformanceKind(kind));
    Pointer = nullptr;
    SecondaryPointer = const_cast<void*>(static_cast<const void*>(c));
    Data = LINKENTITY_SET_FIELD(Kind, unsigned(kind)) |
           LINKENTITY_SET_FIELD(AssociatedTypeIndex,
                                getAssociatedTypeIndex(c, associate));
  }

  void setForProtocolConformanceAndAssociatedConformance(Kind kind,
                                                  const ProtocolConformance *c,
                                                  CanType associatedType,
                                            ProtocolDecl *associatedProtocol) {
    assert(isProtocolConformanceKind(kind));
    Pointer = associatedProtocol;
    SecondaryPointer = const_cast<void*>(static_cast<const void*>(c));
    Data = LINKENTITY_SET_FIELD(Kind, unsigned(kind)) |
           LINKENTITY_SET_FIELD(AssociatedConformanceIndex,
                                getAssociatedConformanceIndex(c, associatedType,
                                                          associatedProtocol));
  }

  // We store associated types using their index in their parent protocol
  // in order to avoid bloating LinkEntity out to three key pointers.
  static unsigned getAssociatedTypeIndex(const ProtocolConformance *conformance,
                                         AssociatedTypeDecl *associate) {
    auto *proto = associate->getProtocol();
    assert(conformance->getProtocol() == proto);
    unsigned result = 0;
    for (auto requirement : proto->getAssociatedTypeMembers()) {
      if (requirement == associate) return result;
      result++;
    }
    llvm_unreachable("didn't find associated type in protocol?");
  }

  static AssociatedTypeDecl *
  getAssociatedTypeByIndex(const ProtocolConformance *conformance,
                           unsigned index) {
    for (auto associate : conformance->getProtocol()->getAssociatedTypeMembers()) {
      if (index == 0) return associate;
      index--;
    }
    llvm_unreachable("didn't find associated type in protocol?");
  }

  // We store associated conformances using their index in the requirement
  // list of the requirement signature of the protocol.
  static unsigned getAssociatedConformanceIndex(const ProtocolDecl *proto,
                                                CanType associatedType,
                                                ProtocolDecl *requirement) {
    unsigned index = 0;
    for (const auto &reqt : proto->getRequirementSignature().getRequirements()) {
      if (reqt.getKind() == RequirementKind::Conformance &&
          reqt.getFirstType()->getCanonicalType() == associatedType &&
          reqt.getProtocolDecl() == requirement) {
        return index;
      }
      ++index;
    }
    llvm_unreachable("requirement not found in protocol");
  }

  // We store associated conformances using their index in the requirement
  // list of the requirement signature of the conformance's protocol.
  static unsigned getAssociatedConformanceIndex(
                                      const ProtocolConformance *conformance,
                                      CanType associatedType,
                                      ProtocolDecl *requirement) {
    return getAssociatedConformanceIndex(conformance->getProtocol(),
                                         associatedType, requirement);
  }

  static std::pair<CanType, ProtocolDecl*>
  getAssociatedConformanceByIndex(const ProtocolDecl *proto,
                                  unsigned index) {
    auto &reqt = proto->getRequirementSignature().getRequirements()[index];
    assert(reqt.getKind() == RequirementKind::Conformance);
    return { reqt.getFirstType()->getCanonicalType(),
             reqt.getProtocolDecl() };
  }

  static std::pair<CanType, ProtocolDecl*>
  getAssociatedConformanceByIndex(const ProtocolConformance *conformance,
                                  unsigned index) {
    return getAssociatedConformanceByIndex(conformance->getProtocol(), index);
  }

  void
  setForDifferentiabilityWitness(Kind kind,
                                 const SILDifferentiabilityWitness *witness) {
    Pointer = const_cast<void *>(static_cast<const void *>(witness));
    SecondaryPointer = nullptr;
    Data = LINKENTITY_SET_FIELD(Kind, unsigned(kind));
  }

  void setForType(Kind kind, CanType type) {
    assert(isTypeKind(kind));
    Pointer = type.getPointer();
    SecondaryPointer = nullptr;
    Data = LINKENTITY_SET_FIELD(Kind, unsigned(kind));
  }

  LinkEntity() : Pointer(nullptr), SecondaryPointer(nullptr), Data(0) {}

  static bool isValidResilientMethodRef(SILDeclRef declRef) {
    if (declRef.isForeign)
      return false;
    
    auto *decl = declRef.getDecl();
    return (isa<ClassDecl>(decl->getDeclContext()) ||
            isa<ProtocolDecl>(decl->getDeclContext()));
  }

  SILDeclRef::Kind getSILDeclRefKind() const;

public:
  static LinkEntity forDispatchThunk(SILDeclRef declRef) {
    assert(isValidResilientMethodRef(declRef));

    if (declRef.isAutoDiffDerivativeFunction()) {
      LinkEntity entity;
      // The derivative function for any decl is always a method (not an
      // initializer).
      entity.setForDecl(Kind::DispatchThunkDerivative, declRef.getDecl());
      entity.SecondaryPointer =
          declRef.getAutoDiffDerivativeFunctionIdentifier();
      return entity;
    }

    LinkEntity::Kind kind;
    switch (declRef.kind) {
    case SILDeclRef::Kind::Func:
      kind = Kind::DispatchThunk;
      break;
    case SILDeclRef::Kind::Initializer:
      kind = Kind::DispatchThunkInitializer;
      break;
    case SILDeclRef::Kind::Allocator:
      kind = Kind::DispatchThunkAllocator;
      break;
    default:
      llvm_unreachable("Bad SILDeclRef for dispatch thunk");
    }

    LinkEntity entity;
    entity.setForDecl(kind, declRef.getDecl());
    return entity;
  }

  static LinkEntity forMethodDescriptor(SILDeclRef declRef) {
    assert(isValidResilientMethodRef(declRef));

    if (declRef.isAutoDiffDerivativeFunction()) {
      LinkEntity entity;
      // The derivative function for any decl is always a method (not an
      // initializer).
      entity.setForDecl(Kind::MethodDescriptorDerivative, declRef.getDecl());
      entity.SecondaryPointer =
          declRef.getAutoDiffDerivativeFunctionIdentifier();
      return entity;
    }

    LinkEntity::Kind kind;
    switch (declRef.kind) {
    case SILDeclRef::Kind::Func:
      kind = Kind::MethodDescriptor;
      break;
    case SILDeclRef::Kind::Initializer:
      kind = Kind::MethodDescriptorInitializer;
      break;
    case SILDeclRef::Kind::Allocator:
      kind = Kind::MethodDescriptorAllocator;
      break;
    default:
      llvm_unreachable("Bad SILDeclRef for method descriptor");
    }

    LinkEntity entity;
    entity.setForDecl(kind, declRef.getDecl());
    return entity;
  }

  static LinkEntity forMethodLookupFunction(ClassDecl *classDecl) {
    LinkEntity entity;
    entity.setForDecl(Kind::MethodLookupFunction, classDecl);
    return entity;
  }

  static LinkEntity forFieldOffset(VarDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::FieldOffset, decl);
    return entity;
  }

  static LinkEntity forEnumCase(EnumElementDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::EnumCase, decl);
    return entity;
  }

  static LinkEntity forObjCClassRef(ClassDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::ObjCClassRef, decl);
    return entity;
  }

  static LinkEntity forObjCClass(ClassDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::ObjCClass, decl);
    return entity;
  }

  static LinkEntity forObjCMetaclass(ClassDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::ObjCMetaclass, decl);
    return entity;
  }

  static LinkEntity forSwiftMetaclassStub(ClassDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::SwiftMetaclassStub, decl);
    return entity;
  }

  static LinkEntity forObjCMetadataUpdateFunction(ClassDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::ObjCMetadataUpdateFunction, decl);
    return entity;
  }

  static LinkEntity forObjCResilientClassStub(ClassDecl *decl,
                                              TypeMetadataAddress addr) {
    LinkEntity entity;
    entity.setForDecl(Kind::ObjCResilientClassStub, decl);
    entity.Data |= LINKENTITY_SET_FIELD(MetadataAddress, unsigned(addr));
    return entity;
  }

  static LinkEntity forTypeMetadata(CanType concreteType,
                                    TypeMetadataAddress addr) {
    assert(!isObjCImplementation(concreteType));
    LinkEntity entity;
    entity.setForType(Kind::TypeMetadata, concreteType);
    entity.Data |= LINKENTITY_SET_FIELD(MetadataAddress, unsigned(addr));
    return entity;
  }

  static LinkEntity forTypeMetadataPattern(NominalTypeDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::TypeMetadataPattern, decl);
    return entity;
  }

  static LinkEntity forTypeMetadataAccessFunction(CanType type) {
    LinkEntity entity;
    entity.setForType(Kind::TypeMetadataAccessFunction, type);
    return entity;
  }

  static LinkEntity forTypeMetadataInstantiationCache(NominalTypeDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::TypeMetadataInstantiationCache, decl);
    return entity;
  }

  static LinkEntity forTypeMetadataInstantiationFunction(NominalTypeDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::TypeMetadataInstantiationFunction, decl);
    return entity;
  }

  static LinkEntity forTypeMetadataSingletonInitializationCache(
                                                      NominalTypeDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::TypeMetadataSingletonInitializationCache, decl);
    return entity;
  }

  static LinkEntity forTypeMetadataCompletionFunction(NominalTypeDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::TypeMetadataCompletionFunction, decl);
    return entity;
  }

  static LinkEntity forTypeMetadataLazyCacheVariable(CanType type) {
    LinkEntity entity;
    entity.setForType(Kind::TypeMetadataLazyCacheVariable, type);
    return entity;
  }

  static LinkEntity forTypeMetadataDemanglingCacheVariable(CanType type) {
    LinkEntity entity;
    entity.setForType(Kind::TypeMetadataDemanglingCacheVariable, type);
    return entity;
  }

  static LinkEntity forClassMetadataBaseOffset(ClassDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::ClassMetadataBaseOffset, decl);
    return entity;
  }

  static LinkEntity forNominalTypeDescriptor(NominalTypeDecl *decl) {
    assert(!isObjCImplementation(decl));
    LinkEntity entity;
    entity.setForDecl(Kind::NominalTypeDescriptor, decl);
    return entity;
  }

  static LinkEntity forNominalTypeDescriptorRecord(NominalTypeDecl *decl) {
    assert(!isObjCImplementation(decl));
    LinkEntity entity;
    entity.setForDecl(Kind::NominalTypeDescriptorRecord, decl);
    return entity;
  }

  static LinkEntity forOpaqueTypeDescriptor(OpaqueTypeDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::OpaqueTypeDescriptor, decl);
    return entity;
  }

  static LinkEntity forOpaqueTypeDescriptorRecord(OpaqueTypeDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::OpaqueTypeDescriptorRecord, decl);
    return entity;
  }

  static LinkEntity forOpaqueTypeDescriptorAccessor(OpaqueTypeDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::OpaqueTypeDescriptorAccessor, decl);
    return entity;
  }

  static LinkEntity forOpaqueTypeDescriptorAccessorImpl(OpaqueTypeDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::OpaqueTypeDescriptorAccessorImpl, decl);
    return entity;
  }

  static LinkEntity forOpaqueTypeDescriptorAccessorKey(OpaqueTypeDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::OpaqueTypeDescriptorAccessorKey, decl);
    return entity;
  }

  static LinkEntity forOpaqueTypeDescriptorAccessorVar(OpaqueTypeDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::OpaqueTypeDescriptorAccessorVar, decl);
    return entity;
  }

  static LinkEntity forPropertyDescriptor(AbstractStorageDecl *decl) {
    assert(decl->exportsPropertyDescriptor());
    LinkEntity entity;
    entity.setForDecl(Kind::PropertyDescriptor, decl);
    return entity;
  }

  static LinkEntity forModuleDescriptor(ModuleDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::ModuleDescriptor, decl);
    return entity;
  }

  static LinkEntity forExtensionDescriptor(ExtensionDecl *decl) {
    LinkEntity entity;
    entity.Pointer = const_cast<void*>(static_cast<const void*>(decl));
    entity.SecondaryPointer = nullptr;
    entity.Data =
      LINKENTITY_SET_FIELD(Kind, unsigned(Kind::ExtensionDescriptor));
    return entity;
  }

  static LinkEntity forAnonymousDescriptor(
                                    PointerUnion<DeclContext *, VarDecl *> dc) {
    LinkEntity entity;
    entity.Pointer = dc.getOpaqueValue();
    entity.SecondaryPointer = nullptr;
    entity.Data =
      LINKENTITY_SET_FIELD(Kind, unsigned(Kind::AnonymousDescriptor));
    return entity;
  }

  static LinkEntity forProtocolDescriptor(ProtocolDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::ProtocolDescriptor, decl);
    return entity;
  }

  static LinkEntity forProtocolDescriptorRecord(ProtocolDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::ProtocolDescriptorRecord, decl);
    return entity;
  }

  static LinkEntity forProtocolRequirementsBaseDescriptor(ProtocolDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::ProtocolRequirementsBaseDescriptor, decl);
    return entity;
  }

  static LinkEntity forValueWitness(CanType concreteType, ValueWitness witness) {
    LinkEntity entity;
    entity.Pointer = concreteType.getPointer();
    entity.Data = LINKENTITY_SET_FIELD(Kind, unsigned(Kind::ValueWitness))
                | LINKENTITY_SET_FIELD(ValueWitness, unsigned(witness));
    return entity;
  }

  static LinkEntity forValueWitnessTable(CanType type) {
    LinkEntity entity;
    entity.setForType(Kind::ValueWitnessTable, type);
    return entity;
  }

  static LinkEntity
  forSILFunction(SILFunction *F,
                 bool IsDynamicallyReplaceableImplementation=false) {
    LinkEntity entity;
    entity.Pointer = F;
    entity.SecondaryPointer = nullptr;
    entity.Data =
        LINKENTITY_SET_FIELD(Kind, unsigned(Kind::SILFunction)) |
        LINKENTITY_SET_FIELD(IsDynamicallyReplaceableImpl,
                             (unsigned)IsDynamicallyReplaceableImplementation);
    return entity;
  }

  static LinkEntity forSILGlobalVariable(SILGlobalVariable *G, IRGenModule &IGM);

  static LinkEntity
  forDifferentiabilityWitness(const SILDifferentiabilityWitness *witness) {
    LinkEntity entity;
    entity.setForDifferentiabilityWitness(Kind::DifferentiabilityWitness,
                                          witness);
    return entity;
  }

  static LinkEntity forProtocolWitnessTable(const RootProtocolConformance *C) {
    LinkEntity entity;
    entity.setForProtocolConformance(Kind::ProtocolWitnessTable, C);
    return entity;
  }

  static LinkEntity
  forProtocolWitnessTablePattern(const ProtocolConformance *C) {
    LinkEntity entity;
    entity.setForProtocolConformance(Kind::ProtocolWitnessTablePattern, C);
    return entity;
  }

  static LinkEntity
  forGenericProtocolWitnessTableInstantiationFunction(
                                      const ProtocolConformance *C) {
    LinkEntity entity;
    entity.setForProtocolConformance(
                     Kind::GenericProtocolWitnessTableInstantiationFunction, C);
    return entity;
  }

  static LinkEntity
  forProtocolWitnessTableLazyAccessFunction(const ProtocolConformance *C,
                                            CanType type) {
    LinkEntity entity;
    entity.setForProtocolConformanceAndType(
             Kind::ProtocolWitnessTableLazyAccessFunction, C, type);
    return entity;
  }

  static LinkEntity
  forProtocolWitnessTableLazyCacheVariable(const ProtocolConformance *C,
                                           CanType type) {
    LinkEntity entity;
    entity.setForProtocolConformanceAndType(
             Kind::ProtocolWitnessTableLazyCacheVariable, C, type);
    return entity;
  }

  static LinkEntity
  forAssociatedTypeDescriptor(AssociatedTypeDecl *assocType) {
    LinkEntity entity;
    entity.setForDecl(Kind::AssociatedTypeDescriptor, assocType);
    return entity;
  }

  static LinkEntity
  forAssociatedConformanceDescriptor(AssociatedConformance conformance) {
    LinkEntity entity;
    entity.setForProtocolAndAssociatedConformance(
        Kind::AssociatedConformanceDescriptor,
        conformance.getSourceProtocol(),
        conformance.getAssociation(),
        conformance.getAssociatedRequirement());
    return entity;
  }

  static LinkEntity
  forBaseConformanceDescriptor(BaseConformance conformance) {
    LinkEntity entity;
    entity.setForProtocolAndAssociatedConformance(
        Kind::BaseConformanceDescriptor,
        conformance.getSourceProtocol(),
        conformance.getSourceProtocol()->getSelfInterfaceType()
          ->getCanonicalType(),
        conformance.getBaseRequirement());
    return entity;
  }

  static LinkEntity
  forAssociatedTypeWitnessTableAccessFunction(const ProtocolConformance *C,
                                     const AssociatedConformance &association) {
    LinkEntity entity;
    entity.setForProtocolConformanceAndAssociatedConformance(
                     Kind::AssociatedTypeWitnessTableAccessFunction, C,
                     association.getAssociation(),
                     association.getAssociatedRequirement());
    return entity;
  }

  static LinkEntity
  forDefaultAssociatedConformanceAccessor(AssociatedConformance conformance) {
    LinkEntity entity;
    entity.setForProtocolAndAssociatedConformance(
        Kind::DefaultAssociatedConformanceAccessor,
        conformance.getSourceProtocol(),
        conformance.getAssociation(),
        conformance.getAssociatedRequirement());
    return entity;
  }

  static LinkEntity forReflectionBuiltinDescriptor(CanType type) {
    LinkEntity entity;
    entity.setForType(Kind::ReflectionBuiltinDescriptor, type);
    return entity;
  }

  static LinkEntity forReflectionFieldDescriptor(CanType type) {
    LinkEntity entity;
    entity.setForType(Kind::ReflectionFieldDescriptor, type);
    return entity;
  }

  static LinkEntity
  forReflectionAssociatedTypeDescriptor(const ProtocolConformance *C) {
    LinkEntity entity;
    entity.setForProtocolConformance(
        Kind::ReflectionAssociatedTypeDescriptor, C);
    return entity;
  }

  static LinkEntity
  forProtocolConformanceDescriptor(const RootProtocolConformance *C) {
    LinkEntity entity;
    entity.setForProtocolConformance(Kind::ProtocolConformanceDescriptor, C);
    return entity;
  }

  static LinkEntity
  forProtocolConformanceDescriptorRecord(const RootProtocolConformance *C) {
    LinkEntity entity;
    entity.setForProtocolConformance(Kind::ProtocolConformanceDescriptorRecord,
                                     C);
    return entity;
  }

  static LinkEntity forCoroutineContinuationPrototype(CanSILFunctionType type) {
    LinkEntity entity;
    entity.setForType(Kind::CoroutineContinuationPrototype, type);
    return entity;
  }

  static LinkEntity forDynamicallyReplaceableFunctionVariable(SILFunction *F) {
    LinkEntity entity;
    entity.Pointer = F;
    entity.SecondaryPointer = nullptr;
    entity.Data = LINKENTITY_SET_FIELD(
        Kind, unsigned(Kind::DynamicallyReplaceableFunctionVariable));
    return entity;
  }

  static LinkEntity
  forDynamicallyReplaceableFunctionVariable(AbstractFunctionDecl *decl,
                                            bool isAllocator) {
    LinkEntity entity;
    entity.setForDecl(Kind::DynamicallyReplaceableFunctionVariableAST, decl);
    entity.SecondaryPointer = isAllocator ? decl : nullptr;
    return entity;
  }

  static LinkEntity forDynamicallyReplaceableFunctionKey(SILFunction *F) {
    LinkEntity entity;
    entity.Pointer = F;
    entity.SecondaryPointer = nullptr;
    entity.Data = LINKENTITY_SET_FIELD(
        Kind, unsigned(Kind::DynamicallyReplaceableFunctionKey));
    return entity;
  }

  static LinkEntity
  forDynamicallyReplaceableFunctionKey(AbstractFunctionDecl *decl,
                                       bool isAllocator) {
    LinkEntity entity;
    entity.setForDecl(Kind::DynamicallyReplaceableFunctionKeyAST, decl);
    entity.SecondaryPointer = isAllocator ? decl : nullptr;
    return entity;
  }

  static LinkEntity
  forDynamicallyReplaceableFunctionImpl(AbstractFunctionDecl *decl,
                                        bool isAllocator) {
    LinkEntity entity;
    entity.setForDecl(Kind::DynamicallyReplaceableFunctionImpl, decl);
    entity.SecondaryPointer = isAllocator ? decl : nullptr;
    return entity;
  }

  static LinkEntity
  forCanonicalPrespecializedGenericTypeCachingOnceToken(NominalTypeDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::CanonicalPrespecializedGenericTypeCachingOnceToken,
                      decl);
    return entity;
  }

  static LinkEntity
  forSpecializedGenericSwiftMetaclassStub(CanType concreteType) {
    LinkEntity entity;
    entity.setForType(Kind::CanonicalSpecializedGenericSwiftMetaclassStub,
                      concreteType);
    return entity;
  }

  static LinkEntity
  forPrespecializedTypeMetadataAccessFunction(CanType theType) {
    LinkEntity entity;
    entity.setForType(
        Kind::CanonicalSpecializedGenericTypeMetadataAccessFunction, theType);
    return entity;
  }

  static LinkEntity
  forNoncanonicalSpecializedGenericTypeMetadata(CanType theType) {
    LinkEntity entity;
    entity.setForType(Kind::NoncanonicalSpecializedGenericTypeMetadata,
                      theType);
    entity.Data |= LINKENTITY_SET_FIELD(
        MetadataAddress, unsigned(TypeMetadataAddress::FullMetadata));
    return entity;
  }


  static LinkEntity
  forNoncanonicalSpecializedGenericTypeMetadataCacheVariable(CanType theType) {
    LinkEntity entity;
    entity.setForType(Kind::NoncanonicalSpecializedGenericTypeMetadataCacheVariable, theType);
    return entity;
  }

  static LinkEntity forAsyncFunctionPointer(LinkEntity other) {
    LinkEntity entity;
    entity.Pointer = other.Pointer;
    entity.SecondaryPointer = nullptr;

    switch (other.getKind()) {
    case LinkEntity::Kind::SILFunction:
      entity.Data = LINKENTITY_SET_FIELD(
          Kind, unsigned(LinkEntity::Kind::AsyncFunctionPointer));
      break;

    case LinkEntity::Kind::DispatchThunk:
      entity.Data = LINKENTITY_SET_FIELD(
          Kind, unsigned(LinkEntity::Kind::DispatchThunkAsyncFunctionPointer));
      break;

    case LinkEntity::Kind::DispatchThunkInitializer:
      entity.Data = LINKENTITY_SET_FIELD(
          Kind, unsigned(LinkEntity::Kind::DispatchThunkInitializerAsyncFunctionPointer));
      break;

    case LinkEntity::Kind::DispatchThunkAllocator:
      entity.Data = LINKENTITY_SET_FIELD(
          Kind, unsigned(LinkEntity::Kind::DispatchThunkAllocatorAsyncFunctionPointer));
      break;
    case LinkEntity::Kind::PartialApplyForwarder:
      entity.Data = LINKENTITY_SET_FIELD(
          Kind, unsigned(LinkEntity::Kind::PartialApplyForwarderAsyncFunctionPointer));
      break;

    case LinkEntity::Kind::DistributedAccessor: {
      entity.Data = LINKENTITY_SET_FIELD(
          Kind, unsigned(LinkEntity::Kind::DistributedAccessorAsyncPointer));
      break;
    }

    default:
      llvm_unreachable("Link entity kind cannot have an async function pointer");
    }

    return entity;
  }

  static LinkEntity forAsyncFunctionPointer(SILDeclRef declRef) {
    LinkEntity entity;
    entity.setForDecl(declRef.isDistributedThunk()
                          ? Kind::DistributedThunkAsyncFunctionPointer
                          : Kind::AsyncFunctionPointerAST,
                      declRef.getAbstractFunctionDecl());
    entity.SecondaryPointer =
        reinterpret_cast<void *>(static_cast<uintptr_t>(declRef.kind));
    return entity;
  }

  static LinkEntity forKnownAsyncFunctionPointer(const char *name) {
    LinkEntity entity;
    entity.Pointer = const_cast<char *>(name);
    entity.SecondaryPointer = nullptr;
    entity.Data =
        LINKENTITY_SET_FIELD(Kind, unsigned(Kind::KnownAsyncFunctionPointer));
    return entity;
  }

  static LinkEntity forDistributedTargetAccessor(SILFunction *target) {
    LinkEntity entity;
    entity.Pointer = target;
    entity.SecondaryPointer = nullptr;
    entity.Data =
        LINKENTITY_SET_FIELD(Kind, unsigned(Kind::DistributedAccessor));
    return entity;
  }

  static LinkEntity forAccessibleFunctionRecord(SILFunction *func) {
    LinkEntity entity;
    entity.Pointer = func;
    entity.SecondaryPointer = nullptr;
    entity.Data =
        LINKENTITY_SET_FIELD(Kind, unsigned(Kind::AccessibleFunctionRecord));
    return entity;
  }

  LinkEntity getUnderlyingEntityForAsyncFunctionPointer() const {
    LinkEntity entity;
    entity.Pointer = Pointer;
    entity.SecondaryPointer = nullptr;

    switch (getKind()) {
    case LinkEntity::Kind::AsyncFunctionPointer:
      entity.Data = LINKENTITY_SET_FIELD(
          Kind, unsigned(LinkEntity::Kind::SILFunction));
      break;

    case LinkEntity::Kind::DispatchThunkAsyncFunctionPointer:
      entity.Data = LINKENTITY_SET_FIELD(
          Kind, unsigned(LinkEntity::Kind::DispatchThunk));
      break;

    case LinkEntity::Kind::DispatchThunkInitializerAsyncFunctionPointer:
      entity.Data = LINKENTITY_SET_FIELD(
          Kind, unsigned(LinkEntity::Kind::DispatchThunkInitializer));
      break;

    case LinkEntity::Kind::DispatchThunkAllocatorAsyncFunctionPointer:
      entity.Data = LINKENTITY_SET_FIELD(
          Kind, unsigned(LinkEntity::Kind::DispatchThunkAllocator));
      break;

    case LinkEntity::Kind::PartialApplyForwarderAsyncFunctionPointer:
      entity.Data = LINKENTITY_SET_FIELD(
          Kind, unsigned(LinkEntity::Kind::PartialApplyForwarder));
      break;

    case LinkEntity::Kind::DistributedAccessorAsyncPointer:
      entity.Data = LINKENTITY_SET_FIELD(
          Kind, unsigned(LinkEntity::Kind::DistributedAccessor));
      break;

    default:
      llvm_unreachable("Link entity is not an async function pointer");
    }

    return entity;
  }

  static LinkEntity forPartialApplyForwarder(llvm::Function *function) {
    LinkEntity entity;
    entity.Pointer = function;
    entity.SecondaryPointer = nullptr;
    entity.Data =
        LINKENTITY_SET_FIELD(Kind, unsigned(Kind::PartialApplyForwarder));
    return entity;
  }

  static LinkEntity forExtendedExistentialTypeShape(CanGenericSignature genSig,
                                                    CanType existentialType,
                                                    bool isUnique,
                                                    bool isShared) {
    LinkEntity entity;
    entity.Pointer = existentialType.getPointer();
    entity.SecondaryPointer =
      const_cast<GenericSignatureImpl*>(genSig.getPointer());
    entity.Data =
        LINKENTITY_SET_FIELD(Kind, unsigned(Kind::ExtendedExistentialTypeShape))
      | LINKENTITY_SET_FIELD(ExtendedExistentialIsUnique, unsigned(isUnique))
      | LINKENTITY_SET_FIELD(ExtendedExistentialIsShared, unsigned(isShared));
    return entity;
  }

  void mangle(llvm::raw_ostream &out) const;
  void mangle(SmallVectorImpl<char> &buffer) const;
  std::string mangleAsString() const;

  SILDeclRef getSILDeclRef() const;
  SILLinkage getLinkage(ForDefinition_t isDefinition) const;

  bool hasDecl() const {
    return isDeclKind(getKind());
  }

  const ValueDecl *getDecl() const {
    assert(isDeclKind(getKind()));
    return reinterpret_cast<ValueDecl*>(Pointer);
  }
  
  const ExtensionDecl *getExtension() const {
    assert(getKind() == Kind::ExtensionDescriptor);
    return reinterpret_cast<ExtensionDecl*>(Pointer);
  }

  const PointerUnion<DeclContext *, VarDecl *> getAnonymousDeclContext() const {
    assert(getKind() == Kind::AnonymousDescriptor);
    return PointerUnion<DeclContext *, VarDecl *>
      ::getFromOpaqueValue(reinterpret_cast<void*>(Pointer));
  }

  bool hasSILFunction() const {
    return getKind() == Kind::AsyncFunctionPointer ||
           getKind() == Kind::DynamicallyReplaceableFunctionVariable ||
           getKind() == Kind::DynamicallyReplaceableFunctionKey ||
           getKind() == Kind::SILFunction ||
           getKind() == Kind::DistributedAccessor ||
           getKind() == Kind::AccessibleFunctionRecord;
  }

  SILFunction *getSILFunction() const {
    assert(hasSILFunction());
    return reinterpret_cast<SILFunction *>(Pointer);
  }

  SILGlobalVariable *getSILGlobalVariable() const {
    assert(getKind() == Kind::SILGlobalVariable ||
           getKind() == Kind::ReadOnlyGlobalObject);
    return reinterpret_cast<SILGlobalVariable*>(Pointer);
  }

  SILDifferentiabilityWitness *getSILDifferentiabilityWitness() const {
    assert(getKind() == Kind::DifferentiabilityWitness);
    return reinterpret_cast<SILDifferentiabilityWitness *>(Pointer);
  }

  const RootProtocolConformance *getRootProtocolConformance() const {
    assert(isRootProtocolConformanceKind(getKind()));
    return cast<RootProtocolConformance>(getProtocolConformance());
  }
  
  const ProtocolConformance *getProtocolConformance() const {
    assert(isProtocolConformanceKind(getKind()));
    return reinterpret_cast<ProtocolConformance*>(SecondaryPointer);
  }

  AssociatedTypeDecl *getAssociatedType() const {
    assert(getKind() == Kind::AssociatedTypeDescriptor);
    return reinterpret_cast<AssociatedTypeDecl *>(Pointer);
  }

  std::pair<CanType, ProtocolDecl *> getAssociatedConformance() const {
    if (getKind() == Kind::AssociatedTypeWitnessTableAccessFunction) {
      return getAssociatedConformanceByIndex(getProtocolConformance(),
                       LINKENTITY_GET_FIELD(Data, AssociatedConformanceIndex));
    }

    assert(getKind() == Kind::AssociatedConformanceDescriptor ||
           getKind() == Kind::DefaultAssociatedConformanceAccessor ||
           getKind() == Kind::BaseConformanceDescriptor);
    return getAssociatedConformanceByIndex(
             cast<ProtocolDecl>(getDecl()),
             LINKENTITY_GET_FIELD(Data, AssociatedConformanceIndex));
  }

  ProtocolDecl *getAssociatedProtocol() const {
    assert(getKind() == Kind::AssociatedTypeWitnessTableAccessFunction);
    return reinterpret_cast<ProtocolDecl*>(Pointer);
  }

  AutoDiffDerivativeFunctionIdentifier *
  getAutoDiffDerivativeFunctionIdentifier() const {
    assert(getKind() == Kind::DispatchThunkDerivative ||
           getKind() == Kind::MethodDescriptorDerivative);
    return reinterpret_cast<AutoDiffDerivativeFunctionIdentifier*>(
        SecondaryPointer);
  }

  CanGenericSignature getExtendedExistentialTypeShapeGenSig() const {
    assert(getKind() == Kind::ExtendedExistentialTypeShape);
    return CanGenericSignature(
             reinterpret_cast<const GenericSignatureImpl*>(SecondaryPointer));
  }

  CanType getExtendedExistentialTypeShapeType() const {
    assert(getKind() == Kind::ExtendedExistentialTypeShape);
    return CanType(reinterpret_cast<TypeBase*>(Pointer));
  }

  bool isExtendedExistentialTypeShapeUnique() const {
    assert(getKind() == Kind::ExtendedExistentialTypeShape);
    return LINKENTITY_GET_FIELD(Data, ExtendedExistentialIsUnique);
  }

  bool isExtendedExistentialTypeShapeShared() const {
    assert(getKind() == Kind::ExtendedExistentialTypeShape);
    return LINKENTITY_GET_FIELD(Data, ExtendedExistentialIsShared);
  }

  bool isDynamicallyReplaceable() const {
    assert(getKind() == Kind::SILFunction);
    return LINKENTITY_GET_FIELD(Data, IsDynamicallyReplaceableImpl);
  }
  bool isDynamicallyReplaceableKey() const {
    return getKind() == Kind::DynamicallyReplaceableFunctionKey ||
      getKind() == Kind::OpaqueTypeDescriptorAccessorKey;
  }
  bool isOpaqueTypeDescriptorAccessor() const {
    return getKind() == Kind::OpaqueTypeDescriptorAccessor ||
           getKind() == Kind::OpaqueTypeDescriptorAccessorImpl ||
           getKind() == Kind::OpaqueTypeDescriptorAccessorKey ||
           getKind() == Kind::OpaqueTypeDescriptorAccessorVar;
  }
  bool isOpaqueTypeDescriptorAccessorImpl() const {
    return getKind() == Kind::OpaqueTypeDescriptorAccessorImpl;
  }
  bool isAllocator() const {
    assert(getKind() == Kind::DynamicallyReplaceableFunctionImpl ||
           getKind() == Kind::DynamicallyReplaceableFunctionKeyAST ||
           getKind() == Kind::DynamicallyReplaceableFunctionVariableAST);
    return SecondaryPointer != nullptr;
  }
  bool isValueWitness() const { return getKind() == Kind::ValueWitness; }
  bool isContextDescriptor() const;
  CanType getType() const {
    assert(isTypeKind(getKind()));
    return CanType(reinterpret_cast<TypeBase*>(Pointer));
  }
  ValueWitness getValueWitness() const {
    assert(getKind() == Kind::ValueWitness);
    return ValueWitness(LINKENTITY_GET_FIELD(Data, ValueWitness));
  }
  TypeMetadataAddress getMetadataAddress() const {
    assert(getKind() == Kind::TypeMetadata ||
           getKind() == Kind::NoncanonicalSpecializedGenericTypeMetadata ||
           getKind() == Kind::ObjCResilientClassStub);
    return (TypeMetadataAddress)LINKENTITY_GET_FIELD(Data, MetadataAddress);
  }
  bool isObjCClassRef() const {
    return getKind() == Kind::ObjCClassRef;
  }
  bool isSILFunction() const {
    return getKind() == Kind::SILFunction;
  }
  bool isDynamicallyReplaceableFunctionKey() const {
    return getKind() == Kind::DynamicallyReplaceableFunctionKey;
  }
  bool isDynamicallyReplaceableFunctionImpl() const {
    return getKind() == Kind::DynamicallyReplaceableFunctionImpl;
  }
  bool isTypeMetadataAccessFunction() const {
    return getKind() == Kind::TypeMetadataAccessFunction;
  }
  bool isDispatchThunk() const {
    return getKind() == Kind::DispatchThunk ||
           getKind() == Kind::DispatchThunkInitializer ||
           getKind() == Kind::DispatchThunkAllocator ||
           getKind() == Kind::DispatchThunkDerivative;
  }

  /// Determine whether this entity will be weak-imported.
  bool isWeakImported(ModuleDecl *module) const;
  
  /// Return the module scope context whose codegen should trigger emission
  /// of this link entity, if one can be identified.
  DeclContext *getDeclContextForEmission() const;
  
  /// Get the preferred alignment for the definition of this entity.
  Alignment getAlignment(IRGenModule &IGM) const;
  
  /// Get the default LLVM type to use for forward declarations of this
  /// entity.
  llvm::Type *getDefaultDeclarationType(IRGenModule &IGM) const;

  bool isAlwaysSharedLinkage() const;
#undef LINKENTITY_GET_FIELD
#undef LINKENTITY_SET_FIELD

private:
  static bool isObjCImplementation(NominalTypeDecl *NTD) {
    if (NTD)
      return NTD->getObjCImplementationDecl();
    return false;
  }
  static bool isObjCImplementation(CanType ty) {
    return isObjCImplementation(ty->getClassOrBoundGenericClass());
  }
};

struct IRLinkage {
  llvm::GlobalValue::LinkageTypes Linkage;
  llvm::GlobalValue::VisibilityTypes Visibility;
  llvm::GlobalValue::DLLStorageClassTypes DLLStorage;

  static const IRLinkage InternalLinkOnceODR;
  static const IRLinkage InternalWeakODR;
  static const IRLinkage Internal;

  static const IRLinkage ExternalCommon;
  static const IRLinkage ExternalImport;
  static const IRLinkage ExternalWeakImport;
  static const IRLinkage ExternalExport;
};

class ApplyIRLinkage {
  IRLinkage IRL;
public:
  ApplyIRLinkage(IRLinkage IRL) : IRL(IRL) {}
  void to(llvm::GlobalValue *GV, bool definition = true) const {
    llvm::Module *M = GV->getParent();
    const llvm::Triple Triple(M->getTargetTriple());

    GV->setLinkage(IRL.Linkage);
    GV->setVisibility(IRL.Visibility);
    if (Triple.isOSBinFormatCOFF() && !Triple.isOSCygMing())
      GV->setDLLStorageClass(IRL.DLLStorage);

    // TODO: BFD and gold do not handle COMDATs properly
    if (Triple.isOSBinFormatELF())
      return;

    // COMDATs cannot be applied to declarations.  If we have a definition,
    // apply the COMDAT.
    if (definition)
      if (IRL.Linkage == llvm::GlobalValue::LinkOnceODRLinkage ||
          IRL.Linkage == llvm::GlobalValue::WeakODRLinkage)
        if (Triple.supportsCOMDAT())
          if (llvm::GlobalObject *GO = dyn_cast<llvm::GlobalObject>(GV))
            GO->setComdat(M->getOrInsertComdat(GV->getName()));
  }
};

/// Encapsulated information about the linkage of an entity.
class LinkInfo {
  LinkInfo() = default;

  llvm::SmallString<32> Name;
  IRLinkage IRL;
  ForDefinition_t ForDefinition;

public:
  /// Compute linkage information for the given
  static LinkInfo get(IRGenModule &IGM, const LinkEntity &entity,
                      ForDefinition_t forDefinition);

  static LinkInfo get(const UniversalLinkageInfo &linkInfo,
                      ModuleDecl *swiftModule,
                      const LinkEntity &entity,
                      ForDefinition_t forDefinition);

  static LinkInfo get(const UniversalLinkageInfo &linkInfo, StringRef name,
                      SILLinkage linkage, ForDefinition_t isDefinition,
                      bool isWeakImported);

  StringRef getName() const {
    return Name.str();
  }
  llvm::GlobalValue::LinkageTypes getLinkage() const {
    return IRL.Linkage;
  }
  llvm::GlobalValue::VisibilityTypes getVisibility() const {
    return IRL.Visibility;
  }
  llvm::GlobalValue::DLLStorageClassTypes getDLLStorage() const {
    return IRL.DLLStorage;
  }

  bool isForDefinition() const { return ForDefinition; }
  bool isUsed() const { return ForDefinition && isUsed(IRL); }

  static bool isUsed(IRLinkage IRL);
};

StringRef encodeForceLoadSymbolName(llvm::SmallVectorImpl<char> &buf,
                                    StringRef name);
}
}

/// Allow LinkEntity to be used as a key for a DenseMap.
namespace llvm {
template <> struct DenseMapInfo<swift::irgen::LinkEntity> {
  using LinkEntity = swift::irgen::LinkEntity;
  static LinkEntity getEmptyKey() {
    LinkEntity entity;
    entity.Pointer = nullptr;
    entity.SecondaryPointer = nullptr;
    entity.Data = 0;
    return entity;
  }
  static LinkEntity getTombstoneKey() {
    LinkEntity entity;
    entity.Pointer = nullptr;
    entity.SecondaryPointer = nullptr;
    entity.Data = 1;
    return entity;
  }
  static unsigned getHashValue(const LinkEntity &entity) {
    return DenseMapInfo<void *>::getHashValue(entity.Pointer) ^
           DenseMapInfo<void *>::getHashValue(entity.SecondaryPointer) ^
           entity.Data;
  }
  static bool isEqual(const LinkEntity &LHS, const LinkEntity &RHS) {
    return LHS.Pointer == RHS.Pointer &&
           LHS.SecondaryPointer == RHS.SecondaryPointer && LHS.Data == RHS.Data;
  }
};
}
#endif
