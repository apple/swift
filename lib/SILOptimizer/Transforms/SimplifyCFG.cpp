//===--- SimplifyCFG.cpp - Clean up the SIL CFG ---------------------------===//
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
/// Note: Unreachable blocks must always be eliminated before simplifying
/// useless phis. Otherwise self-loops will result in invalid SIL:
///
///   bb1(%phi):
///     apply %use(%phi)
///     %def = apply %getValue()
///     br bb1(%def)
///
/// When bb1 is unreachable, %phi will be removed as useless:
///   bb1:
///     apply %use(%def)
///     %def = apply %getValue()
///     br bb1(%def)
///
/// This is considered invalid SIL because SIL has a special SSA dominance rule
/// that does not allow a use above a def in the same block.
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-simplify-cfg"

#include "swift/AST/Module.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/TerminatorUtils.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/ProgramTerminationAnalysis.h"
#include "swift/SILOptimizer/Analysis/SimplifyInstruction.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Transforms/SimplifyCFG.h"
#include "swift/SILOptimizer/Utils/BasicBlockOptUtils.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/CastOptimizer.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/OwnershipOptUtils.h"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "swift/SILOptimizer/Utils/SILSSAUpdater.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

// This is temporarily used for testing until Swift 5.5 branches to reduce risk.
llvm::cl::opt<bool> EnableOSSASimplifyCFG(
    "enable-ossa-simplify-cfg",
    llvm::cl::desc(
        "Enable non-trivial OSSA simplify-cfg and simple jump threading "
        "(staging)."));

// This requires new OwnershipOptUtilities which aren't well tested yet.
llvm::cl::opt<bool> EnableOSSARewriteTerminator(
    "enable-ossa-rewriteterminator",
    llvm::cl::desc(
        "Enable OSSA simplify-cfg with non-trivial terminator rewriting "
        "(staging)."));

llvm::cl::opt<bool> IsInfiniteJumpThreadingBudget(
    "sil-infinite-jump-threading-budget",
    llvm::cl::desc(
        "Use infinite budget for jump threading. Useful for testing purposes"));

STATISTIC(NumBlocksDeleted, "Number of unreachable blocks removed");
STATISTIC(NumBlocksMerged, "Number of blocks merged together");
STATISTIC(NumJumpThreads, "Number of jumps threaded");
STATISTIC(NumTermBlockSimplified, "Number of programterm block simplified");
STATISTIC(NumConstantFolded, "Number of terminators constant folded");
STATISTIC(NumDeadArguments, "Number of unused arguments removed");
STATISTIC(NumSROAArguments, "Number of aggregate argument levels split by "
                            "SROA");

//===----------------------------------------------------------------------===//
//                             CFG Simplification
//===----------------------------------------------------------------------===//

/// dominatorBasedSimplify iterates between dominator based simplification of
/// terminator branch condition values and CFG simplification. This is the
/// maximum number of iterations we run. The number is the maximum number of
/// iterations encountered when compiling the stdlib on April 2 2015.
///
static unsigned MaxIterationsOfDominatorBasedSimplify = 10;

static SILValue getTerminatorCondition(TermInst *Term) {
  if (auto *CondBr = dyn_cast<CondBranchInst>(Term))
    return stripExpectIntrinsic(CondBr->getCondition());

  if (auto *SEI = dyn_cast<SwitchEnumInst>(Term))
    return SEI->getOperand();

  return nullptr;
}

/// Is this basic block jump threadable.
static bool isThreadableBlock(SILBasicBlock *BB,
                              SmallPtrSetImpl<SILBasicBlock *> &LoopHeaders) {
  auto TI = BB->getTerminator();

  // We know how to handle cond_br and switch_enum.
  if (!isa<CondBranchInst>(TI) &&
      !isa<SwitchEnumInst>(TI))
    return false;

  if (LoopHeaders.count(BB))
    return false;

  unsigned Cost = 0;
  for (auto &Inst : *BB) {
    if (!Inst.isTriviallyDuplicatable())
      return false;

    // Don't jumpthread function calls.
    if (FullApplySite::isa(&Inst))
      return false;

    // Only thread 'small blocks'.
    if (instructionInlineCost(Inst) != InlineCost::Free)
      if (++Cost == 4)
        return false;
  }
  return true;
}

/// A description of an edge leading to a conditionally branching (or switching)
/// block and the successor block to thread to.
///
/// Src:
///   br Dest
///     \
///      \  Edge
///       v
///      Dest:
///        ...
///        switch/cond_br
///        /  \
///       ...  v
///            EnumCase/ThreadedSuccessorIdx
struct ThreadInfo {
  SILBasicBlock *Src;
  SILBasicBlock *Dest;
  EnumElementDecl *EnumCase;
  unsigned ThreadedSuccessorIdx;

  ThreadInfo(SILBasicBlock *Src, SILBasicBlock *Dest,
             unsigned ThreadedBlockSuccessorIdx)
      : Src(Src), Dest(Dest), EnumCase(nullptr),
        ThreadedSuccessorIdx(ThreadedBlockSuccessorIdx) {}

  ThreadInfo(SILBasicBlock *Src, SILBasicBlock *Dest, EnumElementDecl *EnumCase)
      : Src(Src), Dest(Dest), EnumCase(EnumCase), ThreadedSuccessorIdx(0) {}

  ThreadInfo() = default;
};

// FIXME: It would be far more efficient to clone the jump-threaded region using
// a single invocation of the RegionCloner (see ArrayPropertyOpt) instead of a
// BasicBlockCloner. Cloning a single block at a time forces the cloner to
// create four extra blocks that immediately become dead after the conditional
// branch and its clone is converted to an unconditional branch.
bool SimplifyCFG::threadEdge(const ThreadInfo &ti) {
  LLVM_DEBUG(llvm::dbgs() << "thread edge from bb" << ti.Src->getDebugID()
                          << " to bb" << ti.Dest->getDebugID() << '\n');
  auto *SrcTerm = cast<BranchInst>(ti.Src->getTerminator());

  BasicBlockCloner Cloner(SrcTerm->getDestBB());
  if (!Cloner.canCloneBlock())
    return false;

  Cloner.cloneBranchTarget(SrcTerm);

  // We have copied the threaded block into the edge.
  auto *clonedSrc = Cloner.getNewBB();
  SmallVector<SILBasicBlock *, 4> clonedSuccessors(
      clonedSrc->getSuccessorBlocks().begin(),
      clonedSrc->getSuccessorBlocks().end());
  SILBasicBlock *ThreadedSuccessorBlock = nullptr;

  // Rewrite the cloned branch to eliminate the non-taken path.
  if (auto *CondTerm = dyn_cast<CondBranchInst>(clonedSrc->getTerminator())) {
    // We know the direction this conditional branch is going to take thread
    // it.
    assert(clonedSrc->getSuccessors().size() > ti.ThreadedSuccessorIdx
           && "Threaded terminator does not have enough successors");

    ThreadedSuccessorBlock =
        clonedSrc->getSuccessors()[ti.ThreadedSuccessorIdx].getBB();
    auto Args = ti.ThreadedSuccessorIdx == 0 ? CondTerm->getTrueArgs()
                                             : CondTerm->getFalseArgs();

    SILBuilderWithScope(CondTerm).createBranch(CondTerm->getLoc(),
                                               ThreadedSuccessorBlock, Args);

    CondTerm->eraseFromParent();

  } else {
    // Get the enum element and the destination block of the block we jump
    // thread.
    auto *SEI = cast<SwitchEnumInst>(clonedSrc->getTerminator());
    ThreadedSuccessorBlock = SEI->getCaseDestination(ti.EnumCase);

    // Instantiate the payload if necessary.
    SILBuilderWithScope Builder(SEI);
    if (!ThreadedSuccessorBlock->args_empty()) {
      auto EnumVal = SEI->getOperand();
      auto EnumTy = EnumVal->getType();
      auto Loc = SEI->getLoc();
      auto Ty = EnumTy.getEnumElementType(ti.EnumCase, SEI->getModule(),
                                          Builder.getTypeExpansionContext());
      SILValue UED(
          Builder.createUncheckedEnumData(Loc, EnumVal, ti.EnumCase, Ty));
      assert(UED->getType()
                 == (*ThreadedSuccessorBlock->args_begin())->getType()
             && "Argument types must match");
      Builder.createBranch(SEI->getLoc(), ThreadedSuccessorBlock, {UED});

    } else {
      Builder.createBranch(SEI->getLoc(), ThreadedSuccessorBlock,
                           ArrayRef<SILValue>());
    }
    SEI->eraseFromParent();
  }
  // If the jump-threading target "Dest" had multiple predecessors, then the
  // cloner will have created unconditional branch predecessors, which can
  // now be removed or folded after converting the source block "Src" to an
  // unconditional branch.
  for (auto *succBB : clonedSuccessors) {
    removeIfDead(succBB);
  }
  if (auto *branchInst =
          dyn_cast<BranchInst>(ThreadedSuccessorBlock->getTerminator())) {
    simplifyBranchBlock(branchInst);
  }
  Cloner.updateSSAAfterCloning();
  return true;
}

/// Give a cond_br or switch_enum instruction and one successor block returns
/// true if we can infer the value of the condition/enum along the edge to these
/// successor blocks.
static bool isKnownEdgeValue(TermInst *Term, SILBasicBlock *SuccBB,
                             EnumElementDecl *&EnumCase) {
  assert((isa<CondBranchInst>(Term) || isa<SwitchEnumInst>(Term)) &&
         "Expect a cond_br or switch_enum");
  if (auto *SEI = dyn_cast<SwitchEnumInst>(Term)) {
    if (auto Case = SEI->getUniqueCaseForDestination(SuccBB)) {
      EnumCase = Case.get();
      return SuccBB->getSinglePredecessorBlock() != nullptr;
    }
    return false;
  }

  return SuccBB->getSinglePredecessorBlock() != nullptr;
}

/// Create an enum element by extracting the operand of a switch_enum.
static SILValue createEnumElement(SILBuilder &Builder,
                                  SwitchEnumInst *SEI,
                                  EnumElementDecl *EnumElement) {
  auto EnumVal = SEI->getOperand();
  // Do we have a payload.
  auto EnumTy = EnumVal->getType();
  if (EnumElement->hasAssociatedValues()) {
    auto Ty = EnumTy.getEnumElementType(EnumElement, SEI->getModule(),
                                        Builder.getTypeExpansionContext());
    SILValue UED(Builder.createUncheckedEnumData(SEI->getLoc(), EnumVal,
                                                 EnumElement, Ty));
    return Builder.createEnum(SEI->getLoc(), UED, EnumElement, EnumTy);
  }
  return Builder.createEnum(SEI->getLoc(), SILValue(), EnumElement, EnumTy);
}

/// Create a value for the condition of the terminator that flows along the edge
/// with 'EdgeIdx'. Insert it before the 'UserInst'.
static SILValue createValueForEdge(SILInstruction *UserInst,
                                   SILInstruction *DominatingTerminator,
                                   unsigned EdgeIdx) {
  SILBuilderWithScope Builder(UserInst);

  if (auto *CBI = dyn_cast<CondBranchInst>(DominatingTerminator))
    return Builder.createIntegerLiteral(
        CBI->getLoc(), CBI->getCondition()->getType(), EdgeIdx == 0 ? -1 : 0);

  auto *SEI = cast<SwitchEnumInst>(DominatingTerminator);
  auto *DstBlock = SEI->getSuccessors()[EdgeIdx].getBB();
  auto Case = SEI->getUniqueCaseForDestination(DstBlock);
  assert(Case && "No unique case found for destination block");
  return createEnumElement(Builder, SEI, Case.get());
}

/// Perform dominator based value simplifications and jump threading on all users
/// of the operand of 'DominatingBB's terminator.
static bool tryDominatorBasedSimplifications(
    SILBasicBlock *DominatingBB, DominanceInfo *DT,
    SmallPtrSetImpl<SILBasicBlock *> &LoopHeaders,
    SmallVectorImpl<ThreadInfo> &JumpThreadableEdges,
    llvm::DenseSet<std::pair<SILBasicBlock *, SILBasicBlock *>>
        &ThreadedEdgeSet,
    bool TryJumpThreading,
    BasicBlockFlag &isThreadable, BasicBlockFlag &threadableComputed) {
  auto *DominatingTerminator = DominatingBB->getTerminator();

  // We handle value propagation from cond_br and switch_enum terminators.
  bool IsEnumValue = isa<SwitchEnumInst>(DominatingTerminator);
  if (!isa<CondBranchInst>(DominatingTerminator) && !IsEnumValue)
    return false;

  auto DominatingCondition = getTerminatorCondition(DominatingTerminator);
  if (!DominatingCondition)
    return false;
  if (isa<SILUndef>(DominatingCondition))
    return false;

  bool Changed = false;

  // We will look at all the outgoing edges from the conditional branch to see
  // whether any other uses of the condition or uses of the condition along an
  // edge are dominated by said outgoing edges. The outgoing edge carries the
  // value on which we switch/cond_branch.
  auto Succs = DominatingBB->getSuccessors();
  for (unsigned Idx = 0; Idx < Succs.size(); ++Idx) {
    auto *DominatingSuccBB = Succs[Idx].getBB();

    EnumElementDecl *EnumCase = nullptr;
    if (!isKnownEdgeValue(DominatingTerminator, DominatingSuccBB, EnumCase))
      continue;

    // Look for other uses of DominatingCondition that are either:
    //  * dominated by the DominatingSuccBB
    //
    //     cond_br %dominating_cond / switch_enum
    //       /
    //      /
    //     /
    //   DominatingSuccBB:
    //     ...
    //     use %dominating_cond
    //
    //  * are a conditional branch that has an incoming edge that is
    //  dominated by DominatingSuccBB.
    //
    //     cond_br %dominating_cond
    //     /
    //    /
    //   /
    //
    //  DominatingSuccBB:
    //   ...
    //   br DestBB
    //
    //    \
    //     \ E -> %dominating_cond = true
    //      \
    //       v
    //        DestBB
    //          cond_br %dominating_cond
    SmallVector<SILInstruction *, 16> UsersToReplace;
    for (auto *Op : ignore_expect_uses(DominatingCondition)) {
      auto *CondUserInst = Op->getUser();

      // Ignore the DominatingTerminator itself.
      if (CondUserInst->getParent() == DominatingBB)
        continue;

      // For enum values we are only interested in switch_enum and select_enum
      // users.
      if (IsEnumValue && !isa<SwitchEnumInst>(CondUserInst) &&
          !isa<SelectEnumInst>(CondUserInst))
        continue;

      // If the use is dominated we can replace this use by the value
      // flowing to DominatingSuccBB.
      if (DT->dominates(DominatingSuccBB, CondUserInst->getParent())) {
        UsersToReplace.push_back(CondUserInst);
        continue;
      }

      // Jump threading is expensive so we don't always do it.
      if (!TryJumpThreading)
        continue;

      auto *DestBB = CondUserInst->getParent();

      // The user must be the terminator we are trying to jump thread.
      if (CondUserInst != DestBB->getTerminator())
        continue;

      // Check whether we have seen this destination block already.
      if (!threadableComputed.testAndSet(DestBB))
        isThreadable.set(DestBB, isThreadableBlock(DestBB, LoopHeaders));

      // If the use is a conditional branch/switch then look for an incoming
      // edge that is dominated by DominatingSuccBB.
      if (isThreadable.get(DestBB)) {
        auto Preds = DestBB->getPredecessorBlocks();

        for (SILBasicBlock *PredBB : Preds) {
          if (!isa<BranchInst>(PredBB->getTerminator()))
            continue;
          if (!DT->dominates(DominatingSuccBB, PredBB))
            continue;

          // Don't jumpthread the same edge twice.
          if (!ThreadedEdgeSet.insert(std::make_pair(PredBB, DestBB)).second)
            continue;

          if (isa<CondBranchInst>(DestBB->getTerminator()))
            JumpThreadableEdges.push_back(ThreadInfo(PredBB, DestBB, Idx));
          else
            JumpThreadableEdges.push_back(ThreadInfo(PredBB, DestBB, EnumCase));
          break;
        }
      }
    }

    // Replace dominated user instructions.
    for (auto *UserInst : UsersToReplace) {
      SILValue EdgeValue;
      for (auto &Op : UserInst->getAllOperands()) {
        if (stripExpectIntrinsic(Op.get()) == DominatingCondition) {
          if (!EdgeValue)
            EdgeValue = createValueForEdge(UserInst, DominatingTerminator, Idx);
          LLVM_DEBUG(llvm::dbgs() << "replace dominated operand\n  in "
                     << *UserInst << "  with " << EdgeValue);
          Op.set(EdgeValue);
          Changed = true;
        }
      }
    }
  }
  return Changed;
}

/// Propagate values of branched upon values along the outgoing edges down the
/// dominator tree.
bool SimplifyCFG::dominatorBasedSimplifications(SILFunction &Fn,
                                                DominanceInfo *DT) {
  bool Changed = false;
  // Collect jump threadable edges and propagate outgoing edge values of
  // conditional branches/switches.
  SmallVector<ThreadInfo, 8> JumpThreadableEdges;
  BasicBlockFlag isThreadable(&Fn);
  BasicBlockFlag threadableComputed(&Fn);
  llvm::DenseSet<std::pair<SILBasicBlock *, SILBasicBlock *>> ThreadedEdgeSet;
  for (auto &BB : Fn) {
    if (DT->getNode(&BB)) {
      if (!transform.continueWithNextSubpassRun(BB.getTerminator()))
        return Changed;
      // Only handle reachable blocks.
      Changed |= tryDominatorBasedSimplifications(
          &BB, DT, LoopHeaders, JumpThreadableEdges, ThreadedEdgeSet,
          EnableJumpThread, isThreadable, threadableComputed);
    }
  }

  // Nothing to jump thread?
  if (JumpThreadableEdges.empty())
    return Changed;

  for (auto &ThreadInfo : JumpThreadableEdges) {
    if (!transform.continueWithNextSubpassRun())
      return Changed;
    if (threadEdge(ThreadInfo)) {
      Changed = true;
    }
  }

  return Changed;
}

/// Simplify terminators that could have been simplified by threading.
bool SimplifyCFG::simplifyThreadedTerminators() {
  bool HaveChangedCFG = false;
  for (auto &BB : Fn) {
    auto *Term = BB.getTerminator();
    if (!transform.continueWithNextSubpassRun(Term))
      return HaveChangedCFG;
    // Simplify a switch_enum.
    if (auto *SEI = dyn_cast<SwitchEnumInst>(Term)) {
      if (auto *EI = dyn_cast<EnumInst>(SEI->getOperand())) {
        LLVM_DEBUG(llvm::dbgs() << "simplify threaded " << *SEI);
        auto *LiveBlock = SEI->getCaseDestination(EI->getElement());
        if (EI->hasOperand() && !LiveBlock->args_empty())
          SILBuilderWithScope(SEI)
              .createBranch(SEI->getLoc(), LiveBlock, EI->getOperand());
        else
          SILBuilderWithScope(SEI).createBranch(SEI->getLoc(), LiveBlock);
        SEI->eraseFromParent();
        if (EI->use_empty())
          EI->eraseFromParent();
        HaveChangedCFG = true;
      }
      continue;
    } else if (auto *CondBr = dyn_cast<CondBranchInst>(Term)) {
      // If the condition is an integer literal, we can constant fold the
      // branch.
      if (auto *IL = dyn_cast<IntegerLiteralInst>(CondBr->getCondition())) {
        LLVM_DEBUG(llvm::dbgs() << "simplify threaded " << *CondBr);
        SILBasicBlock *TrueSide = CondBr->getTrueBB();
        SILBasicBlock *FalseSide = CondBr->getFalseBB();
        auto TrueArgs = CondBr->getTrueArgs();
        auto FalseArgs = CondBr->getFalseArgs();
        bool isFalse = !IL->getValue();
        auto LiveArgs = isFalse ? FalseArgs : TrueArgs;
        auto *LiveBlock = isFalse ? FalseSide : TrueSide;
        SILBuilderWithScope(CondBr)
            .createBranch(CondBr->getLoc(), LiveBlock, LiveArgs);
        CondBr->eraseFromParent();
        if (IL->use_empty())
          IL->eraseFromParent();
        HaveChangedCFG = true;
      }
    }
  }
  return HaveChangedCFG;
}

// Simplifications that walk the dominator tree to prove redundancy in
// conditional branching.
bool SimplifyCFG::dominatorBasedSimplify(DominanceAnalysis *DA) {
  // Get the dominator tree.
  DT = DA->get(&Fn);

  if (!EnableOSSASimplifyCFG && Fn.hasOwnership())
    return false;

  // Split all critical edges such that we can move code onto edges. This is
  // also required for SSA construction in dominatorBasedSimplifications' jump
  // threading. It only splits new critical edges it creates by jump threading.
  bool Changed = false;
  if (!Fn.hasOwnership() && EnableJumpThread) {
    Changed = splitAllCriticalEdges(Fn, DT, nullptr);
  }
  unsigned MaxIter = MaxIterationsOfDominatorBasedSimplify;
  SmallVector<SILBasicBlock *, 16> BlocksForWorklist;

  bool HasChangedInCurrentIter;
  do {
    HasChangedInCurrentIter = false;

    if (!transform.continueWithNextSubpassRun())
      return Changed;

    // Do dominator based simplification of terminator condition. This does not
    // and MUST NOT change the CFG without updating the dominator tree to
    // reflect such change.
    if (tryCheckedCastBrJumpThreading(&Fn, DT, deBlocks, BlocksForWorklist,
                                      EnableOSSARewriteTerminator)) {
      for (auto BB: BlocksForWorklist)
        addToWorklist(BB);

      HasChangedInCurrentIter = true;
      DT->recalculate(Fn);
    }
    BlocksForWorklist.clear();

    if (ShouldVerify)
      DT->verify();

    // Simplify the block argument list. This is extremely subtle: simplifyArgs
    // will not change the CFG iff the DT is null. Really we should move that
    // one optimization out of simplifyArgs ... I am squinting at you
    // simplifySwitchEnumToSelectEnum.
    // simplifyArgs does use the dominator tree, though.
    for (auto &BB : Fn) {
      if (!transform.continueWithNextSubpassRun(BB.getTerminator()))
        return Changed;

      HasChangedInCurrentIter |= simplifyArgs(&BB);
    }

    if (ShouldVerify)
      DT->verify();

    // Jump thread.
    if (dominatorBasedSimplifications(Fn, DT)) {
      if (!transform.continueWithNextSubpassRun())
        return true;
      DominanceInfo *InvalidDT = DT;
      DT = nullptr;
      HasChangedInCurrentIter = true;
      // Simplify terminators.
      simplifyThreadedTerminators();
      DT = InvalidDT;
      DT->recalculate(Fn);
    }

    Changed |= HasChangedInCurrentIter;
  } while (HasChangedInCurrentIter && --MaxIter);

  // Do the simplification that requires both the dom and postdom tree.
  for (auto &BB : Fn)
    Changed |= simplifyArgs(&BB);

  if (ShouldVerify)
    DT->verify();

  // The functions we used to simplify the CFG put things in the worklist. Clear
  // it here.
  clearWorklist();
  return Changed;
}

// If BB is trivially unreachable, remove it from the worklist, add its
// successors to the worklist, and then remove the block.
bool SimplifyCFG::removeIfDead(SILBasicBlock *BB) {
  if (!BB->pred_empty() || BB == &*Fn.begin())
    return false;

  removeFromWorklist(BB);

  // Add successor blocks to the worklist since their predecessor list is about
  // to change.
  for (auto &S : BB->getSuccessors())
    addToWorklist(S);

  LLVM_DEBUG(llvm::dbgs() << "remove dead bb" << BB->getDebugID() << '\n');
  removeDeadBlock(BB);
  ++NumBlocksDeleted;
  return true;
}

/// This is called when a predecessor of a block is dropped, to simplify the
/// block and add it to the worklist.
bool SimplifyCFG::simplifyAfterDroppingPredecessor(SILBasicBlock *BB) {
  // TODO: If BB has only one predecessor and has bb args, fold them away, then
  // use instsimplify on all the users of those values - even ones outside that
  // block.


  // Make sure that DestBB is in the worklist, as well as its remaining
  // predecessors, since they may not be able to be simplified.
  addToWorklist(BB);
  for (auto *P : BB->getPredecessorBlocks())
    addToWorklist(P);

  return false;
}

/// This is called after \p BB has been cloned during jump-threading
/// (tail-duplication) and the new critical edge on its successor has been
/// split. This is necessary to continue jump-threading through the split
/// critical edge (since we only jump-thread one block at a time).
bool SimplifyCFG::addToWorklistAfterSplittingEdges(SILBasicBlock *BB) {
  addToWorklist(BB);
  for (auto *succBB : BB->getSuccessorBlocks()) {
    addToWorklist(succBB);
  }
  return false;
}

static NullablePtr<EnumElementDecl>
getEnumCaseRecursive(SILValue Val, SILBasicBlock *UsedInBB, int RecursionDepth,
                     llvm::SmallPtrSetImpl<SILArgument *> &HandledArgs) {
  // Limit the number of recursions. This is an easy way to cope with cycles
  // in the SSA graph.
  if (RecursionDepth > 3)
    return nullptr;

  // Handle the obvious case.
  if (auto *EI = dyn_cast<EnumInst>(Val))
    return EI->getElement();

  // Check if the value is dominated by a switch_enum, e.g.
  //   switch_enum %val, case A: bb1, case B: bb2
  // bb1:
  //   use %val   // We know that %val has case A
  SILBasicBlock *Pred = UsedInBB->getSinglePredecessorBlock();
  int Limit = 3;
  // A very simple dominator check: just walk up the single predecessor chain.
  // The limit is just there to not run into an infinite loop in case of an
  // unreachable CFG cycle.
  while (Pred && --Limit > 0) {
    if (auto *PredSEI = dyn_cast<SwitchEnumInst>(Pred->getTerminator())) {
      if (PredSEI->getOperand() == Val)
        return PredSEI->getUniqueCaseForDestination(UsedInBB);
    }
    UsedInBB = Pred;
    Pred = UsedInBB->getSinglePredecessorBlock();
  }

  // In case of a phi, recursively check the enum cases of all
  // incoming predecessors.
  if (auto *Arg = dyn_cast<SILArgument>(Val)) {
    HandledArgs.insert(Arg);
    llvm::SmallVector<std::pair<SILBasicBlock *, SILValue>, 8> IncomingVals;
    if (!Arg->getIncomingPhiValues(IncomingVals))
      return nullptr;

    EnumElementDecl *CommonCase = nullptr;
    for (std::pair<SILBasicBlock *, SILValue> Incoming : IncomingVals) {
      SILBasicBlock *IncomingBlock = Incoming.first;
      SILValue IncomingVal = Incoming.second;

      auto *IncomingArg = dyn_cast<SILArgument>(IncomingVal);
      if (IncomingArg && HandledArgs.count(IncomingArg) != 0)
        continue;

      NullablePtr<EnumElementDecl> IncomingCase =
        getEnumCaseRecursive(Incoming.second, IncomingBlock, RecursionDepth + 1,
                             HandledArgs);
      if (!IncomingCase)
        return nullptr;
      if (IncomingCase.get() != CommonCase) {
        if (CommonCase)
          return nullptr;
        CommonCase = IncomingCase.get();
      }
    }
    return CommonCase;
  }
  return nullptr;
}

/// Tries to figure out the enum case of an enum value \p Val which is used in
/// block \p UsedInBB.
static NullablePtr<EnumElementDecl> getEnumCase(SILValue Val,
                                                SILBasicBlock *UsedInBB) {
  llvm::SmallPtrSet<SILArgument *, 8> HandledArgs;
  return getEnumCaseRecursive(Val, UsedInBB, /*RecursionDepth*/ 0, HandledArgs);
}

static int getThreadingCost(SILInstruction *I) {
  if (!I->isTriviallyDuplicatable())
    return 1000;

  // Don't jumpthread function calls.
  if (isa<ApplyInst>(I))
    return 1000;

  // This is a really trivial cost model, which is only intended as a starting
  // point.
  if (instructionInlineCost(*I) != InlineCost::Free)
    return 1;

  return 0;
}

static int maxBranchRecursionDepth = 6;
/// couldSimplifyUsers - Check to see if any simplifications are possible if
/// "Val" is substituted for BBArg.  If so, return true, if nothing obvious
/// is possible, return false.
static bool couldSimplifyEnumUsers(SILArgument *BBArg, int Budget,
                                   int recursionDepth = 0) {
  SILBasicBlock *BB = BBArg->getParent();
  int BudgetForBranch = 100;

  for (Operand *UI : BBArg->getUses()) {
    auto *User = UI->getUser();
    if (User->getParent() != BB)
      continue;

    // We only know we can simplify if the switch_enum user is in the block we
    // are trying to jump thread.
    // The value must not be define in the same basic block as the switch enum
    // user. If this is the case we have a single block switch_enum loop.
    if (isa<SwitchEnumInst>(User) || isa<SelectEnumInst>(User))
      return true;

    // Also allow enum of enum, which usually can be combined to a single
    // instruction. This helps to simplify the creation of an enum from an
    // integer raw value.
    if (isa<EnumInst>(User))
      return true;

    if (auto *SWI = dyn_cast<SwitchValueInst>(User)) {
      if (SWI->getOperand() == BBArg)
        return true;
    }

    if (auto *BI = dyn_cast<BranchInst>(User)) {
      if (recursionDepth >= maxBranchRecursionDepth) {
        return false;
      }
      if (BudgetForBranch > Budget) {
        BudgetForBranch = Budget;
        for (SILInstruction &I : *BB) {
          BudgetForBranch -= getThreadingCost(&I);
          if (BudgetForBranch < 0)
            break;
        }
      }
      if (BudgetForBranch > 0) {
        SILBasicBlock *DestBB = BI->getDestBB();
        unsigned OpIdx = UI->getOperandNumber();
        if (couldSimplifyEnumUsers(DestBB->getArgument(OpIdx), BudgetForBranch,
                                   recursionDepth + 1))
          return true;
      }
    }
  }
  return false;
}

void SimplifyCFG::findLoopHeaders() {
  /// Find back edges in the CFG. This performs a dfs search and identifies
  /// back edges as edges going to an ancestor in the dfs search. If a basic
  /// block is the target of such a back edge we will identify it as a header.
  LoopHeaders.clear();

  BasicBlockSet Visited(&Fn);
  BasicBlockSet InDFSStack(&Fn);
  SmallVector<std::pair<SILBasicBlock *, SILBasicBlock::succ_iterator>, 16>
      DFSStack;

  auto EntryBB = &Fn.front();
  DFSStack.push_back(std::make_pair(EntryBB, EntryBB->succ_begin()));
  Visited.insert(EntryBB);
  InDFSStack.insert(EntryBB);

  while (!DFSStack.empty()) {
    auto &D = DFSStack.back();
    // No successors.
    if (D.second == D.first->succ_end()) {
      // Retreat the dfs search.
      DFSStack.pop_back();
      InDFSStack.erase(D.first);
    } else {
      // Visit the next successor.
      SILBasicBlock *NextSucc = *(D.second);
      ++D.second;
      if (Visited.insert(NextSucc)) {
        InDFSStack.insert(NextSucc);
        DFSStack.push_back(std::make_pair(NextSucc, NextSucc->succ_begin()));
      } else if (InDFSStack.contains(NextSucc)) {
        // We have already visited this node and it is in our dfs search. This
        // is a back-edge.
        LoopHeaders.insert(NextSucc);
      }
    }
  }
}

static bool couldRemoveRelease(SILBasicBlock *SrcBB, SILValue SrcV,
                               SILBasicBlock *DestBB, SILValue DestV) {
  bool IsRetainOfSrc = false;
  for (auto *U: SrcV->getUses())
    if (U->getUser()->getParent() == SrcBB &&
        (isa<StrongRetainInst>(U->getUser()) ||
         isa<RetainValueInst>(U->getUser()))) {
      IsRetainOfSrc = true;
      break;
    }
  if (!IsRetainOfSrc)
    return false;

  bool IsReleaseOfDest = false;
  for (auto *U: DestV->getUses())
    if (U->getUser()->getParent() == DestBB &&
        (isa<StrongReleaseInst>(U->getUser()) ||
         isa<ReleaseValueInst>(U->getUser()))) {
      IsReleaseOfDest = true;
      break;
    }

  return IsReleaseOfDest;
}

/// Returns true if any instruction in \p block may write memory.
static bool blockMayWriteMemory(SILBasicBlock *block) {
  for (auto instAndIdx : llvm::enumerate(*block)) {
    if (instAndIdx.value().mayWriteToMemory())
      return true;
    // Only look at the first 20 instructions to avoid compile time problems for
    // corner cases of very large blocks without memory writes.
    // 20 instructions is more than enough.
    if (instAndIdx.index() > 50)
      return true;
  }
  return false;
}

// Returns true if \p block contains an injected an enum case into \p enumAddr
// which is valid at the end of the block.
static bool hasInjectedEnumAtEndOfBlock(SILBasicBlock *block, SILValue enumAddr) {
  for (auto instAndIdx : llvm::enumerate(llvm::reverse(*block))) {
    SILInstruction &inst = instAndIdx.value();
    if (auto *injectInst = dyn_cast<InjectEnumAddrInst>(&inst)) {
      return injectInst->getOperand() == enumAddr;
    }
    if (inst.mayWriteToMemory())
      return false;
    // Only look at the first 20 instructions to avoid compile time problems for
    // corner cases of very large blocks without memory writes.
    // 20 instructions is more than enough.
    if (instAndIdx.index() > 50)
      return false;
  }
  return false;
}

/// tryJumpThreading - Check to see if it looks profitable to duplicate the
/// destination of an unconditional jump into the bottom of this block.
bool SimplifyCFG::tryJumpThreading(BranchInst *BI) {
  if (!EnableOSSASimplifyCFG && Fn.hasOwnership())
    return false;

  auto *DestBB = BI->getDestBB();
  auto *SrcBB = BI->getParent();
  TermInst *destTerminator = DestBB->getTerminator();
  if (!EnableOSSARewriteTerminator && Fn.hasOwnership()) {
    if (llvm::any_of(DestBB->getArguments(), [this](SILValue op) {
          return !op->getType().isTrivial(Fn);
        })) {
      return false;
   }
  }
  // If the destination block ends with a return, we don't want to duplicate it.
  // We want to maintain the canonical form of a single return where possible.
  if (destTerminator->isFunctionExiting())
    return false;

  // There is no benefit duplicating such a destination.
  if (DestBB->getSinglePredecessorBlock() != nullptr) {
    return false;
  }

  // Jump threading only makes sense if there is an argument on the branch
  // (which is reacted on in the DestBB), or if this goes through a memory
  // location (switch_enum_addr is the only address-instruction which we
  // currently handle).
  if (BI->getArgs().empty() && !isa<SwitchEnumAddrInst>(destTerminator))
    return false;
      
  // We don't have a great cost model at the SIL level, so we don't want to
  // blissly duplicate tons of code with a goal of improved performance (we'll
  // leave that to LLVM).  However, doing limited code duplication can lead to
  // major second order simplifications.  Here we only do it if there are
  // "constant" arguments to the branch or if we know how to fold something
  // given the duplication.
  int ThreadingBudget = IsInfiniteJumpThreadingBudget ? INT_MAX : 0;

  for (unsigned i : indices(BI->getArgs())) {
    SILValue Arg = BI->getArg(i);

    // TODO: Verify if we need to jump thread to remove releases in OSSA.
    // If the value being substituted on is release there is a chance we could
    // remove the release after jump threading.
    if (!Arg->getType().isTrivial(*SrcBB->getParent()) &&
        couldRemoveRelease(SrcBB, Arg, DestBB,
                           DestBB->getArgument(i))) {
        ThreadingBudget = 8;
        break;
    }

    // If the value being substituted is an enum, check to see if there are any
    // switches on it.
    if (!getEnumCase(Arg, BI->getParent()) &&
        !isa<IntegerLiteralInst>(Arg))
      continue;

    if (couldSimplifyEnumUsers(DestBB->getArgument(i), 8)) {
      ThreadingBudget = 8;
      break;
    }
  }

  if (ThreadingBudget == 0) {
    if (isa<CondBranchInst>(destTerminator)) {
      for (auto V : BI->getArgs()) {
        if (isa<IntegerLiteralInst>(V) || isa<FloatLiteralInst>(V)) {
          ThreadingBudget = 4;
          break;
        }
      }
    } else if (auto *SEA = dyn_cast<SwitchEnumAddrInst>(destTerminator)) {
      // If the branch-block injects a certain enum case and the destination
      // switches on that enum, it's worth jump threading. E.g.
      //
      //   inject_enum_addr %enum : $*Optional<T>, #Optional.some
      //   ... // no memory writes here
      //   br DestBB
      // DestBB:
      //   ... // no memory writes here
      //   switch_enum_addr %enum : $*Optional<T>, case #Optional.some ...
      //
      SILValue enumAddr = SEA->getOperand();
      if (!blockMayWriteMemory(DestBB) &&
          hasInjectedEnumAtEndOfBlock(SrcBB, enumAddr)) {
        ThreadingBudget = 4;
      }
    }
  }

  ThreadingBudget -= JumpThreadingCost[SrcBB];
  ThreadingBudget -= JumpThreadingCost[DestBB];

  // If we don't have anything that we can simplify, don't do it.
  if (ThreadingBudget <= 0)
    return false;

  // Don't jump thread through a potential header - this can produce irreducible
  // control flow and lead to infinite loop peeling.
  bool DestIsLoopHeader = (LoopHeaders.count(DestBB) != 0);
  if (DestIsLoopHeader) {
    // Make an exception for switch_enum, but only if it's block was not already
    // peeled out of it's original loop. In that case, further jump threading
    // can accomplish nothing, and the loop will be infinitely peeled.
    if (!isa<SwitchEnumInst>(destTerminator) || ClonedLoopHeaders.count(DestBB))
      return false;
  }

  // If it looks potentially interesting, decide whether we *can* do the
  // operation and whether the block is small enough to be worth duplicating.
  int copyCosts = 0;
  BasicBlockCloner Cloner(DestBB);
  for (auto &inst : *DestBB) {
    copyCosts += getThreadingCost(&inst);
    if (ThreadingBudget <= copyCosts)
      return false;

    // If this is an address projection with outside uses, sink it before
    // checking for SSA update.
    if (!Cloner.canCloneInstruction(&inst))
      return false;
  }
  LLVM_DEBUG(llvm::dbgs() << "jump thread from bb" << SrcBB->getDebugID()
                          << " to bb" << DestBB->getDebugID() << '\n');

  JumpThreadingCost[DestBB] += copyCosts;

  // Duplicate the destination block into this one, rewriting uses of the BBArgs
  // to use the branch arguments as we go.
  Cloner.cloneBranchTarget(BI);
  Cloner.updateSSAAfterCloning();

  // Once all the instructions are copied, we can nuke BI itself.  We also add
  // the threaded and edge block to the worklist now that they (likely) can be
  // simplified.
  addToWorklist(SrcBB);

  // Simplify the cloned block and continue jump-threading through its new
  // successors edges.
  addToWorklistAfterSplittingEdges(Cloner.getNewBB());

  // We may be able to simplify DestBB now that it has one fewer predecessor.
  simplifyAfterDroppingPredecessor(DestBB);

  // If we jump-thread a switch_enum in the loop header, we have to recalculate
  // the loop header info.
  //
  // FIXME: findLoopHeaders should not be called repeatedly during simplify-cfg
  // iteration. It is a whole-function analysis! It also does no nothing help to
  // avoid infinite loop peeling.
  if (DestIsLoopHeader) {
    ClonedLoopHeaders.insert(Cloner.getNewBB());
    findLoopHeaders();
  }

  ++NumJumpThreads;
  return true;
}


/// simplifyBranchOperands - Simplify operands of branches, since it can
/// result in exposing opportunities for CFG simplification.
bool SimplifyCFG::simplifyBranchOperands(OperandValueArrayRef Operands) {
  InstModCallbacks callbacks;
#ifndef NDEBUG
  callbacks = callbacks.onDelete(
    [](SILInstruction *instToKill) {
      LLVM_DEBUG(llvm::dbgs() << "simplify and erase " << *instToKill);
      instToKill->eraseFromParent();
    });
#endif

  for (auto O = Operands.begin(), E = Operands.end(); O != E; ++O) {
    // All of our interesting simplifications are on single-value instructions
    // for now.
    if (auto *I = dyn_cast<SingleValueInstruction>(*O)) {
      simplifyAndReplaceAllSimplifiedUsesAndErase(I, callbacks, deBlocks);
    }
  }
  return callbacks.hadCallbackInvocation();
}

static bool onlyHasTerminatorAndDebugInsts(SILBasicBlock *BB) {
  TermInst *Terminator = BB->getTerminator();
  SILBasicBlock::iterator Iter = BB->begin();
  while (&*Iter != Terminator) {
    if (!(&*Iter)->isDebugInstruction())
      return false;
    ++Iter;
  }
  return true;
}

namespace {

/// Will be valid if the constructor's targetBB has a a single branch and all
/// its block arguments are only used by that branch.
struct TrampolineDest {
  SILBasicBlock *destBB = nullptr;
  // Source block's branch args after bypassing targetBB.
  SmallVector<SILValue, 4> newSourceBranchArgs;

  TrampolineDest() {}
  TrampolineDest(SILBasicBlock *sourceBB, SILBasicBlock *targetBB);
  TrampolineDest(const TrampolineDest &) = delete;
  TrampolineDest &operator=(const TrampolineDest &) = delete;
  TrampolineDest(TrampolineDest &&) = default;
  TrampolineDest &operator=(TrampolineDest &&) = default;

  bool operator==(const TrampolineDest &rhs) {
    return destBB == rhs.destBB
           && newSourceBranchArgs == rhs.newSourceBranchArgs;
  }
  bool operator!=(const TrampolineDest &rhs) {
    return !(*this == rhs);
  }

  operator bool() const { return destBB != nullptr; }
};

} // end anonymous namespace

TrampolineDest::TrampolineDest(SILBasicBlock *sourceBB,
                               SILBasicBlock *targetBB) {
  // Ignore blocks with more than one instruction.
  if (!onlyHasTerminatorAndDebugInsts(targetBB))
    return;

  auto *targetBranch = dyn_cast<BranchInst>(targetBB->getTerminator());
  if (!targetBranch)
    return;

  // Disallow infinite loops through targetBB.
  BasicBlockSet VisitedBBs(sourceBB->getParent());
  BranchInst *nextBI = targetBranch;
  do {
    SILBasicBlock *nextBB = nextBI->getDestBB();
    // We don't care about infinite loops after SBB.
    if (!VisitedBBs.insert(nextBB))
      break;
    // Only if the infinite loop goes through SBB directly we bail.
    if (nextBB == targetBB)
      return;
    nextBI = dyn_cast<BranchInst>(nextBB->getTerminator());
  } while (nextBI);

  // Check that all the target block arguments are only used by the branch.
  //
  // TODO: OSSA; also handle dead block args that are trivial or destroyed in
  // the same block.
  for (SILValue blockArg : targetBB->getArguments()) {
    Operand *operand = blockArg->getSingleUse();
    if (!operand || operand->getUser() != targetBranch) {
      return;
    }
  }
  SILBasicBlock *destBlock = targetBranch->getDestBB();
  newSourceBranchArgs.reserve(targetBranch->getArgs().size());
  for (SILValue branchArg : targetBranch->getArgs()) {
    if (branchArg->getParentBlock() == destBlock) {
      // This can happen if the involved blocks are part of an unreachable CFG
      // cycle (dominance is not meaningful in such a case).
      return;
    }
    if (branchArg->getParentBlock() == targetBB) {
      auto *phi = dyn_cast<SILPhiArgument>(branchArg);
      if (!phi || !phi->isPhi()) {
        return;
      }
      branchArg = phi->getIncomingPhiValue(sourceBB);
    }
    newSourceBranchArgs.push_back(branchArg);
  }
  // Setting destBB constructs a valid TrampolineDest.
  destBB = destBlock;
}

#ifndef NDEBUG
/// Is the block reachable from the entry.
static bool isReachable(SILBasicBlock *Block) {
  BasicBlockWorklist Worklist(Block->getParent()->getEntryBlock());

  while (SILBasicBlock *CurBB = Worklist.pop()) {
    if (CurBB == Block)
      return true;

    for (SILBasicBlock *Succ : CurBB->getSuccessors()) {
      Worklist.pushIfNotVisited(Succ);
    }
  }

  return false;
}
#endif

static llvm::cl::opt<bool> SimplifyUnconditionalBranches(
    "simplify-cfg-simplify-unconditional-branches", llvm::cl::init(true));

/// Returns true if \p block has less instructions than \p other.
static bool hasLessInstructions(SILBasicBlock *block, SILBasicBlock *other) {
  auto blockIter = block->begin();
  auto blockEnd = block->end();
  auto otherIter = other->begin();
  auto otherEnd = other->end();
  while (true) {
    if (otherIter == otherEnd)
      return false;
    if (blockIter == blockEnd)
      return true;
    ++blockIter;
    ++otherIter;
  }
}

/// simplifyBranchBlock - Simplify a basic block that ends with an unconditional
/// branch.
///
/// Performs trivial trampoline removal. May be called as a utility to cleanup
/// successors after removing conditional branches or predecessors after
/// deleting unreachable blocks.
bool SimplifyCFG::simplifyBranchBlock(BranchInst *BI) {
  // If we are asked to not simplify unconditional branches (for testing
  // purposes), exit early.
  if (!SimplifyUnconditionalBranches)
    return false;

  // First simplify instructions generating branch operands since that
  // can expose CFG simplifications.
  bool Simplified = simplifyBranchOperands(BI->getArgs());

  auto *BB = BI->getParent(), *DestBB = BI->getDestBB();

  // If this block branches to a block with a single predecessor, then
  // merge the DestBB into this BB.
  if (BB != DestBB && DestBB->getSinglePredecessorBlock()) {
    LLVM_DEBUG(llvm::dbgs() << "merge bb" << BB->getDebugID() << " with bb"
                            << DestBB->getDebugID() << '\n');

    for (unsigned i = 0, e = BI->getArgs().size(); i != e; ++i) {
      if (DestBB->getArgument(i) == BI->getArg(i)) {
        // We must be processing an unreachable part of the cfg with a cycle.
        // bb1(arg1): // preds: bb3
        //   br bb2
        //
        // bb2: // preds: bb1
        //   br bb3
        //
        // bb3: // preds: bb2
        //   br bb1(arg1)
        assert(!isReachable(BB) && "Should only occur in unreachable block");
        return Simplified;
      }
    }
    
    // If there are any BB arguments in the destination, replace them with the
    // branch operands, since they must dominate the dest block.
    for (unsigned i = 0, e = BI->getArgs().size(); i != e; ++i) {
      assert(DestBB->getArgument(i) != BI->getArg(i));
      SILValue Val = BI->getArg(i);
      DestBB->getArgument(i)->replaceAllUsesWith(Val);
      if (!isVeryLargeFunction) {
        if (auto *I = dyn_cast<SingleValueInstruction>(Val)) {
          // Replacing operands may trigger constant folding which then could
          // trigger other simplify-CFG optimizations.
          ConstFolder.addToWorklist(I);
          ConstFolder.processWorkList();
        }
      }
    }

    BI->eraseFromParent();

    // Move instruction from the smaller block to the larger block.
    // The order is essential because if many blocks are merged and this is done
    // in the wrong order, we end up with quadratic complexity.
    //
    SILBasicBlock *remainingBlock = nullptr, *deletedBlock = nullptr;
    if (BB != Fn.getEntryBlock() && hasLessInstructions(BB, DestBB)) {
      DestBB->spliceAtBegin(BB);
      DestBB->dropAllArguments();
      DestBB->moveArgumentList(BB);
      while (!BB->pred_empty()) {
        SILBasicBlock *pred = *BB->pred_begin();
        pred->getTerminator()->replaceBranchTarget(BB, DestBB);
      }
      remainingBlock = DestBB;
      deletedBlock = BB;
    } else {
      BB->spliceAtEnd(DestBB);
      remainingBlock = BB;
      deletedBlock = DestBB;
    }

    // Revisit this block now that we've changed it.
    addToWorklist(remainingBlock);

    // This can also expose opportunities in the successors of
    // the merged block.
    for (auto &Succ : remainingBlock->getSuccessors())
      addToWorklist(Succ);

    substitutedBlockPreds(deletedBlock, remainingBlock);

    auto Iter = JumpThreadingCost.find(deletedBlock);
    if (Iter != JumpThreadingCost.end()) {
      int costs = Iter->second;
      JumpThreadingCost[remainingBlock] += costs;
    }

    removeFromWorklist(deletedBlock);
    deletedBlock->eraseFromParent();
    ++NumBlocksMerged;
    return true;
  }

  // If the destination block is a simple trampoline (jump to another block)
  // then jump directly.
  if (auto trampolineDest = TrampolineDest(BB, DestBB)) {
    LLVM_DEBUG(llvm::dbgs()
               << "jump to trampoline from bb" << BB->getDebugID() << " to bb"
               << trampolineDest.destBB->getDebugID() << '\n');
    SILBuilderWithScope(BI).createBranch(BI->getLoc(), trampolineDest.destBB,
                                         trampolineDest.newSourceBranchArgs);
    // Eliminating the trampoline can expose opportunities to improve the
    // new block we branch to.
    substitutedBlockPreds(DestBB, trampolineDest.destBB);

    addToWorklist(trampolineDest.destBB);
    BI->eraseFromParent();
    removeIfDead(DestBB);
    addToWorklist(BB);

    return true;
  }
  return Simplified;
}

/// Returns the original boolean value, looking through possible invert
/// builtins. The parameter \p Inverted is inverted if the returned original
/// value is the inverted value of the passed \p Cond.
/// If \p onlyAcceptSingleUse is true and the operand of an invert builtin has
/// more than one use, an invalid SILValue() is returned.
static SILValue skipInvert(SILValue Cond, bool &Inverted,
                           bool onlyAcceptSingleUse) {
  while (auto *BI = dyn_cast<BuiltinInst>(Cond)) {
    
    if (onlyAcceptSingleUse && !BI->hasOneUse())
      return SILValue();
    
    OperandValueArrayRef Args = BI->getArguments();
    
    if (BI->getBuiltinInfo().ID == BuiltinValueKind::Xor) {
      // Check if it's a boolean inversion of the condition.
      if (auto *IL = dyn_cast<IntegerLiteralInst>(Args[1])) {
        if (IL->getValue().isAllOnesValue()) {
          Cond = Args[0];
          Inverted = !Inverted;
          continue;
        }
      } else if (auto *IL = dyn_cast<IntegerLiteralInst>(Args[0])) {
        if (IL->getValue().isAllOnesValue()) {
          Cond = Args[1];
          Inverted = !Inverted;
          continue;
        }
      }
    }
    break;
  }
  return Cond;
}

/// Returns the first cond_fail if it is the first side-effect
/// instruction in this block.
static CondFailInst *getFirstCondFail(SILBasicBlock *BB) {
  CondFailInst *CondFail = nullptr;
  // Skip instructions that don't have side-effects.
  auto It = BB->begin();
  while (It != BB->end() && !(CondFail = dyn_cast<CondFailInst>(It))) {
    if (It->mayHaveSideEffects())
      return nullptr;
    ++It;
  }
  return CondFail;
}

/// If the first side-effect instruction in this block is a cond_fail that
/// is guaranteed to fail, it is returned.
///
/// The returned CondFailInst may be in a successor of \p BB.
///
/// The \p Cond is the condition from a cond_br in the predecessor block. The
/// cond_fail must only fail if \p BB is entered through this predecessor block.
/// If \p Inverted is true, \p BB is on the false-edge of the cond_br.
static CondFailInst *getUnConditionalFail(SILBasicBlock *BB, SILValue Cond,
                                          bool Inverted) {
  // Handle a CFG edge to the cond_fail block with no side effects.
  auto *condfailBB = BB;
  if (isa<BranchInst>(BB->getTerminator())) {
    for (auto It = BB->begin(); It != BB->end(); ++It) {
      if (It->mayHaveSideEffects())
        return nullptr;
    }
    condfailBB = BB->getSingleSuccessorBlock();
  }
  CondFailInst *CondFail = getFirstCondFail(condfailBB);
  if (!CondFail)
    return nullptr;
  
  // The simple case: check if it is a "cond_fail 1".
  auto *IL = dyn_cast<IntegerLiteralInst>(CondFail->getOperand());
  if (IL && IL->getValue() != 0)
    return CondFail;

  // Check if the cond_fail has the same condition as the cond_br in the
  // predecessor block.
  Cond = skipInvert(Cond, Inverted, false);
  SILValue CondFailCond = skipInvert(CondFail->getOperand(), Inverted, false);
  if (Cond == CondFailCond && !Inverted)
    return CondFail;
  return nullptr;
}

/// Creates a new cond_fail instruction, optionally with an xor inverted
/// condition.
static void createCondFail(CondFailInst *Orig, SILValue Cond, StringRef Message,
                           bool inverted, SILBuilder &Builder) {
  Builder.createCondFail(Orig->getLoc(), Cond, Message, inverted);
}

/// Inverts the expected value of 'PotentialExpect' (if it is an expect
/// intrinsic) and returns this expected value apply to 'V'.
static SILValue invertExpectAndApplyTo(SILBuilder &Builder,
                                       SILValue PotentialExpect, SILValue V) {
  auto *BI = dyn_cast<BuiltinInst>(PotentialExpect);
  if (!BI)
    return V;
  if (BI->getIntrinsicInfo().ID != llvm::Intrinsic::expect)
    return V;
  auto Args = BI->getArguments();
  auto *IL = dyn_cast<IntegerLiteralInst>(Args[1]);
  if (!IL)
    return V;
  SILValue NegatedExpectedValue = Builder.createIntegerLiteral(
      IL->getLoc(), Args[1]->getType(), IL->getValue() == 0 ? -1 : 0);
  return Builder.createBuiltin(BI->getLoc(), BI->getName(), BI->getType(), {},
                               {V, NegatedExpectedValue});
}

/// simplifyCondBrBlock - Simplify a basic block that ends with a conditional
/// branch.
bool SimplifyCFG::simplifyCondBrBlock(CondBranchInst *BI) {
  // First simplify instructions generating branch operands since that
  // can expose CFG simplifications.
  simplifyBranchOperands(OperandValueArrayRef(BI->getAllOperands()));
  auto *ThisBB = BI->getParent();
  SILBasicBlock *TrueSide = BI->getTrueBB();
  SILBasicBlock *FalseSide = BI->getFalseBB();
  auto TrueArgs = BI->getTrueArgs();
  auto FalseArgs = BI->getFalseArgs();

  // If the condition is an integer literal, we can constant fold the branch.
  if (auto *IL = dyn_cast<IntegerLiteralInst>(BI->getCondition())) {
    bool isFalse = !IL->getValue();
    auto LiveArgs =  isFalse ? FalseArgs : TrueArgs;
    auto *LiveBlock =  isFalse ? FalseSide : TrueSide;
    auto *DeadBlock = !isFalse ? FalseSide : TrueSide;

    LLVM_DEBUG(llvm::dbgs() << "replace cond_br with br: " << *BI);

    SILBuilderWithScope(BI).createBranch(BI->getLoc(), LiveBlock, LiveArgs);
    BI->eraseFromParent();
    if (IL->use_empty()) IL->eraseFromParent();

    addToWorklist(ThisBB);
    simplifyAfterDroppingPredecessor(DeadBlock);
    addToWorklist(LiveBlock);
    ++NumConstantFolded;
    return true;
  }

  // Canonicalize "cond_br (not %cond), BB1, BB2" to "cond_br %cond, BB2, BB1".
  // This looks through expect intrinsic calls and applies the ultimate expect
  // call inverted to the condition.
  if (auto *Xor =
          dyn_cast<BuiltinInst>(stripExpectIntrinsic(BI->getCondition()))) {
    if (Xor->getBuiltinInfo().ID == BuiltinValueKind::Xor) {
      // Check if it's a boolean inversion of the condition.
      OperandValueArrayRef Args = Xor->getArguments();
      if (auto *IL = dyn_cast<IntegerLiteralInst>(Args[1])) {
        if (IL->getValue().isAllOnesValue()) {
          LLVM_DEBUG(llvm::dbgs() << "canonicalize cond_br: " << *BI);
          auto Cond = Args[0];
          SILBuilderWithScope Builder(BI);
          Builder.createCondBranch(
              BI->getLoc(),
              invertExpectAndApplyTo(Builder, BI->getCondition(), Cond),
              FalseSide, FalseArgs, TrueSide, TrueArgs, BI->getFalseBBCount(),
              BI->getTrueBBCount());
          BI->eraseFromParent();
          addToWorklist(ThisBB);
          return true;
        }
      }
    }
  }

  // For a valid TrampolineDest, the destBB has no other predecessors, so remove
  // all the branch arguments--they are no longer phis once their predecessor
  // block is a cond_br instead of a br.
  auto eraseTrampolineDestArgs = [](TrampolineDest &trampolineDest) {
    SILBasicBlock *destBB = trampolineDest.destBB;
    assert(trampolineDest.newSourceBranchArgs.size()
           == destBB->getArguments().size());
    // Erase in reverse order to pop each element as we go.
    for (unsigned i = destBB->getArguments().size(); i != 0;) {
      --i;
      destBB->getArgument(i)->replaceAllUsesWith(
        trampolineDest.newSourceBranchArgs[i]);
      destBB->eraseArgument(i);
    }
  };

  // If the destination block is a simple trampoline (jump to another block)
  // then jump directly.
  //
  // Avoid creating self-loops on a cond_br. The loop block requires blocks
  // arguments for loop-carried values without breaking dominance--we can't have
  // an earlier instruction depending on a value defined later in the block.
  auto trueTrampolineDest = TrampolineDest(ThisBB, TrueSide);
  if (trueTrampolineDest
      && trueTrampolineDest.destBB->getSinglePredecessorBlock()
      && trueTrampolineDest.destBB != ThisBB) {

    LLVM_DEBUG(llvm::dbgs()
               << "true-trampoline from bb" << ThisBB->getDebugID() << " to bb"
               << trueTrampolineDest.destBB->getDebugID() << '\n');

    SmallVector<SILValue, 4> falseArgsCopy(FalseArgs.begin(), FalseArgs.end());
    eraseTrampolineDestArgs(trueTrampolineDest);
    SILBuilderWithScope(BI).createCondBranch(
        BI->getLoc(), BI->getCondition(), trueTrampolineDest.destBB,
        {}, FalseSide, falseArgsCopy,
        BI->getTrueBBCount(), BI->getFalseBBCount());
    BI->eraseFromParent();

    substitutedBlockPreds(TrueSide, ThisBB);
    removeIfDead(TrueSide);
    addToWorklist(ThisBB);
    return true;
  }

  auto falseTrampolineDest = TrampolineDest(ThisBB, FalseSide);
  if (falseTrampolineDest
      && falseTrampolineDest.destBB->getSinglePredecessorBlock()
      && falseTrampolineDest.destBB != ThisBB) {

    LLVM_DEBUG(llvm::dbgs()
               << "false-trampoline from bb" << ThisBB->getDebugID() << " to bb"
               << falseTrampolineDest.destBB->getDebugID() << '\n');

    SmallVector<SILValue, 4> trueArgsCopy(TrueArgs.begin(), TrueArgs.end());
    eraseTrampolineDestArgs(falseTrampolineDest);
    SILBuilderWithScope(BI).createCondBranch(
        BI->getLoc(), BI->getCondition(), TrueSide, trueArgsCopy,
        falseTrampolineDest.destBB, {}, BI->getTrueBBCount(),
        BI->getFalseBBCount());
    BI->eraseFromParent();

    substitutedBlockPreds(FalseSide, ThisBB);
    removeIfDead(FalseSide);
    addToWorklist(ThisBB);
    return true;
  }

  // Simplify cond_br where both sides jump to the same blocks with the same
  // args.
  auto condBrToBr = [&](ArrayRef<SILValue> branchArgs, SILBasicBlock *newDest) {
    LLVM_DEBUG(llvm::dbgs()
               << "replace cond_br with same dests with br: " << *BI);
    SILBuilderWithScope(BI).createBranch(BI->getLoc(), newDest, branchArgs);
    BI->eraseFromParent();
    addToWorklist(ThisBB);
    ++NumConstantFolded;
  };
  if (trueTrampolineDest.destBB == FalseSide
      && trueTrampolineDest.newSourceBranchArgs == FalseArgs) {
    condBrToBr(trueTrampolineDest.newSourceBranchArgs, FalseSide);
    removeIfDead(TrueSide);
    return true;
  }
  if (falseTrampolineDest.destBB == TrueSide) {
    condBrToBr(falseTrampolineDest.newSourceBranchArgs, TrueSide);
    removeIfDead(FalseSide);
    return true;
  }
  if (trueTrampolineDest && (trueTrampolineDest == falseTrampolineDest)) {
    condBrToBr(trueTrampolineDest.newSourceBranchArgs,
               trueTrampolineDest.destBB);
    removeIfDead(TrueSide);
    removeIfDead(FalseSide);
    return true;
  }
  // If we have a (cond (select_enum)) on a two element enum, always have the
  // first case as our checked tag. If we have the second, create a new
  // select_enum with the first case and swap our operands. This simplifies
  // later dominance based processing.
  if (auto *SEI = dyn_cast<SelectEnumInst>(BI->getCondition())) {
    EnumDecl *E = SEI->getEnumOperand()->getType().getEnumOrBoundGenericEnum();

    auto AllElts = E->getAllElements();
    auto Iter = AllElts.begin();
    EnumElementDecl *FirstElt = *Iter;

    // We can't do this optimization on non-exhaustive enums.
    bool IsExhaustive =
        E->isEffectivelyExhaustive(Fn.getModule().getSwiftModule(),
                                   Fn.getResilienceExpansion());

    if (IsExhaustive
        && SEI->getNumCases() >= 1
        && SEI->getCase(0).first != FirstElt) {
      ++Iter;

      if (Iter != AllElts.end() &&
          std::next(Iter) == AllElts.end() &&
          *Iter == SEI->getCase(0).first) {
        EnumElementDecl *SecondElt = *Iter;
        
        SILValue FirstValue;
        // SelectEnum must be exhaustive, so the second case must be handled
        // either by a case or the default.
        if (SEI->getNumCases() >= 2) {
          assert(FirstElt == SEI->getCase(1).first
                 && "select_enum missing a case?!");
          FirstValue = SEI->getCase(1).second;
        } else {
          FirstValue = SEI->getDefaultResult();
        }
        
        
        std::pair<EnumElementDecl*, SILValue> SwappedCases[2] = {
          {FirstElt, SEI->getCase(0).second},
          {SecondElt, FirstValue},
        };

        LLVM_DEBUG(llvm::dbgs() << "canonicalize " << *SEI);
        auto *NewSEI = SILBuilderWithScope(SEI)
          .createSelectEnum(SEI->getLoc(),
                            SEI->getEnumOperand(),
                            SEI->getType(),
                            SILValue(),
                            SwappedCases);
        
        // We only change the condition to be NewEITI instead of all uses since
        // EITI may have other uses besides this one that need to be updated.
        BI->setCondition(NewSEI);
        BI->swapSuccessors();
        addToWorklist(BI->getParent());
        addToWorklist(TrueSide);
        addToWorklist(FalseSide);
        return true;
      }
    }
  }

  // Simplify a condition branch to a block starting with "cond_fail 1".
  //
  // cond_br %cond, TrueSide, FalseSide
  // TrueSide:
  //   cond_fail 1
  //
  auto CFCondition = BI->getCondition();
  if (auto *TrueCFI = getUnConditionalFail(TrueSide, CFCondition, false)) {
    LLVM_DEBUG(llvm::dbgs() << "replace with cond_fail:" << *BI);
    SILBuilderWithScope Builder(BI);
    createCondFail(TrueCFI, CFCondition, TrueCFI->getMessage(), false, Builder);
    SILBuilderWithScope(BI).createBranch(BI->getLoc(), FalseSide, FalseArgs);

    BI->eraseFromParent();
    addToWorklist(ThisBB);
    simplifyAfterDroppingPredecessor(TrueSide);
    addToWorklist(FalseSide);
    return true;
  }
  if (auto *FalseCFI = getUnConditionalFail(FalseSide, CFCondition, true)) {
    LLVM_DEBUG(llvm::dbgs() << "replace with inverted cond_fail:" << *BI);
    SILBuilderWithScope Builder(BI);
    createCondFail(FalseCFI, CFCondition, FalseCFI->getMessage(), true, Builder);
    SILBuilderWithScope(BI).createBranch(BI->getLoc(), TrueSide, TrueArgs);
    
    BI->eraseFromParent();
    addToWorklist(ThisBB);
    simplifyAfterDroppingPredecessor(FalseSide);
    addToWorklist(TrueSide);
    return true;
  }

  return false;
}

// Does this basic block consist of only an "unreachable" instruction?
static bool isOnlyUnreachable(SILBasicBlock *BB) {
  auto *Term = BB->getTerminator();
  if (!isa<UnreachableInst>(Term))
    return false;

  return (&*BB->begin() == BB->getTerminator());
}


/// simplifySwitchEnumUnreachableBlocks - Attempt to replace a
/// switch_enum where all but one block consists of just an
/// "unreachable" with an unchecked_enum_data and branch.
bool SimplifyCFG::simplifySwitchEnumUnreachableBlocks(SwitchEnumInst *SEI) {
  auto Count = SEI->getNumCases();

  SILBasicBlock *Dest = nullptr;
  EnumElementDecl *Element = nullptr;

  if (SEI->hasDefault())
    if (!isOnlyUnreachable(SEI->getDefaultBB()))
      Dest = SEI->getDefaultBB();

  for (unsigned i = 0; i < Count; ++i) {
    auto EnumCase = SEI->getCase(i);

    if (isOnlyUnreachable(EnumCase.second))
      continue;

    if (Dest)
      return false;

    assert(!Element && "Did not expect to have an element without a block!");
    Element = EnumCase.first;
    Dest = EnumCase.second;
  }

  LLVM_DEBUG(llvm::dbgs() << "remove unreachable case " << *SEI);

  if (!Dest) {
    addToWorklist(SEI->getParent());
    SILBuilderWithScope(SEI).createUnreachable(SEI->getLoc());
    for (auto &succ : SEI->getSuccessors()) {
      removeDeadBlock(succ.getBB());
    }
    SEI->eraseFromParent();
    return true;
  }

  if (!Element || !Element->hasAssociatedValues() || Dest->args_empty()) {
    assert(Dest->args_empty() && "Unexpected argument at destination!");

    SILBuilderWithScope(SEI).createBranch(SEI->getLoc(), Dest);

    addToWorklist(SEI->getParent());
    addToWorklist(Dest);

    SEI->eraseFromParent();
    return true;
  }

  auto &Mod = SEI->getModule();
  auto OpndTy = SEI->getOperand()->getType();
  auto Ty = OpndTy.getEnumElementType(
      Element, Mod, TypeExpansionContext(*SEI->getFunction()));
  auto *UED = SILBuilderWithScope(SEI)
    .createUncheckedEnumData(SEI->getLoc(), SEI->getOperand(), Element, Ty);

  assert(Dest->args_size() == 1 && "Expected only one argument!");
  auto *DestArg = Dest->getArgument(0);
  DestArg->replaceAllUsesWith(UED);
  Dest->eraseArgument(0);

  SILBuilderWithScope(SEI).createBranch(SEI->getLoc(), Dest);

  addToWorklist(SEI->getParent());
  addToWorklist(Dest);

  SEI->eraseFromParent();
  return true;
}

/// Checks that the someBB only contains obj_method calls (possibly chained) on
/// the optional value.
///
/// switch_enum %optionalValue, case #Optional.some!enumelt: someBB
///
/// someBB(%optionalPayload):
///    %1 = objc_method %optionalPayload
///    %2 = apply %1(..., %optionalPayload) // self position
///    %3 = unchecked_ref_cast %2
///    %4 = objc_method %3
///    %... = apply %4(..., %3)
///    br mergeBB(%...)
static bool containsOnlyObjMethodCallOnOptional(SILValue optionalValue,
                                                SILBasicBlock *someBB,
                                                SILValue &outBranchArg,
                                                SILValue &outOptionalPayload) {
  SILValue optionalPayload;
  SmallVector<SILValue, 4> optionalPayloads;
  if (someBB->getNumArguments() == 1) {
    optionalPayload = someBB->getArgument(0);
    optionalPayloads.push_back(optionalPayload);
  } else if (someBB->getNumArguments() != 0)
    return false;

  SmallVector<SILValue, 4> objCApplies;
  for (auto &i : *someBB) {
    SILInstruction *inst = &i;
    if (onlyAffectsRefCount(inst))
      continue;
    if (inst->isDebugInstruction())
      continue;
    // An objc_method has no sideeffects.
    if (isa<ObjCMethodInst>(inst))
      continue;

    // An uncheckedEnumData has no sideeffects.
    if (auto *uncheckedEnumData = dyn_cast<UncheckedEnumDataInst>(inst)) {
      if (uncheckedEnumData->getOperand() != optionalValue)
        continue;
      optionalPayload = uncheckedEnumData;
      optionalPayloads.push_back(uncheckedEnumData);
      continue;
    }

    // An unchecked_ref_cast is safe.
    if (auto *refCast = dyn_cast<UncheckedRefCastInst>(inst)) {
      // An unchecked_ref_cast on a safe objc_method apply behaves like the
      // optional (it is null if the optional was null).
      if (refCast->getType().getClassOrBoundGenericClass() &&
          std::find(objCApplies.begin(), objCApplies.end(),
                    refCast->getOperand()) != objCApplies.end())
        optionalPayloads.push_back(refCast);
      continue;
    }

    // Applies on objc_methods where self is either the optional payload or the
    // result of another 'safe' apply are safe.
    if (auto *objcMethod = dyn_cast<ApplyInst>(inst)) {
      if (!isa<ObjCMethodInst>(objcMethod->getCallee()))
        return false;
      if (std::find(optionalPayloads.begin(), optionalPayloads.end(),
                    objcMethod->getSelfArgument()) == optionalPayloads.end())
        return false;
      objCApplies.push_back(objcMethod);
      continue;
    }

    // The branch should forward one of the objc_method call.
    if (auto *br = dyn_cast<BranchInst>(inst)) {
      if (br->getNumArgs() == 0)
        continue;
      if (br->getNumArgs() > 1)
        return false;
      auto branchArg = br->getArg(0);
      if (std::find(objCApplies.begin(), objCApplies.end(), branchArg) ==
          objCApplies.end())
        return false;
      outBranchArg = branchArg;
      continue;
    }
    // Unexpected instruction.
    return false;
  }
  if (!optionalPayload)
    return false;
  outOptionalPayload = optionalPayload;
  return true;
}

/// Check that all that noneBB does is forwarding none.
/// The only other allowed operations are ref count operations.
static bool onlyForwardsNone(SILBasicBlock *noneBB, SILBasicBlock *someBB,
                             SwitchEnumInst *SEI) {
  // It all the basic blocks leading up to the ultimate block we only expect
  // reference count instructions.
  while (noneBB->getSingleSuccessorBlock() != someBB->getSingleSuccessorBlock()) {
    for (auto &i : *noneBB) {
      auto *inst = &i;
      if (isa<BranchInst>(inst) || onlyAffectsRefCount(inst) ||
          inst->isDebugInstruction())
        continue;
      return false;
    }
    noneBB = noneBB->getSingleSuccessorBlock();
  }
  // The ultimate block forwards the Optional<...>.none value.
  SILValue optionalNone;
  for (auto &i : *noneBB) {
    auto *inst = &i;
    if (onlyAffectsRefCount(inst) || inst->isDebugInstruction())
      continue;
    if (auto *none = dyn_cast<EnumInst>(inst)) {
      if (none->getElement() !=
          SEI->getModule().getASTContext().getOptionalNoneDecl())
        return false;
      optionalNone = none;
      continue;
    }
    if (auto *noneBranch = dyn_cast<BranchInst>(inst)) {
      if (noneBranch->getNumArgs() == 0) {
        continue;
      }
      if (noneBranch->getNumArgs() != 1 ||
          (noneBranch->getArg(0) != SEI->getOperand() &&
           noneBranch->getArg(0) != optionalNone))
        return false;
      continue;
    }
    return false;
  }
  return true;
}

/// Check whether the \p someBB has only one single successor and that successor
/// post-dominates \p noneBB.
///
///                          (maybe otherNoneBB)
///  someBB       noneBB     /
///   \             |       v
///    \            ... more bbs? (A)
///     \           /
///       ultimateBB
///
/// This routine does not support diverging control flow in (A). This means that
/// there must not be any loops or diamonds beginning in that region. We do
/// support side-entrances from blocks not reachable from noneBB in order to
/// ensure that we properly handle other failure cases where the failure case
/// merges into .noneBB before ultimate BB.
///
/// DISCUSSION: We allow this side-entrance pattern to handle iterative
/// conditional checks which all feed the failing case through the .none
/// path. This is a common pattern in swift code. As an example consider a
/// switch statement with multiple pattern binding matching that use the same
/// cleanup code upon failure.
static bool hasSameUltimateSuccessor(SILBasicBlock *noneBB, SILBasicBlock *someBB) {
  // Make sure that both our some, none blocks both have single successors that
  // are not themselves (which can happen due to single block loops).
  auto *someSuccessorBB = someBB->getSingleSuccessorBlock();
  if (!someSuccessorBB || someSuccessorBB == someBB)
    return false;

  auto *noneSuccessorBB = noneBB->getSingleSuccessorBlock();
  if (!noneSuccessorBB || noneSuccessorBB == noneBB)
    return false;

  // If we immediately find a simple diamond, return true. We are done.
  if (noneSuccessorBB == someSuccessorBB)
    return true;

  // Otherwise, lets begin a traversal along the successors of noneSuccessorBB,
  // searching for someSuccessorBB, being careful to only allow for blocks to be
  // visited once. This enables us to guarantee that there no loops or
  // any sub-diamonds in the part of the CFG we are traversing. This /does/
  // allow for side-entrances to the region from blocks not reachable from
  // noneSuccessorBB. See function level comment above.
  SILBasicBlock *iter = noneSuccessorBB;
  BasicBlockSet visitedBlocks(someBB->getParent());
  visitedBlocks.insert(iter);

  do {
    // First try to grab our single successor if we have only one. If we have no
    // successor or more than one successor, bail and do not optimize.
    //
    // DISCUSSION: Trivially, if we do not have a successor, then we have
    // reached either a return/unreachable and this path will never merge with
    // the ultimate block. If we have more than one successor, then for our
    // condition to pass, we must have that both successors eventually join into
    // someSuccessorBB. But this would imply that either someSuccessorBB has
    // more than two predecessors and or that we merge the two paths before we
    // visit someSuccessorBB.
    auto *succBlock = iter->getSingleSuccessorBlock();
    if (!succBlock)
      return false;

    // Then check if our single successor block has been visited already. If so,
    // we have some sort of loop or have some sort of merge point that is not
    // the final merge point.
    //
    // NOTE: We do not need to worry about someSuccessorBB being in
    // visitedBlocks since before we begin the loop, we check that
    // someSuccessorBB != iter and also check that in the do-while condition. So
    // we can never have visited someSuccessorBB on any previous iteration
    // meaning that the only time we can have succBlock equal to someSuccessorBB
    // is on the last iteration before we exit the loop.
    if (!visitedBlocks.insert(succBlock))
      return false;

    // Otherwise, set iter to succBlock.
    iter = succBlock;

    // And then check if this new successor block is someSuccessorBB. If so, we
    // break and then return true since we have found our target. Otherwise, we
    // need to visit further successors, so go back around the loop.
  } while (iter != someSuccessorBB);

  return true;
}

/// Simplify switch_enums on class enums that branch to objc_method calls on
/// that optional on the #Optional.some side to always branch to the some side.
///
/// switch_enum %optionalValue, case #Optional.some!enumelt: someBB,
///                             case #Optional.none: noneBB
///
/// someBB(%optionalPayload):
///    %1 = objc_method %optionalPayload
///    %2 = apply %1(..., %optionalPayload) // self position
///    br mergeBB(%2)
///
/// noneBB:
///    %4 = enum #Optional.none
///    br mergeBB(%4)
bool SimplifyCFG::simplifySwitchEnumOnObjcClassOptional(SwitchEnumInst *SEI) {
  auto optional = SEI->getOperand();
  auto optionalPayloadType = optional->getType().getOptionalObjectType();
  if (!optionalPayloadType ||
      !optionalPayloadType.getClassOrBoundGenericClass())
    return false;

  if (SEI->getNumCases() != 2)
    return false;

  auto *noneBB = SEI->getCase(0).second;
  auto *someBB = SEI->getCase(1).second;
  if (noneBB == someBB)
    return false;
  auto someDecl = SEI->getModule().getASTContext().getOptionalSomeDecl();
  if (SEI->getCaseDestination(someDecl) != someBB)
    std::swap(someBB, noneBB);

  if (!hasSameUltimateSuccessor(noneBB, someBB))
    return false;

  if (!onlyForwardsNone(noneBB, someBB, SEI))
    return false;

  SILValue branchArg;
  SILValue optionalPayload;
  if (!containsOnlyObjMethodCallOnOptional(optional, someBB, branchArg,
                                           optionalPayload))
    return false;

  LLVM_DEBUG(llvm::dbgs() << "simplify switch_enum on ObjC Class Optional\n");

  SILBuilderWithScope Builder(SEI);
  auto *payloadCast = Builder.createUncheckedRefCast(SEI->getLoc(), optional,
                                                     optionalPayloadType);
  optionalPayload->replaceAllUsesWith(payloadCast);
  auto *switchBB = SEI->getParent();

  if (!someBB->args_empty()) {
    assert(someBB->getNumArguments() == 1);
    auto *someBBArg = someBB->getArgument(0);
    if (!someBBArg->use_empty()) {
      assert(optionalPayload != someBBArg);
      someBBArg->replaceAllUsesWith(payloadCast);
    }
    someBB->eraseArgument(0);
    Builder.createBranch(SEI->getLoc(), someBB);
  } else {
    assert(!Fn.hasOwnership());
    Builder.createBranch(SEI->getLoc(), someBB);
  }

  SEI->eraseFromParent();
  addToWorklist(switchBB);
  simplifyAfterDroppingPredecessor(noneBB);
  addToWorklist(someBB);
  ++NumConstantFolded;
  return true;
}

/// simplifySwitchEnumBlock - Simplify a basic block that ends with a
/// switch_enum instruction that gets its operand from an enum
/// instruction.
bool SimplifyCFG::simplifySwitchEnumBlock(SwitchEnumInst *SEI) {
  auto EnumCase = getEnumCase(SEI->getOperand(), SEI->getParent());
  if (!EnumCase)
    return false;

  auto *LiveBlock = SEI->getCaseDestination(EnumCase.get());
  auto *ThisBB = SEI->getParent();

  bool DroppedLiveBlock = false;
  // Copy the successors into a vector, dropping one entry for the liveblock.
  SmallVector<SILBasicBlock*, 4> Dests;
  for (auto &S : SEI->getSuccessors()) {
    if (S == LiveBlock && !DroppedLiveBlock) {
      DroppedLiveBlock = true;
      continue;
    }
    Dests.push_back(S);
  }

  LLVM_DEBUG(llvm::dbgs() << "fold switch " << *SEI);

  auto *EI = dyn_cast<EnumInst>(SEI->getOperand());
  auto loc = SEI->getLoc();
  SILBuilderWithScope Builder(SEI);
  if (!LiveBlock->args_empty()) {
    SILValue PayLoad;
    if (SEI->hasDefault() && LiveBlock == SEI->getDefaultBB()) {
      assert(Fn.hasOwnership() && "Only OSSA default case has an argument");
      PayLoad = SEI->getOperand();
    } else {
      PayLoad = Builder.createUncheckedEnumData(loc, SEI->getOperand(),
                                                EnumCase.get());
    }
    Builder.createBranch(loc, LiveBlock, PayLoad);
  } else {
    Builder.createBranch(loc, LiveBlock);
  }

  SEI->eraseFromParent();
  if (EI && isInstructionTriviallyDead(EI)) {
    EI->replaceAllUsesOfAllResultsWithUndef();
    EI->eraseFromParent();
  }

  addToWorklist(ThisBB);

  for (auto B : Dests)
    simplifyAfterDroppingPredecessor(B);
  addToWorklist(LiveBlock);
  ++NumConstantFolded;
  return true;
}

/// simplifySwitchValueBlock - Simplify a basic block that ends with a
/// switch_value instruction that gets its operand from an integer
/// literal instruction.
bool SimplifyCFG::simplifySwitchValueBlock(SwitchValueInst *SVI) {
  auto *ThisBB = SVI->getParent();
  if (auto *ILI = dyn_cast<IntegerLiteralInst>(SVI->getOperand())) {
    SILBasicBlock *LiveBlock = nullptr;

    auto Value = ILI->getValue();
    // Find a case corresponding to this value
    int i, e;
    for (i = 0, e = SVI->getNumCases(); i < e; ++i) {
      auto Pair = SVI->getCase(i);
      auto *CaseIL = dyn_cast<IntegerLiteralInst>(Pair.first);
      if (!CaseIL)
        break;
      auto CaseValue = CaseIL->getValue();
      if (Value == CaseValue) {
        LiveBlock = Pair.second;
        break;
      }
    }

    if (i == e && !LiveBlock) {
      if (SVI->hasDefault()) {
        LiveBlock = SVI->getDefaultBB();
      }
    }

    if (LiveBlock) {
      bool DroppedLiveBlock = false;
      // Copy the successors into a vector, dropping one entry for the
      // liveblock.
      SmallVector<SILBasicBlock *, 4> Dests;
      for (auto &S : SVI->getSuccessors()) {
        if (S == LiveBlock && !DroppedLiveBlock) {
          DroppedLiveBlock = true;
          continue;
        }
        Dests.push_back(S);
      }

      LLVM_DEBUG(llvm::dbgs() << "fold select " << *SVI);

      SILBuilderWithScope(SVI).createBranch(SVI->getLoc(), LiveBlock);
      SVI->eraseFromParent();
      if (ILI->use_empty())
        ILI->eraseFromParent();

      addToWorklist(ThisBB);

      for (auto B : Dests)
        simplifyAfterDroppingPredecessor(B);
      addToWorklist(LiveBlock);
      ++NumConstantFolded;
      return true;
    }
  }

  return simplifyTermWithIdenticalDestBlocks(ThisBB);
}

bool onlyContainsRefcountAndDeallocStackInst(
    SILBasicBlock::reverse_iterator I, SILBasicBlock::reverse_iterator End) {
  while (I != End) {
    auto MaybeDead = I++;
    switch (MaybeDead->getKind()) {
      // These technically have side effects, but not ones that matter
      // in a block that we shouldn't really reach...
    case SILInstructionKind::StrongRetainInst:
    case SILInstructionKind::StrongReleaseInst:
    case SILInstructionKind::RetainValueInst:
    case SILInstructionKind::ReleaseValueInst:
    case SILInstructionKind::DeallocStackInst:
      break;

    default:
      return false;
    }
  }
  return true;
}
/// simplifyUnreachableBlock - Simplify blocks ending with unreachable by
/// removing instructions that are safe to delete backwards until we
/// hit an instruction we cannot delete.
bool SimplifyCFG::simplifyUnreachableBlock(UnreachableInst *UI) {
  bool Changed = false;
  auto BB = UI->getParent();
  auto I = std::next(BB->rbegin());
  auto End = BB->rend();
  SmallVector<SILInstruction *, 8> DeadInstrs;

  bool canIgnoreRestOfBlock = false;

  // Walk backwards deleting instructions that should be safe to delete
  // in a block that ends with unreachable.
  while (I != End) {
    auto MaybeDead = I++;

    switch (MaybeDead->getKind()) {
      // These technically have side effects, but not ones that matter
      // in a block that we shouldn't really reach...
    case SILInstructionKind::StrongRetainInst:
    case SILInstructionKind::StrongReleaseInst:
    case SILInstructionKind::RetainValueInst:
    case SILInstructionKind::ReleaseValueInst:
      break;
    // We can only ignore a dealloc_stack instruction if we can ignore all
    // instructions in the block.
    case SILInstructionKind::DeallocStackInst: {
      if (canIgnoreRestOfBlock ||
          onlyContainsRefcountAndDeallocStackInst(MaybeDead, End)) {
        canIgnoreRestOfBlock = true;
        break;
      }
      LLVM_FALLTHROUGH;
    }
    default:
      if (MaybeDead->mayHaveSideEffects()) {
         if (Changed)
          for (auto Dead : DeadInstrs)
            Dead->eraseFromParent();
        return Changed;
      }
    }

    MaybeDead->replaceAllUsesOfAllResultsWithUndef();
    DeadInstrs.push_back(&*MaybeDead);
    Changed = true;
  }

  // If this block was changed and it now consists of only the unreachable,
  // make sure we process its predecessors.
  if (Changed) {
    LLVM_DEBUG(llvm::dbgs() << "remove dead insts in unreachable bb"
                            << BB->getDebugID() << '\n');
    for (auto Dead : DeadInstrs)
      Dead->eraseFromParent();

    if (isOnlyUnreachable(BB))
      for (auto *P : BB->getPredecessorBlocks())
        addToWorklist(P);
  }

  return Changed;
}

bool SimplifyCFG::simplifyCheckedCastBranchBlock(CheckedCastBranchInst *CCBI) {
  auto SuccessBB = CCBI->getSuccessBB();
  auto FailureBB = CCBI->getFailureBB();
  auto ThisBB = CCBI->getParent();

  bool MadeChange = false;
  CastOptimizer CastOpt(
      FuncBuilder, nullptr /*SILBuilderContext*/,
      /* replaceValueUsesAction */
      [&MadeChange](SILValue oldValue, SILValue newValue) {
        MadeChange = true;
      },
      /* replaceInstUsesAction */
      [&MadeChange](SILInstruction *I, ValueBase *V) { MadeChange = true; },
      /* eraseInstAction */
      [&MadeChange](SILInstruction *I) {
        MadeChange = true;
        I->eraseFromParent();
      },
      /* willSucceedAction */
      [&]() {
        MadeChange |= removeIfDead(FailureBB);
        addToWorklist(ThisBB);
      },
      /* willFailAction */
      [&]() {
        MadeChange |= removeIfDead(SuccessBB);
        addToWorklist(ThisBB);
      });

  MadeChange |= bool(CastOpt.simplifyCheckedCastBranchInst(CCBI));

  LLVM_DEBUG(if (MadeChange)
               llvm::dbgs() << "simplify checked_cast_br block\n");
  return MadeChange;
}

bool
SimplifyCFG::
simplifyCheckedCastAddrBranchBlock(CheckedCastAddrBranchInst *CCABI) {
  auto SuccessBB = CCABI->getSuccessBB();
  auto FailureBB = CCABI->getFailureBB();
  auto ThisBB = CCABI->getParent();

  bool MadeChange = false;
  CastOptimizer CastOpt(
      FuncBuilder, nullptr /*SILBuilderContext*/,
      /* replaceValueUsesAction */
      [&MadeChange](SILValue, SILValue) { MadeChange = true; },
      /* replaceInstUsesAction */
      [&MadeChange](SILInstruction *I, ValueBase *V) { MadeChange = true; },
      /* eraseInstAction */
      [&MadeChange](SILInstruction *I) {
        MadeChange = true;
        I->eraseFromParent();
      },
      /* willSucceedAction */
      [&]() {
        MadeChange |= removeIfDead(FailureBB);
        addToWorklist(ThisBB);
      },
      /* willFailAction */
      [&]() {
        MadeChange |= removeIfDead(SuccessBB);
        addToWorklist(ThisBB);
      });

  MadeChange |= bool(CastOpt.simplifyCheckedCastAddrBranchInst(CCABI));

  LLVM_DEBUG(if (MadeChange)
               llvm::dbgs() << "simplify checked_cast_addr block\n");
  return MadeChange;
}

static SILValue getActualCallee(SILValue Callee) {
  while (!isa<FunctionRefInst>(Callee)) {
    if (auto *CFI = dyn_cast<ConvertFunctionInst>(Callee)) {
      Callee = CFI->getConverted();
      continue;
    }
    if (auto *Cvt = dyn_cast<ConvertEscapeToNoEscapeInst>(Callee)) {
      Callee = Cvt->getConverted();
      continue;
    }
    if (auto *TTI = dyn_cast<ThinToThickFunctionInst>(Callee)) {
      Callee = TTI->getConverted();
      continue;
    }
    break;
  }
  
  return Callee;
}

/// Checks if the callee of \p TAI is a convert from a function without
/// error result.
///
/// The new \p Callee must be reachable from \p TAI's callee operand by
/// following the chain of OwnershipForwardingConversionInsts.
static bool isTryApplyOfConvertFunction(TryApplyInst *TAI,
                                              SILValue &Callee,
                                              SILType &CalleeType) {
  auto CalleeOperand = TAI->getCallee();

  // Look through a @noescape conversion.
  auto *Cvt = dyn_cast<ConvertEscapeToNoEscapeInst>(CalleeOperand);
  if (Cvt)
    CalleeOperand = Cvt->getConverted();

  auto *CFI = dyn_cast<ConvertFunctionInst>(CalleeOperand);
  if (!CFI)
    return false;
  
  // Check if it is a conversion of a non-throwing function into
  // a throwing function. If this is the case, replace by a
  // simple apply.
  auto OrigFnTy = CFI->getConverted()->getType().getAs<SILFunctionType>();
  if (!OrigFnTy || OrigFnTy->hasErrorResult())
    return false;
  
  auto TargetFnTy = CFI->getType().getAs<SILFunctionType>();
  if (!TargetFnTy || !TargetFnTy->hasErrorResult())
    return false;

  // Look through the conversions and find the real callee.
  Callee = getActualCallee(CFI->getConverted());
  CalleeType = Callee->getType();
  
  // If it a call of a throwing callee, bail.
  auto CalleeFnTy = CalleeType.getAs<SILFunctionType>();
  if (!CalleeFnTy || CalleeFnTy->hasErrorResult())
    return false;
  
  return true;
}

/// Checks if the error block of \p TAI has just an unreachable instruction.
/// In this case we know that the callee cannot throw.
static bool isTryApplyWithUnreachableError(TryApplyInst *TAI,
                                           SILValue &Callee,
                                           SILType &CalleeType) {
  SILBasicBlock *ErrorBlock = TAI->getErrorBB();
  TermInst *Term = ErrorBlock->getTerminator();
  if (!isa<UnreachableInst>(Term))
    return false;
  
  if (&*ErrorBlock->begin() != Term)
    return false;
  
  Callee = TAI->getCallee();
  CalleeType = TAI->getSubstCalleeSILType();
  return true;
}

bool SimplifyCFG::simplifyTryApplyBlock(TryApplyInst *TAI) {
  SILValue Callee;
  SILType CalleeType;

  // Two reasons for converting a try_apply to an apply.
  if (isTryApplyOfConvertFunction(TAI, Callee, CalleeType) ||
      isTryApplyWithUnreachableError(TAI, Callee, CalleeType)) {

    LLVM_DEBUG(llvm::dbgs() << "simplify try_apply block\n");

    auto CalleeFnTy = CalleeType.castTo<SILFunctionType>();
    SILFunctionConventions calleeConv(CalleeFnTy, TAI->getModule());
    auto ResultTy = calleeConv.getSILResultType(
        TAI->getFunction()->getTypeExpansionContext());
    auto OrigResultTy = TAI->getNormalBB()->getArgument(0)->getType();

    SILBuilderWithScope Builder(TAI);

    auto TargetFnTy = CalleeFnTy;
    if (TargetFnTy->isPolymorphic()) {
      TargetFnTy = TargetFnTy->substGenericArgs(
          TAI->getModule(), TAI->getSubstitutionMap(),
          Builder.getTypeExpansionContext());
    }
    SILFunctionConventions targetConv(TargetFnTy, TAI->getModule());

    auto OrigFnTy = TAI->getCallee()->getType().getAs<SILFunctionType>();
    if (OrigFnTy->isPolymorphic()) {
      OrigFnTy = OrigFnTy->substGenericArgs(TAI->getModule(),
                                            TAI->getSubstitutionMap(),
                                            Builder.getTypeExpansionContext());
    }
    SILFunctionConventions origConv(OrigFnTy, TAI->getModule());
    auto context = TAI->getFunction()->getTypeExpansionContext();
    SmallVector<SILValue, 8> Args;
    unsigned numArgs = TAI->getNumArguments();
    for (unsigned i = 0; i < numArgs; ++i) {
      auto Arg = TAI->getArgument(i);
      // Cast argument if required.
      std::tie(Arg, std::ignore) = castValueToABICompatibleType(
          &Builder, TAI->getLoc(), Arg, origConv.getSILArgumentType(i, context),
          targetConv.getSILArgumentType(i, context), {TAI});
      Args.push_back(Arg);
    }

    assert(calleeConv.getNumSILArguments() == Args.size()
           && "The number of arguments should match");

    LLVM_DEBUG(llvm::dbgs() << "replace with apply: " << *TAI);

    // If the new callee is owned, copy it to extend the lifetime
    //
    // TODO: The original convert_function will likely be dead after
    // replacement. It could be deleted on-the-fly with a utility to avoid
    // creating a new copy.
    auto calleeLoc = RegularLocation::getAutoGeneratedLocation();
    auto newCallee = Callee;
    if (requiresOSSACleanup(newCallee)) {
      newCallee = SILBuilderWithScope(newCallee->getNextInstruction())
        .createCopyValue(calleeLoc, newCallee);
      newCallee = makeValueAvailable(newCallee, TAI->getParent());
    }

    ApplyOptions Options = TAI->getApplyOptions();
    if (CalleeFnTy->hasErrorResult())
      Options |= ApplyFlags::DoesNotThrow;
    ApplyInst *NewAI = Builder.createApply(TAI->getLoc(), newCallee,
                                           TAI->getSubstitutionMap(),
                                           Args, Options);

    auto Loc = TAI->getLoc();
    auto *NormalBB = TAI->getNormalBB();

    assert(NewAI->getOwnershipKind() != OwnershipKind::Guaranteed);
    // Non-guaranteed values don't need use points when casting.
    SILValue CastedResult;
    std::tie(CastedResult, std::ignore) = castValueToABICompatibleType(
      &Builder, Loc, NewAI, ResultTy, OrigResultTy, /*usePoints*/ {});

    BranchInst *branch = Builder.createBranch(Loc, NormalBB, { CastedResult });

    auto *oldCalleeOper = TAI->getCalleeOperand();
    if (oldCalleeOper->getOwnershipConstraint().isConsuming()) {
      // Destroy the oldCallee before the new call.
      SILBuilderWithScope(NewAI).createDestroyValue(
        TAI->getLoc(), oldCalleeOper->get());
    } else if (newCallee != Callee) {
      // Destroy the copied newCallee after the call.
      SILBuilderWithScope(branch).createDestroyValue(TAI->getLoc(), newCallee);
    }
    TAI->eraseFromParent();
    return true;
  }
  return false;
}

// Replace the terminator of BB with a simple branch if all successors go
// to trampoline jumps to the same destination block. The successor blocks
// and the destination blocks may have no arguments.
bool SimplifyCFG::simplifyTermWithIdenticalDestBlocks(SILBasicBlock *BB) {
  TrampolineDest commonDest;
  for (auto *SuccBlock : BB->getSuccessorBlocks()) {
    auto trampolineDest = TrampolineDest(BB, SuccBlock);
    if (!trampolineDest) {
      return false;
    }
    // The branch must have the same destination and same branch arguments.
    if (!commonDest) {
      commonDest = std::move(trampolineDest);
    } else if (trampolineDest != commonDest) {
      return false;
    }
  }
  if (!commonDest) {
    return false;
  }
  TermInst *Term = BB->getTerminator();
  LLVM_DEBUG(llvm::dbgs() << "replace term with identical dests: " << *Term);
  SILBuilderWithScope(Term).createBranch(Term->getLoc(), commonDest.destBB,
                                         commonDest.newSourceBranchArgs);
  Term->eraseFromParent();
  addToWorklist(BB);
  addToWorklist(commonDest.destBB);
  return true;
}

/// Checks if the block contains a cond_fail as first side-effect instruction
/// and tries to move it to the predecessors (if beneficial). A sequence
///
///     bb1:
///       br bb3(%c)
///     bb2:
///       %i = integer_literal
///       br bb3(%i)            // at least one input argument must be constant
///     bb3(%a) // = BB
///       cond_fail %a          // %a must not have other uses
///
/// is replaced with
///
///     bb1:
///       cond_fail %c
///       br bb3(%c)
///     bb2:
///       %i = integer_literal
///       cond_fail %i
///       br bb3(%i)
///     bb3(%a)                 // %a is dead
///
static bool tryMoveCondFailToPreds(SILBasicBlock *BB) {
  CondFailInst *CFI = getFirstCondFail(BB);
  if (!CFI)
    return false;
  
  // Find the underlying condition value of the cond_fail.
  // We only accept single uses. This is not a correctness check, but we only
  // want to the optimization if the condition gets dead after moving the
  // cond_fail.
  bool inverted = false;
  SILValue cond = skipInvert(CFI->getOperand(), inverted, true);
  if (!cond)
    return false;
  
  // Check if the condition is a single-used argument in the current block.
  auto *condArg = dyn_cast<SILArgument>(cond);
  if (!condArg || !condArg->hasOneUse())
    return false;
  
  if (condArg->getParent() != BB)
    return false;
  
  // Check if some of the predecessor blocks provide a constant for the
  // cond_fail condition. So that the optimization has a positive effect.
  bool somePredsAreConst = false;
  for (auto *Pred : BB->getPredecessorBlocks()) {

    // The cond_fail must post-dominate the predecessor block. We may not
    // execute the cond_fail speculatively.
    if (!Pred->getSingleSuccessorBlock())
      return false;

    // If we already found a constant pred, we do not need to check the incoming
    // value to see if it is constant. We are already going to perform the
    // optimization.
    if (somePredsAreConst)
      continue;

    SILValue incoming = condArg->getIncomingPhiValue(Pred);
    somePredsAreConst |= isa<IntegerLiteralInst>(incoming);
  }

  if (!somePredsAreConst)
    return false;

  LLVM_DEBUG(llvm::dbgs() << "move to predecessors: " << *CFI);

  // Move the cond_fail to the predecessor blocks.
  for (auto *Pred : BB->getPredecessorBlocks()) {
    SILValue incoming = condArg->getIncomingPhiValue(Pred);

    SILBuilderWithScope Builder(Pred->getTerminator());
    createCondFail(CFI, incoming, CFI->getMessage(), inverted, Builder);
  }
  // cond_fail takes a trivial Int1. No cleanup is needed.
  CFI->eraseFromParent();
  return true;
}

bool SimplifyCFG::simplifyBlocks() {
  bool Changed = false;

  // Add all of the blocks to the function.
  for (auto &BB : Fn)
    addToWorklist(&BB);

  // Iteratively simplify while there is still work to do.
  while (SILBasicBlock *BB = popWorklist()) {
    // If the block is dead, remove it.
    if (removeIfDead(BB)) {
      Changed = true;
      continue;
    }

    // Otherwise, try to simplify the terminator.
    TermInst *TI = BB->getTerminator();

    if (!transform.continueWithNextSubpassRun(TI))
      return Changed;

    switch (TI->getTermKind()) {
    case TermKind::BranchInst:
      if (simplifyBranchBlock(cast<BranchInst>(TI))) {
        Changed = true;
        continue;
      }

      // If this unconditional branch has BBArgs, check to see if duplicating
      // the destination would allow it to be simplified.  This is a simple form
      // of jump threading.
      if (!isVeryLargeFunction && tryJumpThreading(cast<BranchInst>(TI))) {
        Changed = true;
        continue;
      }
      break;
    case TermKind::CondBranchInst:
      Changed |= simplifyCondBrBlock(cast<CondBranchInst>(TI));
      break;
    case TermKind::SwitchValueInst:
      // FIXME: Optimize for known switch values.
      Changed |= simplifySwitchValueBlock(cast<SwitchValueInst>(TI));
      break;
    case TermKind::SwitchEnumInst: {
      auto *SEI = cast<SwitchEnumInst>(TI);
      if (simplifySwitchEnumBlock(SEI)) {
        Changed = true;
      } else if (simplifySwitchEnumOnObjcClassOptional(SEI)) {
        Changed = true;
      } else {
        Changed |= simplifySwitchEnumUnreachableBlocks(SEI);
      }
      Changed |= simplifyTermWithIdenticalDestBlocks(BB);
      break;
    }
    case TermKind::UnreachableInst:
      Changed |= simplifyUnreachableBlock(cast<UnreachableInst>(TI));
      break;
    case TermKind::CheckedCastBranchInst:
      Changed |= simplifyCheckedCastBranchBlock(cast<CheckedCastBranchInst>(TI));
      break;
    case TermKind::CheckedCastAddrBranchInst:
      Changed |= simplifyCheckedCastAddrBranchBlock(cast<CheckedCastAddrBranchInst>(TI));
      break;
    case TermKind::TryApplyInst:
      Changed |= simplifyTryApplyBlock(cast<TryApplyInst>(TI));
      break;
    case TermKind::SwitchEnumAddrInst:
      Changed |= simplifyTermWithIdenticalDestBlocks(BB);
      break;
    case TermKind::ThrowInst:
    case TermKind::DynamicMethodBranchInst:
    case TermKind::ReturnInst:
    case TermKind::UnwindInst:
    case TermKind::YieldInst:
      break;
    case TermKind::AwaitAsyncContinuationInst:
      // TODO(async): Simplify AwaitAsyncContinuationInst
      break;
    }

    // If the block has a cond_fail, try to move it to the predecessors.
    Changed |= tryMoveCondFailToPreds(BB);

    // Simplify the block argument list.
    Changed |= simplifyArgs(BB);

    // Simplify the program termination block.
    Changed |= simplifyProgramTerminationBlock(BB);
  }

  if (Changed) {
    // Simplifying other blocks might have resulted in unreachable
    // loops.
    removeUnreachableBlocks(Fn);
  }
  return Changed;
}

/// Canonicalize all switch_enum and switch_enum_addr instructions.
/// If possible, replace the default with the corresponding unique case.
bool SimplifyCFG::canonicalizeSwitchEnums() {
  bool Changed = false;
  for (auto &BB : Fn) {
    TermInst *TI = BB.getTerminator();
    if (!transform.continueWithNextSubpassRun(TI))
      return Changed;


    SwitchEnumTermInst SWI(TI);
    if (!SWI)
      continue;

    if (!SWI.hasDefault())
      continue;

    NullablePtr<EnumElementDecl> defaultDecl = SWI.getUniqueCaseForDefault();
    if (!defaultDecl)
      continue;

    LLVM_DEBUG(llvm::dbgs() << "simplify canonical switch_enum\n");

    // Construct a new instruction by copying all the case entries.
    SmallVector<std::pair<EnumElementDecl*, SILBasicBlock*>, 4> CaseBBs;
    for (int idx = 0, numIdcs = SWI.getNumCases(); idx < numIdcs; ++idx) {
      CaseBBs.push_back(SWI.getCase(idx));
    }
    // Add the default-entry of the original instruction as case-entry.
    auto *defaultBB = SWI.getDefaultBB();
    CaseBBs.push_back(std::make_pair(defaultDecl.get(), defaultBB));

    if (isa<SwitchEnumInst>(*SWI)) {
      SILBuilderWithScope(SWI).createSwitchEnum(SWI->getLoc(), SWI.getOperand(),
                                                nullptr, CaseBBs);
    } else {
      assert(isa<SwitchEnumAddrInst>(*SWI) &&
             "unknown switch_enum instruction");
      SILBuilderWithScope(SWI).createSwitchEnumAddr(
          SWI->getLoc(), SWI.getOperand(), nullptr, CaseBBs);
    }
    SWI->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

static SILBasicBlock *isObjCMethodCallBlock(SILBasicBlock &Block) {
  auto *Branch = dyn_cast<BranchInst>(Block.getTerminator());
  if (!Branch)
    return nullptr;

  for (auto &Inst : Block) {
    // Look for an objc method call.
    auto *Apply = dyn_cast<ApplyInst>(&Inst);
    if (!Apply)
      continue;
    auto *Callee = dyn_cast<ObjCMethodInst>(Apply->getCallee());
    if (!Callee)
      continue;

    return Branch->getDestBB();
  }
  return nullptr;
}

/// We want to duplicate small blocks that contain a least on release and have
/// multiple predecessor.
static bool shouldTailDuplicate(SILBasicBlock &Block) {
  unsigned Cost = 0;
  bool SawRelease = false;

  if (Block.getTerminator()->isFunctionExiting())
    return false;

  if (Block.getSinglePredecessorBlock())
    return false;

  for (auto &Inst : Block) {
    if (!Inst.isTriviallyDuplicatable())
      return false;

    if (FullApplySite::isa(&Inst))
      return false;

    if (isa<ReleaseValueInst>(&Inst) ||
        isa<StrongReleaseInst>(&Inst))
      SawRelease = true;

    if (instructionInlineCost(Inst) != InlineCost::Free)
      if (++Cost == 12)
        return false;
  }

  return SawRelease;
}


/// Tail duplicate successor blocks of blocks that perform an objc method call
/// and who contain releases. Cloning such blocks can allow ARC to sink retain
/// releases onto the ObjC path.
bool SimplifyCFG::tailDuplicateObjCMethodCallSuccessorBlocks() {
  SmallVector<SILBasicBlock *, 16> ObjCBlocks;

  // TODO: OSSA phi support. Even if all block arguments are trivial,
  // jump-threading may require creation of guaranteed phis, which may require
  // creation of nested borrow scopes.
  if (!EnableOSSARewriteTerminator && Fn.hasOwnership()) {
    return false;
  }
  // Collect blocks to tail duplicate.
  for (auto &BB : Fn) {
    SILBasicBlock *DestBB;
    if ((DestBB = isObjCMethodCallBlock(BB)) && !LoopHeaders.count(DestBB) &&
        shouldTailDuplicate(*DestBB))
      ObjCBlocks.push_back(&BB);
  }

  bool Changed = false;
  for (auto *BB : ObjCBlocks) {
    auto *Branch = cast<BranchInst>(BB->getTerminator());
    auto *DestBB = Branch->getDestBB();

    // Okay, it looks like we want to do this and we can.  Duplicate the
    // destination block into this one, rewriting uses of the BBArgs to use the
    // branch arguments as we go.
    BasicBlockCloner Cloner(DestBB);
    if (!Cloner.canCloneBlock())
      continue;

    Cloner.cloneBranchTarget(Branch);
    Cloner.updateSSAAfterCloning();

    Changed = true;
    // Simplify the cloned block and continue tail duplicating through its new
    // successors edges.
    addToWorklistAfterSplittingEdges(Cloner.getNewBB());
  }
  return Changed;
}

namespace {

class ArgumentSplitter {
  /// The argument we are splitting.
  SILArgument *Arg;

  /// The worklist of arguments that we still need to visit. We
  /// simplify each argument recursively one step at a time.
  std::vector<SILArgument *> &Worklist;

  /// The values incoming into Arg.
  llvm::SmallVector<std::pair<SILBasicBlock *, SILValue>, 8> IncomingValues;

  /// The list of first level projections that Arg can be split into.
  llvm::SmallVector<Projection, 4> Projections;

  llvm::Optional<int> FirstNewArgIndex;

public:
  ArgumentSplitter(SILArgument *A, std::vector<SILArgument *> &W)
      : Arg(A), Worklist(W), IncomingValues() {}
  bool split();

private:
  bool createNewArguments();
  void replaceIncomingArgs(SILBuilder &B, BranchInst *BI,
                           llvm::SmallVectorImpl<SILValue> &NewIncomingValues);
  void replaceIncomingArgs(SILBuilder &B, CondBranchInst *CBI,
                           llvm::SmallVectorImpl<SILValue> &NewIncomingValues);
};
} // end anonymous namespace

void ArgumentSplitter::replaceIncomingArgs(
    SILBuilder &B, BranchInst *BI,
    llvm::SmallVectorImpl<SILValue> &NewIncomingValues) {
  unsigned ArgIndex = Arg->getIndex();

  for (unsigned i : llvm::reverse(indices(BI->getAllOperands()))) {
    // Skip this argument.
    if (i == ArgIndex)
      continue;
    NewIncomingValues.push_back(BI->getArg(i));
  }
  std::reverse(NewIncomingValues.begin(), NewIncomingValues.end());
  B.createBranch(BI->getLoc(), BI->getDestBB(), NewIncomingValues);
}

void ArgumentSplitter::replaceIncomingArgs(
    SILBuilder &B, CondBranchInst *CBI,
    llvm::SmallVectorImpl<SILValue> &NewIncomingValues) {
  llvm::SmallVector<SILValue, 4> OldIncomingValues;
  ArrayRef<SILValue> NewTrueValues, NewFalseValues;

  unsigned ArgIndex = Arg->getIndex();
  if (Arg->getParent() == CBI->getTrueBB()) {
    ArrayRef<Operand> TrueArgs = CBI->getTrueOperands();
    for (unsigned i : llvm::reverse(indices(TrueArgs))) {
      // Skip this argument.
      if (i == ArgIndex)
        continue;
      NewIncomingValues.push_back(TrueArgs[i].get());
    }
    std::reverse(NewIncomingValues.begin(), NewIncomingValues.end());
    for (SILValue V : CBI->getFalseArgs())
      OldIncomingValues.push_back(V);
    NewTrueValues = NewIncomingValues;
    NewFalseValues = OldIncomingValues;
  } else {
    ArrayRef<Operand> FalseArgs = CBI->getFalseOperands();
    for (unsigned i : llvm::reverse(indices(FalseArgs))) {
      // Skip this argument.
      if (i == ArgIndex)
        continue;
      NewIncomingValues.push_back(FalseArgs[i].get());
    }
    std::reverse(NewIncomingValues.begin(), NewIncomingValues.end());
    for (SILValue V : CBI->getTrueArgs())
      OldIncomingValues.push_back(V);
    NewTrueValues = OldIncomingValues;
    NewFalseValues = NewIncomingValues;
  }

  B.createCondBranch(CBI->getLoc(), CBI->getCondition(), CBI->getTrueBB(),
                     NewTrueValues, CBI->getFalseBB(), NewFalseValues,
                     CBI->getTrueBBCount(), CBI->getFalseBBCount());
}

bool ArgumentSplitter::createNewArguments() {
  auto *F = Arg->getFunction();
  SILModule &Mod = F->getModule();
  SILBasicBlock *ParentBB = Arg->getParent();

  // Grab the incoming values. Return false if we can't find them.
  if (!Arg->getIncomingPhiValues(IncomingValues))
    return false;

  // Only handle struct and tuple type.
  SILType Ty = Arg->getType();
  if (!Ty.getStructOrBoundGenericStruct() && !Ty.is<TupleType>())
    return false;

  // Get the first level projection for the struct or tuple type.
  Projection::getFirstLevelProjections(Arg->getType(), Mod,
                                       TypeExpansionContext(*F), Projections);

  // We do not want to split arguments with less than 2 projections.
  if (Projections.size() < 2)
    return false;

  // We do not want to split arguments that have less than 2 non-trivial
  // projections.
  if (count_if(Projections, [&](const Projection &P) {
        return !P.getType(Ty, Mod, TypeExpansionContext(*F)).isTrivial(*F);
      }) < 2)
    return false;

  // We subtract one since this will be the number of the first new argument
  // *AFTER* we remove the old argument.
  FirstNewArgIndex = ParentBB->getNumArguments() - 1;

  // For now for simplicity, we put all new arguments on the end and delete the
  // old one.
  llvm::SmallVector<SILValue, 4> NewArgumentValues;
  for (auto &P : Projections) {
    auto *NewArg = ParentBB->createPhiArgument(
        P.getType(Ty, Mod, TypeExpansionContext(*F)), OwnershipKind::Owned);
    // This is unfortunate, but it feels wrong to put in an API into SILBuilder
    // that only takes in arguments.
    //
    // TODO: We really need some sort of entry point that is more flexible in
    // these apis than a ArrayRef<SILValue>.
    NewArgumentValues.push_back(NewArg);
  }

  SingleValueInstruction *Agg;

  {
    SILBuilder B(ParentBB->begin());
    B.setCurrentDebugScope(ParentBB->getParent()->getDebugScope());

    // Reform the original structure
    //
    // TODO: What is the right location to use here.
    auto Loc = RegularLocation::getAutoGeneratedLocation();
    Agg = Projection::createAggFromFirstLevelProjections(
              B, Loc, Arg->getType(), NewArgumentValues).get();
  }

  Arg->replaceAllUsesWith(Agg);

  // Replace any references to Arg in IncomingValues with Agg. These
  // references are used in generating new instructions that extract
  // from the aggregate.
  for (auto &P : IncomingValues)
    if (P.second == Arg)
      P.second = Agg;

  // Look at all users of agg and see if we can simplify any of them. This will
  // eliminate struct_extracts/tuple_extracts from the newly created aggregate
  // and have them point directly at the argument.
  simplifyUsers(Agg);

  // If we only had such users of Agg and Agg is dead now (ignoring debug
  // instructions), remove it.
  if (onlyHaveDebugUses(Agg))
    eraseFromParentWithDebugInsts(Agg);

  return true;
}

static llvm::cl::opt<bool>
RemoveDeadArgsWhenSplitting("sroa-args-remove-dead-args-after",
                            llvm::cl::init(true));

bool ArgumentSplitter::split() {
  if (!EnableOSSARewriteTerminator && Arg->getFunction()->hasOwnership()) {
    // TODO: OSSA phi support
    if (!Arg->getType().isTrivial(*Arg->getFunction()))
      return false;
  }
  SILBasicBlock *ParentBB = Arg->getParent();

  if (!createNewArguments())
    return false;

  LLVM_DEBUG(llvm::dbgs() << "split argument " << *Arg);

  unsigned ArgIndex = Arg->getIndex();
  llvm::SmallVector<SILValue, 4> NewIncomingValues;
  // Then for each incoming value, fixup the branch, cond_branch instructions.
  for (auto P : IncomingValues) {
    SILBasicBlock *Pred = P.first;
    SILValue Base = P.second;
    auto *OldTerm = Pred->getTerminator();
    SILBuilderWithScope B(OldTerm->getParent(), OldTerm);

    auto Loc = RegularLocation::getAutoGeneratedLocation();
    assert(NewIncomingValues.empty() && "NewIncomingValues was not cleared?");
    for (auto &P : llvm::reverse(Projections)) {
      auto *ProjInst = P.createProjection(B, Loc, Base).get();
      NewIncomingValues.push_back(ProjInst);
    }

    if (auto *Br = dyn_cast<BranchInst>(OldTerm)) {
      replaceIncomingArgs(B, Br, NewIncomingValues);
    } else {
      auto *CondBr = cast<CondBranchInst>(OldTerm);
      replaceIncomingArgs(B, CondBr, NewIncomingValues);
    }

    OldTerm->eraseFromParent();
    NewIncomingValues.clear();
  }

  // Delete the old argument. We need to do this before trying to remove any
  // dead arguments that we added since otherwise the number of incoming values
  // to the phi nodes will differ from the number of values coming
  ParentBB->eraseArgument(ArgIndex);
  ++NumSROAArguments;

  // This is here for testing purposes via sil-opt
  if (!RemoveDeadArgsWhenSplitting)
    return true;

  // Perform some cleanups such as:
  //
  // 1. Removing any newly inserted arguments that are actually dead.
  // 2. As a result of removing these arguments, remove any newly dead object
  // projections.

  // Do a quick pass over the new arguments to see if any of them are dead. We
  // can do this unconditionally in a safe way since we are only dealing with
  // cond_br, br.
  for (int i = ParentBB->getNumArguments() - 1, e = *FirstNewArgIndex; i >= e;
       --i) {
    SILArgument *A = ParentBB->getArgument(i);
    if (!A->use_empty()) {
      // We know that the argument is not dead, so add it to the worklist for
      // recursive processing.
      Worklist.push_back(A);
      continue;
    }
    erasePhiArgument(ParentBB, i);
    ++NumDeadArguments;
  }

  return true;
}

/// This currently invalidates the CFG since parts of PHI nodes are stored in
/// branch instructions and we replace the branch instructions as part of this
/// operation. If/when PHI nodes can be updated without invalidating the CFG,
/// this should be moved to the SROA pass.
static bool splitBBArguments(SILFunction &Fn) {
  bool Changed = false;
  std::vector<SILArgument *> Worklist;

  // We know that we have at least one BB, so this is safe since in such a case
  // std::next(Fn->begin()) == Fn->end(), the exit case of iteration on a range.
  for (auto &BB : make_range(std::next(Fn.begin()), Fn.end())) {
    for (auto *Arg : BB.getArguments()) {
      SILType ArgTy = Arg->getType();

      if (!ArgTy.isObject() ||
          (!ArgTy.is<TupleType>() && !ArgTy.getStructOrBoundGenericStruct())) {
        continue;
      }

      // Make sure that all predecessors of our BB have either a br or cond_br
      // terminator. We only handle those cases.
      if (std::any_of(BB.pred_begin(), BB.pred_end(),
                      [](SILBasicBlock *Pred) -> bool {
                        auto *TI = Pred->getTerminator();
                        return !isa<BranchInst>(TI) && !isa<CondBranchInst>(TI);
                      })) {
        continue;
      }

      Worklist.push_back(Arg);
    }
  }

  while (!Worklist.empty()) {
    SILArgument *Arg = Worklist.back();
    Worklist.pop_back();

    Changed |= ArgumentSplitter(Arg, Worklist).split();
  }

  return Changed;
}

bool SimplifyCFG::run() {
  LLVM_DEBUG(llvm::dbgs() << "### Run SimplifyCFG on " << Fn.getName() << '\n');

  // Disable some expensive optimizations if the function is huge.
  isVeryLargeFunction = (Fn.size() > 10000);

  if (!transform.continueWithNextSubpassRun())
    return false;

  // First remove any block not reachable from the entry.
  bool Changed = removeUnreachableBlocks(Fn);

  DeadEndBlocksAnalysis *deBlocksAnalysis =
    PM->getAnalysis<DeadEndBlocksAnalysis>();
  if (Changed) {
    // Eliminate unreachable blocks from deBlocks. This isn't strictly necessary
    // but avoids excess dangling pointers in deBlocks.
    deBlocksAnalysis->invalidate(&Fn,
                                 SILAnalysis::InvalidationKind::FunctionBody);
  }
  deBlocks = deBlocksAnalysis->get(&Fn);

  // Find the set of loop headers. We don't want to jump-thread through headers.
  findLoopHeaders();

  DT = nullptr;
  if (!transform.continueWithNextSubpassRun())
    return Changed;

  // Perform SROA on BB arguments.
  Changed |= splitBBArguments(Fn);

  Changed |= simplifyBlocks();

  if (!transform.continueWithNextSubpassRun())
    return Changed;

  // Do simplifications that require the dominator tree to be accurate.
  DominanceAnalysis *DA = PM->getAnalysis<DominanceAnalysis>();
  if (Changed) {
    // Force dominator recomputation since we modified the cfg.
    DA->invalidate(&Fn, SILAnalysis::InvalidationKind::FunctionBody);
    // Eliminate unreachable blocks from deBlocks. This isn't strictly necessary
    // but avoids excess dangling pointers in deBlocks.
    deBlocksAnalysis->invalidate(&Fn,
                                 SILAnalysis::InvalidationKind::FunctionBody);
  }
  deBlocks = deBlocksAnalysis->get(&Fn);

  Changed |= dominatorBasedSimplify(DA);

  if (!transform.continueWithNextSubpassRun())
    return Changed;

  DT = nullptr;
  // Now attempt to simplify the remaining blocks.
  Changed |= simplifyBlocks();

  if (!transform.continueWithNextSubpassRun())
    return Changed;

  if (tailDuplicateObjCMethodCallSuccessorBlocks()) {
    Changed = true;
    simplifyBlocks();
  }

  if (!transform.continueWithNextSubpassRun())
    return Changed;

  if (Fn.getModule().getOptions().VerifyAll)
    Fn.verifyCriticalEdges();

  // Canonicalize switch_enum instructions.
  Changed |= canonicalizeSwitchEnums();

  return Changed;
}

/// Is an argument from this terminator considered mandatory?
static bool hasMandatoryArgument(TermInst *term) {
  // It's more maintainable to just explicitly list the instructions that
  // *do* have mandatory arguments.
  return (!isa<BranchInst>(term) && !isa<CondBranchInst>(term));
}


// Get the element of Aggregate corresponding to the one extracted by
// Extract.
static SILValue getInsertedValue(SILInstruction *Aggregate,
                                 SILInstruction *Extract) {
  if (auto *Struct = dyn_cast<StructInst>(Aggregate)) {
    auto *SEI = cast<StructExtractInst>(Extract);
    return Struct->getFieldValue(SEI->getField());
  }
  if (auto *Enum = dyn_cast<EnumInst>(Aggregate)) {
    assert(Enum->getElement() ==
           cast<UncheckedEnumDataInst>(Extract)->getElement());
    return Enum->getOperand();
  }
  auto *Tuple = cast<TupleInst>(Aggregate);
  auto *TEI = cast<TupleExtractInst>(Extract);
  return Tuple->getElement(TEI->getFieldIndex());
}

/// Find a parent SwitchEnumInst of the block \p BB. The block \p BB is a
/// predecessor of the merge-block \p PostBB which should post-dominate the
/// switch_enum. Any successors of the switch_enum which reach \p BB (and are
/// post-dominated by \p BB) are added to \p Blocks.
static SwitchEnumInst *
getSwitchEnumPred(SILBasicBlock *BB, SILBasicBlock *PostBB,
                  SmallVectorImpl<SILBasicBlock *> &Blocks) {

  if (BB->pred_empty())
    return nullptr;

  // Check that this block only produces the value, but does not
  // have any side effects.
  auto First = BB->begin();
  auto *BI = dyn_cast<BranchInst>(BB->getTerminator());
  if (!BI)
    return nullptr;

  assert(BI->getDestBB() == PostBB && "BB not a predecessor of PostBB");

  if (BI != &*First) {
    // There may be only one instruction before the branch.
    if (BI != &*std::next(First))
      return nullptr;

    // There are some instructions besides the branch.
    // It should be only an integer literal instruction.
    // Handle only integer values for now.
    auto *ILI = dyn_cast<IntegerLiteralInst>(First);
    if (!ILI)
      return nullptr;

    // Check that this literal is only used by the terminator.
    for (auto U : ILI->getUses())
      if (U->getUser() != BI)
        return nullptr;
  }

  // Check if BB is reachable from a single enum case, which means that the
  // immediate predecessor of BB is the switch_enum itself.
  if (SILBasicBlock *PredBB = BB->getSinglePredecessorBlock()) {
    // Check if a predecessor BB terminates with a switch_enum instruction
    if (auto *SEI = dyn_cast<SwitchEnumInst>(PredBB->getTerminator())) {
      Blocks.push_back(BB);
      return SEI;
    }
  }

  // Check if BB is reachable from multiple enum cases. This means that there is
  // a single-branch block for each enum case which branch to BB.
  // Usually in this case BB has no arguments. If there are any arguments, bail,
  // because the argument may be used by other instructions.
  if (BB->getNumArguments() != 0)
    return nullptr;

  SILBasicBlock *CommonPredPredBB = nullptr;
  for (auto PredBB : BB->getPredecessorBlocks()) {
    TermInst *PredTerm = PredBB->getTerminator();
    if (!isa<BranchInst>(PredTerm) || PredTerm != &*PredBB->begin())
      return nullptr;

    auto *PredPredBB = PredBB->getSinglePredecessorBlock();
    if (!PredPredBB)
      return nullptr;

    // Check if all predecessors of BB have a single common predecessor (which
    // should be the block with the switch_enum).
    if (CommonPredPredBB && PredPredBB != CommonPredPredBB)
      return nullptr;

    CommonPredPredBB = PredPredBB;
    Blocks.push_back(PredBB);
  }
  // Check if the common predecessor block has a switch_enum.
  return dyn_cast<SwitchEnumInst>(CommonPredPredBB->getTerminator());
}

/// Helper function to produce a SILValue from a result value
/// produced by a basic block responsible for handling a
/// specific enum tag.
static SILValue
getSILValueFromCaseResult(SILBuilder &B, SILLocation Loc,
                          SILType Type, IntegerLiteralInst *ValInst) {
  auto Value = ValInst->getValue();
  if (Value.getBitWidth() != 1)
    return B.createIntegerLiteral(Loc, Type, Value);
  else
    // This is a boolean value
    return B.createIntegerLiteral(Loc, Type, Value.getBoolValue());
}

/// Given an integer argument, see if it is ultimately matching whether
/// a given enum is of a given tag.  If so, create a new select_enum instruction
/// This is used to simplify arbitrary simple switch_enum diamonds into
/// select_enums.
static bool simplifySwitchEnumToSelectEnum(SILBasicBlock *BB, unsigned ArgNum,
                                           SILArgument *IntArg) {

  // Don't know which values should be passed if there is more
  // than one basic block argument.
  if (BB->args_size() > 1)
    return false;

  // Mapping from case values to the results corresponding to this case value.
  SmallVector<std::pair<EnumElementDecl *, SILValue>, 8> CaseToValue;

  // Mapping from BB responsible for a specific case value to the result it
  // produces.
  llvm::DenseMap<SILBasicBlock *, IntegerLiteralInst *> BBToValue;

  // switch_enum instruction to be replaced.
  SwitchEnumInst *SEI = nullptr;

  // Iterate over all immediate predecessors of the target basic block.
  // - Check that each one stems directly or indirectly from the same
  //   switch_enum instruction.
  // - Remember for each case tag of the switch_enum instruction which
  //   integer value it produces.
  // - Check that each block handling a given case tag of a switch_enum
  //   only produces an integer value and does not have any side-effects.
  // Predecessors which do not satisfy these conditions are not included in the
  // BBToValue map (but we don't bail in this case).
  for (auto P : BB->getPredecessorBlocks()) {
    // Only handle branch instructions.
    auto *TI = P->getTerminator();
    if (!isa<BranchInst>(TI))
      return false;

    // Find the Nth argument passed to BB.
    auto Arg = TI->getOperand(ArgNum);
    // Only handle integer values
    auto *IntLit = dyn_cast<IntegerLiteralInst>(Arg);
    if (!IntLit)
      continue;

    // Set of blocks that branch to/reach this basic block P and are immediate
    // successors of a switch_enum instruction.
    SmallVector<SILBasicBlock *, 8> Blocks;

    // Try to find a parent SwitchEnumInst for the current predecessor of BB.
    auto *PredSEI = getSwitchEnumPred(P, BB, Blocks);

    // Check if the predecessor is not produced by a switch_enum instruction.
    if (!PredSEI)
      continue;

    // Check if all predecessors stem from the same switch_enum instruction.
    if (SEI && SEI != PredSEI)
      continue;
    SEI = PredSEI;

    // Remember the result value used to branch to this instruction.
    for (auto B : Blocks)
      BBToValue[B] = IntLit;
  }

  if (!SEI)
    return false;

  // Check if all enum cases and the default case go to one of our collected
  // blocks. This check ensures that the target block BB post-dominates the
  // switch_enum block.
  for (SILBasicBlock *Succ : SEI->getSuccessors()) {
    if (!BBToValue.count(Succ))
      return false;
  }

  // Insert the new enum_select instruction right after enum_switch
  SILBuilder B(SEI);

  // Form a set of case_tag:result pairs for select_enum
  for (unsigned i = 0, e = SEI->getNumCases(); i != e; ++i) {
    std::pair<EnumElementDecl *, SILBasicBlock *> Pair = SEI->getCase(i);
    auto CaseValue = BBToValue[Pair.second];
    auto CaseSILValue = getSILValueFromCaseResult(B, SEI->getLoc(),
                                                  IntArg->getType(),
                                                  CaseValue);
    CaseToValue.push_back(std::make_pair(Pair.first, CaseSILValue));
  }

  // Default value for select_enum.
  SILValue DefaultSILValue = SILValue();

  if (SEI->hasDefault()) {
    // Try to define a default case for enum_select based
    // on the default case of enum_switch.
    auto DefaultValue = BBToValue[SEI->getDefaultBB()];
    DefaultSILValue = getSILValueFromCaseResult(B, SEI->getLoc(),
                                                IntArg->getType(),
                                                DefaultValue);
  } else {
    // Try to see if enum_switch covers all possible cases.
    // If it does, then pick one of those cases as a default.

    // Count the number of possible case tags for a given enum type
    auto *Enum = SEI->getOperand()->getType().getEnumOrBoundGenericEnum();
    unsigned ElemCount = 0;
    for (auto E : Enum->getAllElements()) {
      if (E)
        ++ElemCount;
    }

    // Check if all possible cases are covered.
    if (ElemCount == SEI->getNumCases()) {
      // This enum_switch instruction is exhaustive.
      // Make the last case a default.
      auto Pair = CaseToValue.pop_back_val();
      DefaultSILValue = Pair.second;
    }
  }

  // We don't need to have explicit cases for any case tags which produce the
  // same result as the default branch.
  if (DefaultSILValue != SILValue()) {
    auto DefaultValue = DefaultSILValue;
    auto *DefaultSI = dyn_cast<IntegerLiteralInst>(DefaultValue);
    for (auto I = CaseToValue.begin(); I != CaseToValue.end();) {
      auto CaseValue = I->second;
      if (CaseValue == DefaultValue) {
        I = CaseToValue.erase(I);
        continue;
      }

      if (DefaultSI) {
        if (auto CaseSI = dyn_cast<IntegerLiteralInst>(CaseValue)) {
          if (DefaultSI->getValue() == CaseSI->getValue()) {
            I = CaseToValue.erase(I);
            continue;
          }
        }
      }
      ++I;
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "convert to select_enum: " << *SEI);

  // Create a new select_enum instruction
  auto SelectInst = B.createSelectEnum(SEI->getLoc(), SEI->getOperand(),
                                       IntArg->getType(),
                                       DefaultSILValue, CaseToValue);
  // Do not replace the bbarg
  SmallVector<SILValue, 4> Args;
  Args.push_back(SelectInst);
  B.setInsertionPoint(&*std::next(SelectInst->getIterator()));
  B.createBranch(SEI->getLoc(), BB, Args);
  // Remove switch_enum instruction
  SEI->getParent()->getTerminator()->eraseFromParent();
  return true;
}

/// Collected information for a select_value case or default case.
struct CaseInfo {
  /// The input value or null if it is the default case.
  IntegerLiteralInst *Literal = nullptr;
  
  /// The result value.
  SingleValueInstruction *Result = nullptr;

  /// The block which contains the cond_br of the input value comparison
  /// or the block which assigns the default value.
  SILBasicBlock *CmpOrDefault = nullptr;
};

/// Get information about a potential select_value case (or default).
/// \p Input is set to the common input value.
/// \p Pred is the predecessor block of the last merge block of the CFG pattern.
/// \p ArgNum is the index of the argument passed to the merge block.
CaseInfo getCaseInfo(SILValue &Input, SILBasicBlock *Pred, unsigned ArgNum) {
  
  CaseInfo CaseInfo;
  
  auto *TI = Pred->getTerminator();
  if (!isa<BranchInst>(TI))
    return CaseInfo;
  
  // Find the Nth argument passed to BB.
  auto Arg = TI->getOperand(ArgNum);

  // Currently we only accept enums as result values.
  auto *EI2 = dyn_cast<EnumInst>(Arg);
  if (!EI2)
    return CaseInfo;
  
  if (EI2->hasOperand()) {
    // ... or enums with enum data. This is exactly the pattern for an enum
    // with integer raw value initialization.
    auto *EI1 = dyn_cast<EnumInst>(EI2->getOperand());
    if (!EI1)
      return CaseInfo;
    
    // But not enums with enums with data.
    if (EI1->hasOperand())
      return CaseInfo;
  }
  
  // Check if we come to the Pred block by comparing the input value to a
  // constant.
  SILBasicBlock *CmpBlock = Pred->getSinglePredecessorBlock();
  if (!CmpBlock)
    return CaseInfo;
  
  auto *CmpInst = dyn_cast<CondBranchInst>(CmpBlock->getTerminator());
  if (!CmpInst)
    return CaseInfo;
  
  auto *CondInst = dyn_cast<BuiltinInst>(CmpInst->getCondition());
  if (!CondInst)
    return CaseInfo;
  
  if (!CondInst->getName().str().startswith("cmp_eq"))
    return CaseInfo;
  
  auto CondArgs = CondInst->getArguments();
  assert(CondArgs.size() == 2);
  
  SILValue Arg1 = CondArgs[0];
  SILValue Arg2 = CondArgs[1];
  
  if (isa<IntegerLiteralInst>(Arg1))
    std::swap(Arg1, Arg2);
  
  auto *CmpVal = dyn_cast<IntegerLiteralInst>(Arg2);
  if (!CmpVal)
    return CaseInfo;

  SILBasicBlock *FalseBB = CmpInst->getFalseBB();
  if (!FalseBB)
    return CaseInfo;

  // Check for a common input value.
  if (Input && Input != Arg1)
    return CaseInfo;
  
  Input = Arg1;
  CaseInfo.Result = EI2;
  if (CmpInst->getTrueBB() == Pred) {
    // This is a case for the select_value.
    CaseInfo.Literal = CmpVal;
    CaseInfo.CmpOrDefault = CmpBlock;
  } else {
    // This is the default for the select_value.
    CaseInfo.CmpOrDefault = Pred;
  }
  
  return CaseInfo;
}

/// Move an instruction which is an operand to the new SelectValueInst to its
/// correct place.
/// Either the instruction is somewhere inside the CFG pattern, then we move it
/// up, immediately before the SelectValueInst in the pattern's dominating
/// entry block. Or it is somewhere above the entry block, then we can leave the
/// instruction there.
void moveIfNotDominating(SILInstruction *I, SILInstruction *InsertPos,
                         DominanceInfo *DT) {
  SILBasicBlock *InstBlock = I->getParent();
  SILBasicBlock *InsertBlock = InsertPos->getParent();
  if (!DT->dominates(InstBlock, InsertBlock)) {
    assert(DT->dominates(InsertBlock, InstBlock));
    LLVM_DEBUG(llvm::dbgs() << "  move " << *I);
    I->moveBefore(InsertPos);
  }
}

/// Simplify a pattern of integer compares to a select_value.
/// \code
///   if input == 1 {
///     result = Enum.A
///   } else if input == 2 {
///     result = Enum.B
///   ...
///   } else {
///     result = Enum.X
///   }
/// \endcode
/// Currently this only works if the input value is an integer and the result
/// value is an enum.
/// \p MergeBlock The "last" block which contains an argument in which all
///               result values are merged.
/// \p ArgNum The index of the block argument which is the result value.
/// \p DT The dominance info.
/// \return Returns true if a select_value is generated.
bool simplifyToSelectValue(SILBasicBlock *MergeBlock, unsigned ArgNum,
                           DominanceInfo *DT) {
  if (!DT)
    return false;
  
  // Collect all case infos from the merge block's predecessors.
  BasicBlockSet FoundCmpBlocks(MergeBlock->getParent());
  SmallVector<CaseInfo, 8> CaseInfos;
  SILValue Input;
  for (auto *Pred : MergeBlock->getPredecessorBlocks()) {
    CaseInfo CaseInfo = getCaseInfo(Input, Pred, ArgNum);
    if (!CaseInfo.Result)
      return false;

    FoundCmpBlocks.insert(CaseInfo.CmpOrDefault);
    CaseInfos.push_back(CaseInfo);
  }
  
  SmallVector<std::pair<SILValue, SILValue>, 8> Cases;
  llvm::SmallDenseMap<SILValue, SILValue> CaseLiteralsToResultMap;
  SILValue defaultResult;
  
  // The block of the first input value compare. It dominates all other blocks
  // in this CFG pattern.
  SILBasicBlock *dominatingBlock = nullptr;
  
  // Build the cases for the SelectValueInst and find the first dominatingBlock.
  for (auto &CaseInfo : CaseInfos) {
    if (CaseInfo.Literal) {
      auto *BrInst = cast<CondBranchInst>(CaseInfo.CmpOrDefault->getTerminator());
      if (!FoundCmpBlocks.contains(BrInst->getFalseBB()))
        return false;
      // Ignore duplicate cases
      if (CaseLiteralsToResultMap.find(CaseInfo.Literal) ==
          CaseLiteralsToResultMap.end()) {
        CaseLiteralsToResultMap.insert({CaseInfo.Literal, CaseInfo.Result});
        Cases.push_back({CaseInfo.Literal, CaseInfo.Result});
      } else {
        // Check if the result value matches
        EnumInst *PrevResult =
            dyn_cast<EnumInst>(CaseLiteralsToResultMap[CaseInfo.Literal]);
        assert(PrevResult && "Prev. case result is not an EnumInst");
        auto *CurrResult = dyn_cast<EnumInst>(CaseInfo.Result);
        assert(CurrResult && "Curr. case result is not an EnumInst");
        if (PrevResult->getElement() != CurrResult->getElement()) {
          // result value does not match - bail
          return false;
        }
      }
      SILBasicBlock *Pred = CaseInfo.CmpOrDefault->getSinglePredecessorBlock();
      if (!Pred || !FoundCmpBlocks.contains(Pred)) {
        // There may be only a single block whose predecessor we didn't see. And
        // this is the entry block to the CFG pattern.
        if (dominatingBlock)
          return false;
        dominatingBlock = CaseInfo.CmpOrDefault;
      }
    } else {
      if (defaultResult)
        return false;
      defaultResult = CaseInfo.Result;
    }
  }
  if (!defaultResult)
    return false;

  if (!dominatingBlock)
    return false;
  
  // Generate the select_value right before the first cond_br of the pattern.
  SILInstruction *insertPos = dominatingBlock->getTerminator();
  SILBuilder B(insertPos);
  
  // Move all needed operands to a place where they dominate the select_value.
  for (auto &CaseInfo : CaseInfos) {
    if (CaseInfo.Literal)
      moveIfNotDominating(CaseInfo.Literal, insertPos, DT);
    auto *EI2 = dyn_cast<EnumInst>(CaseInfo.Result);
    assert(EI2);
    
    if (EI2->hasOperand()) {
      auto *EI1 = dyn_cast<EnumInst>(EI2->getOperand());
      assert(EI1);
      assert(!EI1->hasOperand());

      moveIfNotDominating(EI1, insertPos, DT);
    }
    moveIfNotDominating(EI2, insertPos, DT);
  }

  SILArgument *bbArg = MergeBlock->getArgument(ArgNum);
  auto SelectInst = B.createSelectValue(dominatingBlock->getTerminator()->getLoc(),
                                        Input, bbArg->getType(),
                                       defaultResult, Cases);

  bbArg->replaceAllUsesWith(SelectInst);
  LLVM_DEBUG(llvm::dbgs() << "convert if-structure to " << *SelectInst);

  return true;
}

bool SimplifyCFG::simplifyBlockArgs() {
  auto *DA = PM->getAnalysis<DominanceAnalysis>();

  DT = DA->get(&Fn);
  bool Changed = false;
  for (SILBasicBlock &BB : Fn) {
    Changed |= simplifyArgs(&BB);
  }
  DT = nullptr;
  return Changed;
}

// Attempt to simplify the ith argument of BB.  We simplify cases
// where there is a single use of the argument that is an extract from
// a struct, tuple or enum and where the predecessors all build the struct,
// tuple or enum and pass it directly.
bool SimplifyCFG::simplifyArgument(SILBasicBlock *BB, unsigned i) {
  auto *A = BB->getArgument(i);

  // Try to create a select_value.
  if (simplifyToSelectValue(BB, i, DT))
    return true;
  
  // If we are reading an i1, then check to see if it comes from
  // a switch_enum.  If so, we may be able to lower this sequence to
  // a select_enum.
  if (!DT && A->getType().is<BuiltinIntegerType>())
    return simplifySwitchEnumToSelectEnum(BB, i, A);

  // For now, just focus on cases where there is a single use.
  if (!A->hasOneUse())
    return false;

  auto *Use = *A->use_begin();
  auto *User = Use->getUser();

  // Handle projections.
  if (!isa<StructExtractInst>(User) &&
      !isa<TupleExtractInst>(User) &&
      !isa<UncheckedEnumDataInst>(User))
    return false;
  auto proj = cast<SingleValueInstruction>(User);

  // For now, just handle the case where all predecessors are
  // unconditional branches.
  for (auto *Pred : BB->getPredecessorBlocks()) {
    if (!isa<BranchInst>(Pred->getTerminator()))
      return false;
    auto *Branch = cast<BranchInst>(Pred->getTerminator());
    SILValue BranchArg = Branch->getArg(i);
    if (isa<StructInst>(BranchArg))
      continue;
    if (isa<TupleInst>(BranchArg))
      continue;
    if (auto *EI = dyn_cast<EnumInst>(BranchArg)) {
      if (EI->getElement() == cast<UncheckedEnumDataInst>(proj)->getElement())
        continue;
    }
    return false;
  }

  // Okay, we'll replace the BB arg with one with the right type, replace
  // the uses in this block, and then rewrite the branch operands.
  LLVM_DEBUG(llvm::dbgs() << "unwrap argument:" << *A);
  A->replaceAllUsesWith(SILUndef::get(A->getType(), *BB->getParent()));
  auto *NewArg = BB->replacePhiArgument(i, proj->getType(),
                                        BB->getArgument(i)->getOwnershipKind());
  proj->replaceAllUsesWith(NewArg);

  // Rewrite the branch operand for each incoming branch.
  for (auto *Pred : BB->getPredecessorBlocks()) {
    if (auto *Branch = cast<BranchInst>(Pred->getTerminator())) {
      auto *BranchOpValue = cast<SingleValueInstruction>(Branch->getOperand(i));
      auto V = getInsertedValue(cast<SingleValueInstruction>(Branch->getArg(i)),
                                proj);
      Branch->setOperand(i, V);
      if (isInstructionTriviallyDead(BranchOpValue)) {
        BranchOpValue->replaceAllUsesWithUndef();
        BranchOpValue->eraseFromParent();
      }
      addToWorklist(Pred);
    }
  }

  proj->eraseFromParent();

  return true;
}

// OWNERSHIP NOTE: This is always safe for guaranteed and owned arguments since
// in both cases the phi will consume its input.
static void tryToReplaceArgWithIncomingValue(SILBasicBlock *BB, unsigned i,
                                             DominanceInfo *DT) {
  auto *A = BB->getArgument(i);
  SmallVector<SILValue, 4> Incoming;
  if (!A->getIncomingPhiValues(Incoming) || Incoming.empty())
    return;
  
  SILValue V = Incoming[0];
  for (size_t Idx = 1, Size = Incoming.size(); Idx < Size; ++Idx) {
    if (Incoming[Idx] != V)
      return;
  }
  
  // If the incoming values of all predecessors are equal usually this means
  // that the common incoming value dominates the BB. But: this might be not
  // the case if BB is unreachable. Therefore we still have to check it.
  if (!DT->dominates(V->getParentBlock(), BB))
    return;

  // An argument has one result value. We need to replace this with the *value*
  // of the incoming block(s).
  LLVM_DEBUG(llvm::dbgs() << "replace arg with incoming value:" << *A);
  A->replaceAllUsesWith(V);
}

bool SimplifyCFG::simplifyArgs(SILBasicBlock *BB) {
  // Ignore blocks with no arguments.
  if (BB->args_empty())
    return false;

  // Ignore the entry block.
  if (BB->pred_empty())
    return false;

  // Ignore blocks that are successors of terminators with mandatory args.
  for (SILBasicBlock *pred : BB->getPredecessorBlocks()) {
    if (hasMandatoryArgument(pred->getTerminator()))
      return false;
  }

  bool Changed = false;
  for (int i = BB->getNumArguments() - 1; i >= 0; --i) {
    SILArgument *A = BB->getArgument(i);

    // Replace a block argument if all incoming values are equal. If this
    // succeeds, argument A will have no uses afterwards.
    if (DT)
      tryToReplaceArgWithIncomingValue(BB, i, DT);
    
    // Try to simplify the argument
    if (!A->use_empty()) {
      if (simplifyArgument(BB, i))
        Changed = true;
      continue;
    }

    erasePhiArgument(BB, i);
    ++NumDeadArguments;
    Changed = true;
  }

  return Changed;
}

bool SimplifyCFG::simplifyProgramTerminationBlock(SILBasicBlock *BB) {
  // If this is not ARC-inert, do not do anything to it.
  //
  // TODO: should we use ProgramTerminationAnalysis ?. The reason we do not
  // use the analysis is because the CFG is likely to be invalidated right
  // after this pass, that's why we do not really get the benefit of reusing the
  // computation for the next iteration of the pass.
  if (!isARCInertTrapBB(BB))
    return false;

  // This is going to be the last basic block this program is going to execute
  // and this block is inert from the ARC's prospective,so there's no point to do any
  // releases at this point.
  bool Changed = false;
  llvm::SmallPtrSet<SILInstruction *, 4> InstsToRemove;
  for (auto &I : *BB) {
    // We can only remove the instructions below from the ARC-inert BB
    // We *can't* replace copy_addr with move instructions:
    // If the copy_addr was [take] [initialization]:
    //   * previous passes would have replaced it with moves
    // If the copy_addr contains [initialization]:
    //   * nothing we can do - the target address is invalid
    // Else, i.e. the copy_addr was [take] assignment, it is not always safe:
    // The type being operated on might contain weak references,
    // or other side references - We'll corrupt the weak reference table
    // if we fail to release the old value.
    switch (I.getKind()) {
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
    case SILInstructionKind::Name##ReleaseInst:
#include "swift/AST/ReferenceStorage.def"
    case SILInstructionKind::StrongReleaseInst:
    case SILInstructionKind::ReleaseValueInst:
    case SILInstructionKind::DestroyValueInst:
    case SILInstructionKind::DestroyAddrInst:
      break;
    default:
      continue;
    }
    LLVM_DEBUG(llvm::dbgs() << "remove dead-end destroy " << I);
    InstsToRemove.insert(&I);
  }

  // Remove the instructions.
  for (auto I : InstsToRemove) {
    I->eraseFromParent();
    Changed = true;
  }

  if (Changed)
   ++NumTermBlockSimplified;

  return Changed;
}

namespace {
class SimplifyCFGPass : public SILFunctionTransform {
public:
  void run() override {
    if (SimplifyCFG(*getFunction(), *this, getOptions().VerifyAll,
                    /*EnableJumpThread=*/false)
            .run())
      invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
  }
};
} // end anonymous namespace


SILTransform *swift::createSimplifyCFG() {
  return new SimplifyCFGPass();
}

namespace {
class JumpThreadSimplifyCFGPass : public SILFunctionTransform {
public:
  void run() override {
    if (SimplifyCFG(*getFunction(), *this, getOptions().VerifyAll,
                    /*EnableJumpThread=*/true)
            .run())
      invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
  }
};
} // end anonymous namespace

SILTransform *swift::createJumpThreadSimplifyCFG() {
  return new JumpThreadSimplifyCFGPass();
}

//===----------------------------------------------------------------------===//
//                          Passes only for Testing
//===----------------------------------------------------------------------===//

namespace {

// Used to test critical edge splitting with sil-opt.
class SplitCriticalEdges : public SILFunctionTransform {
  bool OnlyNonCondBrEdges;

public:
  SplitCriticalEdges(bool SplitOnlyNonCondBrEdges)
      : OnlyNonCondBrEdges(SplitOnlyNonCondBrEdges) {}

  void run() override {
    auto &Fn = *getFunction();

    if (OnlyNonCondBrEdges && Fn.getModule().getOptions().VerifyAll)
      Fn.verifyCriticalEdges();

    // Split all critical edges from all or non only cond_br terminators.
    bool Changed = splitAllCriticalEdges(Fn, nullptr, nullptr);

    if (Changed) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::BranchesAndInstructions);
    }
  }

};

// Used to test SimplifyCFG::simplifyArgs with sil-opt.
class SimplifyBBArgs : public SILFunctionTransform {
public:
  SimplifyBBArgs() {}
  
  /// The entry point to the transformation.
  void run() override {
    if (SimplifyCFG(*getFunction(), *this, getOptions().VerifyAll, false)
        .simplifyBlockArgs()) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::BranchesAndInstructions);
    }
  }
  
};

// Used to test splitBBArguments with sil-opt
class SROABBArgs : public SILFunctionTransform {
public:
  SROABBArgs() {}

  void run() override {
    if (splitBBArguments(*getFunction())) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::BranchesAndInstructions);
    }
  }

};

// Used to test tryMoveCondFailToPreds with sil-opt
class MoveCondFailToPreds : public SILFunctionTransform {
public:
  MoveCondFailToPreds() {}
  void run() override {
    for (auto &BB : *getFunction()) {
      if (tryMoveCondFailToPreds(&BB)) {
        invalidateAnalysis(
            SILAnalysis::InvalidationKind::BranchesAndInstructions);
      }
    }
  }

};

} // end anonymous namespace

/// Splits all critical edges in a function.
SILTransform *swift::createSplitAllCriticalEdges() {
  return new SplitCriticalEdges(false);
}

/// Splits all critical edges from non cond_br terminators in a function.
SILTransform *swift::createSplitNonCondBrCriticalEdges() {
  return new SplitCriticalEdges(true);
}

// Simplifies basic block arguments.
SILTransform *swift::createSROABBArgs() { return new SROABBArgs(); }

// Simplifies basic block arguments.
SILTransform *swift::createSimplifyBBArgs() {
  return new SimplifyBBArgs();
}

// Moves cond_fail instructions to predecessors.
SILTransform *swift::createMoveCondFailToPreds() {
  return new MoveCondFailToPreds();
}
