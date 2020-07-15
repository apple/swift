//===--- InstOptUtils.cpp - SILOptimizer instruction utilities ------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/ConstExpr.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include <deque>

using namespace swift;

static llvm::cl::opt<bool> EnableExpandAll("enable-expand-all",
                                           llvm::cl::init(false));

/// Creates an increment on \p Ptr before insertion point \p InsertPt that
/// creates a strong_retain if \p Ptr has reference semantics itself or a
/// retain_value if \p Ptr is a non-trivial value without reference-semantics.
NullablePtr<SILInstruction>
swift::createIncrementBefore(SILValue ptr, SILInstruction *insertPt) {
  // Set up the builder we use to insert at our insertion point.
  SILBuilder builder(insertPt);
  auto loc = insertPt->getLoc();

  // If we have a trivial type, just bail, there is no work to do.
  if (ptr->getType().isTrivial(builder.getFunction()))
    return nullptr;

  // If Ptr is refcounted itself, create the strong_retain and
  // return.
  if (ptr->getType().isReferenceCounted(builder.getModule())) {
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
    if (ptr->getType().is<Name##StorageType>())                                \
      return builder.create##Name##Retain(loc, ptr,                            \
                                          builder.getDefaultAtomicity());
#include "swift/AST/ReferenceStorage.def"

    return builder.createStrongRetain(loc, ptr,
                                      builder.getDefaultAtomicity());
  }

  // Otherwise, create the retain_value.
  return builder.createRetainValue(loc, ptr, builder.getDefaultAtomicity());
}

/// Creates a decrement on \p ptr before insertion point \p InsertPt that
/// creates a strong_release if \p ptr has reference semantics itself or
/// a release_value if \p ptr is a non-trivial value without
/// reference-semantics.
NullablePtr<SILInstruction>
swift::createDecrementBefore(SILValue ptr, SILInstruction *insertPt) {
  // Setup the builder we will use to insert at our insertion point.
  SILBuilder builder(insertPt);
  auto loc = insertPt->getLoc();

  if (ptr->getType().isTrivial(builder.getFunction()))
    return nullptr;

  // If ptr has reference semantics itself, create a strong_release.
  if (ptr->getType().isReferenceCounted(builder.getModule())) {
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
    if (ptr->getType().is<Name##StorageType>())                                \
      return builder.create##Name##Release(loc, ptr,                           \
                                          builder.getDefaultAtomicity());
#include "swift/AST/ReferenceStorage.def"

    return builder.createStrongRelease(loc, ptr,
                                       builder.getDefaultAtomicity());
  }

  // Otherwise create a release value.
  return builder.createReleaseValue(loc, ptr, builder.getDefaultAtomicity());
}

/// Perform a fast local check to see if the instruction is dead.
///
/// This routine only examines the state of the instruction at hand.
bool swift::isInstructionTriviallyDead(SILInstruction *inst) {
  // At Onone, consider all uses, including the debug_info.
  // This way, debug_info is preserved at Onone.
  if (inst->hasUsesOfAnyResult()
      && inst->getFunction()->getEffectiveOptimizationMode()
             <= OptimizationMode::NoOptimization)
    return false;

  if (!onlyHaveDebugUsesOfAllResults(inst) || isa<TermInst>(inst))
    return false;

  if (auto *bi = dyn_cast<BuiltinInst>(inst)) {
    // Although the onFastPath builtin has no side-effects we don't want to
    // remove it.
    if (bi->getBuiltinInfo().ID == BuiltinValueKind::OnFastPath)
      return false;
    return !bi->mayHaveSideEffects();
  }

  // condfail instructions that obviously can't fail are dead.
  if (auto *cfi = dyn_cast<CondFailInst>(inst))
    if (auto *ili = dyn_cast<IntegerLiteralInst>(cfi->getOperand()))
      if (!ili->getValue())
        return true;

  // mark_uninitialized is never dead.
  if (isa<MarkUninitializedInst>(inst))
    return false;

  if (isa<DebugValueInst>(inst) || isa<DebugValueAddrInst>(inst))
    return false;

  // These invalidate enums so "write" memory, but that is not an essential
  // operation so we can remove these if they are trivially dead.
  if (isa<UncheckedTakeEnumDataAddrInst>(inst))
    return true;

  if (!inst->mayHaveSideEffects())
    return true;

  return false;
}

/// Return true if this is a release instruction and the released value
/// is a part of a guaranteed parameter.
bool swift::isIntermediateRelease(SILInstruction *inst,
                                  EpilogueARCFunctionInfo *eafi) {
  // Check whether this is a release instruction.
  if (!isa<StrongReleaseInst>(inst) && !isa<ReleaseValueInst>(inst))
    return false;

  // OK. we have a release instruction.
  // Check whether this is a release on part of a guaranteed function argument.
  SILValue Op = stripValueProjections(inst->getOperand(0));
  auto *arg = dyn_cast<SILFunctionArgument>(Op);
  if (!arg)
    return false;

  // This is a release on a guaranteed parameter. Its not the final release.
  if (arg->hasConvention(SILArgumentConvention::Direct_Guaranteed))
    return true;

  // This is a release on an owned parameter and its not the epilogue release.
  // Its not the final release.
  auto rel = eafi->computeEpilogueARCInstructions(
      EpilogueARCContext::EpilogueARCKind::Release, arg);
  if (rel.size() && !rel.count(inst))
    return true;

  // Failed to prove anything.
  return false;
}

static bool hasOnlyEndOfScopeOrDestroyUses(SILInstruction *inst) {
  for (SILValue result : inst->getResults()) {
    for (Operand *use : result->getUses()) {
      SILInstruction *user = use->getUser();
      bool isDebugUser = user->isDebugInstruction();
      if (!isa<DestroyValueInst>(user) && !isEndOfScopeMarker(user) &&
          !isDebugUser)
        return false;
      // Include debug uses only in Onone mode.
      if (isDebugUser && inst->getFunction()->getEffectiveOptimizationMode() <=
                             OptimizationMode::NoOptimization)
        return false;
    }
  }
  return true;
}

unsigned swift::getNumInOutArguments(FullApplySite applySite) {
  assert(applySite);
  auto substConv = applySite.getSubstCalleeConv();
  unsigned numIndirectResults = substConv.getNumIndirectSILResults();
  unsigned numInOutArguments = 0;
  for (unsigned argIndex = 0; argIndex < applySite.getNumArguments();
       argIndex++) {
    // Skip indirect results.
    if (argIndex < numIndirectResults) {
      continue;
    }
    auto paramNumber = argIndex - numIndirectResults;
    auto ParamConvention =
        substConv.getParameters()[paramNumber].getConvention();
    switch (ParamConvention) {
    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Indirect_InoutAliasable: {
      ++numInOutArguments;
      break;
    default:
      break;
    }
    }
  }
  return numInOutArguments;
}

/// Return true iff the \p applySite calls a constant-evaluable function and
/// it is non-generic and read/destroy only, which means that the call can do
/// only the following and nothing else:
///   (1) The call may read any memory location.
///   (2) The call may destroy owned parameters i.e., consume them.
///   (3) The call may write into memory locations newly created by the call.
///   (4) The call may use assertions, which traps at runtime on failure.
///   (5) The call may return a non-generic value.
/// Essentially, these are calls whose "effect" is visible only in their return
/// value or through the parameters that are destroyed. The return value
/// is also guaranteed to have value semantics as it is non-generic and
/// reference semantics is not constant evaluable.
static bool isNonGenericReadOnlyConstantEvaluableCall(FullApplySite applySite) {
  assert(applySite);
  SILFunction *callee = applySite.getCalleeFunction();
  if (!callee || !isConstantEvaluable(callee)) {
    return false;
  }
  return !applySite.hasSubstitutions() && !getNumInOutArguments(applySite) &&
         !applySite.getNumIndirectSILResults();
}

/// A scope-affecting instruction is an instruction which may end the scope of
/// its operand or may produce scoped results that require cleaning up. E.g.
/// begin_borrow, begin_access, copy_value, a call that produces a owned value
/// are scoped instructions. The scope of the results of the first two
/// instructions end with an end_borrow/acess instruction, while those of the
/// latter two end with a consuming operation like destroy_value instruction.
/// These instruction may also end the scope of its operand e.g. a call could
/// consume owned arguments thereby ending its scope. Dead-code eliminating a
/// scope-affecting instruction requires fixing the lifetime of the non-trivial
/// operands of the instruction and requires cleaning up the end-of-scope uses
/// of non-trivial results.
///
/// \param inst instruction that checked for liveness.
static bool isScopeAffectingInstructionDead(SILInstruction *inst) {
  SILFunction *fun = inst->getFunction();
  assert(fun && "Instruction has no function.");
  // Only support ownership SIL for scoped instructions.
  if (!fun->hasOwnership()) {
    return false;
  }
  // If the instruction has any use other than end of scope use or destroy_value
  // use, bail out.
  if (!hasOnlyEndOfScopeOrDestroyUses(inst)) {
    return false;
  }
  // If inst is a copy or beginning of scope, inst is dead, since we know that
  // it is used only in a destroy_value or end-of-scope instruction.
  if (getSingleValueCopyOrCast(inst))
    return true;

  switch (inst->getKind()) {
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
    // Partial applies that are only used in destroys cannot have any effect on
    // the program state, provided the values they capture are explicitly
    // destroyed.
    return true;
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
    return isNonGenericReadOnlyConstantEvaluableCall(applySite);
  }
  default: {
    return false;
  }
  }
}

void InstructionDeleter::trackIfDead(SILInstruction *inst) {
  if (isInstructionTriviallyDead(inst) ||
      isScopeAffectingInstructionDead(inst)) {
    assert(!isIncidentalUse(inst) && !isa<DestroyValueInst>(inst) &&
           "Incidental uses cannot be removed in isolation. "
           "They would be removed iff the operand is dead");
    deadInstructions.insert(inst);
  }
}

/// Given an \p operand that belongs to an instruction that will be removed,
/// destroy the operand just before the instruction, if the instruction consumes
/// \p operand. This function will result in a double consume, which is expected
/// to be resolved when the caller deletes the original instruction. This
/// function works only on ownership SIL.
static void destroyConsumedOperandOfDeadInst(Operand &operand) {
  assert(operand.get() && operand.getUser());
  SILInstruction *deadInst = operand.getUser();
  SILFunction *fun = deadInst->getFunction();
  assert(fun->hasOwnership());

  SILValue operandValue = operand.get();
  if (operandValue->getType().isTrivial(*fun))
    return;
  // Ignore type-dependent operands which are not real operands but are just
  // there to create use-def dependencies.
  if (deadInst->isTypeDependentOperand(operand))
    return;
  // A scope ending instruction cannot be deleted in isolation without removing
  // the instruction defining its operand as well.
  assert(!isEndOfScopeMarker(deadInst) && !isa<DestroyValueInst>(deadInst) &&
         !isa<DestroyAddrInst>(deadInst) &&
         "lifetime ending instruction is deleted without its operand");
  if (operand.isConsumingUse()) {
    // Since deadInst cannot be an end-of-scope instruction (asserted above),
    // this must be a consuming use of an owned value.
    assert(operandValue.getOwnershipKind() == ValueOwnershipKind::Owned);
    SILBuilderWithScope builder(deadInst);
    builder.emitDestroyValueOperation(deadInst->getLoc(), operandValue);
  }
}

namespace {
using CallbackTy = llvm::function_ref<void(SILInstruction *)>;
} // namespace

void InstructionDeleter::deleteInstruction(SILInstruction *inst,
                                           CallbackTy callback,
                                           bool fixOperandLifetimes) {
  // We cannot fix operand lifetimes in non-ownership SIL.
  assert(!fixOperandLifetimes || inst->getFunction()->hasOwnership());
  // Collect instruction and its immediate uses and check if they are all
  // incidental uses. Also, invoke the callback on the instruction and its uses.
  // Note that the Callback is invoked before deleting anything to ensure that
  // the SIL is valid at the time of the callback.
  SmallVector<SILInstruction *, 4> toDeleteInsts;
  toDeleteInsts.push_back(inst);
  callback(inst);
  for (SILValue result : inst->getResults()) {
    for (Operand *use : result->getUses()) {
      SILInstruction *user = use->getUser();
      assert(isIncidentalUse(user) || isa<DestroyValueInst>(user));
      callback(user);
      toDeleteInsts.push_back(user);
    }
  }
  // Record definitions of instruction's operands. Also, in case an operand is
  // consumed by inst, emit necessary compensation code.
  SmallVector<SILInstruction *, 4> operandDefinitions;
  for (Operand &operand : inst->getAllOperands()) {
    SILValue operandValue = operand.get();
    assert(operandValue &&
           "Instruction's operand are deleted before the instruction");
    SILInstruction *defInst = operandValue->getDefiningInstruction();
    // If the operand has a defining instruction, it could be potentially
    // dead. Therefore, record the definition.
    if (defInst)
      operandDefinitions.push_back(defInst);
    // The scope of the operand could be ended by inst. Therefore, emit
    // any compensating code needed to end the scope of the operand value
    // once inst is deleted.
    if (fixOperandLifetimes)
      destroyConsumedOperandOfDeadInst(operand);
  }
  // First drop all references from all instructions to be deleted and then
  // erase the instruction. Note that this is done in this order so that when an
  // instruction is deleted, its uses would have dropped their references.
  // Note that the toDeleteInsts must also be removed from the tracked
  // deadInstructions.
  for (SILInstruction *inst : toDeleteInsts) {
    deadInstructions.remove(inst);
    inst->dropAllReferences();
  }
  for (SILInstruction *inst : toDeleteInsts) {
    inst->eraseFromParent();
  }
  // Record operand definitions that become dead now.
  for (SILInstruction *operandValInst : operandDefinitions) {
    trackIfDead(operandValInst);
  }
}

void InstructionDeleter::cleanUpDeadInstructions(CallbackTy callback) {
  SILFunction *fun = nullptr;
  if (!deadInstructions.empty())
    fun = deadInstructions.front()->getFunction();
  while (!deadInstructions.empty()) {
    SmallVector<SILInstruction *, 8> currentDeadInsts(deadInstructions.begin(),
                                                      deadInstructions.end());
    // Though deadInstructions is cleared here, calls to deleteInstruction may
    // append to deadInstructions. So we need to iterate until this it is empty.
    deadInstructions.clear();
    for (SILInstruction *deadInst : currentDeadInsts) {
      // deadInst will not have been deleted in the previous iterations,
      // because, by definition, deleteInstruction will only delete an earlier
      // instruction and its incidental/destroy uses. The former cannot be
      // deadInst as deadInstructions is a set vector, and the latter cannot be
      // in deadInstructions as they are incidental uses which are never added
      // to deadInstructions.
      deleteInstruction(deadInst, callback, /*Fix lifetime of operands*/
                        fun->hasOwnership());
    }
  }
}

static bool hasOnlyIncidentalUses(SILInstruction *inst,
                                  bool disallowDebugUses = false) {
  for (SILValue result : inst->getResults()) {
    for (Operand *use : result->getUses()) {
      SILInstruction *user = use->getUser();
      if (!isIncidentalUse(user))
        return false;
      if (disallowDebugUses && user->isDebugInstruction())
        return false;
    }
  }
  return true;
}

void InstructionDeleter::deleteIfDead(SILInstruction *inst,
                                      CallbackTy callback) {
  if (isInstructionTriviallyDead(inst) ||
      isScopeAffectingInstructionDead(inst)) {
    deleteInstruction(inst, callback,
      /*Fix lifetime of operands*/ inst->getFunction()->hasOwnership());
  }
}

void InstructionDeleter::forceDeleteAndFixLifetimes(SILInstruction *inst,
                                                    CallbackTy callback) {
  SILFunction *fun = inst->getFunction();
  assert(fun->hasOwnership());
  bool disallowDebugUses =
      fun->getEffectiveOptimizationMode() <= OptimizationMode::NoOptimization;
  assert(hasOnlyIncidentalUses(inst, disallowDebugUses));
  deleteInstruction(inst, callback, /*Fix lifetime of operands*/ true);
}

void InstructionDeleter::forceDelete(SILInstruction *inst,
                                     CallbackTy callback) {
  bool disallowDebugUses =
      inst->getFunction()->getEffectiveOptimizationMode() <=
      OptimizationMode::NoOptimization;
  assert(hasOnlyIncidentalUses(inst, disallowDebugUses));
  deleteInstruction(inst, callback, /*Fix lifetime of operands*/ false);
}

void InstructionDeleter::recursivelyDeleteUsersIfDead(SILInstruction *inst,
                                                      CallbackTy callback) {
  SmallVector<SILInstruction *, 8> users;
  for (SILValue result : inst->getResults())
    for (Operand *use : result->getUses())
      users.push_back(use->getUser());

  for (SILInstruction *user : users)
    recursivelyDeleteUsersIfDead(user, callback);
  deleteIfDead(inst, callback);
}

void InstructionDeleter::recursivelyForceDeleteUsersAndFixLifetimes(
    SILInstruction *inst, CallbackTy callback) {
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
                                     CallbackTy callback) {
  InstructionDeleter deleter;
  deleter.trackIfDead(inst);
  deleter.cleanUpDeadInstructions(callback);
}

void swift::recursivelyDeleteTriviallyDeadInstructions(
    ArrayRef<SILInstruction *> ia, bool force, CallbackTy callback) {
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
      callback(inst);

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
      auto *fri = dyn_cast<FunctionRefInst>(inst);
      if (fri && fri->getInitiallyReferencedFunction())
        fri->dropReferencedFunction();

      auto *dfri = dyn_cast<DynamicFunctionRefInst>(inst);
      if (dfri && dfri->getInitiallyReferencedFunction())
        dfri->dropReferencedFunction();

      auto *pfri = dyn_cast<PreviousDynamicFunctionRefInst>(inst);
      if (pfri && pfri->getInitiallyReferencedFunction())
        pfri->dropReferencedFunction();
    }

    for (auto inst : deadInsts) {
      // This will remove this instruction and all its uses.
      eraseFromParentWithDebugInsts(inst, callback);
    }

    nextInsts.swap(deadInsts);
    nextInsts.clear();
  }
}

/// If the given instruction is dead, delete it along with its dead
/// operands.
///
/// \param inst The instruction to be deleted.
/// \param force If force is set, don't check if the top level instruction is
///        considered dead - delete it regardless.
void swift::recursivelyDeleteTriviallyDeadInstructions(SILInstruction *inst,
                                                       bool force,
                                                       CallbackTy callback) {
  ArrayRef<SILInstruction *> ai = ArrayRef<SILInstruction *>(inst);
  recursivelyDeleteTriviallyDeadInstructions(ai, force, callback);
}

void swift::eraseUsesOfInstruction(SILInstruction *inst, CallbackTy callback) {
  for (auto result : inst->getResults()) {
    while (!result->use_empty()) {
      auto ui = result->use_begin();
      auto *user = ui->getUser();
      assert(user && "User should never be NULL!");

      // If the instruction itself has any uses, recursively zap them so that
      // nothing uses this instruction.
      eraseUsesOfInstruction(user, callback);

      // Walk through the operand list and delete any random instructions that
      // will become trivially dead when this instruction is removed.

      for (auto &operand : user->getAllOperands()) {
        if (auto *operandI = operand.get()->getDefiningInstruction()) {
          // Don't recursively delete the instruction we're working on.
          // FIXME: what if we're being recursively invoked?
          if (operandI != inst) {
            operand.drop();
            recursivelyDeleteTriviallyDeadInstructions(operandI, false,
                                                       callback);
          }
        }
      }
      callback(user);
      user->eraseFromParent();
    }
  }
}

void swift::collectUsesOfValue(SILValue v,
                               llvm::SmallPtrSetImpl<SILInstruction *> &insts) {
  for (auto ui = v->use_begin(), E = v->use_end(); ui != E; ++ui) {
    auto *user = ui->getUser();
    // Instruction has been processed.
    if (!insts.insert(user).second)
      continue;

    // Collect the users of this instruction.
    for (auto result : user->getResults())
      collectUsesOfValue(result, insts);
  }
}

void swift::eraseUsesOfValue(SILValue v) {
  llvm::SmallPtrSet<SILInstruction *, 4> insts;
  // Collect the uses.
  collectUsesOfValue(v, insts);
  // Erase the uses, we can have instructions that become dead because
  // of the removal of these instructions, leave to DCE to cleanup.
  // Its not safe to do recursively delete here as some of the SILInstruction
  // maybe tracked by this set.
  for (auto inst : insts) {
    inst->replaceAllUsesOfAllResultsWithUndef();
    inst->eraseFromParent();
  }
}

// Devirtualization of functions with covariant return types produces
// a result that is not an apply, but takes an apply as an
// argument. Attempt to dig the apply out from this result.
FullApplySite swift::findApplyFromDevirtualizedResult(SILValue v) {
  if (auto Apply = FullApplySite::isa(v))
    return Apply;

  if (isa<UpcastInst>(v) || isa<EnumInst>(v) || isa<UncheckedRefCastInst>(v))
    return findApplyFromDevirtualizedResult(
        cast<SingleValueInstruction>(v)->getOperand(0));

  return FullApplySite();
}

bool swift::mayBindDynamicSelf(SILFunction *F) {
  if (!F->hasSelfMetadataParam())
    return false;

  SILValue mdArg = F->getSelfMetadataArgument();

  for (Operand *mdUse : F->getSelfMetadataArgument()->getUses()) {
    SILInstruction *mdUser = mdUse->getUser();
    for (Operand &typeDepOp : mdUser->getTypeDependentOperands()) {
      if (typeDepOp.get() == mdArg)
        return true;
    }
  }
  return false;
}

static SILValue skipAddrProjections(SILValue v) {
  for (;;) {
    switch (v->getKind()) {
    case ValueKind::IndexAddrInst:
    case ValueKind::IndexRawPointerInst:
    case ValueKind::StructElementAddrInst:
    case ValueKind::TupleElementAddrInst:
      v = cast<SingleValueInstruction>(v)->getOperand(0);
      break;
    default:
      return v;
    }
  }
  llvm_unreachable("there is no escape from an infinite loop");
}

/// Check whether the \p addr is an address of a tail-allocated array element.
bool swift::isAddressOfArrayElement(SILValue addr) {
  addr = stripAddressProjections(addr);
  if (auto *md = dyn_cast<MarkDependenceInst>(addr))
    addr = stripAddressProjections(md->getValue());

  // High-level SIL: check for an get_element_address array semantics call.
  if (auto *ptrToAddr = dyn_cast<PointerToAddressInst>(addr))
    if (auto *sei = dyn_cast<StructExtractInst>(ptrToAddr->getOperand())) {
      ArraySemanticsCall call(sei->getOperand());
      if (call && call.getKind() == ArrayCallKind::kGetElementAddress)
        return true;
    }

  // Check for an tail-address (of an array buffer object).
  if (isa<RefTailAddrInst>(skipAddrProjections(addr)))
    return true;

  return false;
}

/// Find a new position for an ApplyInst's FuncRef so that it dominates its
/// use. Not that FunctionRefInsts may be shared by multiple ApplyInsts.
void swift::placeFuncRef(ApplyInst *ai, DominanceInfo *domInfo) {
  FunctionRefInst *funcRef = cast<FunctionRefInst>(ai->getCallee());
  SILBasicBlock *domBB = domInfo->findNearestCommonDominator(
      ai->getParent(), funcRef->getParent());
  if (domBB == ai->getParent() && domBB != funcRef->getParent())
    // Prefer to place the FuncRef immediately before the call. Since we're
    // moving FuncRef up, this must be the only call to it in the block.
    funcRef->moveBefore(ai);
  else
    // Otherwise, conservatively stick it at the beginning of the block.
    funcRef->moveBefore(&*domBB->begin());
}

/// Add an argument, \p val, to the branch-edge that is pointing into
/// block \p Dest. Return a new instruction and do not erase the old
/// instruction.
TermInst *swift::addArgumentToBranch(SILValue val, SILBasicBlock *dest,
                                     TermInst *branch) {
  SILBuilderWithScope builder(branch);

  if (auto *cbi = dyn_cast<CondBranchInst>(branch)) {
    SmallVector<SILValue, 8> trueArgs;
    SmallVector<SILValue, 8> falseArgs;

    for (auto arg : cbi->getTrueArgs())
      trueArgs.push_back(arg);

    for (auto arg : cbi->getFalseArgs())
      falseArgs.push_back(arg);

    if (dest == cbi->getTrueBB()) {
      trueArgs.push_back(val);
      assert(trueArgs.size() == dest->getNumArguments());
    } else {
      falseArgs.push_back(val);
      assert(falseArgs.size() == dest->getNumArguments());
    }

    return builder.createCondBranch(
        cbi->getLoc(), cbi->getCondition(), cbi->getTrueBB(), trueArgs,
        cbi->getFalseBB(), falseArgs, cbi->getTrueBBCount(),
        cbi->getFalseBBCount());
  }

  if (auto *bi = dyn_cast<BranchInst>(branch)) {
    SmallVector<SILValue, 8> args;

    for (auto arg : bi->getArgs())
      args.push_back(arg);

    args.push_back(val);
    assert(args.size() == dest->getNumArguments());
    return builder.createBranch(bi->getLoc(), bi->getDestBB(), args);
  }

  llvm_unreachable("unsupported terminator");
}

SILLinkage swift::getSpecializedLinkage(SILFunction *f, SILLinkage linkage) {
  if (hasPrivateVisibility(linkage) && !f->isSerialized()) {
    // Specializations of private symbols should remain so, unless
    // they were serialized, which can only happen when specializing
    // definitions from a standard library built with -sil-serialize-all.
    return SILLinkage::Private;
  }

  return SILLinkage::Shared;
}

/// Cast a value into the expected, ABI compatible type if necessary.
/// This may happen e.g. when:
/// - a type of the return value is a subclass of the expected return type.
/// - actual return type and expected return type differ in optionality.
/// - both types are tuple-types and some of the elements need to be casted.
/// Return the cast value and true if a CFG modification was required
/// NOTE: We intentionally combine the checking of the cast's handling
/// possibility and the transformation performing the cast in the same function,
/// to avoid any divergence between the check and the implementation in the
/// future.
///
/// NOTE: The implementation of this function is very closely related to the
/// rules checked by SILVerifier::requireABICompatibleFunctionTypes.
std::pair<SILValue, bool /* changedCFG */>
swift::castValueToABICompatibleType(SILBuilder *builder, SILLocation loc,
                                    SILValue value, SILType srcTy,
                                    SILType destTy) {

  // No cast is required if types are the same.
  if (srcTy == destTy)
    return {value, false};

  if (srcTy.isAddress() && destTy.isAddress()) {
    // Cast between two addresses and that's it.
    return {builder->createUncheckedAddrCast(loc, value, destTy), false};
  }

  // If both types are classes and dest is the superclass of src,
  // simply perform an upcast.
  if (destTy.isExactSuperclassOf(srcTy)) {
    return {builder->createUpcast(loc, value, destTy), false};
  }

  if (srcTy.isHeapObjectReferenceType() && destTy.isHeapObjectReferenceType()) {
    return {builder->createUncheckedRefCast(loc, value, destTy), false};
  }

  if (auto mt1 = srcTy.getAs<AnyMetatypeType>()) {
    if (auto mt2 = destTy.getAs<AnyMetatypeType>()) {
      if (mt1->getRepresentation() == mt2->getRepresentation()) {
        // If builder.Type needs to be casted to A.Type and
        // A is a superclass of builder, then it can be done by means
        // of a simple upcast.
        if (mt2.getInstanceType()->isExactSuperclassOf(mt1.getInstanceType())) {
          return {builder->createUpcast(loc, value, destTy), false};
        }

        // Cast between two metatypes and that's it.
        return {builder->createUncheckedReinterpretCast(loc, value, destTy),
                false};
      }
    }
  }

  // Check if src and dest types are optional.
  auto optionalSrcTy = srcTy.getOptionalObjectType();
  auto optionalDestTy = destTy.getOptionalObjectType();

  // Both types are optional.
  if (optionalDestTy && optionalSrcTy) {
    // If both wrapped types are classes and dest is the superclass of src,
    // simply perform an upcast.
    if (optionalDestTy.isExactSuperclassOf(optionalSrcTy)) {
      // Insert upcast.
      return {builder->createUpcast(loc, value, destTy), false};
    }

    // Unwrap the original optional value.
    auto *someDecl = builder->getASTContext().getOptionalSomeDecl();
    auto *noneBB = builder->getFunction().createBasicBlock();
    auto *someBB = builder->getFunction().createBasicBlock();
    auto *curBB = builder->getInsertionPoint()->getParent();

    auto *contBB = curBB->split(builder->getInsertionPoint());
    contBB->createPhiArgument(destTy, ValueOwnershipKind::Owned);

    SmallVector<std::pair<EnumElementDecl *, SILBasicBlock *>, 1> caseBBs;
    caseBBs.push_back(std::make_pair(someDecl, someBB));
    builder->setInsertionPoint(curBB);
    builder->createSwitchEnum(loc, value, noneBB, caseBBs);

    // Handle the Some case.
    builder->setInsertionPoint(someBB);
    SILValue unwrappedValue =
        builder->createUncheckedEnumData(loc, value, someDecl);
    // Cast the unwrapped value.
    SILValue castedUnwrappedValue;
    std::tie(castedUnwrappedValue, std::ignore) = castValueToABICompatibleType(
        builder, loc, unwrappedValue, optionalSrcTy, optionalDestTy);
    // Wrap into optional.
    auto castedValue =
        builder->createOptionalSome(loc, castedUnwrappedValue, destTy);
    builder->createBranch(loc, contBB, {castedValue});

    // Handle the None case.
    builder->setInsertionPoint(noneBB);
    castedValue = builder->createOptionalNone(loc, destTy);
    builder->createBranch(loc, contBB, {castedValue});
    builder->setInsertionPoint(contBB->begin());

    return {contBB->getArgument(0), true};
  }

  // Src is not optional, but dest is optional.
  if (!optionalSrcTy && optionalDestTy) {
    auto optionalSrcCanTy =
        OptionalType::get(srcTy.getASTType())->getCanonicalType();
    auto loweredOptionalSrcType =
        SILType::getPrimitiveObjectType(optionalSrcCanTy);

    // Wrap the source value into an optional first.
    SILValue wrappedValue =
        builder->createOptionalSome(loc, value, loweredOptionalSrcType);
    // Cast the wrapped value.
    return castValueToABICompatibleType(builder, loc, wrappedValue,
                                        wrappedValue->getType(), destTy);
  }

  // Handle tuple types.
  // Extract elements, cast each of them, create a new tuple.
  if (auto srcTupleTy = srcTy.getAs<TupleType>()) {
    SmallVector<SILValue, 8> expectedTuple;
    bool changedCFG = false;
    for (unsigned i = 0, e = srcTupleTy->getNumElements(); i < e; ++i) {
      SILValue element = builder->createTupleExtract(loc, value, i);
      // Cast the value if necessary.
      bool neededCFGChange;
      std::tie(element, neededCFGChange) = castValueToABICompatibleType(
          builder, loc, element, srcTy.getTupleElementType(i),
          destTy.getTupleElementType(i));
      changedCFG |= neededCFGChange;
      expectedTuple.push_back(element);
    }

    return {builder->createTuple(loc, destTy, expectedTuple), changedCFG};
  }

  // Function types are interchangeable if they're also ABI-compatible.
  if (srcTy.is<SILFunctionType>()) {
    if (destTy.is<SILFunctionType>()) {
      assert(srcTy.getAs<SILFunctionType>()->isNoEscape()
                 == destTy.getAs<SILFunctionType>()->isNoEscape()
             || srcTy.getAs<SILFunctionType>()->getRepresentation()
                        != SILFunctionType::Representation::Thick
                    && "Swift thick functions that differ in escapeness are "
                       "not ABI "
                       "compatible");
      // Insert convert_function.
      return {builder->createConvertFunction(loc, value, destTy,
                                             /*WithoutActuallyEscaping=*/false),
              false};
    }
  }

  llvm::errs() << "Source type: " << srcTy << "\n";
  llvm::errs() << "Destination type: " << destTy << "\n";
  llvm_unreachable("Unknown combination of types for casting");
}

ProjectBoxInst *swift::getOrCreateProjectBox(AllocBoxInst *abi,
                                             unsigned index) {
  SILBasicBlock::iterator iter(abi);
  ++iter;
  assert(iter != abi->getParent()->end()
         && "alloc_box cannot be the last instruction of a block");
  SILInstruction *nextInst = &*iter;
  if (auto *pbi = dyn_cast<ProjectBoxInst>(nextInst)) {
    if (pbi->getOperand() == abi && pbi->getFieldIndex() == index)
      return pbi;
  }

  SILBuilder builder(nextInst);
  return builder.createProjectBox(abi->getLoc(), abi, index);
}

// Peek through trivial Enum initialization, typically for pointless
// Optionals.
//
// Given an UncheckedTakeEnumDataAddrInst, check that there are no
// other uses of the Enum value and return the address used to initialized the
// enum's payload:
//
//   %stack_adr = alloc_stack
//   %data_adr  = init_enum_data_addr %stk_adr
//   %enum_adr  = inject_enum_addr %stack_adr
//   %copy_src  = unchecked_take_enum_data_addr %enum_adr
//   dealloc_stack %stack_adr
//   (No other uses of %stack_adr.)
InitEnumDataAddrInst *
swift::findInitAddressForTrivialEnum(UncheckedTakeEnumDataAddrInst *utedai) {
  auto *asi = dyn_cast<AllocStackInst>(utedai->getOperand());
  if (!asi)
    return nullptr;

  SILInstruction *singleUser = nullptr;
  for (auto use : asi->getUses()) {
    auto *user = use->getUser();
    if (user == utedai)
      continue;

    // As long as there's only one UncheckedTakeEnumDataAddrInst and one
    // InitEnumDataAddrInst, we don't care how many InjectEnumAddr and
    // DeallocStack users there are.
    if (isa<InjectEnumAddrInst>(user) || isa<DeallocStackInst>(user))
      continue;

    if (singleUser)
      return nullptr;

    singleUser = user;
  }
  if (!singleUser)
    return nullptr;

  // Assume, without checking, that the returned InitEnumDataAddr dominates the
  // given UncheckedTakeEnumDataAddrInst, because that's how SIL is defined. I
  // don't know where this is actually verified.
  return dyn_cast<InitEnumDataAddrInst>(singleUser);
}

//===----------------------------------------------------------------------===//
//                       String Concatenation Optimizer
//===----------------------------------------------------------------------===//

namespace {
/// This is a helper class that performs optimization of string literals
/// concatenation.
class StringConcatenationOptimizer {
  /// Apply instruction being optimized.
  ApplyInst *ai;
  /// Builder to be used for creation of new instructions.
  SILBuilder &builder;
  /// Left string literal operand of a string concatenation.
  StringLiteralInst *sliLeft = nullptr;
  /// Right string literal operand of a string concatenation.
  StringLiteralInst *sliRight = nullptr;
  /// Function used to construct the left string literal.
  FunctionRefInst *friLeft = nullptr;
  /// Function used to construct the right string literal.
  FunctionRefInst *friRight = nullptr;
  /// Apply instructions used to construct left string literal.
  ApplyInst *aiLeft = nullptr;
  /// Apply instructions used to construct right string literal.
  ApplyInst *aiRight = nullptr;
  /// String literal conversion function to be used.
  FunctionRefInst *friConvertFromBuiltin = nullptr;
  /// Result type of a function producing the concatenated string literal.
  SILValue funcResultType;

  /// Internal helper methods
  bool extractStringConcatOperands();
  void adjustEncodings();
  APInt getConcatenatedLength();
  bool isAscii() const;

public:
  StringConcatenationOptimizer(ApplyInst *ai, SILBuilder &builder)
      : ai(ai), builder(builder) {}

  /// Tries to optimize a given apply instruction if it is a
  /// concatenation of string literals.
  ///
  /// Returns a new instruction if optimization was possible.
  SingleValueInstruction *optimize();
};

} // end anonymous namespace

/// Checks operands of a string concatenation operation to see if
/// optimization is applicable.
///
/// Returns false if optimization is not possible.
/// Returns true and initializes internal fields if optimization is possible.
bool StringConcatenationOptimizer::extractStringConcatOperands() {
  auto *Fn = ai->getReferencedFunctionOrNull();
  if (!Fn)
    return false;

  if (ai->getNumArguments() != 3 || !Fn->hasSemanticsAttr(semantics::STRING_CONCAT))
    return false;

  // Left and right operands of a string concatenation operation.
  aiLeft = dyn_cast<ApplyInst>(ai->getOperand(1));
  aiRight = dyn_cast<ApplyInst>(ai->getOperand(2));

  if (!aiLeft || !aiRight)
    return false;

  friLeft = dyn_cast<FunctionRefInst>(aiLeft->getCallee());
  friRight = dyn_cast<FunctionRefInst>(aiRight->getCallee());

  if (!friLeft || !friRight)
    return false;

  auto *friLeftFun = friLeft->getReferencedFunctionOrNull();
  auto *friRightFun = friRight->getReferencedFunctionOrNull();

  if (friLeftFun->getEffectsKind() >= EffectsKind::ReleaseNone
      || friRightFun->getEffectsKind() >= EffectsKind::ReleaseNone)
    return false;

  if (!friLeftFun->hasSemanticsAttrs() || !friRightFun->hasSemanticsAttrs())
    return false;

  auto aiLeftOperandsNum = aiLeft->getNumOperands();
  auto aiRightOperandsNum = aiRight->getNumOperands();

  // makeUTF8 should have following parameters:
  // (start: RawPointer, utf8CodeUnitCount: Word, isASCII: Int1)
  if (!((friLeftFun->hasSemanticsAttr(semantics::STRING_MAKE_UTF8)
         && aiLeftOperandsNum == 5)
        || (friRightFun->hasSemanticsAttr(semantics::STRING_MAKE_UTF8)
            && aiRightOperandsNum == 5)))
    return false;

  sliLeft = dyn_cast<StringLiteralInst>(aiLeft->getOperand(1));
  sliRight = dyn_cast<StringLiteralInst>(aiRight->getOperand(1));

  if (!sliLeft || !sliRight)
    return false;

  // Only UTF-8 and UTF-16 encoded string literals are supported by this
  // optimization.
  if (sliLeft->getEncoding() != StringLiteralInst::Encoding::UTF8
      && sliLeft->getEncoding() != StringLiteralInst::Encoding::UTF16)
    return false;

  if (sliRight->getEncoding() != StringLiteralInst::Encoding::UTF8
      && sliRight->getEncoding() != StringLiteralInst::Encoding::UTF16)
    return false;

  return true;
}

/// Ensures that both string literals to be concatenated use the same
/// UTF encoding. Converts UTF-8 into UTF-16 if required.
void StringConcatenationOptimizer::adjustEncodings() {
  if (sliLeft->getEncoding() == sliRight->getEncoding()) {
    friConvertFromBuiltin = friLeft;
    if (sliLeft->getEncoding() == StringLiteralInst::Encoding::UTF8) {
      funcResultType = aiLeft->getOperand(4);
    } else {
      funcResultType = aiLeft->getOperand(3);
    }
    return;
  }

  builder.setCurrentDebugScope(ai->getDebugScope());

  // If one of the string literals is UTF8 and another one is UTF16,
  // convert the UTF8-encoded string literal into UTF16-encoding first.
  if (sliLeft->getEncoding() == StringLiteralInst::Encoding::UTF8
      && sliRight->getEncoding() == StringLiteralInst::Encoding::UTF16) {
    funcResultType = aiRight->getOperand(3);
    friConvertFromBuiltin = friRight;
    // Convert UTF8 representation into UTF16.
    sliLeft = builder.createStringLiteral(ai->getLoc(), sliLeft->getValue(),
                                          StringLiteralInst::Encoding::UTF16);
  }

  if (sliRight->getEncoding() == StringLiteralInst::Encoding::UTF8
      && sliLeft->getEncoding() == StringLiteralInst::Encoding::UTF16) {
    funcResultType = aiLeft->getOperand(3);
    friConvertFromBuiltin = friLeft;
    // Convert UTF8 representation into UTF16.
    sliRight = builder.createStringLiteral(ai->getLoc(), sliRight->getValue(),
                                           StringLiteralInst::Encoding::UTF16);
  }

  // It should be impossible to have two operands with different
  // encodings at this point.
  assert(
      sliLeft->getEncoding() == sliRight->getEncoding()
      && "Both operands of string concatenation should have the same encoding");
}

/// Computes the length of a concatenated string literal.
APInt StringConcatenationOptimizer::getConcatenatedLength() {
  // Real length of string literals computed based on its contents.
  // Length is in code units.
  auto sliLenLeft = sliLeft->getCodeUnitCount();
  (void)sliLenLeft;
  auto sliLenRight = sliRight->getCodeUnitCount();
  (void)sliLenRight;

  // Length of string literals as reported by string.make functions.
  auto *lenLeft = dyn_cast<IntegerLiteralInst>(aiLeft->getOperand(2));
  auto *lenRight = dyn_cast<IntegerLiteralInst>(aiRight->getOperand(2));

  // Real and reported length should be the same.
  assert(sliLenLeft == lenLeft->getValue()
         && "Size of string literal in @_semantics(string.make) is wrong");

  assert(sliLenRight == lenRight->getValue()
         && "Size of string literal in @_semantics(string.make) is wrong");

  // Compute length of the concatenated literal.
  return lenLeft->getValue() + lenRight->getValue();
}

/// Computes the isAscii flag of a concatenated UTF8-encoded string literal.
bool StringConcatenationOptimizer::isAscii() const {
  // Add the isASCII argument in case of UTF8.
  // IsASCII is true only if IsASCII of both literals is true.
  auto *asciiLeft = dyn_cast<IntegerLiteralInst>(aiLeft->getOperand(3));
  auto *asciiRight = dyn_cast<IntegerLiteralInst>(aiRight->getOperand(3));
  auto isAsciiLeft = asciiLeft->getValue() == 1;
  auto isAsciiRight = asciiRight->getValue() == 1;
  return isAsciiLeft && isAsciiRight;
}

SingleValueInstruction *StringConcatenationOptimizer::optimize() {
  // Bail out if string literals concatenation optimization is
  // not possible.
  if (!extractStringConcatOperands())
    return nullptr;

  // Perform string literal encodings adjustments if needed.
  adjustEncodings();

  // Arguments of the new StringLiteralInst to be created.
  SmallVector<SILValue, 4> arguments;

  // Encoding to be used for the concatenated string literal.
  auto encoding = sliLeft->getEncoding();

  // Create a concatenated string literal.
  builder.setCurrentDebugScope(ai->getDebugScope());
  auto lv = sliLeft->getValue();
  auto rv = sliRight->getValue();
  auto *newSLI =
      builder.createStringLiteral(ai->getLoc(), lv + Twine(rv), encoding);
  arguments.push_back(newSLI);

  // Length of the concatenated literal according to its encoding.
  auto *len = builder.createIntegerLiteral(
      ai->getLoc(), aiLeft->getOperand(2)->getType(), getConcatenatedLength());
  arguments.push_back(len);

  // isAscii flag for UTF8-encoded string literals.
  if (encoding == StringLiteralInst::Encoding::UTF8) {
    bool ascii = isAscii();
    auto ilType = aiLeft->getOperand(3)->getType();
    auto *asciiLiteral =
        builder.createIntegerLiteral(ai->getLoc(), ilType, intmax_t(ascii));
    arguments.push_back(asciiLiteral);
  }

  // Type.
  arguments.push_back(funcResultType);

  return builder.createApply(ai->getLoc(), friConvertFromBuiltin,
                             SubstitutionMap(), arguments);
}

/// Top level entry point
SingleValueInstruction *swift::tryToConcatenateStrings(ApplyInst *ai,
                                                       SILBuilder &builder) {
  return StringConcatenationOptimizer(ai, builder).optimize();
}

//===----------------------------------------------------------------------===//
//                              Closure Deletion
//===----------------------------------------------------------------------===//

/// NOTE: Instructions with transitive ownership kind are assumed to not keep
/// the underlying value alive as well. This is meant for instructions only
/// with non-transitive users.
static bool useDoesNotKeepValueAlive(const SILInstruction *inst) {
  switch (inst->getKind()) {
  case SILInstructionKind::StrongRetainInst:
  case SILInstructionKind::StrongReleaseInst:
  case SILInstructionKind::DestroyValueInst:
  case SILInstructionKind::RetainValueInst:
  case SILInstructionKind::ReleaseValueInst:
  case SILInstructionKind::DebugValueInst:
  case SILInstructionKind::EndBorrowInst:
    return true;
  default:
    return false;
  }
}

static bool useHasTransitiveOwnership(const SILInstruction *inst) {
  // convert_escape_to_noescape is used to convert to a @noescape function type.
  // It does not change ownership of the function value.
  if (isa<ConvertEscapeToNoEscapeInst>(inst))
    return true;

  // Look through copy_value, begin_borrow. They are inert for our purposes, but
  // we need to look through it.
  return isa<CopyValueInst>(inst) || isa<BeginBorrowInst>(inst);
}

static bool shouldDestroyPartialApplyCapturedArg(SILValue arg,
                                                 SILParameterInfo paramInfo,
                                                 const SILFunction &F) {
  // If we have a non-trivial type and the argument is passed in @inout, we do
  // not need to destroy it here. This is something that is implicit in the
  // partial_apply design that will be revisited when partial_apply is
  // redesigned.
  if (paramInfo.isIndirectMutating())
    return false;

  // If we have a trivial type, we do not need to put in any extra releases.
  if (arg->getType().isTrivial(F))
    return false;

  // We handle all other cases.
  return true;
}

// *HEY YOU, YES YOU, PLEASE READ*. Even though a textual partial apply is
// printed with the convention of the closed over function upon it, all
// non-inout arguments to a partial_apply are passed at +1. This includes
// arguments that will eventually be passed as guaranteed or in_guaranteed to
// the closed over function. This is because the partial apply is building up a
// boxed aggregate to send off to the closed over function. Of course when you
// call the function, the proper conventions will be used.
void swift::releasePartialApplyCapturedArg(SILBuilder &builder, SILLocation loc,
                                           SILValue arg,
                                           SILParameterInfo paramInfo,
                                           InstModCallbacks callbacks) {
  if (!shouldDestroyPartialApplyCapturedArg(arg, paramInfo,
                                            builder.getFunction()))
    return;

  // Otherwise, we need to destroy the argument. If we have an address, we
  // insert a destroy_addr and return. Any live range issues must have been
  // dealt with by our caller.
  if (arg->getType().isAddress()) {
    // Then emit the destroy_addr for this arg
    SILInstruction *newInst = builder.emitDestroyAddrAndFold(loc, arg);
    callbacks.createdNewInst(newInst);
    return;
  }

  // Otherwise, we have an object. We emit the most optimized form of release
  // possible for that value.

  // If we have qualified ownership, we should just emit a destroy value.
  if (builder.getFunction().hasOwnership()) {
    callbacks.createdNewInst(builder.createDestroyValue(loc, arg));
    return;
  }

  if (arg->getType().hasReferenceSemantics()) {
    auto u = builder.emitStrongRelease(loc, arg);
    if (u.isNull())
      return;

    if (auto *SRI = u.dyn_cast<StrongRetainInst *>()) {
      callbacks.deleteInst(SRI);
      return;
    }

    callbacks.createdNewInst(u.get<StrongReleaseInst *>());
    return;
  }

  auto u = builder.emitReleaseValue(loc, arg);
  if (u.isNull())
    return;

  if (auto *rvi = u.dyn_cast<RetainValueInst *>()) {
    callbacks.deleteInst(rvi);
    return;
  }

  callbacks.createdNewInst(u.get<ReleaseValueInst *>());
}

void swift::deallocPartialApplyCapturedArg(SILBuilder &builder, SILLocation loc,
                                           SILValue arg,
                                           SILParameterInfo paramInfo) {
  if (!paramInfo.isIndirectInGuaranteed())
    return;

  builder.createDeallocStack(loc, arg);
}

static bool
deadMarkDependenceUser(SILInstruction *inst,
                       SmallVectorImpl<SILInstruction *> &deleteInsts) {
  if (!isa<MarkDependenceInst>(inst))
    return false;
  deleteInsts.push_back(inst);
  for (auto *use : cast<SingleValueInstruction>(inst)->getUses()) {
    if (!deadMarkDependenceUser(use->getUser(), deleteInsts))
      return false;
  }
  return true;
}

void swift::getConsumedPartialApplyArgs(PartialApplyInst *pai,
                                        SmallVectorImpl<Operand *> &argOperands,
                                        bool includeTrivialAddrArgs) {
  ApplySite applySite(pai);
  SILFunctionConventions calleeConv = applySite.getSubstCalleeConv();
  unsigned firstCalleeArgIdx = applySite.getCalleeArgIndexOfFirstAppliedArg();
  auto argList = pai->getArgumentOperands();
  SILFunction *F = pai->getFunction();

  for (unsigned i : indices(argList)) {
    auto argConv = calleeConv.getSILArgumentConvention(firstCalleeArgIdx + i);
    if (argConv.isInoutConvention())
      continue;

    Operand &argOp = argList[i];
    SILType ty = argOp.get()->getType();
    if (!ty.isTrivial(*F) || (includeTrivialAddrArgs && ty.isAddress()))
      argOperands.push_back(&argOp);
  }
}

bool swift::collectDestroys(SingleValueInstruction *inst,
                            SmallVectorImpl<SILInstruction *> &destroys) {
  bool isDead = true;
  for (Operand *use : inst->getUses()) {
    SILInstruction *user = use->getUser();
    if (useHasTransitiveOwnership(user)) {
      if (!collectDestroys(cast<SingleValueInstruction>(user), destroys))
        isDead = false;
      destroys.push_back(user);
    } else if (useDoesNotKeepValueAlive(user)) {
      destroys.push_back(user);
    } else {
      isDead = false;
    }
  }
  return isDead;
}

/// Move the original arguments of the partial_apply into newly created
/// temporaries to extend the lifetime of the arguments until the partial_apply
/// is finally destroyed.
///
/// TODO: figure out why this is needed at all. Probably because of some
///       weirdness of the old retain/release ARC model. Most likely this will
///       not be needed anymore with OSSA.
static bool keepArgsOfPartialApplyAlive(PartialApplyInst *pai,
                                        ArrayRef<SILInstruction *> paiUsers,
                                        SILBuilderContext &builderCtxt) {
  SmallVector<Operand *, 8> argsToHandle;
  getConsumedPartialApplyArgs(pai, argsToHandle,
                              /*includeTrivialAddrArgs*/ false);
  if (argsToHandle.empty())
    return true;

  // Compute the set of endpoints, which will be used to insert destroys of
  // temporaries. This may fail if the frontier is located on a critical edge
  // which we may not split.
  ValueLifetimeAnalysis vla(pai, paiUsers);

  ValueLifetimeAnalysis::Frontier partialApplyFrontier;
  if (!vla.computeFrontier(partialApplyFrontier,
                           ValueLifetimeAnalysis::DontModifyCFG)) {
    return false;
  }

  for (Operand *argOp : argsToHandle) {
    SILValue arg = argOp->get();
    int argIdx = argOp->getOperandNumber() - pai->getArgumentOperandNumber();
    SILDebugVariable dbgVar(/*Constant*/ true, argIdx);

    SILValue tmp = arg;
    if (arg->getType().isAddress()) {
      // Move the value to a stack-allocated temporary.
      SILBuilderWithScope builder(pai, builderCtxt);
      tmp = builder.createAllocStack(pai->getLoc(), arg->getType(), dbgVar);
      builder.createCopyAddr(pai->getLoc(), arg, tmp, IsTake_t::IsTake,
                             IsInitialization_t::IsInitialization);
    }

    // Delay the destroy of the value (either as SSA value or in the stack-
    // allocated temporary) at the end of the partial_apply's lifetime.
    endLifetimeAtFrontier(tmp, partialApplyFrontier, builderCtxt);
  }
  return true;
}

bool swift::tryDeleteDeadClosure(SingleValueInstruction *closure,
                                 InstModCallbacks callbacks,
                                 bool needKeepArgsAlive) {
  auto *pa = dyn_cast<PartialApplyInst>(closure);

  // We currently only handle locally identified values that do not escape. We
  // also assume that the partial apply does not capture any addresses.
  if (!pa && !isa<ThinToThickFunctionInst>(closure))
    return false;

  // A stack allocated partial apply does not have any release users. Delete it
  // if the only users are the dealloc_stack and mark_dependence instructions.
  if (pa && pa->isOnStack()) {
    SmallVector<SILInstruction *, 8> deleteInsts;
    for (auto *use : pa->getUses()) {
      if (isa<DeallocStackInst>(use->getUser())
          || isa<DebugValueInst>(use->getUser()))
        deleteInsts.push_back(use->getUser());
      else if (!deadMarkDependenceUser(use->getUser(), deleteInsts))
        return false;
    }
    for (auto *inst : reverse(deleteInsts))
      callbacks.deleteInst(inst);
    callbacks.deleteInst(pa);

    // Note: the lifetime of the captured arguments is managed outside of the
    // trivial closure value i.e: there will already be releases for the
    // captured arguments. Releasing captured arguments is not necessary.
    return true;
  }

  // Collect all destroys of the closure (transitively including destorys of
  // copies) and check if those are the only uses of the closure.
  SmallVector<SILInstruction *, 16> closureDestroys;
  if (!collectDestroys(closure, closureDestroys))
    return false;

  // If we have a partial_apply, release each captured argument at each one of
  // the final release locations of the partial apply.
  if (auto *pai = dyn_cast<PartialApplyInst>(closure)) {
    assert(!pa->isOnStack() &&
           "partial_apply [stack] should have been handled before");
    SILBuilderContext builderCtxt(pai->getModule());
    if (needKeepArgsAlive) {
      if (!keepArgsOfPartialApplyAlive(pai, closureDestroys, builderCtxt))
        return false;
    } else {
      // A preceeding partial_apply -> apply conversion (done in
      // tryOptimizeApplyOfPartialApply) already ensured that the arguments are
      // kept alive until the end of the partial_apply's lifetime.
      SmallVector<Operand *, 8> argsToHandle;
      getConsumedPartialApplyArgs(pai, argsToHandle,
                                  /*includeTrivialAddrArgs*/ false);

      // We can just destroy the arguments at the point of the partial_apply
      // (remember: partial_apply consumes all arguments).
      for (Operand *argOp : argsToHandle) {
        SILValue arg = argOp->get();
        SILBuilderWithScope builder(pai, builderCtxt);
        if (arg->getType().isObject()) {
          builder.emitDestroyValueOperation(pai->getLoc(), arg);
        } else {
          builder.emitDestroyAddr(pai->getLoc(), arg);
        }
      }
    }
  }

  // Delete all copy and destroy instructions in order so that leaf uses are
  // deleted first.
  for (SILInstruction *user : closureDestroys) {
    assert(
        (useDoesNotKeepValueAlive(user) || useHasTransitiveOwnership(user)) &&
        "We expect only ARC operations without "
        "results or a cast from escape to noescape without users");
    callbacks.deleteInst(user);
  }

  callbacks.deleteInst(closure);
  return true;
}

bool swift::simplifyUsers(SingleValueInstruction *inst) {
  bool changed = false;

  for (auto ui = inst->use_begin(), ue = inst->use_end(); ui != ue;) {
    SILInstruction *user = ui->getUser();
    ++ui;

    auto svi = dyn_cast<SingleValueInstruction>(user);
    if (!svi)
      continue;

    SILValue S = simplifyInstruction(svi);
    if (!S)
      continue;

    replaceAllSimplifiedUsesAndErase(svi, S);
    changed = true;
  }

  return changed;
}

/// True if a type can be expanded without a significant increase to code size.
bool swift::shouldExpand(SILModule &module, SILType ty) {
  // FIXME: Expansion
  auto expansion = TypeExpansionContext::minimal();

  if (module.Types.getTypeLowering(ty, expansion).isAddressOnly()) {
    return false;
  }
  if (EnableExpandAll) {
    return true;
  }

  unsigned numFields = module.Types.countNumberOfFields(ty, expansion);
  return (numFields <= 6);
}

/// Some support functions for the global-opt and let-properties-opts

// Encapsulate the state used for recursive analysis of a static
// initializer. Discover all the instruction in a use-def graph and return them
// in topological order.
//
// TODO: We should have a DFS utility for this sort of thing so it isn't
// recursive.
class StaticInitializerAnalysis {
  SmallVectorImpl<SILInstruction *> &postOrderInstructions;
  llvm::SmallDenseSet<SILValue, 8> visited;
  int recursionLevel = 0;

public:
  StaticInitializerAnalysis(
      SmallVectorImpl<SILInstruction *> &postOrderInstructions)
      : postOrderInstructions(postOrderInstructions) {}

  // Perform a recursive DFS on on the use-def graph rooted at `V`. Insert
  // values in the `visited` set in preorder. Insert values in
  // `postOrderInstructions` in postorder so that the instructions are
  // topologically def-use ordered (in execution order).
  bool analyze(SILValue rootValue) {
    return recursivelyAnalyzeOperand(rootValue);
  }

protected:
  bool recursivelyAnalyzeOperand(SILValue v) {
    if (!visited.insert(v).second)
      return true;

    if (++recursionLevel > 50)
      return false;

    // TODO: For multi-result instructions, we could simply insert all result
    // values in the visited set here.
    auto *inst = dyn_cast<SingleValueInstruction>(v);
    if (!inst)
      return false;

    if (!recursivelyAnalyzeInstruction(inst))
      return false;

    postOrderInstructions.push_back(inst);
    --recursionLevel;
    return true;
  }

  bool recursivelyAnalyzeInstruction(SILInstruction *inst) {
    if (auto *si = dyn_cast<StructInst>(inst)) {
      // If it is not a struct which is a simple type, bail.
      if (!si->getType().isTrivial(*si->getFunction()))
        return false;

      return llvm::all_of(si->getAllOperands(), [&](Operand &operand) -> bool {
        return recursivelyAnalyzeOperand(operand.get());
      });
    }
    if (auto *ti = dyn_cast<TupleInst>(inst)) {
      // If it is not a tuple which is a simple type, bail.
      if (!ti->getType().isTrivial(*ti->getFunction()))
        return false;

      return llvm::all_of(ti->getAllOperands(), [&](Operand &operand) -> bool {
        return recursivelyAnalyzeOperand(operand.get());
      });
    }
    if (auto *bi = dyn_cast<BuiltinInst>(inst)) {
      switch (bi->getBuiltinInfo().ID) {
      case BuiltinValueKind::FPTrunc:
        if (auto *li = dyn_cast<LiteralInst>(bi->getArguments()[0])) {
          return recursivelyAnalyzeOperand(li);
        }
        return false;
      default:
        return false;
      }
    }
    return isa<IntegerLiteralInst>(inst) || isa<FloatLiteralInst>(inst)
           || isa<StringLiteralInst>(inst);
  }
};

/// Check if the value of v is computed by means of a simple initialization.
/// Populate `forwardInstructions` with references to all the instructions
/// that participate in the use-def graph required to compute `V`. The
/// instructions will be in def-use topological order.
bool swift::analyzeStaticInitializer(
    SILValue v, SmallVectorImpl<SILInstruction *> &forwardInstructions) {
  return StaticInitializerAnalysis(forwardInstructions).analyze(v);
}

/// FIXME: This must be kept in sync with replaceLoadSequence()
/// below. What a horrible design.
bool swift::canReplaceLoadSequence(SILInstruction *inst) {
  if (auto *cai = dyn_cast<CopyAddrInst>(inst))
    return true;

  if (auto *li = dyn_cast<LoadInst>(inst))
    return true;

  if (auto *seai = dyn_cast<StructElementAddrInst>(inst)) {
    for (auto seaiUse : seai->getUses()) {
      if (!canReplaceLoadSequence(seaiUse->getUser()))
        return false;
    }
    return true;
  }

  if (auto *teai = dyn_cast<TupleElementAddrInst>(inst)) {
    for (auto teaiUse : teai->getUses()) {
      if (!canReplaceLoadSequence(teaiUse->getUser()))
        return false;
    }
    return true;
  }

  if (auto *ba = dyn_cast<BeginAccessInst>(inst)) {
    for (auto use : ba->getUses()) {
      if (!canReplaceLoadSequence(use->getUser()))
        return false;
    }
    return true;
  }

  // Incidental uses of an address are meaningless with regard to the loaded
  // value.
  if (isIncidentalUse(inst) || isa<BeginUnpairedAccessInst>(inst))
    return true;

  return false;
}

/// Replace load sequence which may contain
/// a chain of struct_element_addr followed by a load.
/// The sequence is traversed inside out, i.e.
/// starting with the innermost struct_element_addr
/// Move into utils.
///
/// FIXME: this utility does not make sense as an API. How can the caller
/// guarantee that the only uses of `I` are struct_element_addr and
/// tuple_element_addr?
void swift::replaceLoadSequence(SILInstruction *inst, SILValue value) {
  if (auto *cai = dyn_cast<CopyAddrInst>(inst)) {
    SILBuilder builder(cai);
    builder.createStore(cai->getLoc(), value, cai->getDest(),
                        StoreOwnershipQualifier::Unqualified);
    return;
  }

  if (auto *li = dyn_cast<LoadInst>(inst)) {
    li->replaceAllUsesWith(value);
    return;
  }

  if (auto *seai = dyn_cast<StructElementAddrInst>(inst)) {
    SILBuilder builder(seai);
    auto *sei =
        builder.createStructExtract(seai->getLoc(), value, seai->getField());
    for (auto seaiUse : seai->getUses()) {
      replaceLoadSequence(seaiUse->getUser(), sei);
    }
    return;
  }

  if (auto *teai = dyn_cast<TupleElementAddrInst>(inst)) {
    SILBuilder builder(teai);
    auto *tei =
        builder.createTupleExtract(teai->getLoc(), value, teai->getFieldNo());
    for (auto teaiUse : teai->getUses()) {
      replaceLoadSequence(teaiUse->getUser(), tei);
    }
    return;
  }

  if (auto *ba = dyn_cast<BeginAccessInst>(inst)) {
    for (auto use : ba->getUses()) {
      replaceLoadSequence(use->getUser(), value);
    }
    return;
  }

  // Incidental uses of an addres are meaningless with regard to the loaded
  // value.
  if (isIncidentalUse(inst) || isa<BeginUnpairedAccessInst>(inst))
    return;

  llvm_unreachable("Unknown instruction sequence for reading from a global");
}

/// Are the callees that could be called through Decl statically
/// knowable based on the Decl and the compilation mode?
bool swift::calleesAreStaticallyKnowable(SILModule &module, SILDeclRef decl) {
  if (decl.isForeign)
    return false;

  if (decl.isEnumElement()) {
    return calleesAreStaticallyKnowable(module,
                                        cast<EnumElementDecl>(decl.getDecl()));
  }

  auto *afd = decl.getAbstractFunctionDecl();
  assert(afd && "Expected abstract function decl!");
  return calleesAreStaticallyKnowable(module, afd);
}

/// Are the callees that could be called through Decl statically
/// knowable based on the Decl and the compilation mode?
bool swift::calleesAreStaticallyKnowable(SILModule &module,
                                         AbstractFunctionDecl *afd) {
  // Only handle members defined within the SILModule's associated context.
  if (!afd->isChildContextOf(module.getAssociatedContext()))
    return false;

  if (afd->isDynamic()) {
    return false;
  }

  if (!afd->hasAccess())
    return false;

  // Only consider 'private' members, unless we are in whole-module compilation.
  switch (afd->getEffectiveAccess()) {
  case AccessLevel::Open:
    return false;
  case AccessLevel::Public:
    if (isa<ConstructorDecl>(afd)) {
      // Constructors are special: a derived class in another module can
      // "override" a constructor if its class is "open", although the
      // constructor itself is not open.
      auto *nd = afd->getDeclContext()->getSelfNominalTypeDecl();
      if (nd->getEffectiveAccess() == AccessLevel::Open)
        return false;
    }
    LLVM_FALLTHROUGH;
  case AccessLevel::Internal:
    return module.isWholeModule();
  case AccessLevel::FilePrivate:
  case AccessLevel::Private:
    return true;
  }

  llvm_unreachable("Unhandled access level in switch.");
}

/// Are the callees that could be called through Decl statically
/// knowable based on the Decl and the compilation mode?
// FIXME: Merge this with calleesAreStaticallyKnowable above
bool swift::calleesAreStaticallyKnowable(SILModule &module,
                                         EnumElementDecl *eed) {
  // Only handle members defined within the SILModule's associated context.
  if (!eed->isChildContextOf(module.getAssociatedContext()))
    return false;

  if (eed->isDynamic()) {
    return false;
  }

  if (!eed->hasAccess())
    return false;

  // Only consider 'private' members, unless we are in whole-module compilation.
  switch (eed->getEffectiveAccess()) {
  case AccessLevel::Open:
    return false;
  case AccessLevel::Public:
  case AccessLevel::Internal:
    return module.isWholeModule();
  case AccessLevel::FilePrivate:
  case AccessLevel::Private:
    return true;
  }

  llvm_unreachable("Unhandled access level in switch.");
}

Optional<FindLocalApplySitesResult>
swift::findLocalApplySites(FunctionRefBaseInst *fri) {
  SmallVector<Operand *, 32> worklist(fri->use_begin(), fri->use_end());

  Optional<FindLocalApplySitesResult> f;
  f.emplace();

  // Optimistically state that we have no escapes before our def-use dataflow.
  f->escapes = false;

  while (!worklist.empty()) {
    auto *op = worklist.pop_back_val();
    auto *user = op->getUser();

    // If we have a full apply site as our user.
    if (auto apply = FullApplySite::isa(user)) {
      if (apply.getCallee() == op->get()) {
        f->fullApplySites.push_back(apply);
        continue;
      }
    }

    // If we have a partial apply as a user, start tracking it, but also look at
    // its users.
    if (auto *pai = dyn_cast<PartialApplyInst>(user)) {
      if (pai->getCallee() == op->get()) {
        // Track the partial apply that we saw so we can potentially eliminate
        // dead closure arguments.
        f->partialApplySites.push_back(pai);
        // Look to see if we can find a full application of this partial apply
        // as well.
        llvm::copy(pai->getUses(), std::back_inserter(worklist));
        continue;
      }
    }

    // Otherwise, see if we have any function casts to look through...
    switch (user->getKind()) {
    case SILInstructionKind::ThinToThickFunctionInst:
    case SILInstructionKind::ConvertFunctionInst:
    case SILInstructionKind::ConvertEscapeToNoEscapeInst:
      llvm::copy(cast<SingleValueInstruction>(user)->getUses(),
                 std::back_inserter(worklist));
      continue;

    // A partial_apply [stack] marks its captured arguments with
    // mark_dependence.
    case SILInstructionKind::MarkDependenceInst:
      llvm::copy(cast<SingleValueInstruction>(user)->getUses(),
                 std::back_inserter(worklist));
      continue;

    // Look through any reference count instructions since these are not
    // escapes:
    case SILInstructionKind::CopyValueInst:
      llvm::copy(cast<CopyValueInst>(user)->getUses(),
                 std::back_inserter(worklist));
      continue;
    case SILInstructionKind::StrongRetainInst:
    case SILInstructionKind::StrongReleaseInst:
    case SILInstructionKind::RetainValueInst:
    case SILInstructionKind::ReleaseValueInst:
    case SILInstructionKind::DestroyValueInst:
    // A partial_apply [stack] is deallocated with a dealloc_stack.
    case SILInstructionKind::DeallocStackInst:
      continue;
    default:
      break;
    }

    // But everything else is considered an escape.
    f->escapes = true;
  }

  // If we did escape and didn't find any apply sites, then we have no
  // information for our users that is interesting.
  if (f->escapes && f->partialApplySites.empty() && f->fullApplySites.empty())
    return None;
  return f;
}

/// Insert destroys of captured arguments of partial_apply [stack].
void swift::insertDestroyOfCapturedArguments(
    PartialApplyInst *pai, SILBuilder &builder,
    llvm::function_ref<bool(SILValue)> shouldInsertDestroy) {
  assert(pai->isOnStack());

  ApplySite site(pai);
  SILFunctionConventions calleeConv(site.getSubstCalleeType(),
                                    pai->getModule());
  auto loc = RegularLocation::getAutoGeneratedLocation();
  for (auto &arg : pai->getArgumentOperands()) {
    if (!shouldInsertDestroy(arg.get()))
      continue;
    unsigned calleeArgumentIndex = site.getCalleeArgIndex(arg);
    assert(calleeArgumentIndex >= calleeConv.getSILArgIndexOfFirstParam());
    auto paramInfo = calleeConv.getParamInfoForSILArg(calleeArgumentIndex);
    releasePartialApplyCapturedArg(builder, loc, arg.get(), paramInfo);
  }
}

void swift::insertDeallocOfCapturedArguments(
    PartialApplyInst *pai, SILBuilder &builder) {
  assert(pai->isOnStack());

  ApplySite site(pai);
  SILFunctionConventions calleeConv(site.getSubstCalleeType(),
                                    pai->getModule());
  auto loc = RegularLocation::getAutoGeneratedLocation();
  for (auto &arg : pai->getArgumentOperands()) {
    unsigned calleeArgumentIndex = site.getCalleeArgIndex(arg);
    assert(calleeArgumentIndex >= calleeConv.getSILArgIndexOfFirstParam());
    auto paramInfo = calleeConv.getParamInfoForSILArg(calleeArgumentIndex);
    deallocPartialApplyCapturedArg(builder, loc, arg.get(), paramInfo);
  }
}

AbstractFunctionDecl *swift::getBaseMethod(AbstractFunctionDecl *FD) {
  while (FD->getOverriddenDecl()) {
    FD = FD->getOverriddenDecl();
  }
  return FD;
}

FullApplySite
swift::cloneFullApplySiteReplacingCallee(FullApplySite applySite,
                                         SILValue newCallee,
                                         SILBuilderContext &builderCtx) {
  SmallVector<SILValue, 16> arguments;
  llvm::copy(applySite.getArguments(), std::back_inserter(arguments));

  SILBuilderWithScope builder(applySite.getInstruction(), builderCtx);
  builder.addOpenedArchetypeOperands(applySite.getInstruction());

  switch (applySite.getKind()) {
  case FullApplySiteKind::TryApplyInst: {
    auto *tai = cast<TryApplyInst>(applySite.getInstruction());
    return builder.createTryApply(tai->getLoc(), newCallee,
                                  tai->getSubstitutionMap(), arguments,
                                  tai->getNormalBB(), tai->getErrorBB());
  }
  case FullApplySiteKind::ApplyInst: {
    auto *ai = cast<ApplyInst>(applySite);
    auto fTy = newCallee->getType().getAs<SILFunctionType>();

    // The optimizer can generate a thin_to_thick_function from a throwing thin
    // to a non-throwing thick function (in case it can prove that the function
    // is not throwing).
    // Therefore we have to check if the new callee (= the argument of the
    // thin_to_thick_function) is a throwing function and set the not-throwing
    // flag in this case.
    return builder.createApply(applySite.getLoc(), newCallee,
                               applySite.getSubstitutionMap(), arguments,
                               ai->isNonThrowing() || fTy->hasErrorResult());
  }
  case FullApplySiteKind::BeginApplyInst: {
    llvm_unreachable("begin_apply support not implemented?!");
  }
  }
  llvm_unreachable("Unhandled case?!");
}
