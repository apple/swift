//===--- GenPointerAuth.h - IRGen for pointer authentication ----*- C++ -*-===//
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
// This file defines the basic interface for generating LLVM IR for pointer
// authentication.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_GENPOINTERAUTH_H
#define SWIFT_IRGEN_GENPOINTERAUTH_H

#include "swift/IRGen/ValueWitness.h"
#include "swift/Basic/ExternalUnion.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/ProtocolAssociations.h"
#include "swift/AST/Types.h"
#include "swift/SIL/SILDeclRef.h"

namespace llvm {
class ConstantInt;
class Value;
}

namespace clang {
class PointerAuthSchema;
}

namespace swift {
namespace irgen {
class FunctionPointer;
class IRGenFunction;
class IRGenModule;
class PointerAuthInfo;

/// Additional information about the source of a function pointer.
class PointerAuthEntity {
public:
  enum class Special {
    BlockCopyHelper,
    BlockDisposeHelper,
    HeapDestructor,
    PartialApplyCapture,
    TypeDescriptor,
    TypeDescriptorAsArgument,
    KeyPathDestroy,
    KeyPathCopy,
    KeyPathEquals,
    KeyPathHash,
    KeyPathGetter,
    KeyPathNonmutatingSetter,
    KeyPathMutatingSetter,
    KeyPathGetLayout,
    KeyPathInitializer,
    KeyPathMetadataAccessor,
    DynamicReplacementKey,
    ProtocolConformanceDescriptor,
    ProtocolConformanceDescriptorAsArgument,
    ProtocolDescriptorAsArgument,
    OpaqueTypeDescriptorAsArgument,
    ContextDescriptorAsArgument,
    TypeLayoutString,
  };

private:
  enum class Kind {
    None,
    Special,
    ValueWitness,
    AssociatedType,
    AssociatedConformance,
    CanSILFunctionType,
    CoroutineYieldTypes,
    SILDeclRef,
    SILFunction,
  } StoredKind;

  using Members = ExternalUnionMembers<void,
                                       Special,
                                       ValueWitness,
                                       AssociatedType,
                                       AssociatedConformance,
                                       CanSILFunctionType,
                                       SILDeclRef,
                                       SILFunction *>;
  static Members::Index getStorageIndexForKind(Kind kind) {
    switch (kind) {
    case Kind::None:
      return Members::indexOf<void>();
    case Kind::Special:
      return Members::indexOf<Special>();
    case Kind::ValueWitness:
      return Members::indexOf<ValueWitness>();
    case Kind::CoroutineYieldTypes:
    case Kind::CanSILFunctionType:
      return Members::indexOf<CanSILFunctionType>();
    case Kind::SILDeclRef:
      return Members::indexOf<SILDeclRef>();
    case Kind::AssociatedType:
      return Members::indexOf<AssociatedType>();
    case Kind::AssociatedConformance:
      return Members::indexOf<AssociatedConformance>();
    case Kind::SILFunction:
      return Members::indexOf<SILFunction *>();
    }
    llvm_unreachable("bad kind");
  }
  ExternalUnion<Kind, Members, getStorageIndexForKind> Storage;

  static_assert(decltype(Storage)::union_is_trivially_copyable,
                "please add copy/move/dtor if you need a non-trivial type");

public:
  PointerAuthEntity()
      : StoredKind(Kind::None) {
  }
  PointerAuthEntity(Special specialKind)
      : StoredKind(Kind::Special) {
    Storage.emplace<Special>(StoredKind, specialKind);
  }
  PointerAuthEntity(CanSILFunctionType type)
      : StoredKind(Kind::CanSILFunctionType) {
    Storage.emplace<CanSILFunctionType>(StoredKind, type);
  }
  PointerAuthEntity(SILDeclRef decl)
      : StoredKind(Kind::SILDeclRef) {
    Storage.emplace<SILDeclRef>(StoredKind, decl);
  }
  PointerAuthEntity(ValueWitness witness)
      : StoredKind(Kind::ValueWitness) {
    assert(isValueWitnessFunction(witness));
    Storage.emplace<ValueWitness>(StoredKind, witness);
  }
  PointerAuthEntity(AssociatedType association)
      : StoredKind(Kind::AssociatedType) {
    Storage.emplaceAggregate<AssociatedType>(StoredKind, association);
  }
  PointerAuthEntity(const AssociatedConformance &association)
      : StoredKind(Kind::AssociatedConformance) {
    Storage.emplaceAggregate<AssociatedConformance>(StoredKind, association);
  }
  PointerAuthEntity(SILFunction *f)
      : StoredKind(Kind::SILFunction) {
    Storage.emplace<SILFunction *>(StoredKind, f);
  }

  static PointerAuthEntity forYieldTypes(CanSILFunctionType fnType) {
    assert(fnType->isCoroutine());
    PointerAuthEntity result;
    result.StoredKind = Kind::CoroutineYieldTypes;
    result.Storage.emplace<CanSILFunctionType>(result.StoredKind, fnType);
    return result;
  }

  llvm::ConstantInt *getDeclDiscriminator(IRGenModule &IGM) const;
  llvm::ConstantInt *getTypeDiscriminator(IRGenModule &IGM) const;
};

std::pair<clang::PointerAuthSchema, PointerAuthEntity>
getCoroutineResumeFunctionPointerAuth(IRGenModule &IGM,
                                      CanSILFunctionType coroutineFnType);

/// Blend a small integer discriminator with the given address value
/// in a way that is assumed to maximally preserve entropy from both.
llvm::Value *emitPointerAuthBlend(IRGenFunction &IGF,
                                  llvm::Value *address,
                                  llvm::Value *discriminator);

/// Strip the signature of a signed pointer value.
/// The return value has the same type as the input.
llvm::Value *emitPointerAuthStrip(IRGenFunction &IGF, llvm::Value *fnPtr,
                                  unsigned Key);

/// Sign the given pointer value, which is assumed to be unsigned.
/// The return value has the same type as the input.  This is a no-op if
/// the target auth info has disabled signing.
llvm::Value *emitPointerAuthSign(IRGenFunction &IGF, llvm::Value *fnPtr,
                                 const PointerAuthInfo &newAuth);

/// Authenticate the given pointer value and return an unsigned value.
llvm::Value *emitPointerAuthAuth(IRGenFunction &IGF, llvm::Value *fnPtr,
                                 const PointerAuthInfo &oldAuth);

/// Resign the given function pointer.
FunctionPointer emitPointerAuthResign(IRGenFunction &IGF,
                                      const FunctionPointer &fn,
                                      const PointerAuthInfo &newAuth);

/// Resign the given pointer value.  This function does the right thing
/// for unsigned input and result schemas.  The result will have the same
/// type as the input.
llvm::Value *emitPointerAuthResign(IRGenFunction &IGF,
                                   llvm::Value *fnPtr,
                                   const PointerAuthInfo &oldAuth,
                                   const PointerAuthInfo &newAuth);

/// The (non-ABI) discriminator used for non-constant captures of
/// partial apply functions.
const uint16_t PointerAuthDiscriminator_PartialApplyCapture = 7185;

} // end namespace irgen
} // end namespace swift

#endif
