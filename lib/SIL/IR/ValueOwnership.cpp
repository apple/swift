//===--- ValueOwnership.cpp -----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/SILVisitor.h"
#include "swift/SIL/SILBuiltinVisitor.h"
#include "swift/SIL/SILModule.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                                 Interface
//===----------------------------------------------------------------------===//

namespace {

class ValueOwnershipKindClassifier
    : public SILValueVisitor<ValueOwnershipKindClassifier, ValueOwnershipKind> {

public:
  ValueOwnershipKindClassifier() = default;
  ~ValueOwnershipKindClassifier() = default;
  ValueOwnershipKindClassifier(const ValueOwnershipKindClassifier &) = delete;
  ValueOwnershipKindClassifier(ValueOwnershipKindClassifier &&) = delete;

  ValueOwnershipKind visitForwardingInst(SILInstruction *I,
                                         ArrayRef<Operand> Ops);
  ValueOwnershipKind visitForwardingInst(SILInstruction *I) {
    return visitForwardingInst(I, I->getAllOperands());
  }

#define VALUE(Id, Parent) ValueOwnershipKind visit##Id(Id *ID);
#include "swift/SIL/SILNodes.def"
};

} // end anonymous namespace

#define CONSTANT_OWNERSHIP_INST(OWNERSHIP, INST)                               \
  ValueOwnershipKind ValueOwnershipKindClassifier::visit##INST##Inst(          \
      INST##Inst *Arg) {                                                       \
    return OwnershipKind::OWNERSHIP;                                           \
  }

#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  CONSTANT_OWNERSHIP_INST(Owned, Load##Name)
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                         \
  CONSTANT_OWNERSHIP_INST(Unowned, RefTo##Name)                                \
  CONSTANT_OWNERSHIP_INST(Unowned, Name##ToRef)                                \
  CONSTANT_OWNERSHIP_INST(Owned, StrongCopy##Name##Value)
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, "...") \
  ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, "...")
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  CONSTANT_OWNERSHIP_INST(None, RefTo##Name)                                   \
  CONSTANT_OWNERSHIP_INST(Unowned, Name##ToRef)                                \
  CONSTANT_OWNERSHIP_INST(Owned, StrongCopy##Name##Value)
#include "swift/AST/ReferenceStorage.def"

CONSTANT_OWNERSHIP_INST(Guaranteed, BeginBorrow)
CONSTANT_OWNERSHIP_INST(Guaranteed, LoadBorrow)
CONSTANT_OWNERSHIP_INST(Owned, AllocBox)
CONSTANT_OWNERSHIP_INST(Owned, AllocExistentialBox)
CONSTANT_OWNERSHIP_INST(Owned, AllocRef)
CONSTANT_OWNERSHIP_INST(Owned, AllocRefDynamic)
CONSTANT_OWNERSHIP_INST(Owned, CopyBlock)
CONSTANT_OWNERSHIP_INST(Owned, CopyBlockWithoutEscaping)
CONSTANT_OWNERSHIP_INST(Owned, CopyValue)
CONSTANT_OWNERSHIP_INST(Owned, ExplicitCopyValue)
CONSTANT_OWNERSHIP_INST(Owned, MoveValue)
CONSTANT_OWNERSHIP_INST(Owned, EndCOWMutation)
CONSTANT_OWNERSHIP_INST(Owned, KeyPath)
CONSTANT_OWNERSHIP_INST(Owned, InitExistentialValue)
CONSTANT_OWNERSHIP_INST(Owned, GlobalValue) // TODO: is this correct?

// One would think that these /should/ be unowned. In truth they are owned since
// objc metatypes do not go through the retain/release fast path. In their
// implementations of retain/release nothing happens, so this is safe.
//
// You could even have an optimization that just always removed retain/release
// operations on these objects.
CONSTANT_OWNERSHIP_INST(Owned, ObjCExistentialMetatypeToObject)
CONSTANT_OWNERSHIP_INST(Owned, ObjCMetatypeToObject)

// All addresses have trivial ownership. The values stored at the address may
// not though.
CONSTANT_OWNERSHIP_INST(None, AddressToPointer)
CONSTANT_OWNERSHIP_INST(None, AllocStack)
CONSTANT_OWNERSHIP_INST(None, AllocPack)
CONSTANT_OWNERSHIP_INST(None, AllocPackMetadata)
CONSTANT_OWNERSHIP_INST(None, PackLength)
CONSTANT_OWNERSHIP_INST(None, BeginAccess)
CONSTANT_OWNERSHIP_INST(None, MoveOnlyWrapperToCopyableAddr)
CONSTANT_OWNERSHIP_INST(None, CopyableToMoveOnlyWrapperAddr)
CONSTANT_OWNERSHIP_INST(None, BindMemory)
CONSTANT_OWNERSHIP_INST(None, RebindMemory)
CONSTANT_OWNERSHIP_INST(None, BridgeObjectToWord)
CONSTANT_OWNERSHIP_INST(None, ClassMethod)
CONSTANT_OWNERSHIP_INST(None, ClassifyBridgeObject)
CONSTANT_OWNERSHIP_INST(None, ObjCMethod)
CONSTANT_OWNERSHIP_INST(None, ExistentialMetatype)
CONSTANT_OWNERSHIP_INST(None, FloatLiteral)
CONSTANT_OWNERSHIP_INST(None, FunctionRef)
CONSTANT_OWNERSHIP_INST(None, DynamicFunctionRef)
CONSTANT_OWNERSHIP_INST(None, PreviousDynamicFunctionRef)
CONSTANT_OWNERSHIP_INST(None, GlobalAddr)
CONSTANT_OWNERSHIP_INST(None, BaseAddrForOffset)
CONSTANT_OWNERSHIP_INST(None, HasSymbol)
CONSTANT_OWNERSHIP_INST(None, IndexAddr)
CONSTANT_OWNERSHIP_INST(None, IndexRawPointer)
CONSTANT_OWNERSHIP_INST(None, InitEnumDataAddr)
CONSTANT_OWNERSHIP_INST(None, InitExistentialAddr)
CONSTANT_OWNERSHIP_INST(None, InitExistentialMetatype)
CONSTANT_OWNERSHIP_INST(None, IntegerLiteral)
CONSTANT_OWNERSHIP_INST(None, IsUnique)
CONSTANT_OWNERSHIP_INST(None, IsEscapingClosure)
CONSTANT_OWNERSHIP_INST(None, Metatype)
CONSTANT_OWNERSHIP_INST(None, ObjCToThickMetatype)
CONSTANT_OWNERSHIP_INST(None, OpenExistentialAddr)
CONSTANT_OWNERSHIP_INST(None, OpenExistentialBox)
CONSTANT_OWNERSHIP_INST(None, OpenExistentialMetatype)
CONSTANT_OWNERSHIP_INST(None, PointerToAddress)
CONSTANT_OWNERSHIP_INST(None, ProjectBlockStorage)
CONSTANT_OWNERSHIP_INST(None, ProjectBox)
CONSTANT_OWNERSHIP_INST(None, ProjectExistentialBox)
CONSTANT_OWNERSHIP_INST(None, RefElementAddr)
CONSTANT_OWNERSHIP_INST(None, RefTailAddr)
CONSTANT_OWNERSHIP_INST(None, RefToRawPointer)
CONSTANT_OWNERSHIP_INST(None, SelectEnumAddr)
CONSTANT_OWNERSHIP_INST(None, SelectValue)
CONSTANT_OWNERSHIP_INST(None, StringLiteral)
CONSTANT_OWNERSHIP_INST(None, StructElementAddr)
CONSTANT_OWNERSHIP_INST(None, SuperMethod)
CONSTANT_OWNERSHIP_INST(None, ObjCSuperMethod)
CONSTANT_OWNERSHIP_INST(None, TailAddr)
CONSTANT_OWNERSHIP_INST(None, ThickToObjCMetatype)
CONSTANT_OWNERSHIP_INST(None, TupleElementAddr)
CONSTANT_OWNERSHIP_INST(None, UncheckedAddrCast)
CONSTANT_OWNERSHIP_INST(None, UncheckedTakeEnumDataAddr)
CONSTANT_OWNERSHIP_INST(None, UncheckedTrivialBitCast)
CONSTANT_OWNERSHIP_INST(None, ValueMetatype)
CONSTANT_OWNERSHIP_INST(None, WitnessMethod)
CONSTANT_OWNERSHIP_INST(None, StoreBorrow)
CONSTANT_OWNERSHIP_INST(Owned, ConvertEscapeToNoEscape)
CONSTANT_OWNERSHIP_INST(Unowned, InitBlockStorageHeader)
CONSTANT_OWNERSHIP_INST(None, DifferentiabilityWitnessFunction)
// TODO: It would be great to get rid of these.
CONSTANT_OWNERSHIP_INST(Unowned, RawPointerToRef)
CONSTANT_OWNERSHIP_INST(Unowned, ObjCProtocol)
CONSTANT_OWNERSHIP_INST(Unowned, ValueToBridgeObject)
CONSTANT_OWNERSHIP_INST(None, GetAsyncContinuation)
CONSTANT_OWNERSHIP_INST(None, GetAsyncContinuationAddr)
CONSTANT_OWNERSHIP_INST(None, ThinToThickFunction)
CONSTANT_OWNERSHIP_INST(None, ExtractExecutor)
CONSTANT_OWNERSHIP_INST(None, OpenPackElement)
CONSTANT_OWNERSHIP_INST(None, DynamicPackIndex)
CONSTANT_OWNERSHIP_INST(None, PackPackIndex)
CONSTANT_OWNERSHIP_INST(None, ScalarPackIndex)
CONSTANT_OWNERSHIP_INST(None, PackElementGet)
CONSTANT_OWNERSHIP_INST(None, TuplePackElementAddr)

#undef CONSTANT_OWNERSHIP_INST

#define CONSTANT_OR_NONE_OWNERSHIP_INST(OWNERSHIP, INST)                       \
  ValueOwnershipKind ValueOwnershipKindClassifier::visit##INST##Inst(          \
      INST##Inst *I) {                                                         \
    if (I->getType().isTrivial(*I->getFunction()) ||                           \
        I->getType().isAddress()) {                                            \
      return OwnershipKind::None;                                              \
    }                                                                          \
    return OwnershipKind::OWNERSHIP;                                           \
  }
CONSTANT_OR_NONE_OWNERSHIP_INST(Guaranteed, StructExtract)
CONSTANT_OR_NONE_OWNERSHIP_INST(Guaranteed, TupleExtract)
CONSTANT_OR_NONE_OWNERSHIP_INST(Guaranteed, DifferentiableFunctionExtract)
CONSTANT_OR_NONE_OWNERSHIP_INST(Guaranteed, LinearFunctionExtract)
// OpenExistentialValue opens the boxed value inside an existential
// CoW box. The semantics of an existential CoW box implies that we
// can only consume the projected value inside the box if the box is
// unique. Since we do not know in general if the box is unique
// without additional work, in SIL we require opened archetypes to
// be borrowed sub-objects of the parent CoW box.
CONSTANT_OR_NONE_OWNERSHIP_INST(Guaranteed, OpenExistentialValue)
CONSTANT_OR_NONE_OWNERSHIP_INST(Guaranteed, OpenExistentialBoxValue)

// Given an owned value, mark_uninitialized always forwards an owned value since
// we want to make sure that all destroys of that value must come through the
// mark_uninitialized (which will happen due to mark_uninitialized consuming the
// value).
CONSTANT_OR_NONE_OWNERSHIP_INST(Owned, MarkUninitialized)

// unchecked_bitwise_cast is a bitwise copy. It produces a trivial or unowned
// result.
//
// If the operand is nontrivial and the result is trivial, then it is the
// programmer's responsibility to use Builtin.fixLifetime.
//
// If both the operand and the result are nontrivial, then either the types must
// be compatible so that TBAA doesn't allow the destroy to be hoisted above uses
// of the cast, or the programmer must use Builtin.fixLifetime.
//
// FIXME(https://github.com/apple/swift/issues/49723): Since we model this as unowned, then we must copy the value before use.
// This directly contradicts the semantics mentioned above since we will copy
// the value upon any use lest we use an unowned value in an owned or guaranteed
// way. So really all we will do here is perhaps add a copy slightly earlier
// unless the unowned value immediately is cast to something trivial. In such a
// case, we should be able to simplify the cast to just a trivial value and
// then eliminate the copy. That being said, we should investigate
// this since this is used in reinterpret_cast which is important from
// a performance perspective.
CONSTANT_OR_NONE_OWNERSHIP_INST(Unowned, UncheckedBitwiseCast)

#undef CONSTANT_OR_NONE_OWNERSHIP_INST

// For a forwarding instruction, we loop over all operands and make sure that
// all non-trivial values have the same ownership.
ValueOwnershipKind
ValueOwnershipKindClassifier::visitForwardingInst(SILInstruction *i,
                                                  ArrayRef<Operand> ops) {
  // A forwarding inst without operands must be trivial.
  if (ops.empty())
    return OwnershipKind::None;

  auto mergedValue = ValueOwnershipKind::merge(makeOptionalTransformRange(
      ops, [&i](const Operand &op) -> Optional<ValueOwnershipKind> {
        if (i->isTypeDependentOperand(op))
          return None;
        return op.get()->getOwnershipKind();
      }));

  if (!mergedValue) {
    // If we have mismatched SILOwnership and sil ownership is not enabled,
    // just return None for staging purposes. If SILOwnership is enabled, then
    // we must assert!
    if (!i->getModule().getOptions().VerifySILOwnership) {
      return OwnershipKind::None;
    }
    llvm_unreachable("Forwarding inst with mismatching ownership kinds?!");
  }

  return mergedValue;
}

#define FORWARDING_OWNERSHIP_INST(INST)                                        \
  ValueOwnershipKind ValueOwnershipKindClassifier::visit##INST##Inst(          \
      INST##Inst *I) {                                                         \
    return I->getType().isTrivial(*I->getFunction())                           \
               ? ValueOwnershipKind(OwnershipKind::None)                       \
               : I->getForwardingOwnershipKind();                              \
  }
FORWARDING_OWNERSHIP_INST(BridgeObjectToRef)
FORWARDING_OWNERSHIP_INST(ConvertFunction)
FORWARDING_OWNERSHIP_INST(OpenExistentialRef)
FORWARDING_OWNERSHIP_INST(RefToBridgeObject)
FORWARDING_OWNERSHIP_INST(Object)
FORWARDING_OWNERSHIP_INST(Struct)
FORWARDING_OWNERSHIP_INST(Tuple)
FORWARDING_OWNERSHIP_INST(UncheckedRefCast)
FORWARDING_OWNERSHIP_INST(UnconditionalCheckedCast)
FORWARDING_OWNERSHIP_INST(Upcast)
FORWARDING_OWNERSHIP_INST(UncheckedValueCast)
FORWARDING_OWNERSHIP_INST(UncheckedEnumData)
FORWARDING_OWNERSHIP_INST(SelectEnum)
FORWARDING_OWNERSHIP_INST(Enum)
FORWARDING_OWNERSHIP_INST(MarkDependence)
// NOTE: init_existential_ref from a reference counting perspective is not
// considered to be "owned" since it doesn't affect reference counts. That being
// said in the past, we wanted to conceptually treat it as an owned value that
// produces owned things, rather than a forwarding thing since initialization is
// generally a consuming operation. That being said, there are often cases in
// class based code where we are propagating around a plus zero version of a
// value and need to wrap the class in an existential wrapper in an intermediate
// frame from usage. In such cases, we have been creating unnecessary ref count
// traffic in code.
FORWARDING_OWNERSHIP_INST(InitExistentialRef)
FORWARDING_OWNERSHIP_INST(DifferentiableFunction)
FORWARDING_OWNERSHIP_INST(LinearFunction)
FORWARDING_OWNERSHIP_INST(MarkMustCheck)
FORWARDING_OWNERSHIP_INST(MarkUnresolvedReferenceBinding)
FORWARDING_OWNERSHIP_INST(MoveOnlyWrapperToCopyableValue)
FORWARDING_OWNERSHIP_INST(CopyableToMoveOnlyWrapperValue)
FORWARDING_OWNERSHIP_INST(MoveOnlyWrapperToCopyableBox)
#undef FORWARDING_OWNERSHIP_INST

ValueOwnershipKind
ValueOwnershipKindClassifier::visitUncheckedOwnershipConversionInst(
    UncheckedOwnershipConversionInst *I) {
  return I->getConversionOwnershipKind();
}

ValueOwnershipKind ValueOwnershipKindClassifier::visitSILUndef(SILUndef *arg) {
  return arg->getOwnershipKind();
}

ValueOwnershipKind ValueOwnershipKindClassifier::
visitPlaceholderValue(PlaceholderValue *v) {
  return OwnershipKind::None;
}

ValueOwnershipKind ValueOwnershipKindClassifier::
visitMultipleValueInstructionResult(MultipleValueInstructionResult *Result) {
  return Result->getOwnershipKind();
}

ValueOwnershipKind
ValueOwnershipKindClassifier::visitSILPhiArgument(SILPhiArgument *Arg) {
  return Arg->getOwnershipKind();
}

ValueOwnershipKind ValueOwnershipKindClassifier::visitSILFunctionArgument(
    SILFunctionArgument *Arg) {
  return Arg->getOwnershipKind();
}

ValueOwnershipKind ValueOwnershipKindClassifier::visitApplyInst(ApplyInst *ai) {
  auto *f = ai->getFunction();
  bool isTrivial = ai->getType().isTrivial(*f);
  // Quick is trivial check.
  if (isTrivial)
    return OwnershipKind::None;

  SILFunctionConventions fnConv(ai->getSubstCalleeType(), f->getModule());
  auto results = fnConv.getDirectSILResults();
  // No results => None.
  if (results.empty())
    return OwnershipKind::None;

  // Otherwise, map our results to their ownership kinds and then merge them!
  auto resultOwnershipKinds =
      makeTransformRange(results, [&](const SILResultInfo &info) {
        return info.getOwnershipKind(*f, ai->getSubstCalleeType());
      });
  auto mergedOwnershipKind = ValueOwnershipKind::merge(resultOwnershipKinds);
  if (!mergedOwnershipKind) {
    llvm_unreachable("Forwarding inst with mismatching ownership kinds?!");
  }

  return mergedOwnershipKind;
}

ValueOwnershipKind ValueOwnershipKindClassifier::visitLoadInst(LoadInst *LI) {
  switch (LI->getOwnershipQualifier()) {
  case LoadOwnershipQualifier::Take:
  case LoadOwnershipQualifier::Copy:
    return OwnershipKind::Owned;
  case LoadOwnershipQualifier::Unqualified:
  case LoadOwnershipQualifier::Trivial:
    return OwnershipKind::None;
  }

  llvm_unreachable("Unhandled LoadOwnershipQualifier in switch.");
}

ValueOwnershipKind ValueOwnershipKindClassifier::visitDropDeinitInst(DropDeinitInst *ddi) {
  return ddi->getType().isAddress() ? OwnershipKind::None : OwnershipKind::Owned;
}

ValueOwnershipKind
ValueOwnershipKindClassifier::visitPartialApplyInst(PartialApplyInst *PA) {
  // partial_apply instructions are modeled as creating an owned value during
  // OSSA, to track borrows of their captures, and so that they can themselves
  // be borrowed during calls, but they become trivial once ownership is
  // lowered away.
  if (PA->isOnStack() && !PA->getFunction()->hasOwnership())
    return OwnershipKind::None;
  return OwnershipKind::Owned;
}

//===----------------------------------------------------------------------===//
//                   Builtin OwnershipValueKind Computation
//===----------------------------------------------------------------------===//

namespace {

struct ValueOwnershipKindBuiltinVisitor
    : SILBuiltinVisitor<ValueOwnershipKindBuiltinVisitor, ValueOwnershipKind> {

  ValueOwnershipKind visitLLVMIntrinsic(BuiltinInst *BI,
                                        llvm::Intrinsic::ID ID) {
    // LLVM intrinsics do not traffic in ownership, so if we have a result, it
    // must be any.
    return OwnershipKind::None;
  }

#define BUILTIN(ID, NAME, ATTRS)                                               \
  ValueOwnershipKind visit##ID(BuiltinInst *BI, StringRef Attr);
#include "swift/AST/Builtins.def"
};

} // end anonymous namespace

#define CONSTANT_OWNERSHIP_BUILTIN(OWNERSHIP, ID)                              \
  ValueOwnershipKind ValueOwnershipKindBuiltinVisitor::visit##ID(              \
      BuiltinInst *BI, StringRef Attr) {                                       \
    return OwnershipKind::OWNERSHIP;                                           \
  }
// This returns a value at +1 that is destroyed strictly /after/ the
// UnsafeGuaranteedEnd. This provides the guarantee that we want.
CONSTANT_OWNERSHIP_BUILTIN(Owned, COWBufferForReading)
CONSTANT_OWNERSHIP_BUILTIN(None, AddressOfBorrowOpaque)
CONSTANT_OWNERSHIP_BUILTIN(None, UnprotectedAddressOfBorrowOpaque)
CONSTANT_OWNERSHIP_BUILTIN(None, AShr)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericAShr)
CONSTANT_OWNERSHIP_BUILTIN(None, Add)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericAdd)
CONSTANT_OWNERSHIP_BUILTIN(None, And)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericAnd)
CONSTANT_OWNERSHIP_BUILTIN(None, AssumeAlignment)
CONSTANT_OWNERSHIP_BUILTIN(None, AssumeNonNegative)
CONSTANT_OWNERSHIP_BUILTIN(None, AssumeTrue)
CONSTANT_OWNERSHIP_BUILTIN(None, BitCast)
CONSTANT_OWNERSHIP_BUILTIN(None, CondFailMessage)
CONSTANT_OWNERSHIP_BUILTIN(None, ExactSDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericExactSDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, ExactUDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericExactUDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, FAdd)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericFAdd)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_OEQ)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_OGE)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_OGT)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_OLE)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_OLT)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_ONE)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_UEQ)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_UGE)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_UGT)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_ULE)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_ULT)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_UNE)
CONSTANT_OWNERSHIP_BUILTIN(None, FDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericFDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, FMul)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericFMul)
CONSTANT_OWNERSHIP_BUILTIN(None, FNeg)
CONSTANT_OWNERSHIP_BUILTIN(None, FPExt)
CONSTANT_OWNERSHIP_BUILTIN(None, FPToSI)
CONSTANT_OWNERSHIP_BUILTIN(None, FPToUI)
CONSTANT_OWNERSHIP_BUILTIN(None, FPTrunc)
CONSTANT_OWNERSHIP_BUILTIN(None, FRem)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericFRem)
CONSTANT_OWNERSHIP_BUILTIN(None, FSub)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericFSub)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_EQ)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_NE)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_SGE)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_SGT)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_SLE)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_SLT)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_UGE)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_UGT)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_ULE)
CONSTANT_OWNERSHIP_BUILTIN(None, ICMP_ULT)
CONSTANT_OWNERSHIP_BUILTIN(None, IntToPtr)
CONSTANT_OWNERSHIP_BUILTIN(None, LShr)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericLShr)
CONSTANT_OWNERSHIP_BUILTIN(None, Mul)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericMul)
CONSTANT_OWNERSHIP_BUILTIN(None, Or)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericOr)
CONSTANT_OWNERSHIP_BUILTIN(None, PtrToInt)
CONSTANT_OWNERSHIP_BUILTIN(None, SAddOver)
CONSTANT_OWNERSHIP_BUILTIN(None, SDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericSDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, SExt)
CONSTANT_OWNERSHIP_BUILTIN(None, SExtOrBitCast)
CONSTANT_OWNERSHIP_BUILTIN(None, SIToFP)
CONSTANT_OWNERSHIP_BUILTIN(None, SMulOver)
CONSTANT_OWNERSHIP_BUILTIN(None, SRem)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericSRem)
CONSTANT_OWNERSHIP_BUILTIN(None, SSubOver)
CONSTANT_OWNERSHIP_BUILTIN(None, Expect)
CONSTANT_OWNERSHIP_BUILTIN(None, Shl)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericShl)
CONSTANT_OWNERSHIP_BUILTIN(None, Sub)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericSub)
CONSTANT_OWNERSHIP_BUILTIN(None, Trunc)
CONSTANT_OWNERSHIP_BUILTIN(None, TruncOrBitCast)
CONSTANT_OWNERSHIP_BUILTIN(None, UAddOver)
CONSTANT_OWNERSHIP_BUILTIN(None, UDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericUDiv)
CONSTANT_OWNERSHIP_BUILTIN(None, UIToFP)
CONSTANT_OWNERSHIP_BUILTIN(None, UMulOver)
CONSTANT_OWNERSHIP_BUILTIN(None, URem)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericURem)
CONSTANT_OWNERSHIP_BUILTIN(None, USubOver)
CONSTANT_OWNERSHIP_BUILTIN(None, Xor)
CONSTANT_OWNERSHIP_BUILTIN(None, GenericXor)
CONSTANT_OWNERSHIP_BUILTIN(None, ZExt)
CONSTANT_OWNERSHIP_BUILTIN(None, ZExtOrBitCast)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_ORD)
CONSTANT_OWNERSHIP_BUILTIN(None, FCMP_UNO)
CONSTANT_OWNERSHIP_BUILTIN(None, OnFastPath)
CONSTANT_OWNERSHIP_BUILTIN(None, IsOptionalType)
CONSTANT_OWNERSHIP_BUILTIN(None, Sizeof)
CONSTANT_OWNERSHIP_BUILTIN(None, Strideof)
CONSTANT_OWNERSHIP_BUILTIN(None, StringObjectOr)
CONSTANT_OWNERSHIP_BUILTIN(None, IsPOD)
CONSTANT_OWNERSHIP_BUILTIN(None, IsConcrete)
CONSTANT_OWNERSHIP_BUILTIN(None, IsBitwiseTakable)
CONSTANT_OWNERSHIP_BUILTIN(None, IsSameMetatype)
CONSTANT_OWNERSHIP_BUILTIN(None, Alignof)
CONSTANT_OWNERSHIP_BUILTIN(None, AllocRaw)
CONSTANT_OWNERSHIP_BUILTIN(None, AssertConf)
CONSTANT_OWNERSHIP_BUILTIN(None, UToSCheckedTrunc)
CONSTANT_OWNERSHIP_BUILTIN(None, StackAlloc)
CONSTANT_OWNERSHIP_BUILTIN(None, UnprotectedStackAlloc)
CONSTANT_OWNERSHIP_BUILTIN(None, StackDealloc)
CONSTANT_OWNERSHIP_BUILTIN(None, SToSCheckedTrunc)
CONSTANT_OWNERSHIP_BUILTIN(None, SToUCheckedTrunc)
CONSTANT_OWNERSHIP_BUILTIN(None, UToUCheckedTrunc)
CONSTANT_OWNERSHIP_BUILTIN(None, IntToFPWithOverflow)
CONSTANT_OWNERSHIP_BUILTIN(None, BitWidth)
CONSTANT_OWNERSHIP_BUILTIN(None, IsNegative)
CONSTANT_OWNERSHIP_BUILTIN(None, WordAtIndex)

// This is surprising, Builtin.unreachable returns a "Never" value which is
// trivially typed.
CONSTANT_OWNERSHIP_BUILTIN(None, Unreachable)

/// AtomicRMW has type (Builtin.RawPointer, T) -> T. But it provides overloads
/// for integer or rawpointer, so it should be trivial.
CONSTANT_OWNERSHIP_BUILTIN(None, AtomicRMW)

CONSTANT_OWNERSHIP_BUILTIN(None, CondUnreachable)
CONSTANT_OWNERSHIP_BUILTIN(None, GetObjCTypeEncoding)
CONSTANT_OWNERSHIP_BUILTIN(None, CanBeObjCClass)
CONSTANT_OWNERSHIP_BUILTIN(None, WillThrow)
CONSTANT_OWNERSHIP_BUILTIN(None, StaticReport)

CONSTANT_OWNERSHIP_BUILTIN(None, DestroyArray)
CONSTANT_OWNERSHIP_BUILTIN(None, CopyArray)
CONSTANT_OWNERSHIP_BUILTIN(None, TakeArrayNoAlias)
CONSTANT_OWNERSHIP_BUILTIN(None, TakeArrayFrontToBack)
CONSTANT_OWNERSHIP_BUILTIN(None, TakeArrayBackToFront)
CONSTANT_OWNERSHIP_BUILTIN(None, AssignCopyArrayNoAlias)
CONSTANT_OWNERSHIP_BUILTIN(None, AssignCopyArrayFrontToBack)
CONSTANT_OWNERSHIP_BUILTIN(None, AssignCopyArrayBackToFront)
CONSTANT_OWNERSHIP_BUILTIN(None, AssignTakeArray)
CONSTANT_OWNERSHIP_BUILTIN(None, UnexpectedError)
CONSTANT_OWNERSHIP_BUILTIN(None, ErrorInMain)
CONSTANT_OWNERSHIP_BUILTIN(None, DeallocRaw)
CONSTANT_OWNERSHIP_BUILTIN(None, Fence)
CONSTANT_OWNERSHIP_BUILTIN(None, Ifdef)
CONSTANT_OWNERSHIP_BUILTIN(None, AtomicStore)
CONSTANT_OWNERSHIP_BUILTIN(None, Once)
CONSTANT_OWNERSHIP_BUILTIN(None, OnceWithContext)
CONSTANT_OWNERSHIP_BUILTIN(None, TSanInoutAccess)
CONSTANT_OWNERSHIP_BUILTIN(None, Swift3ImplicitObjCEntrypoint)
CONSTANT_OWNERSHIP_BUILTIN(None, PoundAssert)
CONSTANT_OWNERSHIP_BUILTIN(None, TypePtrAuthDiscriminator)
CONSTANT_OWNERSHIP_BUILTIN(None, TargetOSVersionAtLeast)
CONSTANT_OWNERSHIP_BUILTIN(None, GlobalStringTablePointer)
CONSTANT_OWNERSHIP_BUILTIN(None, GetCurrentAsyncTask)
CONSTANT_OWNERSHIP_BUILTIN(None, CancelAsyncTask)
CONSTANT_OWNERSHIP_BUILTIN(Owned, CreateAsyncTask)
CONSTANT_OWNERSHIP_BUILTIN(Owned, CreateAsyncTaskInGroup)
CONSTANT_OWNERSHIP_BUILTIN(None, ConvertTaskToJob)
CONSTANT_OWNERSHIP_BUILTIN(None, InitializeDefaultActor)
CONSTANT_OWNERSHIP_BUILTIN(None, DestroyDefaultActor)
CONSTANT_OWNERSHIP_BUILTIN(None, InitializeDistributedRemoteActor)
CONSTANT_OWNERSHIP_BUILTIN(None, InitializeNonDefaultDistributedActor)
CONSTANT_OWNERSHIP_BUILTIN(Owned, AutoDiffCreateLinearMapContext)
CONSTANT_OWNERSHIP_BUILTIN(None, AutoDiffProjectTopLevelSubcontext)
CONSTANT_OWNERSHIP_BUILTIN(None, AutoDiffAllocateSubcontext)
CONSTANT_OWNERSHIP_BUILTIN(None, GetCurrentExecutor)
CONSTANT_OWNERSHIP_BUILTIN(None, ResumeNonThrowingContinuationReturning)
CONSTANT_OWNERSHIP_BUILTIN(None, ResumeThrowingContinuationReturning)
CONSTANT_OWNERSHIP_BUILTIN(None, ResumeThrowingContinuationThrowing)
CONSTANT_OWNERSHIP_BUILTIN(None, BuildOrdinarySerialExecutorRef)
CONSTANT_OWNERSHIP_BUILTIN(None, BuildComplexEqualitySerialExecutorRef)
CONSTANT_OWNERSHIP_BUILTIN(None, BuildDefaultActorExecutorRef)
CONSTANT_OWNERSHIP_BUILTIN(None, BuildMainActorExecutorRef)
CONSTANT_OWNERSHIP_BUILTIN(None, StartAsyncLet)
CONSTANT_OWNERSHIP_BUILTIN(None, EndAsyncLet)
CONSTANT_OWNERSHIP_BUILTIN(None, StartAsyncLetWithLocalBuffer)
CONSTANT_OWNERSHIP_BUILTIN(None, EndAsyncLetLifetime)
CONSTANT_OWNERSHIP_BUILTIN(None, CreateTaskGroup)
CONSTANT_OWNERSHIP_BUILTIN(None, CreateTaskGroupWithFlags)
CONSTANT_OWNERSHIP_BUILTIN(None, DestroyTaskGroup)
CONSTANT_OWNERSHIP_BUILTIN(None, TaskRunInline)
CONSTANT_OWNERSHIP_BUILTIN(None, Copy)

#undef CONSTANT_OWNERSHIP_BUILTIN

// Check all of these...
#define UNOWNED_OR_NONE_DEPENDING_ON_RESULT(ID)                                \
  ValueOwnershipKind ValueOwnershipKindBuiltinVisitor::visit##ID(              \
      BuiltinInst *BI, StringRef Attr) {                                       \
    if (BI->getType().isTrivial(*BI->getFunction())) {                         \
      return OwnershipKind::None;                                              \
    }                                                                          \
    return OwnershipKind::Unowned;                                             \
  }
UNOWNED_OR_NONE_DEPENDING_ON_RESULT(CmpXChg)
UNOWNED_OR_NONE_DEPENDING_ON_RESULT(AtomicLoad)
UNOWNED_OR_NONE_DEPENDING_ON_RESULT(ExtractElement)
UNOWNED_OR_NONE_DEPENDING_ON_RESULT(InsertElement)
UNOWNED_OR_NONE_DEPENDING_ON_RESULT(ShuffleVector)
UNOWNED_OR_NONE_DEPENDING_ON_RESULT(ZeroInitializer)
#undef UNOWNED_OR_NONE_DEPENDING_ON_RESULT

#define BUILTIN(X,Y,Z)
#define BUILTIN_SIL_OPERATION(ID, NAME, CATEGORY) \
  ValueOwnershipKind ValueOwnershipKindBuiltinVisitor::visit##ID( \
      BuiltinInst *BI, StringRef Attr) { \
    llvm_unreachable("builtin should have been lowered in SILGen"); \
  }
#include "swift/AST/Builtins.def"

ValueOwnershipKind
ValueOwnershipKindClassifier::visitBuiltinInst(BuiltinInst *BI) {
  // For now, just conservatively say builtins are None. We need to use a
  // builtin in here to guarantee correctness.
  return ValueOwnershipKindBuiltinVisitor().visit(BI);
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

ValueOwnershipKind ValueBase::getOwnershipKind() const {
  // If we do not have an undef, we should always be able to get to our function
  // here. If we do not have ownership enabled, just return none for everything
  // to short circuit ownership optimizations. Since SILUndef in either case
  // will be ValueOwnershipKind::None, we will not get any wonky behavior here.
  //
  // We assume that any time we are in SILBuilder and call this without having a
  // value in a block yet, ossa is enabled.
  if (auto *block = getParentBlock()) {
    auto *f = block->getParent();
    // If our block isn't in a function, then it must be in a global
    // variable. We don't verify ownership there so just return
    // OwnershipKind::None.
    if (!f)
      return OwnershipKind::None;

    // Now that we know that we do have a block/function, check if we have
    // ownership.
    if (!f->hasOwnership())
      return OwnershipKind::None;
  }

  ValueOwnershipKindClassifier Classifier;
  auto result = Classifier.visit(const_cast<ValueBase *>(this));
  assert(result && "Returned ownership kind invalid on values");
  return result;
}
