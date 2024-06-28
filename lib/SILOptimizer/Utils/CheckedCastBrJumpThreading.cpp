//===--- CheckedCastBrJumpThreading.cpp -----------------------------------===//
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

#define DEBUG_TYPE "sil-simplify-cfg"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Utils/BasicBlockOptUtils.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/OwnershipOptUtils.h"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Allocator.h"

using namespace swift;

namespace {
/// This is a class implementing a dominator-based jump-threading
/// for checked_cast_br [exact].
class CheckedCastBrJumpThreading {
  // Basic block of the current checked_cast_br instruction.
  SILBasicBlock *BB;
  // Condition used by the current checked_cast_br instruction.
  SILValue Condition;

  SILBasicBlock *ArgBB;

  // The current function to be optimized.
  SILFunction *Fn;

  // Dominator information to be used.
  DominanceInfo *DT;

  // DeadEndBlocks is used by OwnershipRAUW and incrementally updated within
  // CheckedCastBrJumpThreading.
  //
  // TODO: incrementally update dead-end blocks during SimplifyCFG so it doesn't
  // need to be recomputed each time tryCheckedCastBrJumpThreading is called.
  DeadEndBlocks *deBlocks;

  SILPassManager *pm;

  // Enable non-trivial terminator rewriting in OSSA.
  bool EnableOSSARewriteTerminator;

  InstModCallbacks callbacks;

  // Shared data structures across OwnershipRAUWHelper instances.
  OwnershipFixupContext rauwContext;

  // List of predecessors.
  typedef SmallVector<SILBasicBlock *, 8> PredList;

  // Predecessors reached only via a path along the
  // success branch of the dominating checked_cast_br.
  PredList SuccessPreds;

  // Predecessors reached only via a path along the
  // failure branch of the dominating checked_cast_br.
  PredList FailurePreds;

  // All other predecessors, where the outcome of the
  // checked_cast_br along the path is not known.
  unsigned numUnknownPreds = 0;

  // Basic blocks to be added to for reprocessing
  // after jump-threading is done.
  SmallVectorImpl<SILBasicBlock *> &BlocksForWorklist;

  // Information for transforming a single checked_cast_br.
  // This is the output of the optimization's analysis phase.
  struct Edit {

    // The block containing the checked_cast_br.
    SILBasicBlock *CCBBlock;
    // Copy of CheckedCastBrJumpThreading::SuccessPreds.
    PredList SuccessPreds;
    // Copy of CheckedCastBrJumpThreading::FailurePreds.
    PredList FailurePreds;
    // The argument of the dominating checked_cast_br's successor block.
    SILPhiArgument *SuccessArg;

    // True if the dominating check is inverted AND all the predecessors are on
    // the dominating check's success path.
    bool InvertSuccess;

    // True if CheckedCastBrJumpThreading::numUnknownPreds is not 0.
    bool hasUnknownPreds;

    Edit(SILBasicBlock *CCBBlock, bool InvertSuccess,
         const PredList &SuccessPreds,
         const PredList &FailurePreds,
         bool hasUnknownPreds, SILPhiArgument *SuccessArg) :
      CCBBlock(CCBBlock), SuccessPreds(SuccessPreds), FailurePreds(FailurePreds),
      SuccessArg(SuccessArg), InvertSuccess(InvertSuccess),
      hasUnknownPreds(hasUnknownPreds) { }

    bool canRAUW(OwnershipFixupContext &rauwContext);

    void modifyCFGForFailurePreds(BasicBlockCloner &Cloner);
    void modifyCFGForSuccessPreds(BasicBlockCloner &Cloner,
                                  OwnershipFixupContext &rauwContext);
  };

  // Contains an entry for each checked_cast_br to be optimized.
  llvm::SmallVector<Edit *, 8> Edits;

  llvm::SpecificBumpPtrAllocator<Edit> EditAllocator;

  // Keeps track of what blocks we change the terminator instruction.
  BasicBlockSet BlocksToEdit;
  // Keeps track of what blocks we clone.
  BasicBlockSet BlocksToClone;

  bool areEquivalentConditionsAlongPaths(CheckedCastBranchInst *DomCCBI);
  bool areEquivalentConditionsAlongSomePaths(CheckedCastBranchInst *DomCCBI,
                                             SILValue DomCondition);
  bool handleArgBBIsEntryBlock(SILBasicBlock *ArgBB,
                               CheckedCastBranchInst *DomCCBI);
  bool checkCloningConstraints();

  void classifyPredecessor(SILBasicBlock *Pred, bool SuccessDominates,
                           bool FailureDominates);

  SILValue isArgValueEquivalentToCondition(SILValue Value,
                               SILBasicBlock *DomBB,
                               SILValue DomValue,
                               DominanceInfo *DT);

  bool trySimplify(CheckedCastBranchInst *CCBI);

public:
  CheckedCastBrJumpThreading(
      SILFunction *Fn, SILPassManager *pm, DominanceInfo *DT, DeadEndBlocks *deBlocks,
      SmallVectorImpl<SILBasicBlock *> &BlocksForWorklist,
      bool EnableOSSARewriteTerminator)
      : Fn(Fn), DT(DT), deBlocks(deBlocks), pm(pm),
        EnableOSSARewriteTerminator(EnableOSSARewriteTerminator),
        rauwContext(callbacks, *deBlocks),
        BlocksForWorklist(BlocksForWorklist), BlocksToEdit(Fn),
        BlocksToClone(Fn) {}

  void optimizeFunction();
};
} // end anonymous namespace

/// Estimate the cost of inlining a given basic block.
static unsigned basicBlockInlineCost(SILBasicBlock *BB, unsigned Cutoff) {
  unsigned Cost = 0;
  for (auto &I : *BB) {
    auto ICost = instructionInlineCost(I);
    Cost += unsigned(ICost);
    if (Cost > Cutoff)
      return Cost;
  }
  return Cost;
}

/// We cannot duplicate blocks with AllocStack instructions (they need to be
/// FIFO). Other instructions can be duplicated.
static bool canDuplicateBlock(SILBasicBlock *BB) {
  for (auto &I : *BB) {
    if (!I.isTriviallyDuplicatable())
      return false;
  }
  return true;
}

/// Classify a predecessor of a BB containing checked_cast_br as being
/// reachable via success or failure branches of a dominating checked_cast_br
/// or as unknown if it can be reached via success or failure branches
/// at the same time.
void CheckedCastBrJumpThreading::classifyPredecessor(
    SILBasicBlock *Pred, bool SuccessDominates, bool FailureDominates) {
  if (SuccessDominates == FailureDominates) {
    ++numUnknownPreds;
    return;
  }
  if (SuccessDominates) {
    SuccessPreds.push_back(Pred);
    return;
  }
  assert(FailureDominates && "success and failure should be mutual exclusive");
  FailurePreds.push_back(Pred);
}

/// Check if the root value for Value that comes
/// along the path from DomBB is equivalent to the
/// DomCondition.
SILValue CheckedCastBrJumpThreading::isArgValueEquivalentToCondition(
    SILValue Value, SILBasicBlock *DomBB, SILValue DomValue,
    DominanceInfo *DT) {
  SmallPtrSet<ValueBase *, 16> SeenValues;
  DomValue = stripClassCasts(DomValue);

  while (true) {
    Value = stripClassCasts(Value);
    if (Value == DomValue)
      return Value;

    // We know how to propagate through phi arguments only.
    auto *V = dyn_cast<SILPhiArgument>(Value);
    if (!V)
      return SILValue();

    // Have we visited this BB already?
    if (!SeenValues.insert(Value).second)
      return SILValue();

    if (SeenValues.size() > 10)
      return SILValue();

    SmallVector<SILValue, 4> IncomingValues;
    if (!V->getSingleTerminatorOperands(IncomingValues)
        || IncomingValues.empty())
      return SILValue();

    ValueBase *Def = nullptr;
    for (auto IncomingValue : IncomingValues) {
      // Each incoming value should be either from a block
      // dominated by DomBB or it should be the value used in
      // condition in DomBB
      Value = stripClassCasts(IncomingValue);
      if (Value == DomValue)
        continue;

      // Values should be the same
      if (!Def)
        Def = Value;

      if (Def != Value)
        return SILValue();

      if (!DT->dominates(DomBB, Value->getParentBlock()))
        return SILValue();
      // OK, this value is a potential candidate
    }

    Value = IncomingValues[0];
  }
}

// Return false if an ownership RAUW is necessary but cannot be performed.
bool CheckedCastBrJumpThreading::Edit::
canRAUW(OwnershipFixupContext &rauwContext) {
  if (InvertSuccess || (SuccessPreds.empty() && !hasUnknownPreds))
    return true;

  auto *ccbi = cast<CheckedCastBranchInst>(CCBBlock->getTerminator());
  auto *oldSuccessArg = ccbi->getSuccessBB()->getArgument(0);
  // Check the ownership validity of the RAUW transformation that will replace
  // oldSuccessArg with SuccessArg. This is valid iff it will be valid to
  // replace the new checked_cast_br. The new checked_cast_br will be in a
  // cloned block reachable from a subset of the original block's predecessors,
  // it will have equivalent operands. Checking the current uses is unnecessary,
  // because after cloning, the only use of the cloned checked_cast_br will be
  // a phi in the successor. It is always valid to replace a phi use, because
  // phi itself already guarantees that lifetime extends over its own uses.
  return OwnershipRAUWHelper::hasValidNonLexicalRAUWOwnership(oldSuccessArg,
                                                              SuccessArg);
}

// Erase the checked_cast_br that terminates this block. The caller must replace
// and erase the successful cast result.
//
// The checked_cast_br failure result's uses are replaced with the cast's
// operand, and the block argument representing that result is deleted. Since
// the checked_cast's uses now use its forwarded operand, they are still in
// valid OSSA form, so this can be done before updateOSSAAfterCloning, which
// doesn't need to know about the erased checked_cast.
static void eraseCheckedCastBr(
  CheckedCastBranchInst *checkedCastBr,
  CheckedCastBranchInst::SuccessorPath successorIdx) {

  SILBuilderWithScope Builder(checkedCastBr);
  Builder.createBranch(checkedCastBr->getLoc(),
                       checkedCastBr->getSuccessors()[successorIdx]);
  auto *successBB = checkedCastBr->getSuccessBB();
  assert(successBB->getNumArguments() == 1);
  assert(successBB->getArgument(0)->use_empty());
  successBB->eraseArgument(0);
  if (checkedCastBr->getFunction()->hasOwnership()) {
    auto *failureBB = checkedCastBr->getFailureBB();
    assert(failureBB->getNumArguments() == 1);
    failureBB->getArgument(0)->replaceAllUsesWith(checkedCastBr->getOperand());
    failureBB->eraseArgument(0);
  }
  checkedCastBr->eraseFromParent();
}

void CheckedCastBrJumpThreading::Edit::modifyCFGForFailurePreds(
    BasicBlockCloner &Cloner) {
  if (FailurePreds.empty())
    return;

  assert(!Cloner.wasCloned());
  Cloner.cloneBlock();
  SILBasicBlock *TargetFailureBB = Cloner.getNewBB();
  // This cloned block branches to the FailureBB, so just delete the cast and
  // ignore the success target which will keep it's original predecessor.
  auto *clonedCCBI =
    cast<CheckedCastBranchInst>(TargetFailureBB->getTerminator());
  auto *clonedSuccessArg = clonedCCBI->getSuccessBB()->getArgument(0);
  clonedSuccessArg->replaceAllUsesWithUndef();
  eraseCheckedCastBr(clonedCCBI, CheckedCastBranchInst::FailIdx);

  // Redirect all FailurePreds to the copy of BB.
  for (auto *Pred : FailurePreds) {
    TermInst *TI = Pred->getTerminator();
    // Replace branch to BB by branch to TargetFailureBB.
    TI->replaceBranchTarget(CCBBlock, TargetFailureBB);
  }
  Cloner.updateSSAAfterCloning();
}

/// Create a copy of the BB or reuse BB as a landing basic block for all
/// FailurePreds.
///
/// Note: must be called after modifyCFGForFailurePreds and
/// before modifyCFGForUnknownPreds.
void CheckedCastBrJumpThreading::Edit::modifyCFGForSuccessPreds(
  BasicBlockCloner &Cloner, OwnershipFixupContext &rauwContext) {

  auto *checkedCastBr = cast<CheckedCastBranchInst>(CCBBlock->getTerminator());
  auto *oldSuccessArg = checkedCastBr->getSuccessBB()->getArgument(0);
  if (InvertSuccess || (SuccessPreds.empty() && !hasUnknownPreds)) {
    assert(!hasUnknownPreds && "is not handled, should have been checked");
    // This success path is unused, so undef its uses and delete the cast.
    oldSuccessArg->replaceAllUsesWithUndef();
    eraseCheckedCastBr(checkedCastBr, CheckedCastBranchInst::FailIdx);
    return;
  }
  if (!hasUnknownPreds) {
    // All predecessors are dominated by a successful cast. So the current BB
    // can be re-used instead as their target.
    //
    // NOTE: Assumes that failure predecessors have already been processed and
    // removed from the current block's predecessors.

    // Replace uses with SuccessArg from the dominating BB. Do this while it is
    // still a valid terminator result, before erasing the cast.
    OwnershipRAUWHelper rauwTransform(rauwContext, oldSuccessArg, SuccessArg);
    assert(rauwTransform.isValid() && "sufficiently checked by canRAUW");
    rauwTransform.perform();

    eraseCheckedCastBr(checkedCastBr, CheckedCastBranchInst::SuccessIdx);
    return;
  }
  // Only clone if there are preds on the success path.
  if (SuccessPreds.empty())
    return;

  // Create a copy of the BB as a landing BB.
  // for all SuccessPreds.
  assert(!Cloner.wasCloned());
  Cloner.cloneBlock();
  SILBasicBlock *clonedCCBBlock = Cloner.getNewBB();
  // Redirect all SuccessPreds to the copy of BB.
  for (auto *Pred : SuccessPreds) {
    TermInst *TI = Pred->getTerminator();
    // Replace branch to BB by branch to TargetSuccessBB.
    TI->replaceBranchTarget(CCBBlock, clonedCCBBlock);
  }
  // Remove the unreachable checked_cast_br target.
  auto *clonedCCBI =
      cast<CheckedCastBranchInst>(clonedCCBBlock->getTerminator());
  auto *successBB = clonedCCBI->getSuccessBB();
  // This cloned block branches to the successBB.
  // The checked_cast_br uses are replaced with SuccessArg.
  if (!CCBBlock->getParent()->hasOwnership()) {
    SILBuilderWithScope Builder(clonedCCBI);
    Builder.createBranch(clonedCCBI->getLoc(), successBB, {SuccessArg});
    clonedCCBI->eraseFromParent();
    Cloner.updateSSAAfterCloning();
    return;
  }
  // Remove all uses from the failure path so RAUW can erase the
  // terminator after replacing the successor argument.
  auto *failureBB = clonedCCBI->getFailureBB();
  assert(failureBB->getNumArguments() == 1 && "expecting term result");
  failureBB->getArgument(0)->replaceAllUsesWithUndef();

  // Create nested borrow scopes for new phis either created for the
  // checked_cast's results or during SSA update. This puts the SIL in
  // valid OSSA form before calling OwnershipRAUWHelper.
  Cloner.updateSSAAfterCloning();

  auto *clonedSuccessArg = successBB->getArgument(0);
  OwnershipRAUWHelper rauwUtil(rauwContext, clonedSuccessArg, SuccessArg);
  assert(rauwUtil.isValid() && "sufficiently checked by canRAUW");
  rauwUtil.perform();

  eraseCheckedCastBr(clonedCCBI, CheckedCastBranchInst::SuccessIdx);
}

/// Handle a special case, where ArgBB is the entry block.
bool CheckedCastBrJumpThreading::handleArgBBIsEntryBlock(
    SILBasicBlock *ArgBB, CheckedCastBranchInst *DomCCBI) {
  if (!ArgBB->pred_empty())
    return false;

  // It must be the entry block
  //
  // TODO: Is this a correct assumption? Do we know that at this point that
  // ArgBB can not be unreachable?
  //
  // See if it is reached over Success or Failure path.
  bool SuccessDominates = DomCCBI->getSuccessBB() == BB;
  bool FailureDominates = DomCCBI->getFailureBB() == BB;

  if (BlocksToEdit.contains(ArgBB))
    return false;

  classifyPredecessor(ArgBB, SuccessDominates, FailureDominates);
  return true;
}

// Returns false if cloning required by jump threading cannot
// be performed, because some of the constraints are violated.
//
// This does not check the constraint on address projections with out-of-block
// uses. Those are rare enough that they don't need to be checked first for
// efficiency, but they need to be gathered later, just before cloning, anyway
// in order to sink the projections.
bool CheckedCastBrJumpThreading::checkCloningConstraints() {
  // Check some cloning related constraints.

  // If this argument from a different BB, then jump-threading
  // may require too much code duplication.
  if (ArgBB && ArgBB != BB)
    return false;

  // Bail out if current BB cannot be duplicated.
  if (!canDuplicateBlock(BB))
    return false;

  // Check if code-bloat would be too big when this BB
  // is jump-threaded.
  // TODO: Make InlineCostCutoff parameter configurable?
  // Dec 1, 2014:
  // We looked at the inline costs of BBs from our benchmark suite
  // and found that currently the highest inline cost for the
  // whole benchmark suite is 12. In 95% of all cases it is <=3.
  const unsigned InlineCostCutoff = 20;
  if (basicBlockInlineCost(BB, InlineCostCutoff) >= InlineCostCutoff)
    return false;

  return true;
}

/// If conditions are not equivalent along all paths, try harder
/// to check if they are actually equivalent along a subset of paths.
/// To do it, try to back-propagate the Condition
/// backwards and see if it is actually equivalent to DomCondition.
/// along some of the paths.
bool CheckedCastBrJumpThreading::
areEquivalentConditionsAlongSomePaths(CheckedCastBranchInst *DomCCBI,
                                      SILValue DomCondition) {
  auto *Arg = dyn_cast<SILPhiArgument>(Condition);
  if (!Arg)
    return false;

  ArgBB = Arg->getParent();
  SILBasicBlock *DomBB = DomCCBI->getParent();
  if (!DT->dominates(DomBB, ArgBB))
    return false;

  // Incoming values for the BBArg.
  SmallVector<SILValue, 4> IncomingValues;

  if (ArgBB->getIterator() != ArgBB->getParent()->begin()
      && (!Arg->getSingleTerminatorOperands(IncomingValues)
          || IncomingValues.empty()))
    return false;

  // Check for each predecessor, if the incoming value coming from it
  // is equivalent to the DomCondition. If this is the case, it is
  // possible to try jump-threading along this path.
  if (!handleArgBBIsEntryBlock(ArgBB, DomCCBI)) {
    // ArgBB is not the entry block and has predecessors.
    unsigned idx = 0;
    for (auto *PredBB : ArgBB->getPredecessorBlocks()) {

      // We must avoid that we are going to change a block twice.
      if (BlocksToEdit.contains(PredBB))
        return false;

      auto IncomingValue = IncomingValues[idx];
      SILValue ReachingValue = isArgValueEquivalentToCondition(
          IncomingValue, DomBB, DomCondition, DT);

      if (ReachingValue == SILValue()) {
        ++numUnknownPreds;
        ++idx;
        continue;
      }

      // Condition is the same if BB is reached over a pass through Pred.
      LLVM_DEBUG(llvm::dbgs() << "Condition is the same if reached over ");
      LLVM_DEBUG(PredBB->print(llvm::dbgs()));

      // See if it is reached over Success or Failure path.
      SILBasicBlock *DomSuccessBB = DomCCBI->getSuccessBB();
      bool SuccessDominates = DT->dominates(DomSuccessBB, PredBB) ||
                              DT->dominates(DomSuccessBB, BB) ||
                              DomSuccessBB == BB;
      SILBasicBlock *DomFailureBB = DomCCBI->getFailureBB();
      bool FailureDominates = DT->dominates(DomFailureBB, PredBB) ||
                              DT->dominates(DomFailureBB, BB) ||
                              DomFailureBB == BB;

      classifyPredecessor(
          PredBB, SuccessDominates, FailureDominates);
      ++idx;
    }
  } else {
    // ArgBB is the entry block. Check that conditions are the equivalent in this
    // case as well.
    if (!isArgValueEquivalentToCondition(Condition, DomBB, DomCondition, DT))
      return false;
  }


  // At this point we know for each predecessor of ArgBB if its reached
  // over the success, failure or unknown path from DomBB.

  // Now we can generate a new BB for preds reaching BB over the success
  // path and a new BB for preds reaching BB over the failure path.
  // Then we redirect those preds to those new basic blocks.
  return true;
}

/// Check if conditions of CCBI and DomCCBI are equivalent along
/// all or at least some paths.
bool CheckedCastBrJumpThreading::
areEquivalentConditionsAlongPaths(CheckedCastBranchInst *DomCCBI) {
  // Are conditions equivalent along all paths?
  SILValue DomCondition = stripClassCasts(DomCCBI->getOperand());
  if (DomCondition == Condition) {
    // Conditions are exactly the same, without any restrictions.
    // They are equivalent along all paths.

    // Figure out for each predecessor which branch of
    // the dominating checked_cast_br is used to reach it.
    for (auto *PredBB : BB->getPredecessorBlocks()) {
      // All predecessors should either unconditionally branch
      // to the current BB or be another checked_cast_br instruction.
      if (!isa<CheckedCastBranchInst>(PredBB->getTerminator()) &&
          !isa<BranchInst>(PredBB->getTerminator()))
        return false;

      // We must avoid that we are going to change a block twice.
      if (BlocksToEdit.contains(PredBB))
        return false;

      // Don't allow critical edges from PredBB to BB. This ensures that
      // splitAllCriticalEdges() will not invalidate our predecessor lists.
      if (!BB->getSinglePredecessorBlock() &&
          !PredBB->getSingleSuccessorBlock())
        return false;

      SILBasicBlock *DomSuccessBB = DomCCBI->getSuccessBB();
      bool SuccessDominates =
          DT->dominates(DomSuccessBB, PredBB) || DomSuccessBB == BB;
      SILBasicBlock *DomFailureBB = DomCCBI->getFailureBB();
      bool FailureDominates =
          DT->dominates(DomFailureBB, PredBB) || DomFailureBB == BB;

      classifyPredecessor(PredBB, SuccessDominates, FailureDominates);
    }
    return true;
  }

  // Check if conditions are equivalent along a subset of reaching paths.
  return areEquivalentConditionsAlongSomePaths(DomCCBI, DomCondition);
}

/// Try performing a dominator-based jump-threading for
/// checked_cast_br instructions.
bool CheckedCastBrJumpThreading::trySimplify(CheckedCastBranchInst *CCBI) {
  if (!EnableOSSARewriteTerminator && Fn->hasOwnership()
      && !CCBI->getOperand()->getType().isTrivial(*Fn)) {
    return false;
  }

  // Init information about the checked_cast_br we try to
  // jump-thread.
  BB = CCBI->getParent();
  if (BlocksToEdit.contains(BB))
    return false;

  Condition = stripClassCasts(CCBI->getOperand());

  // Find a dominating checked_cast_br, which performs the same check.
  for (auto *Node = DT->getNode(BB)->getIDom(); Node; Node = Node->getIDom()) {
    // Get current dominating block.
    SILBasicBlock *DomBB = Node->getBlock();
    auto *DomTerm = DomBB->getTerminator();

    if (!DomTerm->getNumOperands())
      continue;

    // Check that it is a dominating checked_cast_br.
    auto *DomCCBI = dyn_cast<CheckedCastBranchInst>(DomTerm);
    if (!DomCCBI)
      continue;

    // We need to verify that the result type is the same in the
    // dominating checked_cast_br, but only for non-exact casts.
    // For exact casts, we are interested only in the
    // fact that the source operand is the same for
    // both instructions.
    if (!CCBI->isExact() && !DomCCBI->isExact()) {
      if (DomCCBI->getTargetFormalType() != CCBI->getTargetFormalType())
        continue;
    }

    // Conservatively check that both checked_cast_br instructions
    // are either exact or non-exact. This is very conservative,
    // but safe.
    //
    // TODO:
    // If the dominating checked_cast_br is non-exact, then
    // it is in general not safe to assume that current exact cast
    // would have the same outcome. But if the dominating non-exact
    // checked_cast_br fails, then the current exact cast would
    // always fail as well.
    //
    // If the dominating checked_cast_br is exact then then
    // it is in general not safe to assume that the current non-exact
    // cast would have the same outcome. But if the dominating exact
    // checked_cast_br succeeds, then the current non-exact cast
    // would always succeed as well.
    //
    // TODO: In some specific cases, it is possible to prove that
    // success or failure of the dominating cast is equivalent to
    // the success or failure of the current cast, even if one
    // of them is exact and the other not. This is the case
    // e.g. if the class has no subclasses.
    if (DomCCBI->isExact() != CCBI->isExact())
      continue;

    // We need the block argument of the DomSuccessBB. If we are going to
    // clone it for a previous checked_cast_br the argument will not dominate
    // the blocks which it's used to dominate anymore.
    if (BlocksToClone.contains(DomCCBI->getSuccessBB()))
      continue;

    // Init state variables for paths analysis
    SuccessPreds.clear();
    FailurePreds.clear();
    numUnknownPreds = 0;
    ArgBB = nullptr;

    // Are conditions of CCBI and DomCCBI equivalent along (some) paths?
    // If this is the case, classify all incoming paths into SuccessPreds,
    // FailurePreds or UnknownPreds depending on how they reach CCBI.
    if (!areEquivalentConditionsAlongPaths(DomCCBI))
      continue;

    // Check if any jump-threading is required and possible.
    if (SuccessPreds.empty() && FailurePreds.empty())
      return false;

    // If this check is reachable via success, failure and unknown
    // at the same time, then we don't know the outcome of the
    // dominating check. No jump-threading is possible in this case.
    if (!SuccessPreds.empty() && !FailurePreds.empty() && numUnknownPreds > 0) {
      return false;
    }

    unsigned TotalPreds =
        SuccessPreds.size() + FailurePreds.size() + numUnknownPreds;

    // We only need to clone the BB if not all of its
    // predecessors are in the same group.
    if (TotalPreds != SuccessPreds.size() &&
        TotalPreds != numUnknownPreds) {
      // Check some cloning related constraints.
      if (!checkCloningConstraints())
        return false;
    }

    bool InvertSuccess = false;
    if (DomCCBI->isExact() && CCBI->isExact() &&
        DomCCBI->getTargetFormalType() != CCBI->getTargetFormalType()) {
      if (TotalPreds == SuccessPreds.size()) {
        // The dominating exact cast was successful, but it casted to a
        // different type. Therefore, the current cast fails for sure.
        // Since we are going to change the BB,
        // add its successors and predecessors
        // for re-processing.
        InvertSuccess = true;
      } else {
        // Otherwise, we don't know if the current cast will succeed or
        // fail.
        return false;
      }
    }
    // If we have predecessors, where it is not known if they are reached over
    // success or failure path, we cannot eliminate a checked_cast_br.
    // We have to generate new dedicated BBs as landing BBs for all
    // FailurePreds and all SuccessPreds.

    // Since we are going to change the BB, add its successors and predecessors
    // for re-processing.
    for (auto *B : BB->getPredecessorBlocks()) {
      BlocksForWorklist.push_back(B);
    }
    for (auto *B : BB->getSuccessorBlocks()) {
      BlocksForWorklist.push_back(B);
    }
    // Remember the blocks we are going to change. So that we ignore them
    // for upcoming checked_cast_br instructions.
    BlocksToEdit.insert(BB);
    BlocksToClone.insert(BB);
    for (auto *B : SuccessPreds)
      BlocksToEdit.insert(B);
    for (auto *B : FailurePreds)
      BlocksToEdit.insert(B);

    // Record what we want to change.
    Edit *edit = new (EditAllocator.Allocate())
        Edit(BB, InvertSuccess, SuccessPreds, FailurePreds,
             numUnknownPreds != 0,
             cast<SILPhiArgument>(DomCCBI->getSuccessBB()->getArgument(0)));
    Edits.push_back(edit);

    return true;
  }

  // Jump-threading was not possible.
  return false;
}

/// Optimize the checked_cast_br instructions in a function.
void CheckedCastBrJumpThreading::optimizeFunction() {

  // We separate the work in two phases: analyze and transform. This avoids
  // re-calculating the dominator tree for each optimized checked_cast_br.

  // First phase: analysis.
  for (auto &BB : *Fn) {
    // Ignore unreachable blocks.
    if (!DT->getNode(&BB))
      continue;

    if (auto *CCBI = dyn_cast<CheckedCastBranchInst>(BB.getTerminator()))
      trySimplify(CCBI);
  }

  assert(BlocksForWorklist.empty() == Edits.empty());

  if (Edits.empty())
    return;

  // Second phase: transformation.
  if (Fn->getModule().getOptions().VerifyAll)
    Fn->verifyCriticalEdges();

  for (Edit *edit : Edits) {
    if (edit->SuccessArg->isErased())
      continue;

    BasicBlockCloner Cloner(edit->CCBBlock, pm, deBlocks);
    if (!Cloner.canCloneBlock())
      continue;

   if (Fn->hasOwnership() && !edit->canRAUW(rauwContext))
     continue;

    // Create a copy of the BB as a landing BB
    // for all FailurePreds.
    edit->modifyCFGForFailurePreds(Cloner);
    // Create a copy of the BB or reuse BB as
    // a landing basic block for all SuccessPreds.
    edit->modifyCFGForSuccessPreds(Cloner, rauwContext);

    if (Cloner.wasCloned()) {
      Cloner.updateSSAAfterCloning();

      if (!Cloner.getNewBB()->pred_empty())
        BlocksForWorklist.push_back(Cloner.getNewBB());
    }
    if (!edit->CCBBlock->pred_empty())
      BlocksForWorklist.push_back(edit->CCBBlock);
  }
}

namespace swift {

bool tryCheckedCastBrJumpThreading(
    SILFunction *Fn, SILPassManager *pm, DominanceInfo *DT, DeadEndBlocks *deBlocks,
    SmallVectorImpl<SILBasicBlock *> &BlocksForWorklist,
    bool EnableOSSARewriteTerminator) {

  // TODO: Disable for OSSA temporarily
  if (Fn->hasOwnership()) {
    return false;
  }

  CheckedCastBrJumpThreading CCBJumpThreading(Fn, pm, DT, deBlocks,
                                              BlocksForWorklist,
                                              EnableOSSARewriteTerminator);
  CCBJumpThreading.optimizeFunction();
  return !BlocksForWorklist.empty();
}

} // end namespace swift
