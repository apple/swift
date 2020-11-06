//===--- OperandOwnership.cpp ---------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
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

//===----------------------------------------------------------------------===//
//                      OperandOwnershipKindClassifier
//===----------------------------------------------------------------------===//

namespace {

class OperandOwnershipKindClassifier
    : public SILInstructionVisitor<OperandOwnershipKindClassifier,
                                   OperandOwnershipKindMap> {
public:
  using Map = OperandOwnershipKindMap;

private:
  LLVM_ATTRIBUTE_UNUSED SILModule &mod;

  const Operand &op;
  bool checkingSubObject;

public:
  /// Create a new OperandOwnershipKindClassifier.
  ///
  /// In most cases, one should only pass in \p Op and \p BaseValue will be set
  /// to Op.get(). In cases where one is trying to verify subobjects, Op.get()
  /// should be the subobject and Value should be the parent object. An example
  /// of where one would want to do this is in the case of value projections
  /// like struct_extract.
  OperandOwnershipKindClassifier(
      SILModule &mod, const Operand &op,
      bool checkingSubObject)
      : mod(mod), op(op),
        checkingSubObject(checkingSubObject) {}

  bool isCheckingSubObject() const { return checkingSubObject; }

  SILValue getValue() const { return op.get(); }

  ValueOwnershipKind getOwnershipKind() const {
    assert(getValue().getOwnershipKind() == op.get().getOwnershipKind() &&
           "Expected ownership kind of parent value and operand");
    return getValue().getOwnershipKind();
  }

  unsigned getOperandIndex() const { return op.getOperandNumber(); }

  SILType getType() const { return op.get()->getType(); }

  bool compatibleWithOwnership(ValueOwnershipKind kind) const {
    return getOwnershipKind().isCompatibleWith(kind);
  }

  bool hasExactOwnership(ValueOwnershipKind kind) const {
    return getOwnershipKind() == kind;
  }

  bool isAddressOrTrivialType() const {
    if (getType().isAddress())
      return true;
    return getOwnershipKind() == ValueOwnershipKind::None;
  }

  OperandOwnershipKindMap visitForwardingInst(SILInstruction *i,
                                              ArrayRef<Operand> ops);
  OperandOwnershipKindMap visitForwardingInst(SILInstruction *i) {
    return visitForwardingInst(i, i->getAllOperands());
  }

  OperandOwnershipKindMap
  visitApplyParameter(ValueOwnershipKind requiredConvention,
                      UseLifetimeConstraint requirement);
  OperandOwnershipKindMap visitFullApply(FullApplySite apply);

  OperandOwnershipKindMap visitCallee(CanSILFunctionType substCalleeType);
  OperandOwnershipKindMap
  checkTerminatorArgumentMatchesDestBB(SILBasicBlock *destBB, unsigned opIndex);

// Create declarations for all instructions, so we get a warning at compile
// time if any instructions do not have an implementation.
#define INST(Id, Parent) OperandOwnershipKindMap visit##Id(Id *);
#include "swift/SIL/SILNodes.def"
};

} // end anonymous namespace

/// Implementation for instructions that we should never visit since they are
/// not valid in ossa or do not have operands. Since we should never visit
/// these, we just assert.
#define SHOULD_NEVER_VISIT_INST(INST)                                          \
  OperandOwnershipKindMap OperandOwnershipKindClassifier::visit##INST##Inst(   \
      INST##Inst *i) {                                                         \
    llvm::errs() << "Unhandled inst: " << *i;                                  \
    llvm::report_fatal_error(                                                  \
        "Visited instruction that should never be visited?!");                 \
  }
SHOULD_NEVER_VISIT_INST(AllocBox)
SHOULD_NEVER_VISIT_INST(AllocExistentialBox)
SHOULD_NEVER_VISIT_INST(AllocGlobal)
SHOULD_NEVER_VISIT_INST(AllocStack)
SHOULD_NEVER_VISIT_INST(DifferentiabilityWitnessFunction)
SHOULD_NEVER_VISIT_INST(FloatLiteral)
SHOULD_NEVER_VISIT_INST(FunctionRef)
SHOULD_NEVER_VISIT_INST(DynamicFunctionRef)
SHOULD_NEVER_VISIT_INST(PreviousDynamicFunctionRef)
SHOULD_NEVER_VISIT_INST(GlobalAddr)
SHOULD_NEVER_VISIT_INST(GlobalValue)
SHOULD_NEVER_VISIT_INST(BaseAddrForOffset)
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

#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  SHOULD_NEVER_VISIT_INST(StrongRetain##Name)                                  \
  SHOULD_NEVER_VISIT_INST(Name##Retain)
#include "swift/AST/ReferenceStorage.def"
#undef SHOULD_NEVER_VISIT_INST

/// Instructions that are interior pointers into a guaranteed value.
#define INTERIOR_POINTER_PROJECTION(INST)                                      \
  OperandOwnershipKindMap OperandOwnershipKindClassifier::visit##INST##Inst(   \
      INST##Inst *i) {                                                         \
    assert(i->getNumOperands() && "Expected to have non-zero operands");       \
    return Map::compatibilityMap(ValueOwnershipKind::Guaranteed,               \
                                 UseLifetimeConstraint::MustBeLive);           \
  }
INTERIOR_POINTER_PROJECTION(RefElementAddr)
INTERIOR_POINTER_PROJECTION(RefTailAddr)
#undef INTERIOR_POINTER_PROJECTION

/// Instructions whose arguments are always compatible with one convention.
#define CONSTANT_OWNERSHIP_INST(OWNERSHIP, USE_LIFETIME_CONSTRAINT, INST)      \
  OperandOwnershipKindMap OperandOwnershipKindClassifier::visit##INST##Inst(   \
      INST##Inst *i) {                                                         \
    assert(i->getNumOperands() && "Expected to have non-zero operands");       \
    return Map::compatibilityMap(                                              \
        ValueOwnershipKind::OWNERSHIP,                                         \
        UseLifetimeConstraint::USE_LIFETIME_CONSTRAINT);                       \
  }
CONSTANT_OWNERSHIP_INST(Guaranteed, MustBeLive, OpenExistentialValue)
CONSTANT_OWNERSHIP_INST(Guaranteed, MustBeLive, OpenExistentialBoxValue)
CONSTANT_OWNERSHIP_INST(Guaranteed, MustBeLive, OpenExistentialBox)
CONSTANT_OWNERSHIP_INST(Guaranteed, MustBeLive, HopToExecutor)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, AutoreleaseValue)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, DeallocBox)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, DeallocExistentialBox)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, DeallocRef)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, DestroyValue)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, EndLifetime)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, BeginCOWMutation)
CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, EndCOWMutation)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, AwaitAsyncContinuation)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, AbortApply)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, AddressToPointer)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, BeginAccess)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, BeginUnpairedAccess)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, BindMemory)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, CheckedCastAddrBranch)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, CondFail)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, CopyAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, DeallocStack)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, DebugValueAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, DeinitExistentialAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, DestroyAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, EndAccess)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, EndApply)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, EndUnpairedAccess)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, GetAsyncContinuationAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, IndexAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, IndexRawPointer)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, InitBlockStorageHeader)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, InitEnumDataAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, InitExistentialAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, InitExistentialMetatype)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, InjectEnumAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, IsUnique)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, Load)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, LoadBorrow)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, MarkFunctionEscape)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, ObjCExistentialMetatypeToObject)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, ObjCMetatypeToObject)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, ObjCToThickMetatype)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, OpenExistentialAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, OpenExistentialMetatype)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, PointerToAddress)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, PointerToThinFunction)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, ProjectBlockStorage)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, ProjectValueBuffer)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, RawPointerToRef)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, SelectEnumAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, SelectValue)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, StructElementAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, SwitchEnumAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, SwitchValue)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, TailAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, ThickToObjCMetatype)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, ThinFunctionToPointer)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, ThinToThickFunction)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, TupleElementAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, UncheckedAddrCast)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, UncheckedRefCastAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, UncheckedTakeEnumDataAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, UnconditionalCheckedCastAddr)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, AllocValueBuffer)
CONSTANT_OWNERSHIP_INST(None, MustBeLive, DeallocValueBuffer)
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                          \
  CONSTANT_OWNERSHIP_INST(None, MustBeLive, Load##Name)
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                         \
  CONSTANT_OWNERSHIP_INST(Owned, MustBeInvalidated, Name##Release)
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                      \
  NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, "...")                              \
  ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, "...")
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  CONSTANT_OWNERSHIP_INST(None, MustBeLive, Name##ToRef)
#include "swift/AST/ReferenceStorage.def"
#undef CONSTANT_OWNERSHIP_INST

/// Instructions whose arguments are always compatible with one convention.
#define CONSTANT_OR_NONE_OWNERSHIP_INST(OWNERSHIP, USE_LIFETIME_CONSTRAINT,    \
                                        INST)                                  \
  OperandOwnershipKindMap OperandOwnershipKindClassifier::visit##INST##Inst(   \
      INST##Inst *i) {                                                         \
    assert(i->getNumOperands() && "Expected to have non-zero operands");       \
    return Map::compatibilityMap(                                              \
        ValueOwnershipKind::OWNERSHIP,                                         \
        UseLifetimeConstraint::USE_LIFETIME_CONSTRAINT);                       \
  }
CONSTANT_OR_NONE_OWNERSHIP_INST(Owned, MustBeInvalidated,
                                CheckedCastValueBranch)
CONSTANT_OR_NONE_OWNERSHIP_INST(Owned, MustBeInvalidated,
                                UnconditionalCheckedCastValue)
CONSTANT_OR_NONE_OWNERSHIP_INST(Owned, MustBeInvalidated, InitExistentialValue)
CONSTANT_OR_NONE_OWNERSHIP_INST(Owned, MustBeInvalidated,
                                DeinitExistentialValue)
#undef CONSTANT_OR_NONE_OWNERSHIP_INST

#define ACCEPTS_ANY_OWNERSHIP_INST(INST)                                       \
  OperandOwnershipKindMap OperandOwnershipKindClassifier::visit##INST##Inst(   \
      INST##Inst *i) {                                                         \
    return Map::allLive();                                                     \
  }
ACCEPTS_ANY_OWNERSHIP_INST(BeginBorrow)
ACCEPTS_ANY_OWNERSHIP_INST(CopyValue)
ACCEPTS_ANY_OWNERSHIP_INST(DebugValue)
ACCEPTS_ANY_OWNERSHIP_INST(FixLifetime)
ACCEPTS_ANY_OWNERSHIP_INST(UncheckedBitwiseCast) // Is this right?
ACCEPTS_ANY_OWNERSHIP_INST(WitnessMethod)        // Is this right?
ACCEPTS_ANY_OWNERSHIP_INST(ProjectBox)           // The result is a T*.
ACCEPTS_ANY_OWNERSHIP_INST(DynamicMethodBranch)
ACCEPTS_ANY_OWNERSHIP_INST(UncheckedTrivialBitCast)
ACCEPTS_ANY_OWNERSHIP_INST(ExistentialMetatype)
ACCEPTS_ANY_OWNERSHIP_INST(ValueMetatype)
ACCEPTS_ANY_OWNERSHIP_INST(UncheckedOwnershipConversion)
ACCEPTS_ANY_OWNERSHIP_INST(ValueToBridgeObject)
ACCEPTS_ANY_OWNERSHIP_INST(IsEscapingClosure)
ACCEPTS_ANY_OWNERSHIP_INST(ClassMethod)
ACCEPTS_ANY_OWNERSHIP_INST(ObjCMethod)
ACCEPTS_ANY_OWNERSHIP_INST(ObjCSuperMethod)
ACCEPTS_ANY_OWNERSHIP_INST(SuperMethod)
ACCEPTS_ANY_OWNERSHIP_INST(BridgeObjectToWord)
ACCEPTS_ANY_OWNERSHIP_INST(ClassifyBridgeObject)
ACCEPTS_ANY_OWNERSHIP_INST(CopyBlock)
ACCEPTS_ANY_OWNERSHIP_INST(RefToRawPointer)
ACCEPTS_ANY_OWNERSHIP_INST(SetDeallocating)
ACCEPTS_ANY_OWNERSHIP_INST(ProjectExistentialBox)
ACCEPTS_ANY_OWNERSHIP_INST(UnmanagedRetainValue)
ACCEPTS_ANY_OWNERSHIP_INST(UnmanagedReleaseValue)
ACCEPTS_ANY_OWNERSHIP_INST(UnmanagedAutoreleaseValue)
ACCEPTS_ANY_OWNERSHIP_INST(ConvertEscapeToNoEscape)
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  ACCEPTS_ANY_OWNERSHIP_INST(RefTo##Name)                                      \
  ACCEPTS_ANY_OWNERSHIP_INST(Name##ToRef)                                      \
  ACCEPTS_ANY_OWNERSHIP_INST(StrongCopy##Name##Value)
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  ACCEPTS_ANY_OWNERSHIP_INST(RefTo##Name)                                      \
  ACCEPTS_ANY_OWNERSHIP_INST(StrongCopy##Name##Value)
#include "swift/AST/ReferenceStorage.def"
#undef ACCEPTS_ANY_OWNERSHIP_INST

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitForwardingInst(SILInstruction *i,
                                                    ArrayRef<Operand> ops) {
  assert(i->getNumOperands() && "Expected to have non-zero operands");
  assert(isOwnershipForwardingInst(i) &&
         "Expected to have an ownership forwarding inst");

  // Merge all of the ownership of our operands. If we get back a .none from the
  // merge, then we return an empty compatibility map. This ensures that we will
  // not be compatible with /any/ input triggering a special error in the
  // ownership verifier.
  Optional<ValueOwnershipKind> optionalKind =
      ValueOwnershipKind::merge(makeOptionalTransformRange(
          ops, [&i](const Operand &op) -> Optional<ValueOwnershipKind> {
            if (i->isTypeDependentOperand(op))
              return None;
            return op.get().getOwnershipKind();
          }));
  if (!optionalKind)
    return Map();

  auto kind = optionalKind.getValue();
  if (kind == ValueOwnershipKind::None)
    return Map::allLive();
  auto lifetimeConstraint = kind.getForwardingLifetimeConstraint();
  return Map::compatibilityMap(kind, lifetimeConstraint);
}

#define FORWARD_ANY_OWNERSHIP_INST(INST)                                       \
  OperandOwnershipKindMap OperandOwnershipKindClassifier::visit##INST##Inst(   \
      INST##Inst *i) {                                                         \
    return visitForwardingInst(i);                                             \
  }
FORWARD_ANY_OWNERSHIP_INST(Tuple)
FORWARD_ANY_OWNERSHIP_INST(Struct)
FORWARD_ANY_OWNERSHIP_INST(Object)
FORWARD_ANY_OWNERSHIP_INST(Enum)
FORWARD_ANY_OWNERSHIP_INST(OpenExistentialRef)
FORWARD_ANY_OWNERSHIP_INST(Upcast)
FORWARD_ANY_OWNERSHIP_INST(UncheckedRefCast)
FORWARD_ANY_OWNERSHIP_INST(ConvertFunction)
FORWARD_ANY_OWNERSHIP_INST(RefToBridgeObject)
FORWARD_ANY_OWNERSHIP_INST(BridgeObjectToRef)
FORWARD_ANY_OWNERSHIP_INST(UnconditionalCheckedCast)
FORWARD_ANY_OWNERSHIP_INST(UncheckedEnumData)
FORWARD_ANY_OWNERSHIP_INST(DestructureStruct)
FORWARD_ANY_OWNERSHIP_INST(DestructureTuple)
FORWARD_ANY_OWNERSHIP_INST(InitExistentialRef)
FORWARD_ANY_OWNERSHIP_INST(DifferentiableFunction)
FORWARD_ANY_OWNERSHIP_INST(LinearFunction)
FORWARD_ANY_OWNERSHIP_INST(UncheckedValueCast)
#undef FORWARD_ANY_OWNERSHIP_INST

// An instruction that forwards a constant ownership or trivial ownership.
#define FORWARD_CONSTANT_OR_NONE_OWNERSHIP_INST(OWNERSHIP,                     \
                                                USE_LIFETIME_CONSTRAINT, INST) \
  OperandOwnershipKindMap OperandOwnershipKindClassifier::visit##INST##Inst(   \
      INST##Inst *i) {                                                         \
    assert(i->getNumOperands() && "Expected to have non-zero operands");       \
    assert(isGuaranteedForwardingInst(i) &&                                    \
           "Expected an ownership forwarding inst");                           \
    OperandOwnershipKindMap map;                                               \
    map.addCompatibilityConstraint(                                            \
        ValueOwnershipKind::OWNERSHIP,                                         \
        UseLifetimeConstraint::USE_LIFETIME_CONSTRAINT);                       \
    return map;                                                                \
  }
FORWARD_CONSTANT_OR_NONE_OWNERSHIP_INST(Guaranteed, MustBeLive, TupleExtract)
FORWARD_CONSTANT_OR_NONE_OWNERSHIP_INST(Guaranteed, MustBeLive, StructExtract)
FORWARD_CONSTANT_OR_NONE_OWNERSHIP_INST(Guaranteed, MustBeLive,
                                        DifferentiableFunctionExtract)
FORWARD_CONSTANT_OR_NONE_OWNERSHIP_INST(Guaranteed, MustBeLive,
                                        LinearFunctionExtract)
FORWARD_CONSTANT_OR_NONE_OWNERSHIP_INST(Owned, MustBeInvalidated,
                                        MarkUninitialized)
#undef CONSTANT_OR_NONE_OWNERSHIP_INST

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitDeallocPartialRefInst(
    DeallocPartialRefInst *i) {
  if (getValue() == i->getInstance()) {
    return Map::compatibilityMap(ValueOwnershipKind::Owned,
                                 UseLifetimeConstraint::MustBeInvalidated);
  }

  return Map::allLive();
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitSelectEnumInst(SelectEnumInst *i) {
  if (getValue() == i->getEnumOperand()) {
    return Map::allLive();
  }

  return visitForwardingInst(i, i->getAllOperands().drop_front());
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitAllocRefInst(AllocRefInst *i) {
  assert(i->getNumOperands() != 0 &&
         "If we reach this point, we must have a tail operand");
  return Map::allLive();
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitAllocRefDynamicInst(
    AllocRefDynamicInst *i) {
  assert(i->getNumOperands() != 0 &&
         "If we reach this point, we must have a tail operand");
  return Map::allLive();
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::checkTerminatorArgumentMatchesDestBB(
    SILBasicBlock *destBB, unsigned opIndex) {
  // Grab the ownership kind of the destination block.
  ValueOwnershipKind destBlockArgOwnershipKind =
      destBB->getArgument(opIndex)->getOwnershipKind();
  auto lifetimeConstraint =
      destBlockArgOwnershipKind.getForwardingLifetimeConstraint();
  return Map::compatibilityMap(destBlockArgOwnershipKind, lifetimeConstraint);
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitBranchInst(BranchInst *bi) {
  ValueOwnershipKind destBlockArgOwnershipKind =
      bi->getDestBB()->getArgument(getOperandIndex())->getOwnershipKind();

  // If we have a guaranteed parameter, treat this as consuming.
  if (destBlockArgOwnershipKind == ValueOwnershipKind::Guaranteed) {
    return Map::compatibilityMap(destBlockArgOwnershipKind,
                                 UseLifetimeConstraint::MustBeInvalidated);
  }

  // Otherwise, defer to defaults.
  auto lifetimeConstraint =
      destBlockArgOwnershipKind.getForwardingLifetimeConstraint();
  return Map::compatibilityMap(destBlockArgOwnershipKind, lifetimeConstraint);
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitCondBranchInst(CondBranchInst *cbi) {
  // In ossa, cond_br insts are not allowed to take non-trivial values. Thus, we
  // just accept anything since we know all of our operands will be trivial.
  return Map::allLive();
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitSwitchEnumInst(SwitchEnumInst *sei) {
  auto opTy = sei->getOperand()->getType();

  // If our passed in type is trivial, we shouldn't have any non-trivial
  // successors. Just bail early returning trivial.
  if (opTy.isTrivial(*sei->getFunction()))
    return Map::allLive();

  // Otherwise, go through the ownership constraints of our successor arguments
  // and merge them.
  auto mergedKind = ValueOwnershipKind::merge(makeTransformRange(
      sei->getSuccessorBlockArgumentLists(),
      [&](ArrayRef<SILArgument *> array) -> ValueOwnershipKind {
        // If the array is empty, we have a non-payloaded case. Return any.
        if (array.empty())
          return ValueOwnershipKind::None;

        // Otherwise, we should have a single element since a payload is
        // a tuple.
        assert(std::distance(array.begin(), array.end()) == 1);
        return array.front()->getOwnershipKind();
      }));

  // If we failed to merge, return an empty map so we will fail to pattern match
  // with any operand. This is a known signal to the verifier that we failed to
  // merge in a forwarding context.
  if (!mergedKind)
    return Map();
  auto kind = mergedKind.getValue();
  if (kind == ValueOwnershipKind::None)
    return Map::allLive();
  auto lifetimeConstraint = kind.getForwardingLifetimeConstraint();
  return Map::compatibilityMap(kind, lifetimeConstraint);
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitCheckedCastBranchInst(
    CheckedCastBranchInst *ccbi) {
  // TODO: Simplify this using ValueOwnershipKind::merge.
  Optional<OperandOwnershipKindMap> map;
  for (auto argArray : ccbi->getSuccessorBlockArgumentLists()) {
    assert(!argArray.empty());

    auto argOwnershipKind = argArray[getOperandIndex()]->getOwnershipKind();
    // If we do not have a map yet, initialize it and continue.
    if (!map) {
      auto lifetimeConstraint =
          argOwnershipKind.getForwardingLifetimeConstraint();
      map = Map::compatibilityMap(argOwnershipKind, lifetimeConstraint);
      continue;
    }

    // Otherwise, make sure that we can accept the rest of our
    // arguments. If not, we return an empty ownership kind to make
    // sure that we flag everything as an error.
    if (map->canAcceptKind(argOwnershipKind)) {
      continue;
    }

    return OperandOwnershipKindMap();
  }

  return map.getValue();
}

//// FIX THIS HERE
OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitReturnInst(ReturnInst *ri) {
  auto *f =ri->getFunction();

  // If we have a trivial value, return allLive().
  bool isTrivial = ri->getOperand()->getType().isTrivial(*f);
  if (isTrivial) {
    return Map::allLive();
  }

  SILFunctionConventions fnConv = f->getConventions();

  auto results = fnConv.getDirectSILResults();
  if (results.empty())
    return Map();

  auto ownershipKindRange = makeTransformRange(results,
     [&](const SILResultInfo &info) {
       return info.getOwnershipKind(*f, f->getLoweredFunctionType());
     });

  // Then merge all of our ownership kinds. If we fail to merge, return an empty
  // map so we fail on all operands.
  auto mergedBase = ValueOwnershipKind::merge(ownershipKindRange);
  if (!mergedBase)
    return Map();

  auto base = *mergedBase;
  return Map::compatibilityMap(base, base.getForwardingLifetimeConstraint());
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitEndBorrowInst(EndBorrowInst *i) {
  // If we are checking a subobject, make sure that we are from a guaranteed
  // basic block argument.
  if (isCheckingSubObject()) {
    auto *phiArg = cast<SILPhiArgument>(op.get());
    (void)phiArg;
    return Map::compatibilityMap(ValueOwnershipKind::Guaranteed,
                                 UseLifetimeConstraint::MustBeLive);
  }

  /// An end_borrow is modeled as invalidating the guaranteed value preventing
  /// any further uses of the value.
  return Map::compatibilityMap(ValueOwnershipKind::Guaranteed,
                               UseLifetimeConstraint::MustBeInvalidated);
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitThrowInst(ThrowInst *i) {
  return Map::compatibilityMap(ValueOwnershipKind::Owned,
                               UseLifetimeConstraint::MustBeInvalidated);
}

#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                          \
  OperandOwnershipKindMap                                                      \
      OperandOwnershipKindClassifier::visitStore##Name##Inst(                  \
          Store##Name##Inst *i) {                                              \
    /* A store instruction implies that the value to be stored to be live, */  \
    /* but it does not touch the strong reference count of the value. We */    \
    /* also just care about liveness for the dest. So just match everything */ \
    /* as must be live. */                                                     \
    return Map::allLive();                                                     \
  }
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                      \
  NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, "...")
#include "swift/AST/ReferenceStorage.def"

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitStoreBorrowInst(StoreBorrowInst *i) {
  if (getValue() == i->getSrc()) {
    return Map::compatibilityMap(ValueOwnershipKind::Guaranteed,
                                 UseLifetimeConstraint::MustBeLive);
  }
  return Map::allLive();
}

// FIXME: Why not use SILArgumentConvention here?
OperandOwnershipKindMap OperandOwnershipKindClassifier::visitCallee(
    CanSILFunctionType substCalleeType) {
  ParameterConvention conv = substCalleeType->getCalleeConvention();
  switch (conv) {
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Indirect_In_Constant:
    assert(!SILModuleConventions(mod).isSILIndirect(
                                      SILParameterInfo(substCalleeType, conv)));
    return Map::compatibilityMap(ValueOwnershipKind::Owned,
                                 UseLifetimeConstraint::MustBeInvalidated);
  case ParameterConvention::Indirect_In_Guaranteed:
    assert(!SILModuleConventions(mod).isSILIndirect(
                                      SILParameterInfo(substCalleeType, conv)));
    return Map::compatibilityMap(ValueOwnershipKind::Guaranteed,
                                 UseLifetimeConstraint::MustBeLive);
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
    llvm_unreachable("Illegal convention for callee");
  case ParameterConvention::Direct_Unowned:
    return Map::allLive();
  case ParameterConvention::Direct_Owned:
    return Map::compatibilityMap(ValueOwnershipKind::Owned,
                                 UseLifetimeConstraint::MustBeInvalidated);
  case ParameterConvention::Direct_Guaranteed:
    if (substCalleeType->isNoEscape())
      return Map::allLive();
    // We want to accept guaranteed/owned in this position since we
    // treat the use of an owned parameter as an instantaneously
    // borrowed value for the duration of the call.
    return Map::compatibilityMap(
        {{ValueOwnershipKind::Guaranteed, UseLifetimeConstraint::MustBeLive},
         {ValueOwnershipKind::Owned, UseLifetimeConstraint::MustBeLive}});
  }

  llvm_unreachable("Unhandled ParameterConvention in switch.");
}

// We allow for trivial cases of enums with non-trivial cases to be passed in
// non-trivial argument positions. This fits with modeling of a
// SILFunctionArgument as a phi in a global program graph.
OperandOwnershipKindMap OperandOwnershipKindClassifier::visitApplyParameter(
    ValueOwnershipKind kind, UseLifetimeConstraint requirement) {

  // Check against the passed in convention. We allow for owned to be passed to
  // apply parameters.
  if (kind != ValueOwnershipKind::Owned) {
    return Map::compatibilityMap(
        {{kind, requirement},
         {ValueOwnershipKind::Owned, UseLifetimeConstraint::MustBeLive}});
  }
  return Map::compatibilityMap(kind, requirement);
}

// Handle Apply and TryApply.
OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitFullApply(FullApplySite apply) {
  // If we are visiting the callee operand, handle it specially.
  if (apply.isCalleeOperand(op)) {
    return visitCallee(apply.getSubstCalleeType());
  }

  // Indirect return arguments are address types.
  if (apply.isIndirectResultOperand(op)) {
    return Map::allLive();
  }

  // If we have a type dependent operand, return an empty map.
  if (apply.getInstruction()->isTypeDependentOperand(op))
    return Map();

  unsigned argIndex = apply.getCalleeArgIndex(op);
  auto conv = apply.getSubstCalleeConv();
  SILParameterInfo paramInfo = conv.getParamInfoForSILArg(argIndex);

  switch (paramInfo.getConvention()) {
  case ParameterConvention::Direct_Owned:
    return visitApplyParameter(ValueOwnershipKind::Owned,
                               UseLifetimeConstraint::MustBeInvalidated);
  case ParameterConvention::Direct_Unowned:
    return Map::allLive();

  case ParameterConvention::Indirect_In: {
    // This expects an @trivial if we have lowered addresses and @
    if (conv.useLoweredAddresses()) {
      return Map::allLive();
    }
    // TODO: Once trivial is subsumed in any, this goes away.
    auto map = visitApplyParameter(ValueOwnershipKind::Owned,
                                   UseLifetimeConstraint::MustBeInvalidated);
    return map;
  }

  case ParameterConvention::Indirect_In_Guaranteed: {
    // This expects an @trivial if we have lowered addresses and @
    if (conv.useLoweredAddresses()) {
      return Map::allLive();
    }
    return visitApplyParameter(ValueOwnershipKind::Guaranteed,
                               UseLifetimeConstraint::MustBeLive);
  }

  // The following conventions should take address types and thus be
  // trivial.
  case ParameterConvention::Indirect_In_Constant:
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
    return Map::allLive();

  case ParameterConvention::Direct_Guaranteed:
    // A +1 value may be passed to a guaranteed argument. From the caller's
    // point of view, this is just like a normal non-consuming use.
    // Direct_Guaranteed only accepts non-trivial types, but trivial types are
    // already handled above.
    return visitApplyParameter(ValueOwnershipKind::Guaranteed,
                               UseLifetimeConstraint::MustBeLive);
  }
  llvm_unreachable("unhandled convension");
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitBeginApplyInst(BeginApplyInst *i) {
  return visitFullApply(i);
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitApplyInst(ApplyInst *i) {
  return visitFullApply(i);
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitTryApplyInst(TryApplyInst *i) {
  return visitFullApply(i);
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitPartialApplyInst(PartialApplyInst *i) {
  // partial_apply [stack] does not take ownership of its operands.
  if (i->isOnStack())
    return Map::allLive();

  return Map::compatibilityMap(
       // All non-trivial types should be captured.
       ValueOwnershipKind::Owned, UseLifetimeConstraint::MustBeInvalidated);
}

// TODO: FIX THIS
OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitYieldInst(YieldInst *i) {
  // Indirect return arguments are address types.
  //
  // TODO: Change this to check if this operand is an indirect result
  if (isAddressOrTrivialType())
    return Map::allLive();

  auto fnType = i->getFunction()->getLoweredFunctionType();
  auto yieldInfo = fnType->getYields()[getOperandIndex()];
  switch (yieldInfo.getConvention()) {
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Direct_Owned:
    return visitApplyParameter(ValueOwnershipKind::Owned,
                               UseLifetimeConstraint::MustBeInvalidated);
  case ParameterConvention::Indirect_In_Constant:
  case ParameterConvention::Direct_Unowned:
    // We accept unowned, owned, and guaranteed in unowned positions.
    return Map::allLive();
  case ParameterConvention::Indirect_In_Guaranteed:
  case ParameterConvention::Direct_Guaranteed:
    return visitApplyParameter(ValueOwnershipKind::Guaranteed,
                               UseLifetimeConstraint::MustBeLive);
  // The following conventions should take address types.
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
    llvm_unreachable("Unexpected non-trivial parameter convention.");
  }
  llvm_unreachable("unhandled convension");
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitAssignInst(AssignInst *i) {
  if (getValue() != i->getSrc()) {
    return Map::allLive();
  }

  return Map::compatibilityMap(ValueOwnershipKind::Owned,
                               UseLifetimeConstraint::MustBeInvalidated);
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitAssignByWrapperInst(AssignByWrapperInst *i) {
  if (getValue() != i->getSrc()) {
    return Map::allLive();
  }

  return Map::compatibilityMap(ValueOwnershipKind::Owned,
                               UseLifetimeConstraint::MustBeInvalidated);
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitStoreInst(StoreInst *i) {
  if (getValue() != i->getSrc()) {
    return Map::allLive();
  }

  return Map::compatibilityMap(ValueOwnershipKind::Owned,
                               UseLifetimeConstraint::MustBeInvalidated);
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitCopyBlockWithoutEscapingInst(
    CopyBlockWithoutEscapingInst *i) {
  // Consumes the closure parameter.
  if (getValue() == i->getClosure()) {
    return Map::compatibilityMap(ValueOwnershipKind::Owned,
                                 UseLifetimeConstraint::MustBeInvalidated);
  }

  return Map::allLive();
}

OperandOwnershipKindMap OperandOwnershipKindClassifier::visitMarkDependenceInst(
    MarkDependenceInst *mdi) {
  // If we are analyzing "the value", we forward ownership.
  if (getValue() == mdi->getValue()) {
    auto kind = mdi->getOwnershipKind();
    if (kind == ValueOwnershipKind::None)
      return Map::allLive();
    auto lifetimeConstraint = kind.getForwardingLifetimeConstraint();
    return Map::compatibilityMap(kind, lifetimeConstraint);
  }

  // If we are not the "value" of the mark_dependence, then we must be the
  // "base". This means that any use that would destroy "value" can not be moved
  // before any uses of "base". We treat this as non-consuming and rely on the
  // rest of the optimizer to respect the movement restrictions.
  return Map::allLive();
}

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitKeyPathInst(KeyPathInst *I) {
  // KeyPath moves the value in memory out of address operands, but the
  // ownership checker doesn't reason about that yet.
  return Map::compatibilityMap(
       ValueOwnershipKind::Owned, UseLifetimeConstraint::MustBeInvalidated);
}

//===----------------------------------------------------------------------===//
//                            Builtin Use Checker
//===----------------------------------------------------------------------===//

namespace {

struct OperandOwnershipKindBuiltinClassifier
    : SILBuiltinVisitor<OperandOwnershipKindBuiltinClassifier,
                        OperandOwnershipKindMap> {
  using Map = OperandOwnershipKindMap;

  OperandOwnershipKindMap visitLLVMIntrinsic(BuiltinInst *bi,
                                             llvm::Intrinsic::ID id) {
    // LLVM intrinsics do not traffic in ownership, so if we have a result, it
    // must be trivial.
    return Map::allLive();
  }

  // BUILTIN_TYPE_CHECKER_OPERATION does not live past the type checker.
#define BUILTIN_TYPE_CHECKER_OPERATION(ID, NAME)

#define BUILTIN(ID, NAME, ATTRS)                                               \
  OperandOwnershipKindMap visit##ID(BuiltinInst *bi, StringRef attr);
#include "swift/AST/Builtins.def"

  OperandOwnershipKindMap check(BuiltinInst *bi) { return visit(bi); }
};

} // end anonymous namespace

#define ANY_OWNERSHIP_BUILTIN(ID)                                              \
  OperandOwnershipKindMap OperandOwnershipKindBuiltinClassifier::visit##ID(    \
      BuiltinInst *, StringRef) {                                              \
    return Map::allLive();                                                     \
  }
ANY_OWNERSHIP_BUILTIN(ErrorInMain)
ANY_OWNERSHIP_BUILTIN(UnexpectedError)
ANY_OWNERSHIP_BUILTIN(WillThrow)
ANY_OWNERSHIP_BUILTIN(AShr)
ANY_OWNERSHIP_BUILTIN(GenericAShr)
ANY_OWNERSHIP_BUILTIN(Add)
ANY_OWNERSHIP_BUILTIN(GenericAdd)
ANY_OWNERSHIP_BUILTIN(Alignof)
ANY_OWNERSHIP_BUILTIN(AllocRaw)
ANY_OWNERSHIP_BUILTIN(And)
ANY_OWNERSHIP_BUILTIN(GenericAnd)
ANY_OWNERSHIP_BUILTIN(AssertConf)
ANY_OWNERSHIP_BUILTIN(AssignCopyArrayNoAlias)
ANY_OWNERSHIP_BUILTIN(AssignCopyArrayFrontToBack)
ANY_OWNERSHIP_BUILTIN(AssignCopyArrayBackToFront)
ANY_OWNERSHIP_BUILTIN(AssignTakeArray)
ANY_OWNERSHIP_BUILTIN(AssumeNonNegative)
ANY_OWNERSHIP_BUILTIN(AssumeTrue)
ANY_OWNERSHIP_BUILTIN(AtomicLoad)
ANY_OWNERSHIP_BUILTIN(AtomicRMW)
ANY_OWNERSHIP_BUILTIN(AtomicStore)
ANY_OWNERSHIP_BUILTIN(BitCast)
ANY_OWNERSHIP_BUILTIN(CanBeObjCClass)
ANY_OWNERSHIP_BUILTIN(CondFailMessage)
ANY_OWNERSHIP_BUILTIN(CmpXChg)
ANY_OWNERSHIP_BUILTIN(CondUnreachable)
ANY_OWNERSHIP_BUILTIN(CopyArray)
ANY_OWNERSHIP_BUILTIN(DeallocRaw)
ANY_OWNERSHIP_BUILTIN(DestroyArray)
ANY_OWNERSHIP_BUILTIN(ExactSDiv)
ANY_OWNERSHIP_BUILTIN(GenericExactSDiv)
ANY_OWNERSHIP_BUILTIN(ExactUDiv)
ANY_OWNERSHIP_BUILTIN(GenericExactUDiv)
ANY_OWNERSHIP_BUILTIN(ExtractElement)
ANY_OWNERSHIP_BUILTIN(FAdd)
ANY_OWNERSHIP_BUILTIN(GenericFAdd)
ANY_OWNERSHIP_BUILTIN(FCMP_OEQ)
ANY_OWNERSHIP_BUILTIN(FCMP_OGE)
ANY_OWNERSHIP_BUILTIN(FCMP_OGT)
ANY_OWNERSHIP_BUILTIN(FCMP_OLE)
ANY_OWNERSHIP_BUILTIN(FCMP_OLT)
ANY_OWNERSHIP_BUILTIN(FCMP_ONE)
ANY_OWNERSHIP_BUILTIN(FCMP_ORD)
ANY_OWNERSHIP_BUILTIN(FCMP_UEQ)
ANY_OWNERSHIP_BUILTIN(FCMP_UGE)
ANY_OWNERSHIP_BUILTIN(FCMP_UGT)
ANY_OWNERSHIP_BUILTIN(FCMP_ULE)
ANY_OWNERSHIP_BUILTIN(FCMP_ULT)
ANY_OWNERSHIP_BUILTIN(FCMP_UNE)
ANY_OWNERSHIP_BUILTIN(FCMP_UNO)
ANY_OWNERSHIP_BUILTIN(FDiv)
ANY_OWNERSHIP_BUILTIN(GenericFDiv)
ANY_OWNERSHIP_BUILTIN(FMul)
ANY_OWNERSHIP_BUILTIN(GenericFMul)
ANY_OWNERSHIP_BUILTIN(FNeg)
ANY_OWNERSHIP_BUILTIN(FPExt)
ANY_OWNERSHIP_BUILTIN(FPToSI)
ANY_OWNERSHIP_BUILTIN(FPToUI)
ANY_OWNERSHIP_BUILTIN(FPTrunc)
ANY_OWNERSHIP_BUILTIN(FRem)
ANY_OWNERSHIP_BUILTIN(GenericFRem)
ANY_OWNERSHIP_BUILTIN(FSub)
ANY_OWNERSHIP_BUILTIN(GenericFSub)
ANY_OWNERSHIP_BUILTIN(Fence)
ANY_OWNERSHIP_BUILTIN(GetObjCTypeEncoding)
ANY_OWNERSHIP_BUILTIN(ICMP_EQ)
ANY_OWNERSHIP_BUILTIN(ICMP_NE)
ANY_OWNERSHIP_BUILTIN(ICMP_SGE)
ANY_OWNERSHIP_BUILTIN(ICMP_SGT)
ANY_OWNERSHIP_BUILTIN(ICMP_SLE)
ANY_OWNERSHIP_BUILTIN(ICMP_SLT)
ANY_OWNERSHIP_BUILTIN(ICMP_UGE)
ANY_OWNERSHIP_BUILTIN(ICMP_UGT)
ANY_OWNERSHIP_BUILTIN(ICMP_ULE)
ANY_OWNERSHIP_BUILTIN(ICMP_ULT)
ANY_OWNERSHIP_BUILTIN(InsertElement)
ANY_OWNERSHIP_BUILTIN(IntToFPWithOverflow)
ANY_OWNERSHIP_BUILTIN(IntToPtr)
ANY_OWNERSHIP_BUILTIN(IsOptionalType)
ANY_OWNERSHIP_BUILTIN(IsPOD)
ANY_OWNERSHIP_BUILTIN(IsConcrete)
ANY_OWNERSHIP_BUILTIN(IsBitwiseTakable)
ANY_OWNERSHIP_BUILTIN(IsSameMetatype)
ANY_OWNERSHIP_BUILTIN(LShr)
ANY_OWNERSHIP_BUILTIN(GenericLShr)
ANY_OWNERSHIP_BUILTIN(Mul)
ANY_OWNERSHIP_BUILTIN(GenericMul)
ANY_OWNERSHIP_BUILTIN(OnFastPath)
ANY_OWNERSHIP_BUILTIN(Once)
ANY_OWNERSHIP_BUILTIN(OnceWithContext)
ANY_OWNERSHIP_BUILTIN(Or)
ANY_OWNERSHIP_BUILTIN(GenericOr)
ANY_OWNERSHIP_BUILTIN(PtrToInt)
ANY_OWNERSHIP_BUILTIN(SAddOver)
ANY_OWNERSHIP_BUILTIN(SDiv)
ANY_OWNERSHIP_BUILTIN(GenericSDiv)
ANY_OWNERSHIP_BUILTIN(SExt)
ANY_OWNERSHIP_BUILTIN(SExtOrBitCast)
ANY_OWNERSHIP_BUILTIN(SIToFP)
ANY_OWNERSHIP_BUILTIN(SMulOver)
ANY_OWNERSHIP_BUILTIN(SRem)
ANY_OWNERSHIP_BUILTIN(GenericSRem)
ANY_OWNERSHIP_BUILTIN(SSubOver)
ANY_OWNERSHIP_BUILTIN(SToSCheckedTrunc)
ANY_OWNERSHIP_BUILTIN(SToUCheckedTrunc)
ANY_OWNERSHIP_BUILTIN(Expect)
ANY_OWNERSHIP_BUILTIN(Shl)
ANY_OWNERSHIP_BUILTIN(GenericShl)
ANY_OWNERSHIP_BUILTIN(Sizeof)
ANY_OWNERSHIP_BUILTIN(StaticReport)
ANY_OWNERSHIP_BUILTIN(Strideof)
ANY_OWNERSHIP_BUILTIN(StringObjectOr)
ANY_OWNERSHIP_BUILTIN(Sub)
ANY_OWNERSHIP_BUILTIN(GenericSub)
ANY_OWNERSHIP_BUILTIN(TakeArrayNoAlias)
ANY_OWNERSHIP_BUILTIN(TakeArrayBackToFront)
ANY_OWNERSHIP_BUILTIN(TakeArrayFrontToBack)
ANY_OWNERSHIP_BUILTIN(Trunc)
ANY_OWNERSHIP_BUILTIN(TruncOrBitCast)
ANY_OWNERSHIP_BUILTIN(TSanInoutAccess)
ANY_OWNERSHIP_BUILTIN(UAddOver)
ANY_OWNERSHIP_BUILTIN(UDiv)
ANY_OWNERSHIP_BUILTIN(GenericUDiv)
ANY_OWNERSHIP_BUILTIN(UIToFP)
ANY_OWNERSHIP_BUILTIN(UMulOver)
ANY_OWNERSHIP_BUILTIN(URem)
ANY_OWNERSHIP_BUILTIN(GenericURem)
ANY_OWNERSHIP_BUILTIN(USubOver)
ANY_OWNERSHIP_BUILTIN(UToSCheckedTrunc)
ANY_OWNERSHIP_BUILTIN(UToUCheckedTrunc)
ANY_OWNERSHIP_BUILTIN(Unreachable)
ANY_OWNERSHIP_BUILTIN(UnsafeGuaranteedEnd)
ANY_OWNERSHIP_BUILTIN(Xor)
ANY_OWNERSHIP_BUILTIN(GenericXor)
ANY_OWNERSHIP_BUILTIN(ZExt)
ANY_OWNERSHIP_BUILTIN(ZExtOrBitCast)
ANY_OWNERSHIP_BUILTIN(ZeroInitializer)
ANY_OWNERSHIP_BUILTIN(Swift3ImplicitObjCEntrypoint)
ANY_OWNERSHIP_BUILTIN(PoundAssert)
ANY_OWNERSHIP_BUILTIN(GlobalStringTablePointer)
ANY_OWNERSHIP_BUILTIN(TypePtrAuthDiscriminator)
ANY_OWNERSHIP_BUILTIN(IntInstrprofIncrement)
#undef ANY_OWNERSHIP_BUILTIN

// This is correct today since we do not have any builtins which return
// @guaranteed parameters. This means that we can only have a lifetime ending
// use with our builtins if it is owned.
#define CONSTANT_OWNERSHIP_BUILTIN(OWNERSHIP, USE_LIFETIME_CONSTRAINT, ID)     \
  OperandOwnershipKindMap OperandOwnershipKindBuiltinClassifier::visit##ID(    \
      BuiltinInst *, StringRef) {                                              \
    return Map::compatibilityMap(                                              \
        ValueOwnershipKind::OWNERSHIP,                                         \
        UseLifetimeConstraint::USE_LIFETIME_CONSTRAINT);                       \
  }
CONSTANT_OWNERSHIP_BUILTIN(Owned, MustBeInvalidated, COWBufferForReading)
CONSTANT_OWNERSHIP_BUILTIN(Owned, MustBeInvalidated, UnsafeGuaranteed)
#undef CONSTANT_OWNERSHIP_BUILTIN

#define SHOULD_NEVER_VISIT_BUILTIN(ID)                              \
  OperandOwnershipKindMap OperandOwnershipKindBuiltinClassifier::visit##ID(    \
      BuiltinInst *, StringRef) {                                              \
    llvm_unreachable("Builtin should never be visited! E.x.: It may not have arguments"); \
  }
SHOULD_NEVER_VISIT_BUILTIN(GetCurrentAsyncTask)
#undef SHOULD_NEVER_VISIT_BUILTIN

// Builtins that should be lowered to SIL instructions so we should never see
// them.
#define BUILTIN_SIL_OPERATION(ID, NAME, CATEGORY)                              \
  OperandOwnershipKindMap OperandOwnershipKindBuiltinClassifier::visit##ID(    \
      BuiltinInst *, StringRef) {                                              \
    llvm_unreachable("Builtin should have been lowered to SIL instruction?!"); \
  }
#define BUILTIN(X, Y, Z)
#include "swift/AST/Builtins.def"

OperandOwnershipKindMap
OperandOwnershipKindClassifier::visitBuiltinInst(BuiltinInst *bi) {
  return OperandOwnershipKindBuiltinClassifier().check(bi);
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

OperandOwnershipKindMap
Operand::getOwnershipKindMap(bool isForwardingSubValue) const {
  // If we do not have ownership enabled, just return all live. This ensures
  // that we do not have any consuming uses and everything from an ownership
  // perspective is just a liveness use short-circuiting many of the
  // optimizations.
  if (!getUser()->getFunction()->hasOwnership())
    return OperandOwnershipKindMap::allLive();

  OperandOwnershipKindClassifier classifier(
      getUser()->getModule(), *this,
      isForwardingSubValue);
  return classifier.visit(const_cast<SILInstruction *>(getUser()));
}
