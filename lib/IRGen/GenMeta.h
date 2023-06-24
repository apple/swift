//===--- GenMeta.h - Swift IR generation for metadata -----------*- C++ -*-===//
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
//  This file provides the private interface to the metadata emission code.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_GENMETA_H
#define SWIFT_IRGEN_GENMETA_H

#include "swift/ABI/MetadataValues.h"
#include <utility>

namespace llvm {
  template <class T> class ArrayRef;
  class Constant;
  class Function;
  class GlobalVariable;
  class Value;
}

namespace swift {
  class AbstractFunctionDecl;
  struct ExistentialTypeGeneralization;
  class FileUnit;
  class FuncDecl;
  enum class ResilienceExpansion : unsigned;
  struct SILDeclRef;
  class SILType;
  class VarDecl;
  enum class SpecialProtocol : uint8_t;
  
namespace irgen {
  class ConstantStructBuilder;
  class FieldTypeInfo;
  class GenericTypeRequirements;
  class IRGenFunction;
  class IRGenModule;
  enum RequireMetadata_t : bool;
  class Size;
  class StructLayout;
  class ClassLayout;
  class LinkEntity;

  bool requiresForeignTypeMetadata(CanType type);
  bool requiresForeignTypeMetadata(NominalTypeDecl *decl);

  /// Emit the metadata associated with the given class declaration.
  void emitClassMetadata(IRGenModule &IGM, ClassDecl *theClass,
                         const ClassLayout &fragileLayout,
                         const ClassLayout &resilientLayout);

  /// Emit the constant initializer of the type metadata candidate for
  /// the given foreign class declaration.
  llvm::Constant *emitForeignTypeMetadataInitializer(IRGenModule &IGM,
                                                     CanType type,
                                                     Size &addressPointOffset);

  /// Emit a type context descriptor that was demanded by a reference from
  /// other generated definitions.
  void emitLazyTypeContextDescriptor(IRGenModule &IGM,
                                     NominalTypeDecl *type,
                                     RequireMetadata_t requireMetadata);

  /// Emit type metadata that was demanded by a reference from other
  /// generated definitions.
  void emitLazyTypeMetadata(IRGenModule &IGM, NominalTypeDecl *type);

  /// Emit the type metadata accessor for a type for which it might be used.
  void emitLazyMetadataAccessor(IRGenModule &IGM, NominalTypeDecl *type);

  void emitLazyCanonicalSpecializedMetadataAccessor(IRGenModule &IGM,
                                                    CanType theType);

  void emitLazySpecializedGenericTypeMetadata(IRGenModule &IGM, CanType type);

  /// Emit metadata for a foreign struct, enum or class.
  void emitForeignTypeMetadata(IRGenModule &IGM, NominalTypeDecl *decl);

  /// Emit the metadata associated with the given struct declaration.
  void emitStructMetadata(IRGenModule &IGM, StructDecl *theStruct);

  /// Emit the metadata associated with the given enum declaration.
  void emitEnumMetadata(IRGenModule &IGM, EnumDecl *theEnum);

  /// Emit the metadata associated with a given instantiation of a generic
  /// struct.
  void emitSpecializedGenericStructMetadata(IRGenModule &IGM, CanType type,
                                            StructDecl &decl);

  /// Emit the metadata associated with a given instantiation of a generic enum.
  void emitSpecializedGenericEnumMetadata(IRGenModule &IGM, CanType type,
                                          EnumDecl &decl);

  /// Emit the metadata associated with a given instantiation of a generic
  // class.
  void emitSpecializedGenericClassMetadata(IRGenModule &IGM, CanType type,
                                           ClassDecl &decl);

  /// Get what will be the index into the generic type argument array at the end
  /// of a nominal type's metadata.
  int32_t getIndexOfGenericArgument(IRGenModule &IGM,
                                    NominalTypeDecl *decl,
                                    ArchetypeType *archetype);

  /// Given a reference to nominal type metadata of the given type,
  /// derive a reference to the type metadata stored in the nth
  /// requirement slot.  The type must have generic arguments.
  llvm::Value *emitArgumentMetadataRef(IRGenFunction &IGF,
                                       NominalTypeDecl *theDecl,
                                       const GenericTypeRequirements &reqts,
                                       unsigned reqtIndex,
                                       llvm::Value *metadata);

  /// Given a reference to nominal type metadata of the given type,
  /// derive a reference to the type metadata pack stored in the nth
  /// requirement slot.  The type must have generic arguments.
  llvm::Value *emitArgumentMetadataPackRef(IRGenFunction &IGF,
                                           NominalTypeDecl *theDecl,
                                           const GenericTypeRequirements &reqts,
                                           unsigned reqtIndex,
                                           llvm::Value *metadata);

  /// Given a reference to nominal type metadata of the given type,
  /// derive a reference to a protocol witness table stored in the nth
  /// requirement slot.  The type must have generic arguments.
  llvm::Value *emitArgumentWitnessTableRef(IRGenFunction &IGF,
                                           NominalTypeDecl *theDecl,
                                           const GenericTypeRequirements &reqts,
                                           unsigned reqtIndex,
                                           llvm::Value *metadata);

  /// Given a reference to nominal type metadata of the given type,
  /// derive a reference to a protocol witness table pack stored in the nth
  /// requirement slot.  The type must have generic arguments.
  llvm::Value *emitArgumentWitnessTablePackRef(IRGenFunction &IGF,
                                               NominalTypeDecl *theDecl,
                                           const GenericTypeRequirements &reqts,
                                               unsigned reqtIndex,
                                               llvm::Value *metadata);

  /// Given a reference to nominal type metadata of the given type,
  /// derive a reference to a the pack shape stored in the nth
  /// requirement slot.  The type must have generic arguments.
  llvm::Value *emitArgumentPackShapeRef(IRGenFunction &IGF,
                                        NominalTypeDecl *theDecl,
                                        const GenericTypeRequirements &reqts,
                                        unsigned reqtIndex,
                                        llvm::Value *metadata);

  /// Given a metatype value, read its instance type.
  llvm::Value *emitMetatypeInstanceType(IRGenFunction &IGF,
                                        llvm::Value *metatypeMetadata);
  
  /// Emit the field type accessor for a nominal type's metadata. This function
  /// lazily generates the metadata for the types of all of the nominal type's
  /// fields for reflection purposes.
  void emitFieldTypeAccessor(IRGenModule &IGM,
                             NominalTypeDecl *type,
                             llvm::Function *fn,
                             ArrayRef<FieldTypeInfo> fieldTypes);

  /// Adjustment indices for the address points of various metadata.
  /// Size is in words.
  namespace MetadataAdjustmentIndex {
    enum : unsigned {
      // Class metadata has two words of head-allocated data: the destructor
      // and the value witness table.
      Class = 3,
      
      // Struct and enum metadata have one word of head-allocated data:
      // the value witness table.
      ValueType = 2,

      // Some builtin and well-known types don't have a layout string
      // for binary compatibility reasons.
      NoTypeLayoutString = 1,
      
      // Other metadata objects have no head allocation.
      None = 0,
    };
  }

  /// Get the runtime identifier for a special protocol, if any.
  SpecialProtocol getSpecialProtocolID(ProtocolDecl *P);

  /// Use the argument as the 'self' type metadata.
  void getArgAsLocalSelfTypeMetadata(IRGenFunction &IGF, llvm::Value *arg,
                                     CanType abstractType);

  struct GenericPackArgument {
    GenericPackKind Kind;
    unsigned Index;
    CanType ReducedShape;

    GenericPackArgument(GenericPackKind kind,
                        unsigned index,
                        CanType reducedShape)
      : Kind(kind), Index(index), ReducedShape(reducedShape) {}
  };

  /// Description of the metadata emitted by adding generic requirements.
  struct GenericArgumentMetadata {
    unsigned NumParams = 0;
    unsigned NumParamsEmitted = 0;
    unsigned NumRequirements = 0;
    unsigned NumGenericKeyArguments = 0;
    SmallVector<CanType, 1> ShapeClasses;
    SmallVector<GenericPackArgument, 1> GenericPackArguments;
  };

  /// Add generic parameters to the given constant struct builder.
  ///
  /// \param sig The generic signature whose parameters we wish to emit.
  GenericArgumentMetadata addGenericParameters(
                                          IRGenModule &IGM,
                                          ConstantStructBuilder &B,
                                          GenericSignature sig,
                                          bool implicit);

  /// Add generic requirements to the given constant struct builder.
  ///
  /// \param sig The generic signature against which the requirements are
  /// described.
  ///
  /// \param requirements The requirements to add.
  GenericArgumentMetadata addGenericRequirements(
                                          IRGenModule &IGM,
                                          ConstantStructBuilder &B,
                                          GenericSignature sig,
                                          ArrayRef<Requirement> requirements);

  /// Add generic pack shape descriptors to the given constant struct builder.
  ///
  /// These appear in generic type metadata, and conformance descriptors with
  /// conditional pack requirements.
  void addGenericPackShapeDescriptors(IRGenModule &IGM,
                                      ConstantStructBuilder &B,
                                      ArrayRef<CanType> shapes,
                                      ArrayRef<GenericPackArgument> packArgs);

  llvm::GlobalValue *emitAsyncFunctionPointer(IRGenModule &IGM,
                                              llvm::Function *function,
                                              LinkEntity entity,
                                              Size size);

  /// Determine whether the given opaque type requires a witness table for the
  /// given requirement.
  ///
  /// \returns the protocol when a witness table is required, or \c nullptr
  /// if the requirement isn't a conformance requirement or doesn't require a
  /// witness table.
  ProtocolDecl *opaqueTypeRequiresWitnessTable(
      OpaqueTypeDecl *opaque, const Requirement &req);

} // end namespace irgen
} // end namespace swift

#endif
