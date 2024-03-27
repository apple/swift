//===--- BasicBlockUtils.cpp - Utilities for SILBasicBlock ----------------===//
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

#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/STLExtras.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/LoopInfo.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/TerminatorUtils.h"
#include "llvm/ADT/STLExtras.h"

using namespace swift;

static bool hasBranchArguments(TermInst *T, unsigned edgeIdx) {
  if (auto *BI = dyn_cast<BranchInst>(T)) {
    assert(edgeIdx == 0);
    return BI->getNumArgs() != 0;
  }
  if (auto CBI = dyn_cast<CondBranchInst>(T)) {
    assert(edgeIdx <= 1);
    return edgeIdx == CondBranchInst::TrueIdx ? !CBI->getTrueArgs().empty()
                                              : !CBI->getFalseArgs().empty();
  }
  // No other terminator have branch arguments.
  return false;
}

void swift::changeBranchTarget(TermInst *T, unsigned edgeIdx,
                               SILBasicBlock *newDest, bool preserveArgs) {
  // In many cases, we can just rewrite the successor in place.
  if (preserveArgs || !hasBranchArguments(T, edgeIdx)) {
    T->getSuccessors()[edgeIdx] = newDest;
    return;
  }

  // Otherwise, we have to build a new branch instruction.
  SILBuilderWithScope B(T);

  switch (T->getTermKind()) {
  // Only Branch and CondBranch may have arguments.
  case TermKind::BranchInst: {
    auto *BI = cast<BranchInst>(T);
    SmallVector<SILValue, 8> args;
    if (preserveArgs) {
      for (auto arg : BI->getArgs())
        args.push_back(arg);
    }
    B.createBranch(T->getLoc(), newDest, args);
    BI->dropAllReferences();
    BI->eraseFromParent();
    return;
  }

  case TermKind::CondBranchInst: {
    auto CBI = cast<CondBranchInst>(T);
    SILBasicBlock *trueDest = CBI->getTrueBB();
    SILBasicBlock *falseDest = CBI->getFalseBB();

    SmallVector<SILValue, 8> trueArgs;
    SmallVector<SILValue, 8> falseArgs;
    if (edgeIdx == CondBranchInst::FalseIdx) {
      falseDest = newDest;
      for (auto arg : CBI->getTrueArgs())
        trueArgs.push_back(arg);
    } else {
      trueDest = newDest;
      for (auto arg : CBI->getFalseArgs())
        falseArgs.push_back(arg);
    }

    B.createCondBranch(CBI->getLoc(), CBI->getCondition(), trueDest, trueArgs,
                       falseDest, falseArgs, CBI->getTrueBBCount(),
                       CBI->getFalseBBCount());
    CBI->dropAllReferences();
    CBI->eraseFromParent();
    return;
  }

  default:
    llvm_unreachable("only branch and cond_branch have branch arguments");
  }
}

template <class SwitchInstTy>
static SILBasicBlock *getNthEdgeBlock(SwitchInstTy *S, unsigned edgeIdx) {
  if (S->getNumCases() == edgeIdx)
    return S->getDefaultBB();
  return S->getCase(edgeIdx).second;
}

static SILBasicBlock *getNthEdgeBlock(SwitchEnumTermInst S, unsigned edgeIdx) {
  if (S.getNumCases() == edgeIdx)
    return S.getDefaultBB();
  return S.getCase(edgeIdx).second;
}

void swift::getEdgeArgs(TermInst *T, unsigned edgeIdx, SILBasicBlock *newEdgeBB,
                        llvm::SmallVectorImpl<SILValue> &args) {
  switch (T->getKind()) {
  case SILInstructionKind::BranchInst: {
    auto *B = cast<BranchInst>(T);
    for (auto V : B->getArgs())
      args.push_back(V);
    return;
  }

  case SILInstructionKind::CondBranchInst: {
    auto CBI = cast<CondBranchInst>(T);
    assert(edgeIdx < 2);
    auto OpdArgs = edgeIdx ? CBI->getFalseArgs() : CBI->getTrueArgs();
    for (auto V : OpdArgs)
      args.push_back(V);
    return;
  }
      
  case SILInstructionKind::AwaitAsyncContinuationInst: {
    auto AACI = cast<AwaitAsyncContinuationInst>(T);
    
    switch (edgeIdx) {
    case 0:
      // resume BB. this takes the resume value argument if the operand is
      // GetAsyncContinuation, or no argument if the operand is
      // GetAsyncContinuationAddr
      if (auto contOperand = dyn_cast<GetAsyncContinuationInst>(AACI->getOperand())) {
        args.push_back(newEdgeBB->createPhiArgument(
            contOperand->getLoweredResumeType(), OwnershipKind::Owned));
      }
      return;
        
    case 1: {
      assert(AACI->getErrorBB());
      auto &C = AACI->getFunction()->getASTContext();
      auto errorTy = C.getErrorExistentialType();
      auto errorSILTy = SILType::getPrimitiveObjectType(errorTy);
      // error BB. this takes the error value argument
      args.push_back(
          newEdgeBB->createPhiArgument(errorSILTy, OwnershipKind::Owned));
      return;
    }
        
    default:
      llvm_unreachable("only has at most two edges");
    }
  }

  case SILInstructionKind::SwitchValueInst: {
    auto SEI = cast<SwitchValueInst>(T);
    auto *succBB = getNthEdgeBlock(SEI, edgeIdx);
    assert(succBB->getNumArguments() == 0 && "Can't take an argument");
    (void)succBB;
    return;
  }

  // A switch_enum can implicitly pass the enum payload. We need to look at the
  // destination block to figure this out.
  case SILInstructionKind::SwitchEnumInst:
  case SILInstructionKind::SwitchEnumAddrInst: {
    SwitchEnumTermInst branch(T);
    auto *succBB = getNthEdgeBlock(branch, edgeIdx);
    assert(succBB->getNumArguments() < 2 && "Can take at most one argument");
    if (!succBB->getNumArguments())
      return;
    args.push_back(newEdgeBB->createPhiArgument(
        succBB->getArgument(0)->getType(),
        succBB->getArgument(0)->getOwnershipKind()));
    return;
  }

  // A dynamic_method_br passes the function to the first basic block.
  case SILInstructionKind::DynamicMethodBranchInst: {
    auto DMBI = cast<DynamicMethodBranchInst>(T);
    auto *succBB =
        (edgeIdx == 0) ? DMBI->getHasMethodBB() : DMBI->getNoMethodBB();
    if (!succBB->getNumArguments())
      return;
    args.push_back(newEdgeBB->createPhiArgument(
        succBB->getArgument(0)->getType(),
        succBB->getArgument(0)->getOwnershipKind()));
    return;
  }

  /// A checked_cast_br passes the result of the cast to the first basic block.
  case SILInstructionKind::CheckedCastBranchInst: {
    auto CBI = cast<CheckedCastBranchInst>(T);
    auto succBB = edgeIdx == 0 ? CBI->getSuccessBB() : CBI->getFailureBB();
    if (!succBB->getNumArguments())
      return;
    args.push_back(newEdgeBB->createPhiArgument(
        succBB->getArgument(0)->getType(),
        succBB->getArgument(0)->getOwnershipKind()));
    return;
  }
  case SILInstructionKind::CheckedCastAddrBranchInst: {
    auto CBI = cast<CheckedCastAddrBranchInst>(T);
    auto succBB = edgeIdx == 0 ? CBI->getSuccessBB() : CBI->getFailureBB();
    if (!succBB->getNumArguments())
      return;
    args.push_back(newEdgeBB->createPhiArgument(
        succBB->getArgument(0)->getType(),
        succBB->getArgument(0)->getOwnershipKind()));
    return;
  }

  case SILInstructionKind::TryApplyInst: {
    auto *TAI = cast<TryApplyInst>(T);
    auto *succBB = edgeIdx == 0 ? TAI->getNormalBB() : TAI->getErrorBB();
    if (!succBB->getNumArguments())
      return;
    args.push_back(newEdgeBB->createPhiArgument(
        succBB->getArgument(0)->getType(),
        succBB->getArgument(0)->getOwnershipKind()));
    return;
  }

  case SILInstructionKind::YieldInst:
    // The edges from 'yield' never have branch arguments.
    return;

  case SILInstructionKind::ReturnInst:
  case SILInstructionKind::ThrowInst:
  case SILInstructionKind::ThrowAddrInst:
  case SILInstructionKind::UnwindInst:
  case SILInstructionKind::UnreachableInst:
    llvm_unreachable("terminator never has successors");

#define TERMINATOR(ID, ...)
#define INST(ID, BASE) case SILInstructionKind::ID:
#include "swift/SIL/SILNodes.def"
    llvm_unreachable("not a terminator");
  }
  llvm_unreachable("bad instruction kind");
}

SILBasicBlock *swift::splitEdge(TermInst *T, unsigned edgeIdx,
                                DominanceInfo *DT, SILLoopInfo *LI) {
  auto *srcBB = T->getParent();
  auto *F = srcBB->getParent();

  SILBasicBlock *destBB = T->getSuccessors()[edgeIdx];

  // Create a new basic block in the edge, and insert it after the srcBB.
  auto *edgeBB = F->createBasicBlockAfter(srcBB);

  SmallVector<SILValue, 16> args;
  getEdgeArgs(T, edgeIdx, edgeBB, args);

  SILBuilderWithScope(edgeBB, T).createBranch(T->getLoc(), destBB, args);

  // Strip the arguments and rewire the branch in the source block.
  changeBranchTarget(T, edgeIdx, edgeBB, /*PreserveArgs=*/false);

  if (!DT && !LI)
    return edgeBB;

  // Update the dominator tree.
  if (DT) {
    auto *srcBBNode = DT->getNode(srcBB);

    // Unreachable code could result in a null return here.
    if (srcBBNode) {
      // The new block is dominated by the srcBB.
      auto *edgeBBNode = DT->addNewBlock(edgeBB, srcBB);

      // Are all predecessors of destBB dominated by destBB?
      auto *destBBNode = DT->getNode(destBB);
      bool oldSrcBBDominatesAllPreds = std::all_of(
          destBB->pred_begin(), destBB->pred_end(), [=](SILBasicBlock *B) {
            if (B == edgeBB)
              return true;
            auto *PredNode = DT->getNode(B);
            if (!PredNode)
              return true;
            if (DT->dominates(destBBNode, PredNode))
              return true;
            return false;
          });

      // If so, the new bb dominates destBB now.
      if (oldSrcBBDominatesAllPreds)
        DT->changeImmediateDominator(destBBNode, edgeBBNode);
    }
  }

  if (!LI)
    return edgeBB;

  // Update loop info. Both blocks must be in a loop otherwise the split block
  // is outside the loop.
  SILLoop *srcBBLoop = LI->getLoopFor(srcBB);
  if (!srcBBLoop)
    return edgeBB;
  SILLoop *DstBBLoop = LI->getLoopFor(destBB);
  if (!DstBBLoop)
    return edgeBB;

  // Same loop.
  if (DstBBLoop == srcBBLoop) {
    DstBBLoop->addBasicBlockToLoop(edgeBB, LI->getBase());
    return edgeBB;
  }

  // Edge from inner to outer loop.
  if (DstBBLoop->contains(srcBBLoop)) {
    DstBBLoop->addBasicBlockToLoop(edgeBB, LI->getBase());
    return edgeBB;
  }

  // Edge from outer to inner loop.
  if (srcBBLoop->contains(DstBBLoop)) {
    srcBBLoop->addBasicBlockToLoop(edgeBB, LI->getBase());
    return edgeBB;
  }

  // Neither loop contains the other. The destination must be the header of its
  // loop. Otherwise, we would be creating irreducible control flow.
  assert(DstBBLoop->getHeader() == destBB
         && "Creating irreducible control flow?");

  // Add to outer loop if there is one.
  if (auto *parent = DstBBLoop->getParentLoop())
    parent->addBasicBlockToLoop(edgeBB, LI->getBase());

  return edgeBB;
}

/// Merge the basic block with its successor if possible.
void swift::mergeBasicBlockWithSingleSuccessor(SILBasicBlock *BB,
                                               SILBasicBlock *succBB) {
  auto *BI = cast<BranchInst>(BB->getTerminator());
  assert(succBB->getSinglePredecessorBlock());

  // If there are any BB arguments in the destination, replace them with the
  // branch operands, since they must dominate the dest block.
  for (unsigned i = 0, e = BI->getArgs().size(); i != e; ++i) {
    SILArgument *arg = succBB->getArgument(i);
    if (auto *bfi = getBorrowedFromUser(arg)) {
      bfi->replaceAllUsesWith(arg);
      bfi->eraseFromParent();
    }
    arg->replaceAllUsesWith(BI->getArg(i));
  }

  BI->eraseFromParent();

  // Move the instruction from the successor block to the current block.
  BB->spliceAtEnd(succBB);

  succBB->eraseFromParent();
}

//===----------------------------------------------------------------------===//
//                              DeadEndBlocks
//===----------------------------------------------------------------------===//

// Propagate the reachability up the control flow graph.
void DeadEndBlocks::propagateNewlyReachableBlocks(unsigned startIdx) {
  for (unsigned idx = startIdx; idx < reachableBlocks.size(); ++idx) {
    const SILBasicBlock *bb = reachableBlocks[idx];
    for (SILBasicBlock *predBB : bb->getPredecessorBlocks())
      reachableBlocks.insert(predBB);
  }
}

void DeadEndBlocks::compute() {
  assert(reachableBlocks.empty() && "Computed twice");

  // First step: find blocks which end up in a no-return block (terminated by
  // an unreachable instruction).
  // Search for function-exiting blocks, i.e. return and throw.
  for (const SILBasicBlock &BB : *f) {
    const TermInst *TI = BB.getTerminator();
    if (TI->isFunctionExiting())
      reachableBlocks.insert(&BB);
  }
  propagateNewlyReachableBlocks(0);
}

void DeadEndBlocks::updateForReachableBlock(SILBasicBlock *reachableBB) {
  if (!didComputeValue)
    return;

  assert(reachableBlocks.count(reachableBB));
  unsigned numReachable = reachableBlocks.size();
  for (SILBasicBlock *predBB : reachableBB->getPredecessorBlocks()) {
    reachableBlocks.insert(predBB);
  }
  propagateNewlyReachableBlocks(numReachable);
}

void DeadEndBlocks::updateForNewBlock(SILBasicBlock *newBB) {
  if (!didComputeValue)
    return;

  assert(reachableBlocks.count(newBB) == 0);
  unsigned numReachable = reachableBlocks.size();
  reachableBlocks.insert(newBB);
  propagateNewlyReachableBlocks(numReachable);
}

bool DeadEndBlocks::triviallyEndsInUnreachable(SILBasicBlock *block) {
  // Handle the case where a single "unreachable" block (e.g. containing a call
  // to fatalError()), is jumped to from multiple source blocks.
  if (SILBasicBlock *singleSucc = block->getSingleSuccessorBlock())
    block = singleSucc;
  return isa<UnreachableInst>(block->getTerminator());
}

//===----------------------------------------------------------------------===//
//                  Post Dominance Set Completion Utilities
//===----------------------------------------------------------------------===//

void swift::findJointPostDominatingSet(
    SILBasicBlock *dominatingBlock, ArrayRef<SILBasicBlock *> dominatedBlockSet,
    function_ref<void(SILBasicBlock *)> inputBlocksFoundDuringWalk,
    function_ref<void(SILBasicBlock *)> foundJointPostDomSetCompletionBlocks,
    function_ref<void(SILBasicBlock *)> inputBlocksInJointPostDomSet) {
  // If our reachable block set is empty, assert. This is most likely programmer
  // error.
  assert(dominatedBlockSet.size() != 0);

  // If we have a reachable block set with a single block and that block is
  // dominatingBlock, then we return success since a block post-doms its self so
  // it is already complete.
  //
  // NOTE: We do not consider this a visited
  if (dominatedBlockSet.size() == 1 && dominatingBlock == dominatedBlockSet[0]) {
    if (inputBlocksInJointPostDomSet)
      inputBlocksInJointPostDomSet(dominatingBlock);
    return;
  }

  /// The worklist that drives the algorithm.
  SmallVector<SILBasicBlock *, 32> worklist;

  /// All blocks visited during the backwards walk of the CFG, but not including
  /// the initial blocks in `dominatedBlockSet`.
  BasicBlockSet visitedBlocks(dominatingBlock->getParent());

  /// All blocks in `dominatedBlockSet` (= blocks where we begin our walk).
  BasicBlockSet initialBlocks(visitedBlocks.getFunction());

  // Compute our joint post dominating set. We do this by performing a backwards
  // walk up the CFG tracking back liveness until we find our dominating block.
  for (auto *block : dominatedBlockSet) {
    // We require dominatedBlockSet to be a set and thus assert if we hit it to
    // flag user error to our caller.
    assert(!initialBlocks.contains(block) &&
           "dominatedBlockSet must not contain duplicate elements");
    initialBlocks.insert(block);
    worklist.push_back(block);
  }

  // Then until we run out of blocks...
  while (!worklist.empty()) {
    auto *block = worklist.pop_back_val();

    // If we are the dominating block, we are done.
    if (dominatingBlock == block)
      continue;

    for (auto *predBlock : block->getPredecessorBlocks()) {
      if (visitedBlocks.insert(predBlock))
        worklist.push_back(predBlock);
    }
  }

  // Do the same walk over all visited blocks again to find the "leaking"
  // blocks. These leaking blocks are the completion of the post dom set.
  //
  // Note that we could also keep all visited blocks in a SmallVector in the
  // first run. But the worklist algorithm is fast and we don't want
  // to risk that the small vector overflows (the set of visited blocks can be
  // much larger than the maximum worklist size).
  BasicBlockSet visitedBlocksInSecondRun(visitedBlocks.getFunction());
  assert(worklist.empty());
  worklist.append(dominatedBlockSet.begin(), dominatedBlockSet.end());
  while (!worklist.empty()) {
    auto *block = worklist.pop_back_val();
    if (dominatingBlock == block)
      continue;

    for (auto *predBlock : block->getPredecessorBlocks()) {
      assert(visitedBlocks.contains(predBlock));
      if (visitedBlocksInSecondRun.insert(predBlock)) {
        worklist.push_back(predBlock);
        
        for (auto *succBlock : predBlock->getSuccessorBlocks()) {
          // All not-visited successors of a visited block are "leaking" blocks.
          if (!visitedBlocks.contains(succBlock) &&
              // For this purpose also the initial blocks count as "visited",
              // although they are not added to the visitedBlocks set.
              !initialBlocks.contains(succBlock) &&
              // Ignore blocks which end in an unreachable. This is a very
              // simple check, but covers most of the cases, e.g. block which
              // calls fatalError().
              !DeadEndBlocks::triviallyEndsInUnreachable(succBlock)) {
            assert(succBlock->getSinglePredecessorBlock() == predBlock &&
                   "CFG must not contain critical edge");
            // Note that since there are no critical edges in the CFG, we are
            // not calling the closure for a leaking successor block twice.
            foundJointPostDomSetCompletionBlocks(succBlock);
          }
        }
      }
    }
  }

  // Pass back the reachable input blocks that were not reachable from other
  // input blocks to.
  for (auto *block : dominatedBlockSet) {
    if (visitedBlocks.contains(block)) {
      inputBlocksFoundDuringWalk(block);
    } else if (inputBlocksInJointPostDomSet) {
      inputBlocksInJointPostDomSet(block);
    }
  }
}

//===----------------------------------------------------------------------===//
//                          checkReachingBlockDominance
//===----------------------------------------------------------------------===//

#ifndef NDEBUG
/// Check that \p sourceBlock dominates \p destBlock.
///
/// Useful for *temporary* assertions when Dominance is unavailable. This is
/// worst case O(numberOfBlocksInFunction). It should only be used when \p
/// sourceBlock is expected to be "close to" \p destBlock in almost all
/// cases. Because of the potential for quadratic behavior, it should only be
/// used during feature development, never as a permanent check.  If a dominance
/// check is required for correctness, then DominanceInfo should be passed down
/// to the utility function that needs this check.
bool
swift::checkDominates(SILBasicBlock *sourceBlock, SILBasicBlock *destBlock) {
  SILBasicBlock *entryBlock = sourceBlock->getParent()->getEntryBlock();
  BasicBlockWorklist worklist(destBlock);
  bool reaches = false;
  while (SILBasicBlock *block = worklist.pop()) {
    if (block == sourceBlock) {
      reaches = true;
      continue;
    }
    if (block == entryBlock) {
      return false; // does not dominate
    }
    for (auto *predBlock : block->getPredecessorBlocks()) {
      worklist.pushIfNotVisited(predBlock);
    }
  }
  return reaches;
}
#endif
