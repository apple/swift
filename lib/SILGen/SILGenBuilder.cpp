//===--- SILGenBuilder.cpp ------------------------------------------------===//
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

#include "SILGenBuilder.h"
#include "ArgumentSource.h"
#include "Cleanup.h"
#include "RValue.h"
#include "SILGenFunction.h"
#include "Scope.h"
#include "SwitchEnumBuilder.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/SILInstruction.h"

using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//
//                              Utility Methods
//===----------------------------------------------------------------------===//

SILGenModule &SILGenBuilder::getSILGenModule() const { return SGF.SGM; }

//===----------------------------------------------------------------------===//
//                                Constructors
//===----------------------------------------------------------------------===//

SILGenBuilder::SILGenBuilder(SILGenFunction &SGF)
    : SILBuilder(SGF.F), SGF(SGF) {}

SILGenBuilder::SILGenBuilder(SILGenFunction &SGF, SILBasicBlock *insertBB,
                             SmallVectorImpl<SILInstruction *> *insertedInsts)
    : SILBuilder(insertBB, insertedInsts), SGF(SGF) {
  auto FirstInsn = insertBB->begin();
  if (FirstInsn != insertBB->end())
    setCurrentDebugScope(FirstInsn->getDebugScope());
}

SILGenBuilder::SILGenBuilder(SILGenFunction &SGF, SILBasicBlock *insertBB,
                             SILBasicBlock::iterator insertInst)
    : SILBuilder(insertBB, insertInst), SGF(SGF) {
  if (insertInst != insertBB->end())
    setCurrentDebugScope(insertInst->getDebugScope());
}

SILDebugLocation SILGenBuilder::getSILDebugLocation(SILLocation Loc,
                                                    bool ForMetaInstruction) {
  return SGF.getSILDebugLocation(*this, Loc, getCurrentDebugLocOverride(),
                                 ForMetaInstruction);
}

//===----------------------------------------------------------------------===//
//                             Managed Value APIs
//===----------------------------------------------------------------------===//

ManagedValue SILGenBuilder::createPartialApply(SILLocation loc, SILValue fn,
                                               SubstitutionMap subs,
                                               ArrayRef<ManagedValue> args,
                                               ParameterConvention calleeConvention) {
  llvm::SmallVector<SILValue, 8> values;
  llvm::transform(args, std::back_inserter(values),
                  [&](ManagedValue mv) -> SILValue {
    return mv.forward(getSILGenFunction());
  });
  SILValue result =
      createPartialApply(loc, fn, subs, values, calleeConvention);
  // Partial apply instructions create a box, so we need to put on a cleanup.
  return getSILGenFunction().emitManagedRValueWithCleanup(result);
}

ManagedValue
SILGenBuilder::createConvertFunction(SILLocation loc, ManagedValue fn,
                                     SILType resultTy,
                                     bool withoutActuallyEscaping) {
  CleanupCloner cloner(*this, fn);
  SILValue result = createConvertFunction(loc, fn.forward(getSILGenFunction()),
                                          resultTy, withoutActuallyEscaping);
  return cloner.clone(result);
}

ManagedValue SILGenBuilder::createConvertEscapeToNoEscape(
    SILLocation loc, ManagedValue fn, SILType resultTy) {

  auto fnType = fn.getType().castTo<SILFunctionType>();
  auto resultFnType = resultTy.castTo<SILFunctionType>();

  // Escaping to noescape conversion.
  assert(resultFnType->getRepresentation() ==
             SILFunctionTypeRepresentation::Thick &&
         fnType->getRepresentation() ==
             SILFunctionTypeRepresentation::Thick &&
         !fnType->isNoEscape() && resultFnType->isNoEscape() &&
         "Expect a escaping to noescape conversion");
  (void)fnType;
  (void)resultFnType;
  SILValue fnValue = fn.getValue();
  SILValue result =
      createConvertEscapeToNoEscape(loc, fnValue, resultTy, false);
  
  return SGF.emitManagedRValueWithCleanup(result);
}

ManagedValue SILGenBuilder::createInitExistentialValue(
    SILLocation loc, SILType existentialType, CanType formalConcreteType,
    ManagedValue concrete, ArrayRef<ProtocolConformanceRef> conformances) {
  // *NOTE* we purposely do not use a cleanup cloner here. The reason why is no
  // matter whether we have a trivial or non-trivial input,
  // init_existential_value returns a +1 value (the COW box).
  SILValue v =
      createInitExistentialValue(loc, existentialType, formalConcreteType,
                                 concrete.forward(SGF), conformances);
  return SGF.emitManagedRValueWithCleanup(v);
}

ManagedValue SILGenBuilder::createInitExistentialRef(
    SILLocation Loc, SILType ExistentialType, CanType FormalConcreteType,
    ManagedValue Concrete, ArrayRef<ProtocolConformanceRef> Conformances) {
  CleanupCloner Cloner(*this, Concrete);
  InitExistentialRefInst *IERI =
      createInitExistentialRef(Loc, ExistentialType, FormalConcreteType,
                               Concrete.forward(SGF), Conformances);
  return Cloner.clone(IERI);
}

ManagedValue SILGenBuilder::createStructExtract(SILLocation loc,
                                                ManagedValue base,
                                                VarDecl *decl) {
  ManagedValue borrowedBase = base.formalAccessBorrow(SGF, loc);
  SILValue extract = createStructExtract(loc, borrowedBase.getValue(), decl);
  return ManagedValue::forUnmanaged(extract);
}

ManagedValue SILGenBuilder::createRefElementAddr(SILLocation loc,
                                                 ManagedValue operand,
                                                 VarDecl *field,
                                                 SILType resultTy) {
  operand = operand.formalAccessBorrow(SGF, loc);
  SILValue result = createRefElementAddr(loc, operand.getValue(), field);
  return ManagedValue::forUnmanaged(result);
}

ManagedValue SILGenBuilder::createCopyValue(SILLocation loc,
                                            ManagedValue originalValue) {
  auto &lowering = SGF.getTypeLowering(originalValue.getType());
  return createCopyValue(loc, originalValue, lowering);
}

ManagedValue SILGenBuilder::createCopyValue(SILLocation loc,
                                            ManagedValue originalValue,
                                            const TypeLowering &lowering) {
  if (lowering.isTrivial())
    return originalValue;

  SILType ty = originalValue.getType();
  assert(!ty.isAddress() && "Can not perform a copy value of an address typed "
         "value");

  if (ty.isObject() &&
      originalValue.getOwnershipKind() == OwnershipKind::None) {
    return originalValue;
  }

  SILValue result =
      lowering.emitCopyValue(*this, loc, originalValue.getValue());
  return SGF.emitManagedRValueWithCleanup(result, lowering);
}

#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                      \
  ManagedValue SILGenBuilder::createStrongCopy##Name##Value(                   \
      SILLocation loc, ManagedValue originalValue) {                           \
    auto ty = originalValue.getType().castTo<Name##StorageType>();             \
    assert(ty->isLoadable(ResilienceExpansion::Maximal));                      \
    (void)ty;                                                                  \
    SILValue result =                                                          \
        createStrongCopy##Name##Value(loc, originalValue.getValue());          \
    return SGF.emitManagedRValueWithCleanup(result);                           \
  }
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                         \
  ManagedValue SILGenBuilder::createStrongCopy##Name##Value(                   \
      SILLocation loc, ManagedValue originalValue) {                           \
    SILValue result =                                                          \
        createStrongCopy##Name##Value(loc, originalValue.getValue());          \
    return SGF.emitManagedRValueWithCleanup(result);                           \
  }
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  ManagedValue SILGenBuilder::createStrongCopy##Name##Value(                   \
      SILLocation loc, ManagedValue originalValue) {                           \
    /* *NOTE* The reason why this is unsafe is that we are converting and */   \
    /* unconditionally retaining, rather than before converting from */        \
    /* type->ref checking that our value is not yet uninitialized. */          \
    SILValue result =                                                          \
        createStrongCopy##Name##Value(loc, originalValue.getValue());          \
    return SGF.emitManagedRValueWithCleanup(result);                           \
  }
#include "swift/AST/ReferenceStorage.def"

ManagedValue SILGenBuilder::createForwardedTermResult(SILType type) {
  auto *succBB = getInsertionBB();
  auto *term = cast<OwnershipForwardingTermInst>(
      succBB->getSinglePredecessorBlock()->getTerminator());
  auto *arg = term->createResult(succBB, type);
  return ManagedValue::forForwardedRValue(SGF, arg);
}

ManagedValue SILGenBuilder::createTermResult(SILType type,
                                             ValueOwnershipKind ownership) {
  // Despite the name, 'arg' is a terminator result, not a phi.
  auto *arg = getInsertionBB()->createPhiArgument(type, ownership);
  return ManagedValue::forForwardedRValue(SGF, arg);
}

ManagedValue SILGenBuilder::createPhi(SILType type,
                                      ValueOwnershipKind ownership) {
  SILPhiArgument *arg = getInsertionBB()->createPhiArgument(type, ownership);
  switch (ownership) {
  case OwnershipKind::Any:
    llvm_unreachable("Invalid ownership for value");

  case OwnershipKind::Owned:
    return SGF.emitManagedRValueWithCleanup(arg);

  case OwnershipKind::Guaranteed:
    return SGF.emitManagedBorrowedArgumentWithCleanup(arg);

  case OwnershipKind::None:
  case OwnershipKind::Unowned:
    return ManagedValue::forUnmanaged(arg);
  }
}

ManagedValue SILGenBuilder::createAllocRef(
    SILLocation loc, SILType refType, bool objc,
    ArrayRef<SILType> inputElementTypes,
    ArrayRef<ManagedValue> inputElementCountOperands) {
  llvm::SmallVector<SILType, 8> elementTypes(inputElementTypes.begin(),
                                             inputElementTypes.end());
  llvm::SmallVector<SILValue, 8> elementCountOperands;
  llvm::transform(inputElementCountOperands,
                  std::back_inserter(elementCountOperands),
                  [](ManagedValue mv) -> SILValue { return mv.getValue(); });

  AllocRefInst *i = createAllocRef(loc, refType, objc, false, false,
                                   elementTypes, elementCountOperands);
  return SGF.emitManagedRValueWithCleanup(i);
}

ManagedValue SILGenBuilder::createAllocRefDynamic(
    SILLocation loc, ManagedValue operand, SILType refType, bool objc,
    ArrayRef<SILType> inputElementTypes,
    ArrayRef<ManagedValue> inputElementCountOperands) {
  llvm::SmallVector<SILType, 8> elementTypes(inputElementTypes.begin(),
                                             inputElementTypes.end());
  llvm::SmallVector<SILValue, 8> elementCountOperands;
  llvm::transform(inputElementCountOperands,
                  std::back_inserter(elementCountOperands),
                  [](ManagedValue mv) -> SILValue { return mv.getValue(); });

  AllocRefDynamicInst *i =
      createAllocRefDynamic(loc, operand.getValue(), refType, objc, false,
                            elementTypes, elementCountOperands);
  return SGF.emitManagedRValueWithCleanup(i);
}

ManagedValue SILGenBuilder::createTupleExtract(SILLocation loc,
                                               ManagedValue base,
                                               unsigned index, SILType type) {
  ManagedValue borrowedBase = SGF.emitManagedBeginBorrow(loc, base.getValue());
  SILValue extract =
      createTupleExtract(loc, borrowedBase.getValue(), index, type);
  return ManagedValue::forUnmanaged(extract);
}

ManagedValue SILGenBuilder::createTupleExtract(SILLocation loc,
                                               ManagedValue value,
                                               unsigned index) {
  SILType type = value.getType().getTupleElementType(index);
  return createTupleExtract(loc, value, index, type);
}

ManagedValue SILGenBuilder::createLoadBorrow(SILLocation loc,
                                             ManagedValue base) {
  if (SGF.getTypeLowering(base.getType()).isTrivial()) {
    auto *i = createLoad(loc, base.getValue(), LoadOwnershipQualifier::Trivial);
    return ManagedValue::forUnmanaged(i);
  }

  auto *i = createLoadBorrow(loc, base.getValue());
  return SGF.emitManagedBorrowedRValueWithCleanup(base.getValue(), i);
}

ManagedValue SILGenBuilder::createFormalAccessLoadBorrow(SILLocation loc,
                                                         ManagedValue base) {
  if (SGF.getTypeLowering(base.getType()).isTrivial()) {
    auto *i = createLoad(loc, base.getValue(), LoadOwnershipQualifier::Trivial);
    return ManagedValue::forUnmanaged(i);
  }

  SILValue baseValue = base.getValue();
  auto *i = createLoadBorrow(loc, baseValue);
  return SGF.emitFormalEvaluationManagedBorrowedRValueWithCleanup(loc,
                                                                  baseValue, i);
}

ManagedValue SILGenBuilder::createFormalAccessLoadTake(SILLocation loc,
                                                       ManagedValue base) {
  if (SGF.getTypeLowering(base.getType()).isTrivial()) {
    auto *i = createLoad(loc, base.getValue(), LoadOwnershipQualifier::Trivial);
    return ManagedValue::forUnmanaged(i);
  }

  SILValue baseValue = base.getValue();
  auto i = emitLoadValueOperation(loc, baseValue, LoadOwnershipQualifier::Take);
  return SGF.emitFormalAccessManagedRValueWithCleanup(loc, i);
}

ManagedValue SILGenBuilder::createFormalAccessLoadCopy(SILLocation loc,
                                                       ManagedValue base) {
  if (SGF.getTypeLowering(base.getType()).isTrivial()) {
    auto *i = createLoad(loc, base.getValue(), LoadOwnershipQualifier::Trivial);
    return ManagedValue::forUnmanaged(i);
  }

  SILValue baseValue = base.getValue();
  auto i = emitLoadValueOperation(loc, baseValue, LoadOwnershipQualifier::Copy);
  return SGF.emitFormalAccessManagedRValueWithCleanup(loc, i);
}

ManagedValue
SILGenBuilder::createFormalAccessCopyValue(SILLocation loc,
                                           ManagedValue originalValue) {
  SILType ty = originalValue.getType();
  const auto &lowering = SGF.getTypeLowering(ty);
  if (lowering.isTrivial())
    return originalValue;

  assert(!lowering.isAddressOnly() && "cannot perform a copy value of an "
                                      "address only type");

  if (ty.isObject() &&
      originalValue.getOwnershipKind() == OwnershipKind::None) {
    return originalValue;
  }

  SILValue result =
      lowering.emitCopyValue(*this, loc, originalValue.getValue());
  return SGF.emitFormalAccessManagedRValueWithCleanup(loc, result);
}

ManagedValue SILGenBuilder::createFormalAccessCopyAddr(
    SILLocation loc, ManagedValue originalAddr, SILValue newAddr,
    IsTake_t isTake, IsInitialization_t isInit) {
  createCopyAddr(loc, originalAddr.getValue(), newAddr, isTake, isInit);
  return SGF.emitFormalAccessManagedBufferWithCleanup(loc, newAddr);
}

ManagedValue
SILGenBuilder::bufferForExpr(SILLocation loc, SILType ty,
                             const TypeLowering &lowering, SGFContext context,
                             llvm::function_ref<void(SILValue)> rvalueEmitter) {
  // If we have a single-buffer "emit into" initialization, use that for the
  // result.
  SILValue address = context.getAddressForInPlaceInitialization(SGF, loc);

  // If we couldn't emit into the Initialization, emit into a temporary
  // allocation.
  if (!address) {
    address = SGF.emitTemporaryAllocation(loc, ty.getObjectType());
  }

  rvalueEmitter(address);

  // If we have a single-buffer "emit into" initialization, use that for the
  // result.
  if (context.finishInPlaceInitialization(SGF)) {
    return ManagedValue::forInContext();
  }

  // Add a cleanup for the temporary we allocated.
  if (lowering.isTrivial())
    return ManagedValue::forUnmanaged(address);

  return SGF.emitManagedBufferWithCleanup(address);
}

ManagedValue SILGenBuilder::formalAccessBufferForExpr(
    SILLocation loc, SILType ty, const TypeLowering &lowering,
    SGFContext context, llvm::function_ref<void(SILValue)> rvalueEmitter) {
  // If we have a single-buffer "emit into" initialization, use that for the
  // result.
  SILValue address = context.getAddressForInPlaceInitialization(SGF, loc);

  // If we couldn't emit into the Initialization, emit into a temporary
  // allocation.
  if (!address) {
    address = SGF.emitTemporaryAllocation(loc, ty.getObjectType());
  }

  rvalueEmitter(address);

  // If we have a single-buffer "emit into" initialization, use that for the
  // result.
  if (context.finishInPlaceInitialization(SGF)) {
    return ManagedValue::forInContext();
  }

  // Add a cleanup for the temporary we allocated.
  if (lowering.isTrivial())
    return ManagedValue::forUnmanaged(address);

  return SGF.emitFormalAccessManagedBufferWithCleanup(loc, address);
}

ManagedValue SILGenBuilder::createUncheckedEnumData(SILLocation loc,
                                                    ManagedValue operand,
                                                    EnumElementDecl *element) {
  CleanupCloner cloner(*this, operand);
  SILValue result = createUncheckedEnumData(loc, operand.forward(SGF), element);
  return cloner.clone(result);
}

ManagedValue SILGenBuilder::createUncheckedTakeEnumDataAddr(
    SILLocation loc, ManagedValue operand, EnumElementDecl *element,
    SILType ty) {
  CleanupCloner cloner(*this, operand);
  SILValue result =
      createUncheckedTakeEnumDataAddr(loc, operand.forward(SGF), element);
  return cloner.clone(result);
}

ManagedValue
SILGenBuilder::createLoadIfLoadable(SILLocation loc, ManagedValue addr) {
  assert(addr.getType().isAddress());
  if (!addr.getType().isLoadable(SGF.F))
    return addr;
  return createLoadWithSameOwnership(loc, addr);
}

ManagedValue
SILGenBuilder::createLoadWithSameOwnership(SILLocation loc,
                                           ManagedValue addr) {
  if (addr.isPlusOne(SGF))
    return createLoadTake(loc, addr);
  else
    return createLoadBorrow(loc, addr);
}

ManagedValue SILGenBuilder::createLoadTake(SILLocation loc, ManagedValue v) {
  auto &lowering = SGF.getTypeLowering(v.getType());
  return createLoadTake(loc, v, lowering);
}

ManagedValue SILGenBuilder::createLoadTake(SILLocation loc, ManagedValue v,
                                           const TypeLowering &lowering) {
  assert(lowering.getLoweredType().getAddressType() == v.getType());
  SILValue result =
      lowering.emitLoadOfCopy(*this, loc, v.forward(SGF), IsTake);
  if (lowering.isTrivial())
    return ManagedValue::forUnmanaged(result);
  assert((!lowering.isAddressOnly() || !SGF.silConv.useLoweredAddresses()) &&
         "cannot retain an unloadable type");
  return SGF.emitManagedRValueWithCleanup(result, lowering);
}

ManagedValue SILGenBuilder::createLoadCopy(SILLocation loc, ManagedValue v) {
  auto &lowering = SGF.getTypeLowering(v.getType());
  return createLoadCopy(loc, v, lowering);
}

ManagedValue SILGenBuilder::createLoadCopy(SILLocation loc, ManagedValue v,
                                           const TypeLowering &lowering) {
  assert(lowering.getLoweredType().getAddressType() == v.getType());
  SILValue result =
      lowering.emitLoadOfCopy(*this, loc, v.getValue(), IsNotTake);
  if (lowering.isTrivial())
    return ManagedValue::forUnmanaged(result);
  assert((!lowering.isAddressOnly()
          || !SGF.silConv.useLoweredAddresses()) &&
         "cannot retain an unloadable type");
  return SGF.emitManagedRValueWithCleanup(result, lowering);
}

static ManagedValue createInputFunctionArgument(
    SILGenBuilder &B, SILType type, SILLocation loc, ValueDecl *decl = nullptr,
    bool isNoImplicitCopy = false,
    LifetimeAnnotation lifetimeAnnotation = LifetimeAnnotation::None,
    bool isClosureCapture = false,
    bool isFormalParameterPack = false) {
  auto &SGF = B.getSILGenFunction();
  SILFunction &F = B.getFunction();
  assert((F.isBare() || isFormalParameterPack || decl) &&
         "Function arguments of non-bare functions must have a decl");
  auto *arg = F.begin()->createFunctionArgument(type, decl);
  if (auto *pd = dyn_cast_or_null<ParamDecl>(decl)) {
    if (!arg->getType().isPureMoveOnly()) {
      isNoImplicitCopy |= pd->getSpecifier() == ParamSpecifier::Borrowing;
      isNoImplicitCopy |= pd->getSpecifier() == ParamSpecifier::Consuming;
    }
  }
  if (isNoImplicitCopy)
    arg->setNoImplicitCopy(isNoImplicitCopy);
  arg->setClosureCapture(isClosureCapture);
  arg->setLifetimeAnnotation(lifetimeAnnotation);
  arg->setFormalParameterPack(isFormalParameterPack);
  switch (arg->getArgumentConvention()) {
  case SILArgumentConvention::Indirect_In_Guaranteed:
  case SILArgumentConvention::Direct_Guaranteed:
  case SILArgumentConvention::Pack_Guaranteed:
    // Guaranteed parameters are passed at +0.
    return ManagedValue::forUnmanaged(arg);
  case SILArgumentConvention::Direct_Unowned:
    // Unowned parameters are only guaranteed at the instant of the call, so we
    // must retain them even if we're in a context that can accept a +0 value.
    //
    // NOTE: If we have a trivial value, the copy will do nothing, so this is
    // just a convenient way to avoid writing conditional code.
    return ManagedValue::forUnmanaged(arg).copy(SGF, loc);

  case SILArgumentConvention::Direct_Owned:
    return SGF.emitManagedRValueWithCleanup(arg);

  case SILArgumentConvention::Pack_Owned:
    return SGF.emitManagedPackWithCleanup(arg);

  case SILArgumentConvention::Indirect_In:
    if (SGF.silConv.useLoweredAddresses())
      return SGF.emitManagedBufferWithCleanup(arg);
    return SGF.emitManagedRValueWithCleanup(arg);

  case SILArgumentConvention::Indirect_Inout:
  case SILArgumentConvention::Indirect_InoutAliasable:
  case SILArgumentConvention::Pack_Inout:
    // An inout parameter is +0 and guaranteed, but represents an lvalue.
    return ManagedValue::forLValue(arg);
  case SILArgumentConvention::Indirect_Out:
  case SILArgumentConvention::Pack_Out:
    llvm_unreachable("unsupported convention for API");
  }
  llvm_unreachable("bad parameter convention");
}

ManagedValue SILGenBuilder::createInputFunctionArgument(
    SILType type, ValueDecl *decl, bool isNoImplicitCopy,
    LifetimeAnnotation lifetimeAnnotation, bool isClosureCapture,
    bool isFormalParameterPack) {
  return ::createInputFunctionArgument(*this, type, SILLocation(decl), decl,
                                       isNoImplicitCopy, lifetimeAnnotation,
                                       isClosureCapture,
                                       isFormalParameterPack);
}

ManagedValue SILGenBuilder::createInputFunctionArgument(
    SILType type, llvm::Optional<SILLocation> inputLoc) {
  assert(inputLoc.has_value() && "This optional is only for overload resolution "
                                "purposes! Do not pass in None here!");
  return ::createInputFunctionArgument(*this, type, *inputLoc);
}

ManagedValue
SILGenBuilder::createMarkUninitialized(ValueDecl *decl, ManagedValue operand,
                                       MarkUninitializedInst::Kind muKind) {
  // We either have an owned or trivial value.
  SILValue value = createMarkUninitialized(decl, operand.forward(SGF), muKind);
  assert(value->getType().isObject() && "Expected only objects here");

  // If we have a trivial value, just return without a cleanup.
  if (operand.getOwnershipKind() != OwnershipKind::Owned) {
    return ManagedValue::forUnmanaged(value);
  }

  // Otherwise, recreate the cleanup.
  return SGF.emitManagedRValueWithCleanup(value);
}

ManagedValue SILGenBuilder::createEnum(SILLocation loc, ManagedValue payload,
                                       EnumElementDecl *decl, SILType type) {
  SILValue result = createEnum(loc, payload.forward(SGF), decl, type);
  if (result->getOwnershipKind() != OwnershipKind::Owned)
    return ManagedValue::forUnmanaged(result);
  return SGF.emitManagedRValueWithCleanup(result);
}

ManagedValue SILGenBuilder::createUnconditionalCheckedCast(
    SILLocation loc, ManagedValue op,
    SILType destLoweredTy, CanType destFormalTy) {
  SILValue result =
      createUnconditionalCheckedCast(loc, op.forward(SGF),
                                     destLoweredTy, destFormalTy);
  return SGF.emitManagedRValueWithCleanup(result);
}

void SILGenBuilder::createCheckedCastBranch(SILLocation loc, bool isExact,
                                            ManagedValue op,
                                            SILType destLoweredTy,
                                            CanType destFormalTy,
                                            SILBasicBlock *trueBlock,
                                            SILBasicBlock *falseBlock,
                                            ProfileCounter Target1Count,
                                            ProfileCounter Target2Count) {
  // Casting a guaranteed value requires ownership preservation.
  if (!doesCastPreserveOwnershipForTypes(SGF.SGM.M, op.getType().getASTType(),
                                         destFormalTy)) {
    op = op.ensurePlusOne(SGF, loc);
  }
  createCheckedCastBranch(loc, isExact, op.forward(SGF),
                          destLoweredTy, destFormalTy,
                          trueBlock, falseBlock,
                          Target1Count, Target2Count);
}

ManagedValue SILGenBuilder::createUpcast(SILLocation loc, ManagedValue original,
                                         SILType type) {
  CleanupCloner cloner(*this, original);
  SILValue convertedValue = createUpcast(loc, original.forward(SGF), type);
  return cloner.clone(convertedValue);
}

ManagedValue SILGenBuilder::createOptionalSome(SILLocation loc,
                                               ManagedValue arg) {
  CleanupCloner cloner(*this, arg);
  auto &argTL = SGF.getTypeLowering(arg.getType());
  SILType optionalType = SILType::getOptionalType(arg.getType());
  if (argTL.isLoadable() || !SGF.silConv.useLoweredAddresses()) {
    SILValue someValue =
        createOptionalSome(loc, arg.forward(SGF), optionalType);
    return cloner.clone(someValue);
  }

  SILValue tempResult = SGF.emitTemporaryAllocation(loc, optionalType);
  RValue rvalue(SGF, loc, arg.getType().getASTType(), arg);
  ArgumentSource argValue(loc, std::move(rvalue));
  SGF.emitInjectOptionalValueInto(
      loc, std::move(argValue), tempResult,
      SGF.getTypeLowering(tempResult->getType()));
  return ManagedValue::forUnmanaged(tempResult);
}

ManagedValue SILGenBuilder::createManagedOptionalNone(SILLocation loc,
                                                      SILType type) {
  if (!type.isAddressOnly(getFunction()) ||
      !SGF.silConv.useLoweredAddresses()) {
    SILValue noneValue = createOptionalNone(loc, type);
    return ManagedValue::forUnmanaged(noneValue);
  }

  SILValue tempResult = SGF.emitTemporaryAllocation(loc, type);
  SGF.emitInjectOptionalNothingInto(loc, tempResult,
                                    SGF.getTypeLowering(type));
  return ManagedValue::forUnmanaged(tempResult);
}

ManagedValue SILGenBuilder::createManagedFunctionRef(SILLocation loc,
                                                     SILFunction *f) {
  return ManagedValue::forUnmanaged(createFunctionRefFor(loc, f));
}

ManagedValue SILGenBuilder::createTupleElementAddr(SILLocation Loc,
                                                   ManagedValue Base,
                                                   unsigned Index,
                                                   SILType Type) {
  SILValue TupleEltAddr =
      createTupleElementAddr(Loc, Base.getValue(), Index, Type);
  return ManagedValue::forUnmanaged(TupleEltAddr);
}

ManagedValue SILGenBuilder::createTupleElementAddr(SILLocation Loc,
                                                   ManagedValue Value,
                                                   unsigned Index) {
  SILType Type = Value.getType().getTupleElementType(Index);
  return createTupleElementAddr(Loc, Value, Index, Type);
}

ManagedValue SILGenBuilder::createUncheckedRefCast(SILLocation loc,
                                                   ManagedValue value,
                                                   SILType type) {
  CleanupCloner cloner(*this, value);
  SILValue cast = createUncheckedRefCast(loc, value.forward(SGF), type);
  return cloner.clone(cast);
}

ManagedValue SILGenBuilder::createUncheckedBitCast(SILLocation loc,
                                                   ManagedValue value,
                                                   SILType type) {
  CleanupCloner cloner(*this, value);
  SILValue cast = createUncheckedReinterpretCast(loc, value.getValue(), type);

  // Currently createUncheckedBitCast only produces these
  // instructions. We assert here to make sure if this changes, this code is
  // updated.
  assert((isa<UncheckedTrivialBitCastInst>(cast) ||
          isa<UncheckedRefCastInst>(cast) ||
          isa<UncheckedBitwiseCastInst>(cast) ||
          isa<ConvertFunctionInst>(cast)) &&
         "SILGenBuilder is out of sync with SILBuilder.");

  // If we have a trivial inst, just return early.
  if (isa<UncheckedTrivialBitCastInst>(cast))
    return ManagedValue::forUnmanaged(cast);

  // If we perform an unchecked bitwise case, then we are producing a new RC
  // identity implying that we need a copy of the casted value to be returned so
  // that the inputs/outputs of the case have separate ownership.
  if (isa<UncheckedBitwiseCastInst>(cast)) {
    return ManagedValue::forUnmanaged(cast).copy(SGF, loc);
  }

  // Otherwise, we forward the cleanup of the input value and place the cleanup
  // on the cast value since unchecked_ref_cast is "forwarding".
  value.forward(SGF);
  return cloner.clone(cast);
}

ManagedValue SILGenBuilder::createOpenExistentialRef(SILLocation loc,
                                                     ManagedValue original,
                                                     SILType type) {
  CleanupCloner cloner(*this, original);
  SILValue openedExistential =
      createOpenExistentialRef(loc, original.forward(SGF), type);
  return cloner.clone(openedExistential);
}

ManagedValue SILGenBuilder::createOpenExistentialValue(SILLocation loc,
                                                       ManagedValue original,
                                                       SILType type) {
  ManagedValue borrowedExistential = original.formalAccessBorrow(SGF, loc);
  SILValue openedExistential =
      createOpenExistentialValue(loc, borrowedExistential.getValue(), type);
  return ManagedValue::forUnmanaged(openedExistential);
}

ManagedValue SILGenBuilder::createOpenExistentialBoxValue(SILLocation loc,
                                                          ManagedValue original,
                                                          SILType type) {
  ManagedValue borrowedExistential = original.formalAccessBorrow(SGF, loc);
  SILValue openedExistential =
      createOpenExistentialBoxValue(loc, borrowedExistential.getValue(), type);
  return ManagedValue::forUnmanaged(openedExistential);
}

ManagedValue SILGenBuilder::createOpenExistentialBox(SILLocation loc,
                                                     ManagedValue original,
                                                     SILType type) {
  ManagedValue borrowedExistential = original.formalAccessBorrow(SGF, loc);
  SILValue openedExistentialAddr =
      createOpenExistentialBox(loc, borrowedExistential.getValue(), type);
  return ManagedValue::forUnmanaged(openedExistentialAddr);
}

ManagedValue SILGenBuilder::createOpenExistentialMetatype(SILLocation loc,
                                                          ManagedValue value,
                                                          SILType openedType) {
  SILValue result = SILGenBuilder::createOpenExistentialMetatype(
      loc, value.getValue(), openedType);
  return ManagedValue::forTrivialRValue(result);
}

ManagedValue SILGenBuilder::createStore(SILLocation loc, ManagedValue value,
                                        SILValue address,
                                        StoreOwnershipQualifier qualifier) {
  CleanupCloner cloner(*this, value);
  if (value.getOwnershipKind() == OwnershipKind::None)
    qualifier = StoreOwnershipQualifier::Trivial;
  createStore(loc, value.forward(SGF), address, qualifier);
  return cloner.clone(address);
}

ManagedValue SILGenBuilder::createSuperMethod(SILLocation loc,
                                              ManagedValue operand,
                                              SILDeclRef member,
                                              SILType methodTy) {
  SILValue v = createSuperMethod(loc, operand.getValue(), member, methodTy);
  return ManagedValue::forUnmanaged(v);
}

ManagedValue SILGenBuilder::createObjCSuperMethod(SILLocation loc,
                                                  ManagedValue operand,
                                                  SILDeclRef member,
                                                  SILType methodTy) {
  SILValue v = createObjCSuperMethod(loc, operand.getValue(), member, methodTy);
  return ManagedValue::forUnmanaged(v);
}

ManagedValue SILGenBuilder::
createValueMetatype(SILLocation loc, SILType metatype,
                    ManagedValue base) {
  SILValue v = createValueMetatype(loc, metatype, base.getValue());
  return ManagedValue::forUnmanaged(v);
}

ManagedValue SILGenBuilder::createStoreBorrow(SILLocation loc,
                                              ManagedValue value,
                                              SILValue address) {
  assert(value.getOwnershipKind() == OwnershipKind::Guaranteed);
  auto *sbi = createStoreBorrow(loc, value.getValue(), address);
  SGF.Cleanups.pushCleanup<EndBorrowCleanup>(sbi);
  return ManagedValue(sbi, CleanupHandle::invalid());
}

ManagedValue SILGenBuilder::createFormalAccessStoreBorrow(SILLocation loc,
                                                          ManagedValue value,
                                                          SILValue address) {
  assert(value.getOwnershipKind() == OwnershipKind::Guaranteed);
  auto *sbi = createStoreBorrow(loc, value.getValue(), address);
  return SGF.emitFormalEvaluationManagedBorrowedRValueWithCleanup(
      loc, value.getValue(), sbi);
}

ManagedValue SILGenBuilder::createStoreBorrowOrTrivial(SILLocation loc,
                                                       ManagedValue value,
                                                       SILValue address) {
  if (value.getOwnershipKind() == OwnershipKind::None) {
    createStore(loc, value, address, StoreOwnershipQualifier::Trivial);
    return ManagedValue(address, CleanupHandle::invalid());
  }

  return createStoreBorrow(loc, value, address);
}

ManagedValue SILGenBuilder::createBridgeObjectToRef(SILLocation loc,
                                                    ManagedValue mv,
                                                    SILType destType) {
  CleanupCloner cloner(*this, mv);
  SILValue result = createBridgeObjectToRef(loc, mv.forward(SGF), destType);
  return cloner.clone(result);
}

ManagedValue SILGenBuilder::createRefToBridgeObject(SILLocation loc,
                                                    ManagedValue mv,
                                                    SILValue bits) {
  CleanupCloner cloner(*this, mv);
  SILValue result = createRefToBridgeObject(loc, mv.forward(SGF), bits);
  return cloner.clone(result);
}

ManagedValue SILGenBuilder::createBlockToAnyObject(SILLocation loc,
                                                   ManagedValue v,
                                                   SILType destType) {
  assert(SGF.getASTContext().LangOpts.EnableObjCInterop);
  assert(destType.isAnyObject());
  assert(v.getType().is<SILFunctionType>());
  assert(v.getType().castTo<SILFunctionType>()->getRepresentation() ==
           SILFunctionTypeRepresentation::Block);

  // For now, we don't have a better instruction than this.
  return createUncheckedRefCast(loc, v, destType);
}

BranchInst *SILGenBuilder::createBranch(SILLocation loc,
                                        SILBasicBlock *targetBlock,
                                        ArrayRef<ManagedValue> args) {
  llvm::SmallVector<SILValue, 8> newArgs;
  llvm::transform(args, std::back_inserter(newArgs),
                  [&](ManagedValue mv) -> SILValue { return mv.forward(SGF); });
  return createBranch(loc, targetBlock, newArgs);
}

ReturnInst *SILGenBuilder::createReturn(SILLocation loc,
                                        ManagedValue returnValue) {
  return createReturn(loc, returnValue.forward(SGF));
}

ReturnInst *
SILGenBuilder::createReturn(SILLocation loc, SILValue returnValue,
                            AssertingManualScope &&functionLevelScope) {
  std::move(functionLevelScope).pop();
  return createReturn(loc, returnValue);
}

ManagedValue SILGenBuilder::createTuple(SILLocation loc, SILType type,
                                        ArrayRef<ManagedValue> elements) {
  // Handle the empty tuple case.
  if (elements.empty()) {
    SILValue result = createTuple(loc, type, ArrayRef<SILValue>());
    return ManagedValue::forUnmanaged(result);
  }

  // We need to look for the first value without .none ownership and use that as
  // our cleanup cloner value.
  auto iter = find_if(elements, [&](ManagedValue mv) -> bool {
    return mv.getOwnershipKind() != OwnershipKind::None;
  });

  llvm::SmallVector<SILValue, 8> forwardedValues;

  // If we have all .none values, then just create the tuple and return. No
  // cleanups need to be cloned.
  if (iter == elements.end()) {
    llvm::transform(elements, std::back_inserter(forwardedValues),
                    [&](ManagedValue mv) -> SILValue {
                      return mv.forward(getSILGenFunction());
                    });
    SILValue result = createTuple(loc, type, forwardedValues);
    return ManagedValue::forUnmanaged(result);
  }

  // Otherwise, we use that values cloner. This is taking advantage of
  // instructions that forward ownership requiring that all input values have
  // the same ownership if they are non-trivial.
  CleanupCloner cloner(*this, *iter);
  llvm::transform(elements, std::back_inserter(forwardedValues),
                  [&](ManagedValue mv) -> SILValue {
                    return mv.forward(getSILGenFunction());
                  });
  return cloner.clone(createTuple(loc, type, forwardedValues));
}

ManagedValue SILGenBuilder::createUncheckedAddrCast(SILLocation loc, ManagedValue op,
                                                    SILType resultTy) {
  CleanupCloner cloner(*this, op);
  SILValue cast = createUncheckedAddrCast(loc, op.forward(SGF), resultTy);
  return cloner.clone(cast);
}

ManagedValue SILGenBuilder::createUncheckedTrivialBitCast(SILLocation loc,
                                                          ManagedValue original,
                                                          SILType type) {
  SILValue result =
      SGF.B.createUncheckedTrivialBitCast(loc, original.getValue(), type);
  return ManagedValue::forUnmanaged(result);
}

void SILGenBuilder::emitDestructureValueOperation(
    SILLocation loc, ManagedValue value,
    llvm::function_ref<void(unsigned, ManagedValue)> func) {
  CleanupCloner cloner(*this, value);

  // NOTE: We can not directly use SILBuilder::emitDestructureValueOperation()
  // here since we need to create all of our cleanups before invoking \p
  // func. This is necessary since our func may want to emit conditional code
  // with an early exit, emitting unused cleanups from the current scope via the
  // function emitBranchAndCleanups(). If we have not yet created those
  // cleanups, we will introduce a leak along that path.
  SmallVector<ManagedValue, 8> destructuredValues;
  emitDestructureValueOperation(
      loc, value.forward(SGF), [&](unsigned index, SILValue subValue) {
        destructuredValues.push_back(cloner.clone(subValue));
      });
  for (auto p : llvm::enumerate(destructuredValues)) {
    func(p.index(), p.value());
  }
}

void SILGenBuilder::emitDestructureValueOperation(
    SILLocation loc, ManagedValue value,
    SmallVectorImpl<ManagedValue> &destructuredValues) {
  CleanupCloner cloner(*this, value);
  emitDestructureValueOperation(
      loc, value.forward(SGF), [&](unsigned index, SILValue subValue) {
        destructuredValues.push_back(cloner.clone(subValue));
      });
}

void SILGenBuilder::emitDestructureAddressOperation(
    SILLocation loc, ManagedValue value,
    llvm::function_ref<void(unsigned, ManagedValue)> func) {
  CleanupCloner cloner(*this, value);
  // NOTE: We can not directly use SILBuilder::emitDestructureAddressOperation()
  // here since we need to create all of our cleanups before invoking \p
  // func. This is necessary since our func may want to emit conditional code
  // with an early exit, emitting unused cleanups from the current scope via the
  // function emitBranchAndCleanups(). If we have not yet created those
  // cleanups, we will introduce a leak along that path.
  SmallVector<ManagedValue, 8> destructuredAddresses;
  emitDestructureAddressOperation(
      loc, value.forward(SGF), [&](unsigned index, SILValue subValue) {
        destructuredAddresses.push_back(cloner.clone(subValue));
      });
  for (auto p : llvm::enumerate(destructuredAddresses)) {
    func(p.index(), p.value());
  }
}

ManagedValue SILGenBuilder::createProjectBox(SILLocation loc, ManagedValue mv,
                                             unsigned index) {
  auto *pbi = createProjectBox(loc, mv.getValue(), index);
  return ManagedValue::forUnmanaged(pbi);
}

ManagedValue SILGenBuilder::createMarkDependence(SILLocation loc,
                                                 ManagedValue value,
                                                 ManagedValue base) {
  CleanupCloner cloner(*this, value);
  auto *mdi = createMarkDependence(loc, value.forward(getSILGenFunction()),
                                   base.forward(getSILGenFunction()));
  return cloner.clone(mdi);
}

ManagedValue SILGenBuilder::createBeginBorrow(SILLocation loc,
                                              ManagedValue value,
                                              bool isLexical) {
  auto *newValue =
      SILBuilder::createBeginBorrow(loc, value.getValue(), isLexical);
  SGF.emitManagedBorrowedRValueWithCleanup(newValue);
  return ManagedValue::forUnmanaged(newValue);
}

ManagedValue SILGenBuilder::createMoveValue(SILLocation loc, ManagedValue value,
                                            bool isLexical) {
  assert(value.isPlusOne(SGF) && "Must be +1 to be moved!");
  CleanupCloner cloner(*this, value);
  auto *mdi =
      createMoveValue(loc, value.forward(getSILGenFunction()), isLexical);
  return cloner.clone(mdi);
}

ManagedValue
SILGenBuilder::createOwnedMoveOnlyWrapperToCopyableValue(SILLocation loc,
                                                         ManagedValue value) {
  assert(value.isPlusOne(SGF) && "Argument must be at +1!");
  CleanupCloner cloner(*this, value);
  auto *mdi = createOwnedMoveOnlyWrapperToCopyableValue(
      loc, value.forward(getSILGenFunction()));
  return cloner.clone(mdi);
}

ManagedValue SILGenBuilder::createGuaranteedMoveOnlyWrapperToCopyableValue(
    SILLocation loc, ManagedValue value) {
  auto *mdi =
      createGuaranteedMoveOnlyWrapperToCopyableValue(loc, value.getValue());
  assert(mdi->getOperand()->getType().isObject() && "Expected an object?!");
  return ManagedValue::forUnmanaged(mdi);
}

ManagedValue
SILGenBuilder::createOwnedCopyableToMoveOnlyWrapperValue(SILLocation loc,
                                                         ManagedValue value) {
  assert(value.isPlusOne(SGF) && "Argument must be at +1!");
  CleanupCloner cloner(*this, value);
  auto *mdi = createOwnedCopyableToMoveOnlyWrapperValue(
      loc, value.forward(getSILGenFunction()));
  return cloner.clone(mdi);
}

ManagedValue SILGenBuilder::createGuaranteedCopyableToMoveOnlyWrapperValue(
    SILLocation loc, ManagedValue value) {
  auto *mdi =
      createGuaranteedCopyableToMoveOnlyWrapperValue(loc, value.getValue());
  assert(mdi->getOperand()->getType().isObject() && "Expected an object?!");
  return ManagedValue::forUnmanaged(mdi);
}

ManagedValue
SILGenBuilder::createMarkMustCheckInst(SILLocation loc, ManagedValue value,
                                       MarkMustCheckInst::CheckKind kind) {
  assert((value.isPlusOne(SGF) || value.isLValue() ||
          value.getType().isAddress()) &&
         "Argument must be at +1 or be an address!");
  CleanupCloner cloner(*this, value);
  auto *mdi = SILBuilder::createMarkMustCheckInst(
      loc, value.forward(getSILGenFunction()), kind);
  return cloner.clone(mdi);
}

ManagedValue SILGenBuilder::emitCopyValueOperation(SILLocation loc,
                                                   ManagedValue value) {
  auto cvi = SILBuilder::emitCopyValueOperation(loc, value.getValue());
  // Trivial type.
  if (cvi == value.getValue())
    return value;
  return SGF.emitManagedRValueWithCleanup(cvi);
}

void SILGenBuilder::emitCopyAddrOperation(SILLocation loc, SILValue srcAddr,
                                          SILValue destAddr, IsTake_t isTake,
                                          IsInitialization_t isInitialize) {
  auto &lowering = getTypeLowering(srcAddr->getType());
  lowering.emitCopyInto(*this, loc, srcAddr, destAddr, isTake, isInitialize);
}

ManagedValue SILGenBuilder::createExplicitCopyValue(SILLocation loc,
                                                    ManagedValue operand) {
  auto cvi = SILBuilder::createExplicitCopyValue(loc, operand.getValue());
  return SGF.emitManagedRValueWithCleanup(cvi);
}
