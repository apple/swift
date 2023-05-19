//===--- InstructionDeleter.cpp - InstructionDeleter utility --------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/SILFunction.h"
#include "swift/SILOptimizer/Utils/ConstExpr.h"
#include "swift/SILOptimizer/Utils/DebugOptUtils.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"

using namespace swift;

static bool hasOnlyIncidentalUses(SILInstruction *inst,
                                  bool preserveDebugInfo = false) {
  for (SILValue result : inst->getResults()) {
    for (Operand *use : result->getUses()) {
      SILInstruction *user = use->getUser();
      if (!isIncidentalUse(user))
        return false;
      if (preserveDebugInfo && user->isDebugInstruction())
        return false;
    }
  }
  return true;
}

/// A scope-affecting instruction is an instruction which may end the scope of
/// its operand or may produce scoped results that require cleaning up. E.g.
/// begin_borrow, begin_access, copy_value, a call that produces a owned value
/// are scoped instructions. The scope of the results of the first two
/// instructions end with an end_borrow/access instruction, while those of the
/// latter two end with a consuming operation like destroy_value instruction.
/// These instruction may also end the scope of its operand e.g. a call could
/// consume owned arguments thereby ending its scope. Dead-code eliminating a
/// scope-affecting instruction requires fixing the lifetime of the non-trivial
/// operands of the instruction and requires cleaning up the end-of-scope uses
/// of non-trivial results.
///
/// \param inst instruction that checked for liveness.
///
/// TODO: Handle partial_apply [stack] which has a dealloc_stack user.
static bool isScopeAffectingInstructionDead(SILInstruction *inst,
                                            bool fixLifetime) {
  SILFunction *fun = inst->getFunction();
  assert(fun && "Instruction has no function.");
  // Only support ownership SIL for scoped instructions.
  if (!fun->hasOwnership()) {
    return false;
  }
  // If the instruction has any use other than end of scope use or destroy_value
  // use, bail out.
  if (!hasOnlyEndOfScopeOrEndOfLifetimeUses(inst)) {
    return false;
  }
  // If inst is a copy or beginning of scope, inst is dead, since we know that
  // it is used only in a destroy_value or end-of-scope instruction.
  if (getSingleValueCopyOrCast(inst))
    return true;

  switch (inst->getKind()) {
  case SILInstructionKind::AllocStackInst: {
    // An alloc_stack only used by dealloc_stack is dead.
    return
      fun->getEffectiveOptimizationMode() > OptimizationMode::NoOptimization
      || !cast<AllocStackInst>(inst)->getVarInfo();
  }
  case SILInstructionKind::LoadBorrowInst: {
    // A load_borrow only used in an end_borrow is dead.
    return true;
  }
  case SILInstructionKind::LoadInst: {
    LoadOwnershipQualifier loadOwnershipQual =
        cast<LoadInst>(inst)->getOwnershipQualifier();
    // If the load creates a copy, it is dead, since we know that if at all it
    // is used, it is only in a destroy_value instruction.
    return (loadOwnershipQual == LoadOwnershipQualifier::Copy ||
            loadOwnershipQual == LoadOwnershipQualifier::Trivial);
    // TODO: we can handle load [take] but we would have to know that the
    // operand has been consumed. Note that OperandOwnershipKind map does not
    // say this for load.
  }
  case SILInstructionKind::PartialApplyInst: {
    bool onlyTrivialArgs = true;
    for (auto &arg : cast<PartialApplyInst>(inst)->getArgumentOperands()) {
      auto argTy = arg.get()->getType();
      // Non-stack partial apply captures that are passed by address are always
      // captured at +1 by the closure contrext, regardless of the calling
      // convention.
      //
      // TODO: When on-stack partial applies are also handled, then their +0
      // address arguments can be ignored.
      //
      // FIXME: Even with fixLifetimes enabled, InstructionDeleter does not know
      // how to cleanup arguments captured by address. This should be as simple
      // as inserting a destroy_addr. But the analagous code in
      // tryDeleteDeadClosure() and keepArgsOfPartialApplyAlive() mysteriously
      // creates new alloc_stack's and invalidates stack nesting. So we
      // conservatively bail-out until we understand why that hack exists.
      if (argTy.isAddress())
        return false;

      onlyTrivialArgs &= argTy.isTrivial(*fun);
    }
    // Partial applies that are only used in destroys cannot have any effect on
    // the program state, provided the values they capture are explicitly
    // destroyed, which only happens when fixLifetime is true.
    return onlyTrivialArgs || fixLifetime;
  }
  case SILInstructionKind::StructInst:
  case SILInstructionKind::EnumInst:
  case SILInstructionKind::TupleInst:
  case SILInstructionKind::ConvertFunctionInst:
  case SILInstructionKind::DestructureStructInst:
  case SILInstructionKind::DestructureTupleInst: {
    // All these ownership forwarding instructions that are only used in
    // destroys are dead provided the values they consume are destroyed
    // explicitly.
    return true;
  }
  case SILInstructionKind::ApplyInst: {
    // The following property holds for constant-evaluable functions that do
    // not take arguments of generic type:
    // 1. they do not create objects having deinitializers with global
    // side effects, as they can only create objects consisting of trivial
    // values, (non-generic) arrays and strings.
    // 2. they do not use global variables or call arbitrary functions with
    // side effects.
    // The above two properties imply that a value returned by a constant
    // evaluable function does not have a deinitializer with global side
    // effects. Therefore, the deinitializer can be sinked.
    //
    // A generic, read-only constant evaluable call only reads and/or
    // destroys its (non-generic) parameters. It therefore cannot have any
    // side effects (note that parameters being non-generic have value
    // semantics). Therefore, the constant evaluable call can be removed
    // provided the parameter lifetimes are handled correctly, which is taken
    // care of by the function: \c deleteInstruction.
    FullApplySite applySite(cast<ApplyInst>(inst));
    return isReadOnlyConstantEvaluableCall(applySite);
  }
  default: {
    return false;
  }
  }
}

bool InstructionDeleter::trackIfDead(SILInstruction *inst) {
  bool fixLifetime = inst->getFunction()->hasOwnership();
  if (isInstructionTriviallyDead(inst)
      || isScopeAffectingInstructionDead(inst, fixLifetime)) {
    assert(!isIncidentalUse(inst) && !isa<DestroyValueInst>(inst) &&
           "Incidental uses cannot be removed in isolation. "
           "They would be removed iff the operand is dead");
    getCallbacks().notifyWillBeDeleted(inst);
    deadInstructions.insert(inst);
    return true;
  }
  return false;
}

void InstructionDeleter::forceTrackAsDead(SILInstruction *inst) {
  bool preserveDebugInfo = inst->getFunction()->getEffectiveOptimizationMode()
                           <= OptimizationMode::NoOptimization;
  assert(hasOnlyIncidentalUses(inst, preserveDebugInfo));
  getCallbacks().notifyWillBeDeleted(inst);
  deadInstructions.insert(inst);
}

/// Force-delete \p inst and all its uses.
///
/// \p fixLifetimes causes new destroys to be inserted after dropping
/// operands.
///
/// \p forceDeleteUsers allows deleting an instruction with non-incidental,
/// non-destroy uses, such as a store.
///
/// Does not call callbacks.notifyWillBeDeleted for \p inst. But does
/// for any other instructions that become dead as a result.
///
/// Carefully orchestrated steps for deleting an instruction with its uses:
///
/// Recursively gather the instruction's uses into the toDeleteInsts set and
/// dropping the operand for each use traversed.
///
/// For the remaining operands, insert destroys for consuming operands and track
/// newly dead operand definitions.
///
/// Finally, erase the instruction.
void InstructionDeleter::deleteWithUses(SILInstruction *inst, bool fixLifetimes,
                                        bool forceDeleteUsers) {
  // Cannot fix operand lifetimes in non-ownership SIL.
  assert(!fixLifetimes || inst->getFunction()->hasOwnership());

  // Recursively visit all uses while growing toDeleteInsts in def-use order and
  // dropping dead operands.
  SmallVector<SILInstruction *, 4> toDeleteInsts;

  toDeleteInsts.push_back(inst);
  swift::salvageDebugInfo(inst);
  for (unsigned idx = 0; idx < toDeleteInsts.size(); ++idx) {
    for (SILValue result : toDeleteInsts[idx]->getResults()) {
      // Temporary use vector to avoid iterator invalidation.
      auto uses = llvm::to_vector<4>(result->getUses());
      for (Operand *use : uses) {
        SILInstruction *user = use->getUser();
        assert(forceDeleteUsers || isIncidentalUse(user)
               || isa<DestroyValueInst>(user) || isa<DeallocStackInst>(user));
        assert(!isa<BranchInst>(user) && "can't delete phis");

        toDeleteInsts.push_back(user);
        swift::salvageDebugInfo(user);
        use->drop();
      }
    }
  }
  // Process the remaining operands. Insert destroys for consuming
  // operands. Track newly dead operand values. Instructions with multiple dead
  // operands may occur in toDeleteInsts multiple times.
  for (auto *inst : toDeleteInsts) {
    if (inst->isDeleted())
      continue;

    for (Operand &operand : inst->getAllOperands()) {
      SILValue operandValue = operand.get();
      // Check for dead operands, which are dropped above.
      if (!operandValue)
        continue;

      if (fixLifetimes && operand.isConsuming()) {
        SILBuilderWithScope builder(inst);
        auto *dvi = builder.createDestroyValue(inst->getLoc(), operandValue);
        getCallbacks().createdNewInst(dvi);
      }
      auto *operDef = operandValue->getDefiningInstruction();
      operand.drop();
      if (operDef) {
        trackIfDead(operDef);
      }
    }
    inst->dropNonOperandReferences();
    deadInstructions.remove(inst);
    getCallbacks().deleteInst(inst, false /*notify when deleting*/);
  }
}

void InstructionDeleter::cleanupDeadInstructions() {
  while (!deadInstructions.empty()) {
    SmallVector<SILInstruction *, 8> currentDeadInsts(deadInstructions.begin(),
                                                      deadInstructions.end());
    // Though deadInstructions is cleared here, calls to deleteInstruction may
    // append to deadInstructions. So we need to iterate until this it is empty.
    deadInstructions.clear();
    for (SILInstruction *deadInst : currentDeadInsts) {
      if (deadInst->isDeleted())
        continue;

      // deadInst will not have been deleted in the previous iterations,
      // because, by definition, deleteInstruction will only delete an earlier
      // instruction and its incidental/destroy uses. The former cannot be
      // deadInst as deadInstructions is a set vector, and the latter cannot be
      // in deadInstructions as they are incidental uses which are never added
      // to deadInstructions.
      deleteWithUses(deadInst,
                     /*fixLifetimes*/ deadInst->getFunction()->hasOwnership());
    }
  }
}

bool InstructionDeleter::deleteIfDead(SILInstruction *inst) {
  bool fixLifetime = inst->getFunction()->hasOwnership();
  return deleteIfDead(inst, fixLifetime);
}

bool InstructionDeleter::deleteIfDead(SILInstruction *inst, bool fixLifetime) {
  if (isInstructionTriviallyDead(inst)
      || isScopeAffectingInstructionDead(inst, fixLifetime)) {
    getCallbacks().notifyWillBeDeleted(inst);
    deleteWithUses(inst, fixLifetime);
    return true;
  }
  return false;
}

void InstructionDeleter::forceDeleteAndFixLifetimes(SILInstruction *inst) {
  SILFunction *fun = inst->getFunction();
  bool preserveDebugInfo =
      fun->getEffectiveOptimizationMode() <= OptimizationMode::NoOptimization;
  assert(hasOnlyIncidentalUses(inst, preserveDebugInfo));
  deleteWithUses(inst, /*fixLifetimes*/ fun->hasOwnership());
}

void InstructionDeleter::forceDelete(SILInstruction *inst) {
  bool preserveDebugInfo = inst->getFunction()->getEffectiveOptimizationMode()
                           <= OptimizationMode::NoOptimization;
  assert(hasOnlyIncidentalUses(inst, preserveDebugInfo));
  deleteWithUses(inst, /*fixLifetimes*/ false);
}

void InstructionDeleter::recursivelyDeleteUsersIfDead(SILInstruction *inst) {
  SmallVector<SILInstruction *, 8> users;
  for (SILValue result : inst->getResults())
    for (Operand *use : result->getUses())
      users.push_back(use->getUser());

  for (SILInstruction *user : users)
    recursivelyDeleteUsersIfDead(user);
  deleteIfDead(inst);
}

void InstructionDeleter::recursivelyForceDeleteUsersAndFixLifetimes(
    SILInstruction *inst) {
  for (SILValue result : inst->getResults()) {
    while (!result->use_empty()) {
      SILInstruction *user = result->use_begin()->getUser();
      recursivelyForceDeleteUsersAndFixLifetimes(user);
    }
  }
  if (isIncidentalUse(inst) || isa<DestroyValueInst>(inst)) {
    forceDelete(inst);
    return;
  }
  forceDeleteAndFixLifetimes(inst);
}

void swift::eliminateDeadInstruction(SILInstruction *inst,
                                     InstModCallbacks callbacks) {
  InstructionDeleter deleter(std::move(callbacks));
  deleter.trackIfDead(inst);
  deleter.cleanupDeadInstructions();
}

void swift::recursivelyDeleteTriviallyDeadInstructions(
    ArrayRef<SILInstruction *> ia, bool force, InstModCallbacks callbacks) {
  // Delete these instruction and others that become dead after it's deleted.
  llvm::SmallPtrSet<SILInstruction *, 8> deadInsts;
  for (auto *inst : ia) {
    // If the instruction is not dead and force is false, do nothing.
    if (force || isInstructionTriviallyDead(inst))
      deadInsts.insert(inst);
  }
  llvm::SmallPtrSet<SILInstruction *, 8> nextInsts;
  while (!deadInsts.empty()) {
    for (auto inst : deadInsts) {
      // Call the callback before we mutate the to be deleted instruction in any
      // way.
      callbacks.notifyWillBeDeleted(inst);

      // Check if any of the operands will become dead as well.
      MutableArrayRef<Operand> operands = inst->getAllOperands();
      for (Operand &operand : operands) {
        SILValue operandVal = operand.get();
        if (!operandVal)
          continue;

        // Remove the reference from the instruction being deleted to this
        // operand.
        operand.drop();

        // If the operand is an instruction that is only used by the instruction
        // being deleted, delete it.
        if (auto *operandValInst = operandVal->getDefiningInstruction())
          if (!deadInsts.count(operandValInst) &&
              isInstructionTriviallyDead(operandValInst))
            nextInsts.insert(operandValInst);
      }

      // If we have a function ref inst, we need to especially drop its function
      // argument so that it gets a proper ref decrement.
      if (auto *fri = dyn_cast<FunctionRefBaseInst>(inst))
        fri->dropReferencedFunction();
    }

    for (auto inst : deadInsts) {
      // This will remove this instruction and all its uses.
      eraseFromParentWithDebugInsts(inst, callbacks);
    }

    nextInsts.swap(deadInsts);
    nextInsts.clear();
  }
}

