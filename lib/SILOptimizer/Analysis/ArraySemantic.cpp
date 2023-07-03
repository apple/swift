//===--- ArraySemantic.cpp - Wrapper around array semantic calls. ---------===//
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

#include "swift/SILOptimizer/Analysis/ArraySemantic.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/StringSwitch.h"

using namespace swift;

/// Determine which kind of array semantics function this is.
ArrayCallKind swift::getArraySemanticsKind(SILFunction *f) {
  ArrayCallKind Kind = ArrayCallKind::kNone;

  for (auto &Attrs : f->getSemanticsAttrs()) {
    auto Tmp =
        llvm::StringSwitch<ArrayCallKind>(Attrs)
            .Case("array.props.isNativeTypeChecked",
                  ArrayCallKind::kArrayPropsIsNativeTypeChecked)
            .Case("array.init", ArrayCallKind::kArrayInit)
            .Case("array.init.empty", ArrayCallKind::kArrayInitEmpty)
            .Case("array.uninitialized", ArrayCallKind::kArrayUninitialized)
            .Case("array.uninitialized_intrinsic", ArrayCallKind::kArrayUninitializedIntrinsic)
            .Case("array.finalize_intrinsic", ArrayCallKind::kArrayFinalizeIntrinsic)
            .Case("array.check_subscript", ArrayCallKind::kCheckSubscript)
            .Case("array.check_index", ArrayCallKind::kCheckIndex)
            .Case("array.get_count", ArrayCallKind::kGetCount)
            .Case("array.get_capacity", ArrayCallKind::kGetCapacity)
            .Case("array.get_element", ArrayCallKind::kGetElement)
            .Case("array.make_mutable", ArrayCallKind::kMakeMutable)
            .Case("array.end_mutation", ArrayCallKind::kEndMutation)
            .Case("array.get_element_address",
                  ArrayCallKind::kGetElementAddress)
            .Case("array.mutate_unknown", ArrayCallKind::kMutateUnknown)
            .Case("array.reserve_capacity_for_append",
                  ArrayCallKind::kReserveCapacityForAppend)
            .Case("array.withUnsafeMutableBufferPointer",
                  ArrayCallKind::kWithUnsafeMutableBufferPointer)
            .Case("array.append_contentsOf", ArrayCallKind::kAppendContentsOf)
            .Case("array.append_element", ArrayCallKind::kAppendElement)
            .Default(ArrayCallKind::kNone);
    if (Tmp != ArrayCallKind::kNone) {
      assert(Kind == ArrayCallKind::kNone && "Multiple array semantic "
                                             "strings?!");
      Kind = Tmp;
    }
  }

  return Kind;
}

static ParameterConvention
getSelfParameterConvention(ApplyInst *SemanticsCall) {
  FunctionRefInst *FRI = cast<FunctionRefInst>(SemanticsCall->getCallee());
  SILFunction *F = FRI->getReferencedFunction();
  auto FnTy = F->getLoweredFunctionType();

  return FnTy->getSelfParameter().getConvention();
}

/// Make sure that all parameters are passed with a reference count
/// neutral parameter convention except for self.
bool swift::ArraySemanticsCall::isValidSignature() {
  assert(SemanticsCall && getKind() != ArrayCallKind::kNone &&
         "Need an array semantic call");
  FunctionRefInst *FRI = cast<FunctionRefInst>(SemanticsCall->getCallee());
  SILFunction *F = FRI->getReferencedFunction();
  auto FnTy = F->getLoweredFunctionType();
  auto &Mod = F->getModule();

  // Check whether we have a valid signature for semantic calls that we hoist.
  switch (getKind()) {
  // All other calls can be consider valid.
  default: break;
  case ArrayCallKind::kArrayPropsIsNativeTypeChecked: {
    // @guaranteed/@owned Self
    if (SemanticsCall->getNumArguments() != 1)
      return false;
    auto SelfConvention = FnTy->getSelfParameter().getConvention();
    return SelfConvention == ParameterConvention::Direct_Guaranteed ||
           SelfConvention == ParameterConvention::Direct_Owned;
  }
  case ArrayCallKind::kCheckIndex: {
    // Int, @guaranteed/@owned Self
    if (SemanticsCall->getNumArguments() != 2 ||
        !SemanticsCall->getArgument(0)->getType().isTrivial(*F))
      return false;
    auto SelfConvention = FnTy->getSelfParameter().getConvention();
    return SelfConvention == ParameterConvention::Direct_Guaranteed ||
           SelfConvention == ParameterConvention::Direct_Owned;
  }
  case ArrayCallKind::kCheckSubscript: {
    // Int, Bool, Self
    unsigned numArgs = SemanticsCall->getNumArguments();
    if (numArgs != 2 && numArgs != 3)
      return false;
    if (!SemanticsCall->getArgument(0)->getType().isTrivial(*F))
      return false;
    if (numArgs == 3 && !SemanticsCall->getArgument(1)->getType().isTrivial(*F))
      return false;
    auto SelfConvention = FnTy->getSelfParameter().getConvention();
    return SelfConvention == ParameterConvention::Direct_Guaranteed ||
           SelfConvention == ParameterConvention::Direct_Owned;
  }
  case ArrayCallKind::kMakeMutable: {
    auto SelfConvention = FnTy->getSelfParameter().getConvention();
    return SelfConvention == ParameterConvention::Indirect_Inout;
  }
  case ArrayCallKind::kArrayUninitialized: {
    // Make sure that if we are a _adoptStorage call that our storage is
    // uniquely referenced by us.
    SILValue Arg0 = SemanticsCall->getArgument(0);
    if (Arg0->getType().isExistentialType()) {
      auto *AllocBufferAI = dyn_cast<ApplyInst>(Arg0);
      if (!AllocBufferAI)
        return false;
      auto *AllocFn = AllocBufferAI->getReferencedFunctionOrNull();
      if (!AllocFn || AllocFn->getName() != "swift_bufferAllocate" ||
          !hasOneNonDebugUse(AllocBufferAI))
        return false;
    }
    return true;
  }
  case ArrayCallKind::kWithUnsafeMutableBufferPointer: {
    SILFunctionConventions origConv(SemanticsCall->getOrigCalleeType(), Mod);
    if (origConv.getNumIndirectSILResults() != 1
        || SemanticsCall->getNumArguments() != 3)
      return false;
    auto SelfConvention = FnTy->getSelfParameter().getConvention();
    return SelfConvention == ParameterConvention::Indirect_Inout;
  }
  }

  return true;
}

/// Match array semantic calls.
swift::ArraySemanticsCall::ArraySemanticsCall(SILValue V,
                                              StringRef semanticName,
                                              bool matchPartialName)
    : SemanticsCall(nullptr) {
  if (auto AI = dyn_cast<ApplyInst>(V))
    initialize(AI, semanticName, matchPartialName);
}

/// Match array semantic calls.
swift::ArraySemanticsCall::ArraySemanticsCall(SILInstruction *I,
                                              StringRef semanticName,
                                              bool matchPartialName)
    : SemanticsCall(nullptr) {
  if (auto AI = dyn_cast<ApplyInst>(I))
    initialize(AI, semanticName, matchPartialName);
}

/// Match array semantic calls.
swift::ArraySemanticsCall::ArraySemanticsCall(ApplyInst *AI,
                                              StringRef semanticName,
                                              bool matchPartialName)
    : SemanticsCall(nullptr) {
  initialize(AI, semanticName, matchPartialName);
}

void ArraySemanticsCall::initialize(ApplyInst *AI, StringRef semanticName,
                                    bool matchPartialName) {
  auto *fn = AI->getReferencedFunctionOrNull();
  if (!fn)
    return;

  if (!(matchPartialName
          ? fn->hasSemanticsAttrThatStartsWith(semanticName)
          : fn->hasSemanticsAttr(semanticName)))
    return;

  SemanticsCall = AI;

  // Need a 'self' argument otherwise this is not a semantic call that
  // we recognize.
  if (getKind() < ArrayCallKind::kArrayInit && !hasSelf())
    SemanticsCall = nullptr;

  // A arguments must be passed reference count neutral except for self.
  if (SemanticsCall && !isValidSignature())
    SemanticsCall = nullptr;
}

/// Determine which kind of array semantics call this is.
ArrayCallKind swift::ArraySemanticsCall::getKind() const {
  if (!SemanticsCall)
    return ArrayCallKind::kNone;

  auto F = cast<FunctionRefInst>(SemanticsCall->getCallee())
               ->getReferencedFunction();

  return getArraySemanticsKind(F);
}

bool swift::ArraySemanticsCall::hasSelf() const {
  assert(SemanticsCall && "Must have a semantics call");
  // Array.init and Array.uninitialized return 'self' @owned.
  return SemanticsCall->getOrigCalleeType()->hasSelfParam();
}

SILValue swift::ArraySemanticsCall::getSelf() const {
  return SemanticsCall->getSelfArgument();
}

Operand &swift::ArraySemanticsCall::getSelfOperand() const {
  return SemanticsCall->getSelfArgumentOperand();
}

bool swift::ArraySemanticsCall::hasGuaranteedSelf() const {
  if (!hasSelf())
    return false;
  return getSelfParameterConvention(SemanticsCall) ==
    ParameterConvention::Direct_Guaranteed;
}

bool swift::ArraySemanticsCall::hasGetElementDirectResult() const {
  assert(getKind() == ArrayCallKind::kGetElement &&
         "must be an array.get_element call");
  bool DirectResult =
      (SemanticsCall->getOrigCalleeConv().getNumIndirectSILResults() == 0);
  assert((DirectResult && SemanticsCall->getNumArguments() == 4 ||
          !DirectResult && SemanticsCall->getNumArguments() == 5) &&
         "wrong number of array.get_element call arguments");
  return DirectResult;
}

SILValue swift::ArraySemanticsCall::getTypeCheckedArgument() const {
  return SemanticsCall->getArgument(hasGetElementDirectResult() ? 1 : 2);
}

SILValue swift::ArraySemanticsCall::getSubscriptCheckArgument() const {
  return SemanticsCall->getArgument(hasGetElementDirectResult() ? 2 : 3);
}

SILValue swift::ArraySemanticsCall::getIndex() const {
  assert(SemanticsCall && "Must have a semantics call");
  assert(SemanticsCall->getNumArguments() && "Must have arguments");
  assert(getKind() == ArrayCallKind::kCheckSubscript ||
         getKind() == ArrayCallKind::kCheckIndex ||
         getKind() == ArrayCallKind::kGetElement ||
         getKind() == ArrayCallKind::kGetElementAddress);

  if (getKind() == ArrayCallKind::kGetElement)
    return SemanticsCall->getArgument(hasGetElementDirectResult() ? 0 : 1);

  return SemanticsCall->getArgument(0);
}

llvm::Optional<int64_t> swift::ArraySemanticsCall::getConstantIndex() const {
  auto *IndexStruct = dyn_cast<StructInst>(getIndex());
  if (!IndexStruct)
    return llvm::None;
  auto StructOpds = IndexStruct->getElements();
  if (StructOpds.size() != 1)
    return llvm::None;
  auto *Literal = dyn_cast<IntegerLiteralInst>(StructOpds[0]);
  if (!Literal)
    return llvm::None;

  auto Val = Literal->getValue();
  if (Val.getNumWords()>1)
    return llvm::None;

  return Val.getSExtValue();
}

static bool canHoistArrayArgument(ApplyInst *SemanticsCall, SILValue Arr,
                                  SILInstruction *InsertBefore,
                                  DominanceInfo *DT) {

  // We only know how to hoist inout, owned or guaranteed parameters.
  auto Convention = getSelfParameterConvention(SemanticsCall);
  if (Convention != ParameterConvention::Indirect_Inout &&
      Convention != ParameterConvention::Direct_Owned &&
      Convention != ParameterConvention::Direct_Guaranteed)
    return false;

  ValueBase *SelfVal = Arr;
  auto *SelfBB = SelfVal->getParentBlock();
  if (DT->dominates(SelfBB, InsertBefore->getParent()))
    return true;

  if (auto *Copy = dyn_cast<CopyValueInst>(SelfVal)) {
    // look through one level
    SelfVal = Copy->getOperand();
  }
  if (auto LI = dyn_cast<LoadInst>(SelfVal)) {
    // Are we loading a value from an address in a struct defined at a point
    // dominating the hoist point.
    auto Val = LI->getOperand();
    bool DoesNotDominate;
    StructElementAddrInst *SEI;
    while ((DoesNotDominate = !DT->dominates(Val->getParentBlock(),
                                             InsertBefore->getParent())) &&
           (SEI = dyn_cast<StructElementAddrInst>(Val)))
      Val = SEI->getOperand();
    return !DoesNotDominate;
  }

  return false;
}

bool swift::ArraySemanticsCall::canHoist(SILInstruction *InsertBefore,
                                         DominanceInfo *DT) const {
  auto Kind = getKind();
  switch (Kind) {
  default:
    break;

  case ArrayCallKind::kCheckIndex:
  case ArrayCallKind::kArrayPropsIsNativeTypeChecked:
  case ArrayCallKind::kGetElementAddress:
  case ArrayCallKind::kGetCount:
  case ArrayCallKind::kGetCapacity:
    return canHoistArrayArgument(SemanticsCall, getSelf(), InsertBefore, DT);

  case ArrayCallKind::kGetElement:
    // Not implemented yet.
    return false;

  case ArrayCallKind::kCheckSubscript:
    if (SILValue IsNativeArg = getArrayPropertyIsNativeTypeChecked()) {
      ArraySemanticsCall IsNative(IsNativeArg,
                                  "array.props.isNativeTypeChecked", true);
      if (!IsNative) {
        // Do we have a constant parameter?
        auto *SI = dyn_cast<StructInst>(IsNativeArg);
        if (!SI)
          return false;
        if (!isa<IntegerLiteralInst>(SI->getOperand(0)))
          return false;
      } else if (!IsNative.canHoist(InsertBefore, DT))
        // Otherwise, we must be able to hoist the function call.
        return false;
    }
    return canHoistArrayArgument(SemanticsCall, getSelf(), InsertBefore, DT);

  case ArrayCallKind::kMakeMutable:
  case ArrayCallKind::kEndMutation:
    return canHoistArrayArgument(SemanticsCall, getSelf(), InsertBefore, DT);
  } // End switch.

  return false;
}

/// Copy the array self value to the insert point.
static SILValue copySelfValue(SILValue ArrayStructValue,
                              SILInstruction *InsertBefore, DominanceInfo *DT) {
  auto *func = InsertBefore->getFunction();
  if (DT->dominates(ArrayStructValue->getParentBlock(),
                    InsertBefore->getParent())) {
    assert(!func->hasOwnership() ||
           ArrayStructValue->getOwnershipKind() == OwnershipKind::Owned ||
           ArrayStructValue->getOwnershipKind() == OwnershipKind::Guaranteed);
    return ArrayStructValue;
  }

  assert(!func->hasOwnership() ||
         ArrayStructValue->getOwnershipKind() == OwnershipKind::Owned);

  SILValue Val;
  if (auto *Load = dyn_cast<LoadInst>(ArrayStructValue)) {
    Val = Load->getOperand();
  } else {
    auto *Copy = cast<CopyValueInst>(ArrayStructValue);
    Val = cast<LoadInst>(Copy->getOperand())->getOperand();
  }
  auto *InsertPt = InsertBefore;
  while (!DT->dominates(Val->getParentBlock(), InsertBefore->getParent())) {
    auto *Inst = cast<StructElementAddrInst>(Val);
    Inst->moveBefore(InsertPt);
    Val = Inst->getOperand();
    InsertPt = Inst;
  }

  if (!ArrayStructValue->getFunction()->hasOwnership()) {
    return cast<LoadInst>(ArrayStructValue)->clone(InsertBefore);
  }
  if (auto *Load = dyn_cast<LoadInst>(ArrayStructValue)) {
    return Load->clone(InsertBefore);
  }
  auto *Copy = cast<CopyValueInst>(ArrayStructValue);
  auto Addr = cast<LoadInst>(Copy->getOperand())->getOperand();
  return SILBuilderWithScope(InsertPt).createLoad(InsertPt->getLoc(), Addr,
                                                  LoadOwnershipQualifier::Copy);
}

static ApplyInst *hoistOrCopyCall(ApplyInst *AI, SILInstruction *InsertBefore,
                                  bool LeaveOriginal, DominanceInfo *DT) {
  if (!LeaveOriginal) {
    AI->moveBefore(InsertBefore);
  } else {
    // Leave the original and 'hoist' a clone.
    AI = cast<ApplyInst>(AI->clone(InsertBefore));
  }
  placeFuncRef(AI, DT);
  return AI;
}


/// Hoist or copy the self argument of the semantics call.
/// Return the hoisted self argument.
static SILValue hoistOrCopySelf(ApplyInst *SemanticsCall,
                                SILInstruction *InsertBefore,
                                DominanceInfo *DT, bool LeaveOriginal) {

  auto SelfConvention = getSelfParameterConvention(SemanticsCall);

  assert((SelfConvention == ParameterConvention::Direct_Owned ||
          SelfConvention == ParameterConvention::Direct_Guaranteed) &&
         "Expect @owned or @guaranteed self");

  auto Self = SemanticsCall->getSelfArgument();
  bool IsOwnedSelf = SelfConvention == ParameterConvention::Direct_Owned;
  auto *Func = SemanticsCall->getFunction();

  // Emit matching release for owned self if we are moving the original call.
  if (!LeaveOriginal && IsOwnedSelf) {
    SILBuilderWithScope Builder(SemanticsCall);
    Builder.emitDestroyValueOperation(SemanticsCall->getLoc(), Self);
  }
  auto NewArrayStructValue = copySelfValue(Self, InsertBefore, DT);
  if (!Func->hasOwnership() && IsOwnedSelf) {
    // Retain the array.
    SILBuilderWithScope Builder(InsertBefore, SemanticsCall);
    Builder.createRetainValue(SemanticsCall->getLoc(), NewArrayStructValue,
                              Builder.getDefaultAtomicity());
  }

  return NewArrayStructValue;
}

ApplyInst *swift::ArraySemanticsCall::hoistOrCopy(SILInstruction *InsertBefore,
                                                  DominanceInfo *DT,
                                                  bool LeaveOriginal) {
  assert(canHoist(InsertBefore, DT) &&
         "Must be able to hoist the semantics call");

  auto Kind = getKind();
  switch (Kind) {
  case ArrayCallKind::kArrayPropsIsNativeTypeChecked:
  case ArrayCallKind::kGetCount:
  case ArrayCallKind::kGetCapacity: {
    assert(SemanticsCall->getNumArguments() == 1 &&
           "Expect 'self' parameter only");

    auto HoistedSelf =
        hoistOrCopySelf(SemanticsCall, InsertBefore, DT, LeaveOriginal);

    auto *Call =
        hoistOrCopyCall(SemanticsCall, InsertBefore, LeaveOriginal, DT);
    Call->setSelfArgument(HoistedSelf);
    return Call;
  }

  case ArrayCallKind::kCheckSubscript:
  case ArrayCallKind::kCheckIndex: {
    auto HoistedSelf =
        hoistOrCopySelf(SemanticsCall, InsertBefore, DT, LeaveOriginal);

    SILValue NewArrayProps;
    if (SILValue IsNativeArg = getArrayPropertyIsNativeTypeChecked()) {
      // Copy the array.props argument call.
      ArraySemanticsCall IsNative(IsNativeArg,
                                  "array.props.isNativeTypeChecked", true);
      if (!IsNative) {
        // Do we have a constant parameter?
        auto *SI = dyn_cast<StructInst>(IsNativeArg);
        assert(SI && isa<IntegerLiteralInst>(SI->getOperand(0)) &&
               "Must have a constant parameter or an array.props.isNative call "
               "as argument");
        SI->moveBefore(&*DT->findNearestCommonDominator(
                               InsertBefore->getParent(), SI->getParent())
                             ->begin());
        auto *IL = cast<IntegerLiteralInst>(SI->getOperand(0));
        IL->moveBefore(&*DT->findNearestCommonDominator(
                               InsertBefore->getParent(), IL->getParent())
                             ->begin());
      } else {
        NewArrayProps = IsNative.copyTo(InsertBefore, DT);
      }

      // Replace all uses of the check subscript call by a use of the empty
      // dependence. The check subscript call is no longer associated with
      // another operation.
      auto EmptyDep = SILBuilderWithScope(SemanticsCall)
                          .createStruct(SemanticsCall->getLoc(),
                                        SemanticsCall->getType(), {});
      SemanticsCall->replaceAllUsesWith(EmptyDep);
    }

    // Hoist the call.
    auto Call = hoistOrCopyCall(SemanticsCall, InsertBefore, LeaveOriginal, DT);
    Call->setSelfArgument(HoistedSelf);

    if (NewArrayProps) {
      // Set the array.props argument.
      Call->setArgument(1, NewArrayProps);
    }


    return Call;
  }

  case ArrayCallKind::kMakeMutable:
  case ArrayCallKind::kEndMutation: {
    // Hoist the call.
    auto Call = hoistOrCopyCall(SemanticsCall, InsertBefore, LeaveOriginal, DT);
    return Call;
  }

  default:
    llvm_unreachable("Don't know how to hoist this instruction");
    break;
  } // End switch.
}

void swift::ArraySemanticsCall::removeCall() {
  if (getSelfParameterConvention(SemanticsCall) ==
      ParameterConvention::Direct_Owned) {
    SILBuilderWithScope Builder(SemanticsCall);
    Builder.emitDestroyValueOperation(SemanticsCall->getLoc(), getSelf());
  }

  switch (getKind()) {
  default: break;
  case ArrayCallKind::kCheckSubscript:
    if (!SemanticsCall->getType().isVoid()){
      // Remove all uses with the empty tuple ().
      auto EmptyDep = SILBuilderWithScope(SemanticsCall)
                          .createStruct(SemanticsCall->getLoc(),
                                        SemanticsCall->getType(), {});
      SemanticsCall->replaceAllUsesWith(EmptyDep);
    }
    break;
  case ArrayCallKind::kGetElement: {
    // Remove the matching isNativeTypeChecked and check_subscript call.
    ArraySemanticsCall IsNative(getTypeCheckedArgument(),
                                "array.props.isNativeTypeChecked");
    ArraySemanticsCall SubscriptCheck(getSubscriptCheckArgument(),
                                      "array.check_subscript");
    if (SubscriptCheck)
      SubscriptCheck.removeCall();

    // array.isNativeTypeChecked might be shared among several get_element
    // calls. The last user should delete it.
    if (IsNative && getSingleNonDebugUser((ApplyInst *)IsNative) ==
                      SemanticsCall) {
      deleteAllDebugUses(IsNative);
      (*IsNative).replaceAllUsesWithUndef();
      IsNative.removeCall();
    }
  }
  break;
  }

  SemanticsCall->eraseFromParent();
  SemanticsCall = nullptr;
}

SILValue
swift::ArraySemanticsCall::getArrayPropertyIsNativeTypeChecked() const {
  switch (getKind()) {
    case ArrayCallKind::kCheckSubscript:
      if (SemanticsCall->getNumArguments() == 3)
        return SemanticsCall->getArgument(1);
      return SILValue();
    case ArrayCallKind::kGetElement:
      return getTypeCheckedArgument();
    default:
      return SILValue();
  }
}

bool swift::ArraySemanticsCall::doesNotChangeArray() const {
  switch (getKind()) {
    default: return false;
    case ArrayCallKind::kArrayPropsIsNativeTypeChecked:
    case ArrayCallKind::kCheckSubscript:
    case ArrayCallKind::kCheckIndex:
    case ArrayCallKind::kGetCount:
    case ArrayCallKind::kGetCapacity:
    case ArrayCallKind::kGetElement:
    case ArrayCallKind::kEndMutation:
      return true;
  }
}

bool swift::ArraySemanticsCall::mayHaveBridgedObjectElementType() const {
  assert(hasSelf() && "Need self parameter");

  auto Ty = getSelf()->getType();
  if (auto BGT = Ty.getAs<BoundGenericStructType>()) {
    // Check the array element type parameter.
    bool isClass = true;
    for (auto EltTy : BGT->getGenericArgs()) {
      if (EltTy->isBridgeableObjectType())
        return true;
      isClass = false;
    }
    return isClass;
  }
  return true;
}

bool swift::ArraySemanticsCall::canInlineEarly() const {
  switch (getKind()) {
    default:
      return false;
    case ArrayCallKind::kAppendContentsOf:
    case ArrayCallKind::kReserveCapacityForAppend:
    case ArrayCallKind::kAppendElement:
    case ArrayCallKind::kArrayUninitializedIntrinsic:
      // append(Element) calls other semantics functions. Therefore it's
      // important that it's inlined by the early inliner (which is before all
      // the array optimizations). Also, this semantics is only used to lookup
      // Array.append(Element), so inlining it does not prevent any other
      // optimization.
      //
      // Early inlining array.uninitialized_intrinsic semantic call helps in
      // stack promotion.
      return true;
  }
}

SILValue swift::ArraySemanticsCall::getInitializationCount() const {
  if (getKind() == ArrayCallKind::kArrayUninitialized) {
    // Can be either a call to _adoptStorage or _allocateUninitialized.
    // A call to _adoptStorage has the buffer as AnyObject as the first
    // argument. The count is the second argument.
    // A call to _allocateUninitialized has the count as first argument.
    SILValue Arg0 = SemanticsCall->getArgument(0);
    if (Arg0->getType().isExistentialType() ||
        Arg0->getType().hasReferenceSemantics())
      return SemanticsCall->getArgument(1);
    else return SemanticsCall->getArgument(0);
  }

  if (getKind() == ArrayCallKind::kArrayInit &&
      SemanticsCall->getNumArguments() == 3)
    // Repeated-value array initializer. Arguments are the value to
    // repeat, the count, and the value's type.
    return SemanticsCall->getArgument(1);

  return SILValue();
}

/// Given an array semantic call \c arrayCall, if it is an "array.uninitialized"
/// initializer, which returns a two-element tuple, return the element of the
/// tuple at \c tupleElementIndex. Return a null SILValue if the
/// array call is not an "array.uninitialized" initializer or if the extraction
/// of the result tuple fails.
static SILValue getArrayUninitializedInitResult(ArraySemanticsCall arrayCall,
                                                unsigned tupleElementIndex) {
  assert(tupleElementIndex <= 1 && "tupleElementIndex must be 0 or 1");
  ArrayCallKind arrayCallKind = arrayCall.getKind();
  if (arrayCallKind != ArrayCallKind::kArrayUninitialized &&
      arrayCallKind != ArrayCallKind::kArrayUninitializedIntrinsic)
    return SILValue();

  // In OSSA, the call result will be extracted through a destructure_tuple
  // instruction.
  ApplyInst *callInst = arrayCall;
  if (callInst->getFunction()->hasOwnership()) {
    Operand *singleUse = callInst->getSingleUse();
    if (!singleUse)
      return SILValue();
    if (DestructureTupleInst *destructTuple =
            dyn_cast<DestructureTupleInst>(singleUse->getUser())) {
      return destructTuple->getResult(tupleElementIndex);
    }
    return SILValue();
  }

  // In non-OSSA, look for a tuple_extract instruction of the call result with
  // the requested tupleElementIndex.
  TupleExtractInst *tupleExtractInst = nullptr;
  for (auto *op : callInst->getUses()) {
    auto *tupleElt = dyn_cast<TupleExtractInst>(op->getUser());
    if (!tupleElt)
      return SILValue();
    if (tupleElt->getFieldIndex() != tupleElementIndex)
      continue;
    tupleExtractInst = tupleElt;
    break;
  }
  return SILValue(tupleExtractInst);
}

SILValue swift::ArraySemanticsCall::getArrayValue() const {
  ArrayCallKind arrayCallKind = getKind();
  if (arrayCallKind == ArrayCallKind::kArrayInit
      || arrayCallKind == ArrayCallKind::kArrayInitEmpty) {
    return SILValue(SemanticsCall);
  }
  return getArrayUninitializedInitResult(*this, 0);
}

SILValue swift::ArraySemanticsCall::getArrayElementStoragePointer() const {
  return getArrayUninitializedInitResult(*this, 1);
}

bool swift::ArraySemanticsCall::replaceByValue(SILValue V) {
  assert(getKind() == ArrayCallKind::kGetElement &&
         "Must be a get_element call");
  // We only handle loadable types.
  if (!V->getType().isLoadable(*SemanticsCall->getFunction()))
   return false;

  if (!hasGetElementDirectResult())
    return false;

  // Expect a check_subscript call or the empty dependence.
  auto SubscriptCheck = getSubscriptCheckArgument();
  ArraySemanticsCall Check(SubscriptCheck, "array.check_subscript");
  auto *EmptyDep = dyn_cast<StructInst>(SubscriptCheck);
  if (!Check && (!EmptyDep || !EmptyDep->getElements().empty()))
    return false;

  // In OSSA, the InsertPt is after V's definition and not before SemanticsCall
  // Because we are creating copy_value in ossa, and the source may have been
  // taken previously. So our insert point for copy_value is immediately after
  // V, where we can be sure it is live.
  auto InsertPt = V->getFunction()->hasOwnership()
                      ? getInsertAfterPoint(V)
                      : SemanticsCall->getIterator();
  assert(InsertPt.has_value());

  SILValue CopiedVal = SILBuilderWithScope(InsertPt.value())
                           .emitCopyValueOperation(SemanticsCall->getLoc(), V);
  SemanticsCall->replaceAllUsesWith(CopiedVal);

  removeCall();
  return true;
}

bool swift::ArraySemanticsCall::replaceByAppendingValues(
    SILFunction *AppendFn, SILFunction *ReserveFn,
    const SmallVectorImpl<SILValue> &Vals, SubstitutionMap Subs) {
  assert(getKind() == ArrayCallKind::kAppendContentsOf &&
         "Must be an append_contentsOf call");
  assert(AppendFn && "Must provide an append SILFunction");

  auto *F = SemanticsCall->getFunction();

  // We only handle loadable types.
  if (any_of(Vals, [F](SILValue V) -> bool {
        return !V->getType().isLoadable(*F);
      }))
    return false;
  
  CanSILFunctionType AppendFnTy = AppendFn->getLoweredFunctionType();
  SILValue ArrRef = SemanticsCall->getArgument(1);
  SILBuilderWithScope Builder(SemanticsCall);
  auto Loc = SemanticsCall->getLoc();
  auto *FnRef = Builder.createFunctionRefFor(Loc, AppendFn);

  if (Vals.size() > 1) {
    // Create a call to reserveCapacityForAppend() to reserve space for multiple
    // elements.
    FunctionRefBaseInst *ReserveFnRef =
        Builder.createFunctionRefFor(Loc, ReserveFn);
    SILFunctionType *ReserveFnTy =
      ReserveFnRef->getType().castTo<SILFunctionType>();
    assert(ReserveFnTy->getNumParameters() == 2);
    StructType *IntType =
        ReserveFnTy->getParameters()[0]
            .getArgumentType(F->getModule(), ReserveFnTy,
                             Builder.getTypeExpansionContext())
            ->castTo<StructType>();
    StructDecl *IntDecl = IntType->getDecl();
    VarDecl *field = IntDecl->getStoredProperties()[0];
    SILType BuiltinIntTy =SILType::getPrimitiveObjectType(
                               field->getInterfaceType()->getCanonicalType());
    IntegerLiteralInst *CapacityLiteral =
      Builder.createIntegerLiteral(Loc, BuiltinIntTy, Vals.size());
    StructInst *Capacity = Builder.createStruct(Loc,
        SILType::getPrimitiveObjectType(CanType(IntType)), {CapacityLiteral});
    Builder.createApply(Loc, ReserveFnRef, Subs, {Capacity, ArrRef});
  }

  for (SILValue V : Vals) {
    auto SubTy = V->getType();
    auto &ValLowering = Builder.getTypeLowering(SubTy);
    // In OSSA, the InsertPt is after V's definition and not before
    // SemanticsCall. Because we are creating copy_value in ossa, and the source
    // may have been taken previously. So our insert point for copy_value is
    // immediately after V, where we can be sure it is live.
    auto InsertPt = F->hasOwnership() ? getInsertAfterPoint(V)
                                      : SemanticsCall->getIterator();
    assert(InsertPt.has_value());
    SILValue CopiedVal = SILBuilderWithScope(InsertPt.value())
                             .emitCopyValueOperation(V.getLoc(), V);
    auto *AllocStackInst = Builder.createAllocStack(Loc, SubTy);
    ValLowering.emitStoreOfCopy(Builder, Loc, CopiedVal, AllocStackInst,
                                IsInitialization_t::IsInitialization);

    SILValue Args[] = {AllocStackInst, ArrRef};
    Builder.createApply(Loc, FnRef, Subs, Args);
    Builder.createDeallocStack(Loc, AllocStackInst);
    if (!isConsumedParameter(AppendFnTy->getParameters()[0].getConvention())) {
      ValLowering.emitDestroyValue(Builder, Loc, CopiedVal);
    }
  }
  CanSILFunctionType AppendContentsOfFnTy =
      SemanticsCall->getReferencedFunctionOrNull()->getLoweredFunctionType();
  if (AppendContentsOfFnTy->getParameters()[0].getConvention() ==
        ParameterConvention::Direct_Owned) {
    SILValue SrcArray = SemanticsCall->getArgument(0);
    Builder.emitDestroyValueOperation(SemanticsCall->getLoc(), SrcArray);
  }

  removeCall();

  return true;
}

bool swift::ArraySemanticsCall::mapInitializationStores(
    llvm::DenseMap<uint64_t, StoreInst *> &ElementValueMap) {
  if (getKind() != ArrayCallKind::kArrayUninitialized &&
      getKind() != ArrayCallKind::kArrayUninitializedIntrinsic)
    return false;
  SILValue ElementBuffer = getArrayElementStoragePointer();
  if (!ElementBuffer)
    return false;

  // Match initialization stores into ElementBuffer. E.g.
  // %83 = struct_extract %element_buffer : $UnsafeMutablePointer<Int>
  // %84 = pointer_to_address %83 : $Builtin.RawPointer to strict $*Int
  // store %85 to %84 : $*Int
  // %87 = integer_literal $Builtin.Word, 1
  // %88 = index_addr %84 : $*Int, %87 : $Builtin.Word
  // store %some_value to %88 : $*Int

  // If this an ArrayUninitializedIntrinsic then the ElementBuffer is a
  // builtin.RawPointer. Otherwise, it is an UnsafeMutablePointer, which would
  // be struct-extracted to obtain a builtin.RawPointer.
  SILValue UnsafeMutablePointerExtract =
      (getKind() == ArrayCallKind::kArrayUninitialized)
          ? dyn_cast_or_null<StructExtractInst>(
                getSingleNonDebugUser(ElementBuffer))
          : ElementBuffer;
  if (!UnsafeMutablePointerExtract)
    return false;

  auto *PointerToAddress = dyn_cast_or_null<PointerToAddressInst>(
      getSingleNonDebugUser(UnsafeMutablePointerExtract));
  if (!PointerToAddress)
    return false;

  // Match the stores. We can have either a store directly to the address or
  // to an index_addr projection.
  for (auto *Op : PointerToAddress->getUses()) {
    auto *Inst = Op->getUser();

    // Store to the base.
    auto *SI = dyn_cast<StoreInst>(Inst);
    if (SI && SI->getDest() == PointerToAddress) {
      // We have already seen an entry for this index bail.
      if (ElementValueMap.count(0))
        return false;
      ElementValueMap[0] = SI;
      continue;
    } else if (SI)
      return false;

    // Store to an index_addr projection.
    auto *IndexAddr = dyn_cast<IndexAddrInst>(Inst);
    if (!IndexAddr)
      return false;
    SI = dyn_cast_or_null<StoreInst>(getSingleNonDebugUser(IndexAddr));
    if (!SI || SI->getDest() != IndexAddr)
      return false;
    auto *Index = dyn_cast<IntegerLiteralInst>(IndexAddr->getIndex());
    if (!Index)
      return false;
    auto IndexVal = Index->getValue();
    // Let's not blow up our map.
    if (IndexVal.getActiveBits() > 16)
      return false;
    // Already saw an entry.
    if (ElementValueMap.count(IndexVal.getZExtValue()))
      return false;

    ElementValueMap[IndexVal.getZExtValue()] = SI;
  }
  return !ElementValueMap.empty();
}
