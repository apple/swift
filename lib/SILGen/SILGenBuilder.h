//===--- SILGenBuilder.h ----------------------------------------*- C++ -*-===//
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
///
/// \file
///
/// This file defines SILGenBuilder, a subclass of SILBuilder that provides APIs
/// that traffic in ManagedValue. The intention is that if one is using a
/// SILGenBuilder, the SILGenBuilder will handle preserving ownership invariants
/// (or assert upon failure) freeing the implementor of such concerns.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILGEN_SILGENBUILDER_H
#define SWIFT_SILGEN_SILGENBUILDER_H

#include "Cleanup.h"
#include "JumpDest.h"
#include "ManagedValue.h"
#include "RValue.h"
#include "swift/Basic/ProfileCounter.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILLocation.h"

namespace swift {
namespace ast_scope {
class ASTScopeImpl;
}

namespace Lowering {

class SILGenFunction;
class SGFContext;
class AssertingManualScope;

/// A subclass of SILBuilder that wraps APIs to vend ManagedValues.
/// APIs only vend ManagedValues.
class SILGenBuilder : public SILBuilder {
  SILGenFunction &SGF;

public:
  SILGenBuilder(SILGenFunction &SGF);
  SILGenBuilder(SILGenFunction &SGF, SILBasicBlock *insertBB,
                SmallVectorImpl<SILInstruction *> *insertedInsts = nullptr);
  SILGenBuilder(SILGenFunction &SGF, SILBasicBlock *insertBB,
                SILBasicBlock::iterator insertInst);

  /// Create a new builder, inheriting the given builder's context and debug
  /// scope.
  SILGenBuilder(SILGenBuilder &builder, SILBasicBlock *insertBB)
      : SILBuilder(insertBB, builder.getBuilderContext(),
                   builder.getCurrentDebugScope()),
        SGF(builder.SGF) {}

  SILGenModule &getSILGenModule() const;
  SILGenFunction &getSILGenFunction() const { return SGF; }

  SILDebugLocation
  getSILDebugLocation(SILLocation Loc,
                      bool ForMetaInstruction = false) override;

  using SILBuilder::createInitExistentialValue;
  ManagedValue
  createInitExistentialValue(SILLocation loc, SILType existentialType,
                             CanType formalConcreteType, ManagedValue concrete,
                             ArrayRef<ProtocolConformanceRef> conformances);
  using SILBuilder::createInitExistentialRef;
  ManagedValue
  createInitExistentialRef(SILLocation loc, SILType existentialType,
                           CanType formalConcreteType, ManagedValue concrete,
                           ArrayRef<ProtocolConformanceRef> conformances);

  using SILBuilder::createPartialApply;
  ManagedValue createPartialApply(SILLocation loc, SILValue fn,
                                  SubstitutionMap subs,
                                  ArrayRef<ManagedValue> args,
                                  ParameterConvention calleeConvention);
  ManagedValue createPartialApply(SILLocation loc, ManagedValue fn,
                                  SubstitutionMap subs,
                                  ArrayRef<ManagedValue> args,
                                  ParameterConvention calleeConvention) {
    return createPartialApply(loc, fn.getValue(), subs, args,
                              calleeConvention);
  }

  using SILBuilder::createStructExtract;
  ManagedValue createStructExtract(SILLocation loc, ManagedValue base,
                                   VarDecl *decl);

  using SILBuilder::createRefElementAddr;
  ManagedValue createRefElementAddr(SILLocation loc, ManagedValue operand,
                                    VarDecl *field, SILType resultTy);

  using SILBuilder::createCopyValue;

  /// Emit a +1 copy on \p originalValue that lives until the end of the current
  /// lexical scope.
  ManagedValue createCopyValue(SILLocation loc, ManagedValue originalValue);

  /// Emit a +1 copy on \p originalValue that lives until the end of the current
  /// lexical scope.
  ///
  /// This reuses a passed in lowering.
  ManagedValue createCopyValue(SILLocation loc, ManagedValue originalValue,
                               const TypeLowering &lowering);

  /// Emit a +1 copy of \p originalValue into newAddr that lives until the end
  /// of the current Formal Evaluation Scope.
  ManagedValue createFormalAccessCopyAddr(SILLocation loc,
                                          ManagedValue originalAddr,
                                          SILValue newAddr, IsTake_t isTake,
                                          IsInitialization_t isInit);

  /// Emit a +1 copy of \p originalValue into newAddr that lives until the end
  /// Formal Evaluation Scope.
  ManagedValue createFormalAccessCopyValue(SILLocation loc,
                                           ManagedValue originalValue);

  using SILBuilder::createExplicitCopyValue;

  /// A copy_value operation that to the move checker looks like just a normal
  /// liveness use. Used to implement an explicit copy for no implicit copy
  /// values.
  ManagedValue createExplicitCopyValue(SILLocation Loc, ManagedValue operand);

  using SILBuilder::createWeakCopyValue;

  ManagedValue createWeakCopyValue(SILLocation loc, ManagedValue originalValue);

#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                          \
  using SILBuilder::createStrongCopy##Name##Value;                             \
  ManagedValue createStrongCopy##Name##Value(SILLocation loc,                  \
                                             ManagedValue originalValue);
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  using SILBuilder::createStrongCopy##Name##Value;                             \
  ManagedValue createStrongCopy##Name##Value(SILLocation loc,                  \
                                             ManagedValue originalValue);
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  using SILBuilder::createStrongCopy##Name##Value;                             \
  ManagedValue createStrongCopy##Name##Value(SILLocation loc,                  \
                                             ManagedValue originalValue);
#include "swift/AST/ReferenceStorage.def"

  /// Create the block argument for an OwnershipForwardingTermInst result
  /// (checked_cast_br or switch_enum). Allows creating terminator
  /// results separately from creating the terminator. Alternatively, the result
  /// can be directly created as follows:
  ///
  ///   ManagedValue::forForwardedRValue(term->createResult(succBB, resultTy))
  ///
  /// For a switch_enum with a default payload, use:
  ///
  ///   ManagedValue::forForwardedRValue(switchEnum->createDefaultResult())
  ///
  ManagedValue createForwardedTermResult(SILType type);

  /// Create a terminator result with specified ownership.
  ///
  /// Typically for an apply's return or error value, which should use
  /// OwnershipKind::Owned. For OwnershipForwardingTermInst (checked_cast_br or
  /// switch_enum), use createForwardedTermResult instead.
  ManagedValue createTermResult(SILType type, ValueOwnershipKind ownership);

  /// Create the block argument for an Optional switch_enum.
  ManagedValue createOptionalSomeResult(SwitchEnumInst *switchEnum) {
    return ManagedValue::forForwardedRValue(
        SGF, switchEnum->createOptionalSomeResult());
  }

  // Create the block argument for a phi.
  ManagedValue createPhi(SILType type, ValueOwnershipKind ownership);

  using SILBuilder::createMarkUninitialized;
  ManagedValue createMarkUninitialized(ValueDecl *decl, ManagedValue operand,
                                       MarkUninitializedInst::Kind muKind);

  using SILBuilder::createAllocRef;
  ManagedValue createAllocRef(SILLocation loc, SILType refType, bool objc,
                              ArrayRef<SILType> elementTypes,
                              ArrayRef<ManagedValue> elementCountOperands);
  using SILBuilder::createAllocRefDynamic;
  ManagedValue
  createAllocRefDynamic(SILLocation loc, ManagedValue operand, SILType refType,
                        bool objc, ArrayRef<SILType> elementTypes,
                        ArrayRef<ManagedValue> elementCountOperands);

  using SILBuilder::createTuple;
  ManagedValue createTuple(SILLocation loc, SILType type,
                           ArrayRef<ManagedValue> elements);
  using SILBuilder::createTupleExtract;
  ManagedValue createTupleExtract(SILLocation loc, ManagedValue value,
                                  unsigned index, SILType type);
  ManagedValue createTupleExtract(SILLocation loc, ManagedValue value,
                                  unsigned index);
  using SILBuilder::createTupleElementAddr;
  ManagedValue createTupleElementAddr(SILLocation loc, ManagedValue addr,
                                      unsigned index, SILType type);
  ManagedValue createTupleElementAddr(SILLocation loc, ManagedValue addr,
                                      unsigned index);

  using SILBuilder::createLoadBorrow;
  ManagedValue createLoadBorrow(SILLocation loc, ManagedValue base);
  ManagedValue createFormalAccessLoadBorrow(SILLocation loc, ManagedValue base);
  ManagedValue createFormalAccessLoadTake(SILLocation loc, ManagedValue base);
  ManagedValue createFormalAccessLoadCopy(SILLocation loc, ManagedValue base);

  using SILBuilder::createStoreBorrow;
  ManagedValue createStoreBorrow(SILLocation loc, ManagedValue value,
                                 SILValue address);
  ManagedValue createFormalAccessStoreBorrow(SILLocation loc, ManagedValue value,
                                             SILValue address);

  /// Create a store_borrow if we have a non-trivial value and a store [trivial]
  /// otherwise.
  ManagedValue createStoreBorrowOrTrivial(SILLocation loc, ManagedValue value,
                                          SILValue address);

  /// Prepares a buffer to receive the result of an expression, either using the
  /// 'emit into' initialization buffer if available, or allocating a temporary
  /// allocation if not. After the buffer has been prepared, the rvalueEmitter
  /// closure will be called with the buffer ready for initialization. After the
  /// emitter has been called, the buffer will complete its initialization.
  ///
  /// \return an empty value if the buffer was taken from the context.
  ManagedValue bufferForExpr(SILLocation loc, SILType ty,
                             const TypeLowering &lowering, SGFContext context,
                             llvm::function_ref<void(SILValue)> rvalueEmitter);

  using SILBuilder::createUncheckedEnumData;
  ManagedValue createUncheckedEnumData(SILLocation loc, ManagedValue operand,
                                       EnumElementDecl *element);

  using SILBuilder::createUncheckedTakeEnumDataAddr;
  ManagedValue createUncheckedTakeEnumDataAddr(SILLocation loc, ManagedValue operand,
                                               EnumElementDecl *element, SILType ty);

  /// Given the address of a value, load a scalar value from it if the type
  /// is loadable.  Most general routines in SILGen expect to work with
  /// values with the canonical scalar-ness for their type.
  ManagedValue createLoadIfLoadable(SILLocation loc, ManagedValue addr);

  /// Given the address of a loadable value, load the value but don't
  /// change the ownership.
  ManagedValue createLoadWithSameOwnership(SILLocation loc, ManagedValue addr);

  ManagedValue createLoadTake(SILLocation loc, ManagedValue addr);
  ManagedValue createLoadTake(SILLocation loc, ManagedValue addr,
                              const TypeLowering &lowering);
  ManagedValue createLoadCopy(SILLocation loc, ManagedValue addr);
  ManagedValue createLoadCopy(SILLocation loc, ManagedValue addr,
                              const TypeLowering &lowering);

  /// Create a SILArgument for an input parameter. Asserts if used to create a
  /// function argument for an out parameter.
  ManagedValue createInputFunctionArgument(
      SILType type, ValueDecl *decl, bool isNoImplicitCopy = false,
      LifetimeAnnotation lifetimeAnnotation = LifetimeAnnotation::None,
      bool isClosureCapture = false, bool isFormalParameterPack = false);

  /// Create a SILArgument for an input parameter. Uses \p loc to create any
  /// copies necessary. Asserts if used to create a function argument for an out
  /// parameter.
  ///
  /// *NOTE* This API purposely used an Optional<SILLocation> to distinguish
  /// this API from the ValueDecl * API in C++. This is necessary since
  /// ValueDecl * can implicitly convert to SILLocation. The optional forces the
  /// user to be explicit that they want to use this API.
  ManagedValue createInputFunctionArgument(SILType type,
                                           llvm::Optional<SILLocation> loc);

  using SILBuilder::createEnum;
  ManagedValue createEnum(SILLocation loc, ManagedValue payload,
                          EnumElementDecl *decl, SILType type);

  ManagedValue createSemanticLoadBorrow(SILLocation loc, ManagedValue addr);

  ManagedValue
  formalAccessBufferForExpr(SILLocation loc, SILType ty,
                            const TypeLowering &lowering, SGFContext context,
                            llvm::function_ref<void(SILValue)> rvalueEmitter);

  using SILBuilder::createUnconditionalCheckedCast;
  ManagedValue createUnconditionalCheckedCast(SILLocation loc,
                                              ManagedValue op,
                                              SILType destLoweredTy,
                                              CanType destFormalTy);

  using SILBuilder::createCheckedCastBranch;
  void createCheckedCastBranch(SILLocation loc, bool isExact,
                               ManagedValue op,
                               CanType sourceFormalTy,
                               SILType destLoweredTy,
                               CanType destFormalTy,
                               SILBasicBlock *trueBlock,
                               SILBasicBlock *falseBlock,
                               ProfileCounter Target1Count,
                               ProfileCounter Target2Count);

  using SILBuilder::createUpcast;
  ManagedValue createUpcast(SILLocation loc, ManagedValue original,
                            SILType type);

  using SILBuilder::createUncheckedTrivialBitCast;
  ManagedValue createUncheckedTrivialBitCast(SILLocation loc,
                                             ManagedValue original,
                                             SILType type);

  using SILBuilder::createUncheckedRefCast;
  ManagedValue createUncheckedRefCast(SILLocation loc, ManagedValue value,
                                      SILType type);

  using SILBuilder::createUncheckedAddrCast;
  ManagedValue createUncheckedAddrCast(SILLocation loc, ManagedValue op,
                                       SILType resultTy);

  using SILBuilder::createUncheckedReinterpretCast;
  ManagedValue createUncheckedBitCast(SILLocation loc, ManagedValue original,
                                      SILType type);

  using SILBuilder::createOpenExistentialRef;
  ManagedValue createOpenExistentialRef(SILLocation loc, ManagedValue arg,
                                        SILType openedType);

  using SILBuilder::createOpenExistentialValue;
  ManagedValue createOpenExistentialValue(SILLocation loc,
                                          ManagedValue original, SILType type);

  using SILBuilder::createOpenExistentialBoxValue;
  ManagedValue createOpenExistentialBoxValue(SILLocation loc,
                                          ManagedValue original, SILType type);

  using SILBuilder::createOpenExistentialBox;
  ManagedValue createOpenExistentialBox(SILLocation loc, ManagedValue original,
                                        SILType type);

  using SILBuilder::createOpenExistentialMetatype;
  ManagedValue createOpenExistentialMetatype(SILLocation loc,
                                             ManagedValue value,
                                             SILType openedType);

  /// Convert a @convention(block) value to AnyObject.
  ManagedValue createBlockToAnyObject(SILLocation loc, ManagedValue block,
                                      SILType type);

  using SILBuilder::createOptionalSome;
  ManagedValue createOptionalSome(SILLocation Loc, ManagedValue Arg);
  ManagedValue createManagedOptionalNone(SILLocation Loc, SILType Type);

  // TODO: Rename this to createFunctionRef once all calls to createFunctionRef
  // are removed.
  ManagedValue createManagedFunctionRef(SILLocation loc, SILFunction *f);

  using SILBuilder::createConvertFunction;
  ManagedValue createConvertFunction(SILLocation loc, ManagedValue fn,
                                     SILType resultTy,
                                     bool WithoutActuallyEscaping = false);

  using SILBuilder::createConvertEscapeToNoEscape;
  ManagedValue
  createConvertEscapeToNoEscape(SILLocation loc, ManagedValue fn,
                                SILType resultTy);

  using SILBuilder::createStore;
  /// Forward \p value into \p address.
  ///
  /// This will forward value's cleanup (if it has one) into the equivalent
  /// cleanup on address. In practice this means if the value is non-trivial,
  /// the memory location will at end of scope have a destroy_addr applied to
  /// it.
  ManagedValue createStore(SILLocation loc, ManagedValue value,
                           SILValue address, StoreOwnershipQualifier qualifier);

  using SILBuilder::createSuperMethod;
  ManagedValue createSuperMethod(SILLocation loc, ManagedValue operand,
                                 SILDeclRef member, SILType methodTy);

  using SILBuilder::createObjCSuperMethod;
  ManagedValue createObjCSuperMethod(SILLocation loc, ManagedValue operand,
                                     SILDeclRef member, SILType methodTy);

  using SILBuilder::createValueMetatype;
  ManagedValue createValueMetatype(SILLocation loc, SILType metatype,
                                   ManagedValue base);

  using SILBuilder::createBridgeObjectToRef;
  ManagedValue createBridgeObjectToRef(SILLocation loc, ManagedValue mv,
                                       SILType destType);

  using SILBuilder::createRefToBridgeObject;
  ManagedValue createRefToBridgeObject(SILLocation loc, ManagedValue mv,
                                       SILValue bits);

  using SILBuilder::createBranch;
  BranchInst *createBranch(SILLocation Loc, SILBasicBlock *TargetBlock,
                           ArrayRef<ManagedValue> Args);

  ReturnInst *createReturn(SILLocation Loc, SILValue ReturnValue) {
    // If we have a move only type as our "result type", convert it back to
    // being a copyable type. Move only types are never returned today and we
    // will rely on the SIL level move only and no escape checker to validate
    // that this is a correct usage. So just make the types line up.
    if (ReturnValue->getType().isMoveOnlyWrapped()) {
      auto cvtLoc = RegularLocation::getAutoGeneratedLocation();
      ReturnValue =
          createOwnedMoveOnlyWrapperToCopyableValue(cvtLoc, ReturnValue);
    }
    return SILBuilder::createReturn(Loc, ReturnValue);
  }

  ReturnInst *createReturn(SILLocation Loc, ManagedValue ReturnValue);

  ReturnInst *createReturn(SILLocation Loc, SILValue ReturnValue,
                           AssertingManualScope &&functionLevelScope);

  using SILBuilder::emitDestructureValueOperation;
  /// Perform either a tuple or struct destructure and then pass its components
  /// as managed value one by one with an index to the closure.
  void emitDestructureValueOperation(
      SILLocation loc, ManagedValue value,
      function_ref<void(unsigned, ManagedValue)> func);
  void emitDestructureValueOperation(
      SILLocation loc, ManagedValue value,
      SmallVectorImpl<ManagedValue> &destructuredValues);

  using SILBuilder::emitDestructureAddressOperation;
  void emitDestructureAddressOperation(
      SILLocation loc, ManagedValue value,
      function_ref<void(unsigned, ManagedValue)> func);

  using SILBuilder::emitDestructureOperation;
  void
  emitDestructureOperation(SILLocation loc, ManagedValue value,
                           function_ref<void(unsigned, ManagedValue)> func) {
    if (value.getType().isObject())
      return emitDestructureValueOperation(loc, value, func);
    return emitDestructureAddressOperation(loc, value, func);
  }

  using SILBuilder::createProjectBox;
  ManagedValue createProjectBox(SILLocation loc, ManagedValue mv,
                                unsigned index);

  using SILBuilder::createMarkDependence;
  ManagedValue createMarkDependence(SILLocation loc, ManagedValue value,
                                    ManagedValue base);

  using SILBuilder::createBeginBorrow;
  ManagedValue createBeginBorrow(SILLocation loc, ManagedValue value,
                                 bool isLexical = false);

  using SILBuilder::createMoveValue;
  ManagedValue createMoveValue(SILLocation loc, ManagedValue value,
                               bool isLexical = false);

  using SILBuilder::createOwnedMoveOnlyWrapperToCopyableValue;
  ManagedValue createOwnedMoveOnlyWrapperToCopyableValue(SILLocation loc,
                                                         ManagedValue value);

  using SILBuilder::createGuaranteedMoveOnlyWrapperToCopyableValue;
  ManagedValue
  createGuaranteedMoveOnlyWrapperToCopyableValue(SILLocation loc,
                                                 ManagedValue value);

  using SILBuilder::createOwnedCopyableToMoveOnlyWrapperValue;
  ManagedValue createOwnedCopyableToMoveOnlyWrapperValue(SILLocation loc,
                                                         ManagedValue value);

  using SILBuilder::createGuaranteedCopyableToMoveOnlyWrapperValue;
  ManagedValue
  createGuaranteedCopyableToMoveOnlyWrapperValue(SILLocation loc,
                                                 ManagedValue value);

  using SILBuilder::createMarkMustCheckInst;
  ManagedValue createMarkMustCheckInst(SILLocation loc, ManagedValue value,
                                       MarkMustCheckInst::CheckKind kind);

  using SILBuilder::emitCopyValueOperation;
  ManagedValue emitCopyValueOperation(SILLocation Loc, ManagedValue v);

  void emitCopyAddrOperation(SILLocation loc, SILValue srcAddr,
                             SILValue destAddr, IsTake_t isTake,
                             IsInitialization_t isInitialize);
};

} // namespace Lowering
} // namespace swift

#endif
