//===--- SemanticARCOptVisitor.h ------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_SEMANTICARC_SEMANTICARCOPTVISITOR_H
#define SWIFT_SILOPTIMIZER_SEMANTICARC_SEMANTICARCOPTVISITOR_H

#include "Context.h"
#include "OwnershipLiveRange.h"
#include "SemanticARCOpts.h"

#include "swift/Basic/BlotSetVector.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/Basic/MultiMapCache.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"

namespace swift {
namespace semanticarc {

/// A visitor that optimizes ownership instructions and eliminates any trivially
/// dead code that results after optimization. It uses an internal worklist that
/// is initialized on construction with targets to avoid iterator invalidation
/// issues. Rather than revisit the entire CFG like SILCombine and other
/// visitors do, we maintain a visitedSinceLastMutation list to ensure that we
/// revisit all interesting instructions in between mutations.
struct LLVM_LIBRARY_VISIBILITY SemanticARCOptVisitor
    : SILValueVisitor<SemanticARCOptVisitor, bool> {
  /// Our main worklist. We use this after an initial run through.
  SmallBlotSetVector<SILValue, 32> worklist;

  /// A set of values that we have visited since the last mutation. We use this
  /// to ensure that we do not visit values twice without mutating.
  ///
  /// This is specifically to ensure that we do not go into an infinite loop
  /// when visiting phi nodes.
  SmallBlotSetVector<SILValue, 16> visitedSinceLastMutation;

  Context ctx;

  explicit SemanticARCOptVisitor(SILFunction &fn, DeadEndBlocks &deBlocks,
                                 bool onlyMandatoryOpts)
      : ctx(fn, deBlocks, onlyMandatoryOpts,
            InstModCallbacks()
                .onDelete(
                    [this](SILInstruction *inst) { eraseInstruction(inst); })
                .onSetUseValue([this](Operand *use, SILValue newValue) {
                  use->set(newValue);
                  worklist.insert(newValue);
                })) {}

  void reset() {
    ctx.reset();
    worklist.clear();
    visitedSinceLastMutation.clear();
  }

  DeadEndBlocks &getDeadEndBlocks() { return ctx.getDeadEndBlocks(); }

  /// Given a single value instruction, RAUW it with newValue, add newValue to
  /// the worklist, and then call eraseInstruction on i.
  void eraseAndRAUWSingleValueInstruction(SingleValueInstruction *i,
                                          SILValue newValue) {
    worklist.insert(newValue);
    for (auto *use : i->getUses()) {
      for (SILValue result : use->getUser()->getResults()) {
        worklist.insert(result);
      }
    }
    i->replaceAllUsesWith(newValue);
    eraseInstructionAndAddOperandsToWorklist(i);
  }

  /// Add all operands of i to the worklist and then call eraseInstruction on
  /// i. Assumes that the instruction doesnt have users.
  void eraseInstructionAndAddOperandsToWorklist(SILInstruction *i) {
    // Then copy all operands into the worklist for future processing.
    for (SILValue v : i->getOperandValues()) {
      worklist.insert(v);
    }
    eraseInstruction(i);
  }

  /// Pop values off of visitedSinceLastMutation, adding .some values to the
  /// worklist.
  void drainVisitedSinceLastMutationIntoWorklist() {
    while (!visitedSinceLastMutation.empty()) {
      Optional<SILValue> nextValue = visitedSinceLastMutation.pop_back_val();
      if (!nextValue.has_value()) {
        continue;
      }
      worklist.insert(*nextValue);
    }
  }

  /// Remove all results of the given instruction from the worklist and then
  /// erase the instruction. Assumes that the instruction does not have any
  /// users left.
  void eraseInstruction(SILInstruction *i) {
    // Remove all SILValues of the instruction from the worklist and then erase
    // the instruction.
    for (SILValue result : i->getResults()) {
      worklist.erase(result);
      visitedSinceLastMutation.erase(result);
    }
    i->eraseFromParent();

    // Add everything else from visitedSinceLastMutation to the worklist.
    drainVisitedSinceLastMutationIntoWorklist();
  }

  InstModCallbacks &getCallbacks() { return ctx.instModCallbacks; }

  bool visitSILInstruction(SILInstruction *i) {
    assert((isa<OwnershipForwardingTermInst>(i) ||
            !ForwardingInstruction::isa(i)) &&
           "Should have forwarding visitor for all ownership forwarding "
           "non-term instructions");
    return false;
  }

  /// The default visitor.
  bool visitValueBase(ValueBase *v) {
    auto *inst = v->getDefiningInstruction();
    (void)inst;
    assert((!inst || !ForwardingInstruction::isa(inst)) &&
           "Should have forwarding visitor for all ownership forwarding "
           "instructions");
    return false;
  }

  bool visitCopyValueInst(CopyValueInst *cvi);
  bool visitBeginBorrowInst(BeginBorrowInst *bbi);
  bool visitLoadInst(LoadInst *li);
  bool visitMoveValueInst(MoveValueInst *mvi);
  bool
  visitUncheckedOwnershipConversionInst(UncheckedOwnershipConversionInst *uoci);

  static bool shouldVisitInst(SILInstruction *i) {
    switch (i->getKind()) {
    default:
      return false;
    case SILInstructionKind::CopyValueInst:
    case SILInstructionKind::BeginBorrowInst:
    case SILInstructionKind::LoadInst:
    case SILInstructionKind::MoveValueInst:
    case SILInstructionKind::UncheckedOwnershipConversionInst:
      return true;
    }
  }

#define FORWARDING_INST(NAME)                                                  \
  bool visit##NAME##Inst(NAME##Inst *cls) {                                    \
    for (SILValue v : cls->getResults()) {                                     \
      worklist.insert(v);                                                      \
    }                                                                          \
    return false;                                                              \
  }
  FORWARDING_INST(Tuple)
  FORWARDING_INST(Object)
  FORWARDING_INST(Struct)
  FORWARDING_INST(Enum)
  FORWARDING_INST(UncheckedValueCast)
  FORWARDING_INST(ThinToThickFunction)
  FORWARDING_INST(OpenExistentialRef)
  FORWARDING_INST(Upcast)
  FORWARDING_INST(UncheckedRefCast)
  FORWARDING_INST(ConvertFunction)
  FORWARDING_INST(RefToBridgeObject)
  FORWARDING_INST(BridgeObjectToRef)
  FORWARDING_INST(UnconditionalCheckedCast)
  FORWARDING_INST(UncheckedEnumData)
  FORWARDING_INST(MarkUninitialized)
  FORWARDING_INST(SelectEnum)
  FORWARDING_INST(DestructureStruct)
  FORWARDING_INST(DestructureTuple)
  FORWARDING_INST(TupleExtract)
  FORWARDING_INST(StructExtract)
  FORWARDING_INST(OpenExistentialValue)
  FORWARDING_INST(OpenExistentialBoxValue)
  FORWARDING_INST(MarkDependence)
  FORWARDING_INST(InitExistentialRef)
  FORWARDING_INST(DifferentiableFunction)
  FORWARDING_INST(LinearFunction)
  FORWARDING_INST(DifferentiableFunctionExtract)
  FORWARDING_INST(LinearFunctionExtract)
#undef FORWARDING_INST

  bool processWorklist();
  bool optimize();
  bool optimizeWithoutFixedPoint();

  bool performLoadCopyToLoadBorrowOptimization(LoadInst *li, SILValue original);
  bool performGuaranteedCopyValueOptimization(CopyValueInst *cvi);
  bool eliminateDeadLiveRangeCopyValue(CopyValueInst *cvi);
  bool tryJoiningCopyValueLiveRangeWithOperand(CopyValueInst *cvi);
  bool tryPerformOwnedCopyValueOptimization(CopyValueInst *cvi);
};

} // namespace semanticarc
} // namespace swift

#endif // SWIFT_SILOPTIMIZER_SEMANTICARC_SEMANTICARCOPTVISITOR_H
