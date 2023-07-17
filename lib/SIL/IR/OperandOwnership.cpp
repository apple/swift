//===--- OperandOwnership.cpp ---------------------------------------------===//
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

#include "swift/SIL/ApplySite.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILBuiltinVisitor.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/SILVisitor.h"

using namespace swift;

/// Return true if all OperandOwnership invariants hold.
bool swift::checkOperandOwnershipInvariants(const Operand *operand,
                                            SILModuleConventions *silConv) {
  OperandOwnership opOwnership = operand->getOperandOwnership(silConv);
  if (opOwnership == OperandOwnership::Borrow) {
    // Must be a valid BorrowingOperand.
    return bool(BorrowingOperand(const_cast<Operand *>(operand)));
  }
  return true;
}

//===----------------------------------------------------------------------===//
//                         OperandOwnershipClassifier
//===----------------------------------------------------------------------===//

namespace {

class OperandOwnershipClassifier
  : public SILInstructionVisitor<OperandOwnershipClassifier, OperandOwnership> {
  LLVM_ATTRIBUTE_UNUSED SILModule &mod;
  // Allow module conventions to be overridden while lowering between canonical
  // and lowered SIL stages.
  SILModuleConventions silConv;

  const Operand &op;

public:
  /// Create a new OperandOwnershipClassifier.
  ///
  /// In most cases, one should only pass in \p Op and \p BaseValue will be set
  /// to Op.get(). In cases where one is trying to verify subobjects, Op.get()
  /// should be the subobject and Value should be the parent object. An example
  /// of where one would want to do this is in the case of value projections
  /// like struct_extract.
  OperandOwnershipClassifier(SILModuleConventions silConv, const Operand &op)
      : mod(silConv.getModule()), silConv(silConv), op(op) {}

  SILValue getValue() const { return op.get(); }

  ValueOwnershipKind getOwnershipKind() const {
    return op.get()->getOwnershipKind();
  }

  unsigned getOperandIndex() const { return op.getOperandNumber(); }

  SILType getType() const { return op.get()->getType(); }

  bool compatibleWithOwnership(ValueOwnershipKind kind) const {
    return getOwnershipKind().isCompatibleWith(kind);
  }

  bool hasExactOwnership(ValueOwnershipKind kind) const {
    return getOwnershipKind() == kind;
  }

  OperandOwnership visitFullApply(FullApplySite apply);

// Create declarations for all instructions, so we get a warning at compile
// time if any instructions do not have an implementation.
#define INST(Id, Parent) OperandOwnership visit##Id(Id *);
#include "swift/SIL/SILNodes.def"
};

} // end anonymous namespace

/// Implementation for instructions that we should never visit since they are
/// not valid in ossa or do not have operands. Since we should never visit
/// these, we just assert.
#define SHOULD_NEVER_VISIT_INST(INST)                                          \
  OperandOwnership OperandOwnershipClassifier::visit##INST##Inst(              \
      INST##Inst *i) {                                                         \
    llvm::errs() << "Unhandled inst: " << *i;                                  \
    llvm::report_fatal_error(                                                  \
        "Visited instruction that should never be visited?!");                 \
  }
SHOULD_NEVER_VISIT_INST(AllocBox)
SHOULD_NEVER_VISIT_INST(AllocExistentialBox)
SHOULD_NEVER_VISIT_INST(AllocGlobal)
SHOULD_NEVER_VISIT_INST(AllocStack)
SHOULD_NEVER_VISIT_INST(AllocPack)
SHOULD_NEVER_VISIT_INST(AllocPackMetadata)
SHOULD_NEVER_VISIT_INST(PackLength)
SHOULD_NEVER_VISIT_INST(DifferentiabilityWitnessFunction)
SHOULD_NEVER_VISIT_INST(FloatLiteral)
SHOULD_NEVER_VISIT_INST(FunctionRef)
SHOULD_NEVER_VISIT_INST(DebugStep)
SHOULD_NEVER_VISIT_INST(DynamicFunctionRef)
SHOULD_NEVER_VISIT_INST(PreviousDynamicFunctionRef)
SHOULD_NEVER_VISIT_INST(GlobalAddr)
SHOULD_NEVER_VISIT_INST(GlobalValue)
SHOULD_NEVER_VISIT_INST(BaseAddrForOffset)
SHOULD_NEVER_VISIT_INST(HasSymbol)
SHOULD_NEVER_VISIT_INST(IntegerLiteral)
SHOULD_NEVER_VISIT_INST(Metatype)
SHOULD_NEVER_VISIT_INST(ObjCProtocol)
SHOULD_NEVER_VISIT_INST(RetainValue)
SHOULD_NEVER_VISIT_INST(RetainValueAddr)
SHOULD_NEVER_VISIT_INST(StringLiteral)
SHOULD_NEVER_VISIT_INST(StrongRetain)
SHOULD_NEVER_VISIT_INST(Unreachable)
SHOULD_NEVER_VISIT_INST(Unwind)
SHOULD_NEVER_VISIT_INST(ReleaseValue)
SHOULD_NEVER_VISIT_INST(ReleaseValueAddr)
SHOULD_NEVER_VISIT_INST(StrongRelease)
SHOULD_NEVER_VISIT_INST(GetAsyncContinuation)
SHOULD_NEVER_VISIT_INST(IncrementProfilerCounter)
SHOULD_NEVER_VISIT_INST(TestSpecification)
SHOULD_NEVER_VISIT_INST(ScalarPackIndex)

#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  SHOULD_NEVER_VISIT_INST(StrongRetain##Name)                                  \
  SHOULD_NEVER_VISIT_INST(Name##Retain)
#include "swift/AST/ReferenceStorage.def"
#undef SHOULD_NEVER_VISIT_INST

// FIXME: The assert should be moved to the verifier.
#define OPERAND_OWNERSHIP(OWNERSHIP, INST)                                     \
  OperandOwnership                                                             \
  OperandOwnershipClassifier::visit##INST##Inst(INST##Inst *i) {               \
    assert(i->getNumOperands() && "Expected to have non-zero operands");       \
    return OperandOwnership::OWNERSHIP;                                        \
  }

// Instructions that require trivial operands.
OPERAND_OWNERSHIP(TrivialUse, AwaitAsyncContinuation)
OPERAND_OWNERSHIP(TrivialUse, AddressToPointer)
OPERAND_OWNERSHIP(TrivialUse, AllocRef)        // with tail operand
OPERAND_OWNERSHIP(TrivialUse, AllocRefDynamic) // with tail operand
OPERAND_OWNERSHIP(TrivialUse, BeginAccess)
OPERAND_OWNERSHIP(TrivialUse, MoveOnlyWrapperToCopyableAddr)
OPERAND_OWNERSHIP(TrivialUse, CopyableToMoveOnlyWrapperAddr)
OPERAND_OWNERSHIP(TrivialUse, BeginUnpairedAccess)
OPERAND_OWNERSHIP(TrivialUse, BindMemory)
OPERAND_OWNERSHIP(TrivialUse, RebindMemory)
OPERAND_OWNERSHIP(TrivialUse, CheckedCastAddrBranch)
OPERAND_OWNERSHIP(TrivialUse, CondBranch)
OPERAND_OWNERSHIP(TrivialUse, CondFail)
OPERAND_OWNERSHIP(TrivialUse, CopyAddr)
OPERAND_OWNERSHIP(TrivialUse, ExplicitCopyAddr)
OPERAND_OWNERSHIP(TrivialUse, MarkUnresolvedMoveAddr)
OPERAND_OWNERSHIP(TrivialUse, DeallocStack)
OPERAND_OWNERSHIP(TrivialUse, DeallocPack)
OPERAND_OWNERSHIP(TrivialUse, DeallocPackMetadata)
OPERAND_OWNERSHIP(TrivialUse, DeinitExistentialAddr)
OPERAND_OWNERSHIP(TrivialUse, DestroyAddr)
OPERAND_OWNERSHIP(TrivialUse, EndAccess)
OPERAND_OWNERSHIP(TrivialUse, EndUnpairedAccess)
OPERAND_OWNERSHIP(TrivialUse, GetAsyncContinuationAddr)
OPERAND_OWNERSHIP(TrivialUse, IndexAddr)
OPERAND_OWNERSHIP(TrivialUse, IndexRawPointer)
OPERAND_OWNERSHIP(TrivialUse, InitBlockStorageHeader)
OPERAND_OWNERSHIP(TrivialUse, InitEnumDataAddr)
OPERAND_OWNERSHIP(TrivialUse, InitExistentialAddr)
OPERAND_OWNERSHIP(TrivialUse, InitExistentialMetatype)
OPERAND_OWNERSHIP(TrivialUse, InjectEnumAddr)
OPERAND_OWNERSHIP(TrivialUse, IsUnique)
OPERAND_OWNERSHIP(TrivialUse, Load)
OPERAND_OWNERSHIP(TrivialUse, LoadBorrow)
OPERAND_OWNERSHIP(TrivialUse, MarkFunctionEscape)
OPERAND_OWNERSHIP(TrivialUse, ObjCExistentialMetatypeToObject)
OPERAND_OWNERSHIP(TrivialUse, ObjCMetatypeToObject)
OPERAND_OWNERSHIP(TrivialUse, ObjCToThickMetatype)
OPERAND_OWNERSHIP(TrivialUse, OpenExistentialAddr)
OPERAND_OWNERSHIP(TrivialUse, OpenExistentialMetatype)
OPERAND_OWNERSHIP(TrivialUse, OpenPackElement)
OPERAND_OWNERSHIP(TrivialUse, PointerToAddress)
OPERAND_OWNERSHIP(TrivialUse, ProjectBlockStorage)
OPERAND_OWNERSHIP(TrivialUse, RawPointerToRef)
OPERAND_OWNERSHIP(TrivialUse, SelectEnumAddr)
OPERAND_OWNERSHIP(TrivialUse, StructElementAddr)
OPERAND_OWNERSHIP(TrivialUse, SwitchEnumAddr)
OPERAND_OWNERSHIP(TrivialUse, SwitchValue)
OPERAND_OWNERSHIP(TrivialUse, TailAddr)
OPERAND_OWNERSHIP(TrivialUse, ThickToObjCMetatype)
OPERAND_OWNERSHIP(TrivialUse, ThinToThickFunction)
OPERAND_OWNERSHIP(TrivialUse, TupleElementAddr)
OPERAND_OWNERSHIP(TrivialUse, UncheckedAddrCast)
OPERAND_OWNERSHIP(TrivialUse, UncheckedRefCastAddr)
OPERAND_OWNERSHIP(TrivialUse, UncheckedTakeEnumDataAddr)
OPERAND_OWNERSHIP(TrivialUse, UnconditionalCheckedCastAddr)
OPERAND_OWNERSHIP(TrivialUse, DynamicPackIndex)
OPERAND_OWNERSHIP(TrivialUse, PackPackIndex)
OPERAND_OWNERSHIP(TrivialUse, PackElementGet)
OPERAND_OWNERSHIP(TrivialUse, PackElementSet)
OPERAND_OWNERSHIP(TrivialUse, TuplePackElementAddr)

// The dealloc_stack_ref operand needs to have NonUse ownership because
// this use comes after the last consuming use (which is usually a dealloc_ref).
OPERAND_OWNERSHIP(NonUse, DeallocStackRef)

// Use an owned or guaranteed value only for the duration of the operation.
OPERAND_OWNERSHIP(InstantaneousUse, ExistentialMetatype)
OPERAND_OWNERSHIP(InstantaneousUse, FixLifetime)
OPERAND_OWNERSHIP(InstantaneousUse, WitnessMethod)
OPERAND_OWNERSHIP(InstantaneousUse, DynamicMethodBranch)
OPERAND_OWNERSHIP(InstantaneousUse, ValueMetatype)
OPERAND_OWNERSHIP(InstantaneousUse, IsEscapingClosure)
OPERAND_OWNERSHIP(InstantaneousUse, ClassMethod)
OPERAND_OWNERSHIP(InstantaneousUse, SuperMethod)
OPERAND_OWNERSHIP(InstantaneousUse, ClassifyBridgeObject)
OPERAND_OWNERSHIP(InstantaneousUse, SetDeallocating)
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  OPERAND_OWNERSHIP(InstantaneousUse, StrongCopy##Name##Value)
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  OPERAND_OWNERSHIP(InstantaneousUse, StrongCopy##Name##Value)
#include "swift/AST/ReferenceStorage.def"

// Unowned uses ignore the value's ownership
OPERAND_OWNERSHIP(UnownedInstantaneousUse, DebugValue)
OPERAND_OWNERSHIP(UnownedInstantaneousUse, CopyBlock)
OPERAND_OWNERSHIP(UnownedInstantaneousUse, CopyValue)
OPERAND_OWNERSHIP(UnownedInstantaneousUse, ExplicitCopyValue)
OPERAND_OWNERSHIP(UnownedInstantaneousUse, ObjCMethod)
OPERAND_OWNERSHIP(UnownedInstantaneousUse, ObjCSuperMethod)
OPERAND_OWNERSHIP(UnownedInstantaneousUse, UnmanagedRetainValue)
OPERAND_OWNERSHIP(UnownedInstantaneousUse, UnmanagedReleaseValue)
OPERAND_OWNERSHIP(UnownedInstantaneousUse, UnmanagedAutoreleaseValue)

// These act as a form of conversion that does not imply ownership. Thus from an
// operand perspective we treat them as a pointer escape and from a value
// perspective, they return a value with OwnershipKind::Unowned.
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  OPERAND_OWNERSHIP(PointerEscape, RefTo##Name)                                \
  OPERAND_OWNERSHIP(PointerEscape, Name##ToRef)
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  OPERAND_OWNERSHIP(PointerEscape, RefTo##Name)
#include "swift/AST/ReferenceStorage.def"

// Instructions that currently violate structural ownership requirements,
// and therefore completely defeat canonicalization and optimization of any
// OSSA value that they use.
OPERAND_OWNERSHIP(PointerEscape, ProjectBox) // The result is a T*.
OPERAND_OWNERSHIP(PointerEscape, ProjectExistentialBox)
OPERAND_OWNERSHIP(PointerEscape, UncheckedOwnershipConversion)
OPERAND_OWNERSHIP(PointerEscape, ConvertEscapeToNoEscape)

// UncheckedBitwiseCast ownership behaves like RefToUnowned. It produces an
// Unowned value from a non-trivial value, without consuming or borrowing the
// non-trivial value. Unlike RefToUnowned, a bitwise cast works on a compound
// value and may truncate the value. The resulting value is still Unowned and
// should be immediately copied to produce an owned value. These happen for two
// reasons:
//
// (1) A Builtin.reinterpretCast is used on a nontrivial type. If the result is
// non-trivial, then, as part of emitting the cast, SILGen emits a copy_value
// immediately after the unchecked_bitwise_cast for all uses of the cast.
//
// (2) SILGen emits special conversions using SILGenBuilder's
// createUncheckedBitCast utility. For non-trivial types, this emits an
// unchecked_bitwise_cast immediately followed by a copy.
//
// The only thing protecting the lifetime of the Unowned value is the cast
// operand's PointerEscape ownership, which prevents OSSA analysis and blocks
// most optimization of the incoming value.
//
// TODO: Verify that Unowned values are only used by copies and that the cast
// operand's lifetime exceeds the copies.
OPERAND_OWNERSHIP(PointerEscape, UncheckedBitwiseCast)

// Instructions that escape reference bits with unenforced lifetime.
// TODO: verify that BitwiseEscape results always have a trivial type.
OPERAND_OWNERSHIP(BitwiseEscape, ValueToBridgeObject)
OPERAND_OWNERSHIP(BitwiseEscape, RefToRawPointer)
OPERAND_OWNERSHIP(BitwiseEscape, UncheckedTrivialBitCast)
OPERAND_OWNERSHIP(BitwiseEscape, BridgeObjectToWord)

// Instructions that end the lifetime of an owned value.
OPERAND_OWNERSHIP(DestroyingConsume, AutoreleaseValue)
OPERAND_OWNERSHIP(DestroyingConsume, DeallocBox)
OPERAND_OWNERSHIP(DestroyingConsume, DeallocExistentialBox)
OPERAND_OWNERSHIP(DestroyingConsume, DeallocRef)
OPERAND_OWNERSHIP(DestroyingConsume, DestroyValue)
OPERAND_OWNERSHIP(DestroyingConsume, EndLifetime)
OPERAND_OWNERSHIP(DestroyingConsume, BeginCOWMutation)
OPERAND_OWNERSHIP(DestroyingConsume, EndCOWMutation)
// The move_value instruction creates a distinct lifetime.
OPERAND_OWNERSHIP(DestroyingConsume, MoveValue)

// Instructions that move an owned value.
OPERAND_OWNERSHIP(ForwardingConsume, InitExistentialValue)
OPERAND_OWNERSHIP(ForwardingConsume, DeinitExistentialValue)
OPERAND_OWNERSHIP(ForwardingConsume, MarkUninitialized)
OPERAND_OWNERSHIP(ForwardingConsume, Throw)

// Instructions that expose a pointer within a borrow scope.
OPERAND_OWNERSHIP(InteriorPointer, RefElementAddr)
OPERAND_OWNERSHIP(InteriorPointer, RefTailAddr)
OPERAND_OWNERSHIP(InteriorPointer, OpenExistentialBox)
OPERAND_OWNERSHIP(InstantaneousUse, HopToExecutor)
OPERAND_OWNERSHIP(PointerEscape, ExtractExecutor)

// Instructions that propagate a value within a borrow scope.
OPERAND_OWNERSHIP(GuaranteedForwarding, TupleExtract)
OPERAND_OWNERSHIP(GuaranteedForwarding, StructExtract)
OPERAND_OWNERSHIP(GuaranteedForwarding, DifferentiableFunctionExtract)
OPERAND_OWNERSHIP(GuaranteedForwarding, LinearFunctionExtract)
// FIXME: OpenExistential[Box]Value should be able to take owned values too by
// using getForwardingOperandOwnership.
OPERAND_OWNERSHIP(GuaranteedForwarding, OpenExistentialValue)
OPERAND_OWNERSHIP(GuaranteedForwarding, OpenExistentialBoxValue)

OPERAND_OWNERSHIP(EndBorrow, EndBorrow)

// The begin_apply token represents the borrow scope of all owned and guaranteed
// call arguments. Although SILToken is (currently) trivially typed, it must
// have guaranteed ownership so end_apply and abort_apply will be recognized
// as lifetime-ending uses.
OPERAND_OWNERSHIP(EndBorrow, EndApply)
OPERAND_OWNERSHIP(EndBorrow, AbortApply)

#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                          \
  OPERAND_OWNERSHIP(TrivialUse, Load##Name)
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                         \
  OPERAND_OWNERSHIP(DestroyingConsume, Name##Release)
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                      \
  NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, "...")                              \
  ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, "...")
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  OPERAND_OWNERSHIP(TrivialUse, Name##ToRef)
#include "swift/AST/ReferenceStorage.def"

#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                          \
  OPERAND_OWNERSHIP(InstantaneousUse, Store##Name)
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                      \
  NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, "...")
#include "swift/AST/ReferenceStorage.def"

#undef OPERAND_OWNERSHIP

// Forwarding operations are conditionally either ForwardingConsumes or
// GuaranteedForwarding, depending on the instruction's constant ownership
// attribute.
#define FORWARDING_OWNERSHIP(INST)                                             \
  OperandOwnership OperandOwnershipClassifier::visit##INST##Inst(              \
      INST##Inst *i) {                                                         \
    return i->getForwardingOwnershipKind().getForwardingOperandOwnership(      \
        /*allowUnowned*/ false);                                               \
  }
FORWARDING_OWNERSHIP(Object)
FORWARDING_OWNERSHIP(OpenExistentialRef)
FORWARDING_OWNERSHIP(ConvertFunction)
FORWARDING_OWNERSHIP(RefToBridgeObject)
FORWARDING_OWNERSHIP(BridgeObjectToRef)
FORWARDING_OWNERSHIP(UnconditionalCheckedCast)
FORWARDING_OWNERSHIP(InitExistentialRef)
FORWARDING_OWNERSHIP(DifferentiableFunction)
FORWARDING_OWNERSHIP(LinearFunction)
FORWARDING_OWNERSHIP(MarkMustCheck)
FORWARDING_OWNERSHIP(MarkUnresolvedReferenceBinding)
FORWARDING_OWNERSHIP(MoveOnlyWrapperToCopyableValue)
FORWARDING_OWNERSHIP(CopyableToMoveOnlyWrapperValue)
FORWARDING_OWNERSHIP(MoveOnlyWrapperToCopyableBox)
#undef FORWARDING_OWNERSHIP

// Arbitrary value casts are forwarding instructions that are also allowed to
// propagate Unowned values.
#define FORWARDING_ANY_OWNERSHIP(INST)                                         \
  OperandOwnership OperandOwnershipClassifier::visit##INST##Inst(              \
      INST##Inst *i) {                                                         \
    return i->getForwardingOwnershipKind().getForwardingOperandOwnership(      \
        /*allowUnowned*/ true);                                                \
  }
FORWARDING_ANY_OWNERSHIP(Upcast)
FORWARDING_ANY_OWNERSHIP(UncheckedRefCast)
FORWARDING_ANY_OWNERSHIP(UncheckedValueCast)
FORWARDING_ANY_OWNERSHIP(CheckedCastBranch)
#undef FORWARDING_ANY_OWNERSHIP

// Any valid ownership kind can be combined with values of None ownership, but
// they cannot be combined with each other. An aggregates result ownership is
// the meet of its operands' ownership. A destructured member has the same
// ownership as its aggregate unless its type gives it None ownership.
//
// TODO: Aggregate operations should be Reborrows, not GuaranteedForwarding,
// because the borrowed value is different on either side of the operation and
// the lifetimes of borrowed members could differ.
#define AGGREGATE_OWNERSHIP(INST)                                              \
  OperandOwnership OperandOwnershipClassifier::visit##INST##Inst(              \
      INST##Inst *i) {                                                         \
    return i->getForwardingOwnershipKind().getForwardingOperandOwnership(      \
        /*allowUnowned*/ true);                                                \
  }
AGGREGATE_OWNERSHIP(Tuple)
AGGREGATE_OWNERSHIP(Struct)
AGGREGATE_OWNERSHIP(DestructureStruct)
AGGREGATE_OWNERSHIP(DestructureTuple)
AGGREGATE_OWNERSHIP(Enum)
AGGREGATE_OWNERSHIP(UncheckedEnumData)
AGGREGATE_OWNERSHIP(SwitchEnum)
#undef AGGREGATE_OWNERSHIP

// A begin_borrow is conditionally nested.
OperandOwnership
OperandOwnershipClassifier::visitBeginBorrowInst(BeginBorrowInst *borrow) {
  return OperandOwnership::Borrow;
}

// MARK: Instructions whose use ownership depends on the operand in question.

OperandOwnership OperandOwnershipClassifier::
visitDeallocPartialRefInst(DeallocPartialRefInst *i) {
  if (getValue() == i->getInstance()) {
    return OperandOwnership::DestroyingConsume;
  }
  return OperandOwnership::TrivialUse;
}

OperandOwnership
OperandOwnershipClassifier::visitSelectEnumInst(SelectEnumInst *i) {
  if (getValue() == i->getEnumOperand()) {
    return OperandOwnership::InstantaneousUse;
  }
  return getOwnershipKind().getForwardingOperandOwnership(
    /*allowUnowned*/true);
}

OperandOwnership OperandOwnershipClassifier::visitBranchInst(BranchInst *bi) {
  auto *destArg = bi->getDestBB()->getArgument(getOperandIndex());
  ValueOwnershipKind destBlockArgOwnershipKind = destArg->getOwnershipKind();

  if (destBlockArgOwnershipKind == OwnershipKind::Guaranteed) {
    return destArg->isGuaranteedForwarding()
               ? OperandOwnership::GuaranteedForwarding
               : OperandOwnership::Reborrow;
  }
  return destBlockArgOwnershipKind.getForwardingOperandOwnership(
    /*allowUnowned*/true);
}

OperandOwnership
OperandOwnershipClassifier::visitStoreBorrowInst(StoreBorrowInst *i) {
  if (getValue() == i->getSrc()) {
    return OperandOwnership::InteriorPointer;
  }
  return OperandOwnership::TrivialUse;
}

OperandOwnership
OperandOwnershipClassifier::visitDropDeinitInst(DropDeinitInst *i) {
  return i->getType().isAddress() ? OperandOwnership::TrivialUse
                                  : OperandOwnership::ForwardingConsume;
}

// Get the OperandOwnership for instantaneous apply, yield, and return uses.
// This does not apply to uses that begin an explicit borrow scope in the
// caller, such as begin_apply.
static OperandOwnership getFunctionArgOwnership(SILArgumentConvention argConv,
                                                bool hasScopeInCaller) {

  switch (argConv) {
  case SILArgumentConvention::Indirect_In:
  case SILArgumentConvention::Direct_Owned:
  case SILArgumentConvention::Pack_Owned:
    return OperandOwnership::ForwardingConsume;

  // A guaranteed argument is forwarded into the callee. If the call itself has
  // no scope (i.e. it is a single apply, try_apply or yield), then from the
  // caller's point of view it looks like an instantaneous use. Consequently,
  // owned values may be passed to guaranteed arguments without an explicit
  // borrow scope in the caller. In contrast, a begin_apply /does/ have an
  // explicit borrow scope in the caller so we must treat arguments passed to it
  // as being borrowed for the entire region of coroutine execution.
  case SILArgumentConvention::Indirect_In_Guaranteed:
  case SILArgumentConvention::Direct_Guaranteed:
  case SILArgumentConvention::Pack_Guaranteed:
  case SILArgumentConvention::Pack_Inout:
  case SILArgumentConvention::Pack_Out:
    // For an apply that begins a borrow scope, its arguments are borrowed
    // throughout the caller's borrow scope.
    return hasScopeInCaller ? OperandOwnership::Borrow
                            : OperandOwnership::InstantaneousUse;

  case SILArgumentConvention::Direct_Unowned:
    return OperandOwnership::UnownedInstantaneousUse;

  case SILArgumentConvention::Indirect_Out:
  case SILArgumentConvention::Indirect_Inout:
  case SILArgumentConvention::Indirect_InoutAliasable:
    llvm_unreachable("Illegal convention for non-address types");
  }
  llvm_unreachable("covered switch");
}

OperandOwnership
OperandOwnershipClassifier::visitFullApply(FullApplySite apply) {
  // Before considering conventions, filter all (trivial) indirect
  // arguments. This also rules out result arguments.
  if (getValue()->getType().isAddress()) {
    return OperandOwnership::TrivialUse;
  }
  auto calleeTy = apply.getSubstCalleeType();
  SILArgumentConvention argConv = [&]() {
    if (apply.isCalleeOperand(op)) {
      return SILArgumentConvention(calleeTy->getCalleeConvention());
    } else {
      unsigned calleeArgIdx = apply.getCalleeArgIndexOfFirstAppliedArg()
                              + apply.getAppliedArgIndex(op);
      return silConv.getFunctionConventions(calleeTy).getSILArgumentConvention(
          calleeArgIdx);
    }
  }();

  auto argOwnership = getFunctionArgOwnership(
    argConv, /*hasScopeInCaller*/ apply.beginsCoroutineEvaluation());

  // OSSA cleanup needs to handle each of these callee ownership cases.
  //
  // OperandOwnership::ForwardingConsume is only for thick @callee_owned.
  //
  // OperandOwnership::Borrow would only happen for a coroutine closure, which
  // isn't yet possible.
  if (apply.isCalleeOperand(op)) {
    assert((argOwnership == OperandOwnership::TrivialUse
            || argOwnership == OperandOwnership::UnownedInstantaneousUse
            || argOwnership == OperandOwnership::InstantaneousUse
            || argOwnership == OperandOwnership::ForwardingConsume
            || argOwnership == OperandOwnership::Borrow) &&
           "unsupported callee ownership");
  }
  return argOwnership;
}

OperandOwnership
OperandOwnershipClassifier::visitBeginApplyInst(BeginApplyInst *i) {
  return visitFullApply(i);
}

OperandOwnership OperandOwnershipClassifier::visitApplyInst(ApplyInst *i) {
  return visitFullApply(i);
}

OperandOwnership
OperandOwnershipClassifier::visitTryApplyInst(TryApplyInst *i) {
  return visitFullApply(i);
}

OperandOwnership
OperandOwnershipClassifier::visitPartialApplyInst(PartialApplyInst *i) {
  // partial_apply [stack] borrows its operands.
  if (i->isOnStack()) {
    auto operandTy = getValue()->getType();
    // Trivial values we can treat as trivial uses.
    if (operandTy.isTrivial(*i->getFunction())) {
      return OperandOwnership::TrivialUse;
    }
    
    // Borrowing of address operands is ultimately handled by the move-only
    // address checker and/or exclusivity checker rather than by value ownership.
    if (operandTy.isAddress()) {
      return OperandOwnership::TrivialUse;
    }
  
    return OperandOwnership::Borrow;
  }
  // All non-trivial types should be captured.
  return OperandOwnership::ForwardingConsume;
}

OperandOwnership OperandOwnershipClassifier::visitYieldInst(YieldInst *i) {
  // Before considering conventions, filter all indirect arguments.
  if (getValue()->getType().isAddress()) {
    return OperandOwnership::TrivialUse;
  }
  auto fnType = i->getFunction()->getLoweredFunctionType();
  SILArgumentConvention argConv(
    fnType->getYields()[getOperandIndex()].getConvention());
  return getFunctionArgOwnership(argConv, /*hasScopeInCaller*/ false);
}

OperandOwnership OperandOwnershipClassifier::visitReturnInst(ReturnInst *i) {
  switch (i->getOwnershipKind()) {
  case OwnershipKind::Any:
  case OwnershipKind::Guaranteed:
    llvm_unreachable("invalid value ownership");
  case OwnershipKind::None:
    return OperandOwnership::TrivialUse;
  case OwnershipKind::Unowned:
    return OperandOwnership::UnownedInstantaneousUse;
  case OwnershipKind::Owned:
    return OperandOwnership::ForwardingConsume;
  }
  llvm_unreachable("covered switch");
}

OperandOwnership OperandOwnershipClassifier::visitAssignInst(AssignInst *i) {
  if (getValue() != i->getSrc()) {
    return OperandOwnership::TrivialUse;
  }
  return OperandOwnership::DestroyingConsume;
}

OperandOwnership
OperandOwnershipClassifier::visitAssignByWrapperInst(AssignByWrapperInst *i) {
  if (getValue() == i->getSrc()) {
    return OperandOwnership::DestroyingConsume;
  }
  if (getValue() == i->getDest()) {
    return OperandOwnership::TrivialUse;
  }
  return OperandOwnership::InstantaneousUse; // initializer/setter closure
}

OperandOwnership
OperandOwnershipClassifier::visitAssignOrInitInst(AssignOrInitInst *i) {
  if (getValue() == i->getSrc()) {
    return OperandOwnership::DestroyingConsume;
  }

  // initializer/setter closure
  return OperandOwnership::InstantaneousUse;
}

OperandOwnership OperandOwnershipClassifier::visitStoreInst(StoreInst *i) {
  if (getValue() != i->getSrc()) {
    return OperandOwnership::TrivialUse;
  }
  return OperandOwnership::DestroyingConsume;
}

OperandOwnership OperandOwnershipClassifier::visitCopyBlockWithoutEscapingInst(
    CopyBlockWithoutEscapingInst *i) {
  // Consumes the closure parameter.
  if (getValue() == i->getClosure()) {
    return OperandOwnership::ForwardingConsume;
  }
  return OperandOwnership::UnownedInstantaneousUse;
}

OperandOwnership
OperandOwnershipClassifier::visitMarkDependenceInst(MarkDependenceInst *mdi) {
  // If we are analyzing "the value", we forward ownership.
  if (getValue() == mdi->getValue()) {
    return getOwnershipKind().getForwardingOperandOwnership(
      /*allowUnowned*/true);
  }
  // FIXME: Add an end_dependence instruction so we can treat mark_dependence as
  // a borrow of the base (mark_dependence %base -> end_dependence is analogous
  // to a borrow scope).
  return OperandOwnership::PointerEscape;
}

OperandOwnership OperandOwnershipClassifier::visitKeyPathInst(KeyPathInst *I) {
  // KeyPath moves the value in memory out of address operands, but the
  // ownership checker doesn't reason about that yet.
  return OperandOwnership::ForwardingConsume;
}

//===----------------------------------------------------------------------===//
//                            Builtin Use Checker
//===----------------------------------------------------------------------===//

namespace {

struct OperandOwnershipBuiltinClassifier
    : SILBuiltinVisitor<OperandOwnershipBuiltinClassifier, OperandOwnership> {
  using Map = OperandOwnership;
      
  const Operand &op;
  OperandOwnershipBuiltinClassifier(const Operand &op) : op(op) {}

  OperandOwnership visitLLVMIntrinsic(BuiltinInst *bi, llvm::Intrinsic::ID id) {
    // LLVM intrinsics do not traffic in ownership, so if we have a result, it
    // must be trivial.
    return OperandOwnership::TrivialUse;
  }

  // BUILTIN_TYPE_CHECKER_OPERATION does not live past the type checker.
#define BUILTIN_TYPE_CHECKER_OPERATION(ID, NAME)

#define BUILTIN(ID, NAME, ATTRS)                                               \
  OperandOwnership visit##ID(BuiltinInst *bi, StringRef attr);
#include "swift/AST/Builtins.def"

  OperandOwnership check(BuiltinInst *bi) { return visit(bi); }
};

} // end anonymous namespace

#define BUILTIN_OPERAND_OWNERSHIP(OWNERSHIP, ID)                               \
  OperandOwnership                                                             \
  OperandOwnershipBuiltinClassifier::visit##ID(BuiltinInst *, StringRef) {     \
    return OperandOwnership::OWNERSHIP;                                        \
  }
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ErrorInMain)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, UnexpectedError)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, WillThrow)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, AddressOfBorrowOpaque)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, UnprotectedAddressOfBorrowOpaque)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, AShr)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericAShr)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Add)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericAdd)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Alignof)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, AllocRaw)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, And)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericAnd)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, AssertConf)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, AssignCopyArrayNoAlias)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, AssignCopyArrayFrontToBack)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, AssignCopyArrayBackToFront)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, AssignTakeArray)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, AssumeAlignment)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, AssumeNonNegative)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, AssumeTrue)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, AtomicLoad)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, AtomicRMW)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, AtomicStore)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, BitCast)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, CanBeObjCClass)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, CondFailMessage)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, CmpXChg)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, CondUnreachable)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, CopyArray)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, DeallocRaw)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, DestroyArray)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ExactSDiv)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericExactSDiv)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ExactUDiv)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericExactUDiv)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ExtractElement)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FAdd)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericFAdd)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FCMP_OEQ)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FCMP_OGE)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FCMP_OGT)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FCMP_OLE)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FCMP_OLT)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FCMP_ONE)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FCMP_ORD)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FCMP_UEQ)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FCMP_UGE)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FCMP_UGT)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FCMP_ULE)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FCMP_ULT)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FCMP_UNE)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FCMP_UNO)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FDiv)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericFDiv)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FMul)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericFMul)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FNeg)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FPExt)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FPToSI)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FPToUI)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FPTrunc)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FRem)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericFRem)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, FSub)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericFSub)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Fence)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Ifdef)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GetObjCTypeEncoding)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ICMP_EQ)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ICMP_NE)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ICMP_SGE)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ICMP_SGT)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ICMP_SLE)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ICMP_SLT)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ICMP_UGE)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ICMP_UGT)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ICMP_ULE)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ICMP_ULT)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, InsertElement)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, IntToFPWithOverflow)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, BitWidth)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, IsNegative)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, WordAtIndex)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, IntToPtr)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, IsOptionalType)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, IsPOD)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, IsConcrete)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, IsBitwiseTakable)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, IsSameMetatype)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, LShr)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericLShr)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Mul)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericMul)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, OnFastPath)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Once)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, OnceWithContext)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Or)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericOr)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, PtrToInt)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, SAddOver)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, SDiv)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericSDiv)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, SExt)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, SExtOrBitCast)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, SIToFP)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, SMulOver)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, SRem)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericSRem)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, SSubOver)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, StackAlloc)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, UnprotectedStackAlloc)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, StackDealloc)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, SToSCheckedTrunc)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, SToUCheckedTrunc)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Expect)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Shl)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericShl)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ShuffleVector)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Sizeof)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, StaticReport)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Strideof)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, StringObjectOr)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Sub)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericSub)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, TakeArrayNoAlias)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, TakeArrayBackToFront)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, TakeArrayFrontToBack)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Trunc)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, TruncOrBitCast)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, TSanInoutAccess)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, UAddOver)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, UDiv)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericUDiv)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, UIToFP)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, UMulOver)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, URem)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericURem)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, USubOver)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, UToSCheckedTrunc)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, UToUCheckedTrunc)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Unreachable)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Xor)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GenericXor)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ZExt)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ZExtOrBitCast)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, ZeroInitializer)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, Swift3ImplicitObjCEntrypoint)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, PoundAssert)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, GlobalStringTablePointer)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, TypePtrAuthDiscriminator)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, TargetOSVersionAtLeast)
OperandOwnership OperandOwnershipBuiltinClassifier::visitCopy(BuiltinInst *bi,
                                                              StringRef) {
  if (bi->getFunction()->getConventions().useLoweredAddresses()) {
    return OperandOwnership::UnownedInstantaneousUse;
  } else {
    return OperandOwnership::DestroyingConsume;
  }
}
BUILTIN_OPERAND_OWNERSHIP(DestroyingConsume, StartAsyncLet)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, EndAsyncLet)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, EndAsyncLetLifetime)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, CreateTaskGroup)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, CreateTaskGroupWithFlags)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, DestroyTaskGroup)

BUILTIN_OPERAND_OWNERSHIP(ForwardingConsume, COWBufferForReading)

OperandOwnership
OperandOwnershipBuiltinClassifier
::visitStartAsyncLetWithLocalBuffer(BuiltinInst *bi, StringRef attr) {
  if (&op == &bi->getOperandRef(0)) {
    // The result buffer pointer is a trivial use.
    return OperandOwnership::TrivialUse;
  }
  
  // The closure is borrowed while the async let task is executing.
  return OperandOwnership::Borrow;
}

const int PARAMETER_INDEX_CREATE_ASYNC_TASK_FUTURE_FUNCTION = 2;
const int PARAMETER_INDEX_CREATE_ASYNC_TASK_GROUP_FUTURE_FUNCTION = 3;

OperandOwnership
OperandOwnershipBuiltinClassifier::visitCreateAsyncTask(BuiltinInst *bi,
                                                        StringRef attr) {
  // The function operand is consumed by the new task.
  if (&op == &bi->getOperandRef(PARAMETER_INDEX_CREATE_ASYNC_TASK_FUTURE_FUNCTION))
    return OperandOwnership::DestroyingConsume;

  return OperandOwnership::InstantaneousUse;
}

OperandOwnership
OperandOwnershipBuiltinClassifier::visitCreateAsyncTaskInGroup(BuiltinInst *bi,
                                                               StringRef attr) {
  // The function operand is consumed by the new task.
  if (&op == &bi->getOperandRef(PARAMETER_INDEX_CREATE_ASYNC_TASK_GROUP_FUTURE_FUNCTION))
    return OperandOwnership::DestroyingConsume;

  return OperandOwnership::InstantaneousUse;
}

OperandOwnership OperandOwnershipBuiltinClassifier::
visitResumeNonThrowingContinuationReturning(BuiltinInst *bi, StringRef attr) {
  // The value operand is consumed.
  if (&op == &bi->getOperandRef(1))
    return OperandOwnership::DestroyingConsume;

  return OperandOwnership::TrivialUse;
}

OperandOwnership OperandOwnershipBuiltinClassifier::
visitResumeThrowingContinuationReturning(BuiltinInst *bi, StringRef attr) {
  // The value operand is consumed.
  if (&op == &bi->getOperandRef(1))
    return OperandOwnership::DestroyingConsume;

  return OperandOwnership::TrivialUse;
}

OperandOwnership OperandOwnershipBuiltinClassifier::
visitResumeThrowingContinuationThrowing(BuiltinInst *bi, StringRef attr) {
  // The value operand is consumed.
  if (&op == &bi->getOperandRef(1))
    return OperandOwnership::DestroyingConsume;

  return OperandOwnership::TrivialUse;
}

BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, TaskRunInline)

BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, CancelAsyncTask)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, InitializeDefaultActor)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, DestroyDefaultActor)

BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse, InitializeDistributedRemoteActor)
BUILTIN_OPERAND_OWNERSHIP(InstantaneousUse,
                          InitializeNonDefaultDistributedActor)

BUILTIN_OPERAND_OWNERSHIP(PointerEscape, AutoDiffAllocateSubcontext)
BUILTIN_OPERAND_OWNERSHIP(PointerEscape, AutoDiffProjectTopLevelSubcontext)

// FIXME: ConvertTaskToJob is documented as taking NativePointer. It's operand's
// ownership should be 'TrivialUse'.
BUILTIN_OPERAND_OWNERSHIP(ForwardingConsume, ConvertTaskToJob)

BUILTIN_OPERAND_OWNERSHIP(BitwiseEscape, BuildOrdinarySerialExecutorRef)
BUILTIN_OPERAND_OWNERSHIP(BitwiseEscape, BuildComplexEqualitySerialExecutorRef)
BUILTIN_OPERAND_OWNERSHIP(BitwiseEscape, BuildDefaultActorExecutorRef)
BUILTIN_OPERAND_OWNERSHIP(BitwiseEscape, BuildMainActorExecutorRef)

BUILTIN_OPERAND_OWNERSHIP(TrivialUse, AutoDiffCreateLinearMapContext)

#undef BUILTIN_OPERAND_OWNERSHIP

#define SHOULD_NEVER_VISIT_BUILTIN(ID)                                         \
  OperandOwnership                                                             \
  OperandOwnershipBuiltinClassifier::visit##ID(BuiltinInst *, StringRef) {     \
    llvm_unreachable(                                                          \
        "Builtin should never be visited! E.x.: It may not have arguments");   \
  }
SHOULD_NEVER_VISIT_BUILTIN(GetCurrentAsyncTask)
SHOULD_NEVER_VISIT_BUILTIN(GetCurrentExecutor)
#undef SHOULD_NEVER_VISIT_BUILTIN

// Builtins that should be lowered to SIL instructions so we should never see
// them.
#define BUILTIN_SIL_OPERATION(ID, NAME, CATEGORY)                              \
  OperandOwnership                                                             \
  OperandOwnershipBuiltinClassifier::visit##ID(BuiltinInst *, StringRef) {     \
    llvm_unreachable("Builtin should have been lowered to SIL instruction?!"); \
  }
#define BUILTIN(X, Y, Z)
#include "swift/AST/Builtins.def"

OperandOwnership OperandOwnershipClassifier::visitBuiltinInst(BuiltinInst *bi) {
  return OperandOwnershipBuiltinClassifier(op).check(bi);
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

OperandOwnership
Operand::getOperandOwnership(SILModuleConventions *silConv) const {
  // A type-dependent operand is a NonUse (as opposed to say an
  // InstantaneousUse) because it does not require liveness.
  if (isTypeDependent())
    return OperandOwnership::NonUse;

  // If we do not have ownership enabled, just return any. This ensures that we
  // do not have any consuming uses and everything from an ownership perspective
  // is just a liveness use short-circuiting many of the optimizations.
  //
  // We do not ever call this function when an instruction isn't in a block.
  assert(getUser()->getParent() &&
         "Can not lookup ownership constraint unless inserted into block");
  if (auto *block = getUser()->getParent()) {
    auto *func = block->getParent();
    // If we don't have a function, then we must have a SILGlobalVariable. In
    // that case, we act as if we aren't in ownership.
    if (!func || !func->hasOwnership()) {
      return OperandOwnership(OperandOwnership::InstantaneousUse);
    }
  }
  SILModuleConventions overrideConv =
      silConv ? *silConv : SILModuleConventions(getUser()->getModule());
  OperandOwnershipClassifier classifier(overrideConv, *this);
  return classifier.visit(const_cast<SILInstruction *>(getUser()));
}
