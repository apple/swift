//===-------------------- SimplifyCFG.h -----------------------------------===//
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


#ifndef SWIFT_SILOPTIMIZER_SIMPLIFYCFG_H
#define SWIFT_SILOPTIMIZER_SIMPLIFYCFG_H

#include "swift/SILOptimizer/Utils/ConstantFolding.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"

namespace swift {
class DominanceAnalysis;
}

using namespace swift;

struct ThreadInfo;

class SimplifyCFG {
  SILOptFunctionBuilder FuncBuilder;
  SILFunction &Fn;
  SILFunctionTransform &transform;
  SILPassManager *PM = nullptr;

  // DeadEndBlocks remains conservatively valid across updates that rewrite
  // branches and remove edges. Any transformation that adds a block must call
  // updateForReachableBlock(). Removing a block causes a dangling pointer
  // within DeadEndBlocks, but this pointer can't be accessed by queries.
  DeadEndBlocks *deBlocks = nullptr;

  // WorklistList is the actual list that we iterate over (for determinism).
  // Slots may be null, which should be ignored.
  SmallVector<SILBasicBlock *, 32> WorklistList;
  // WorklistMap keeps track of which slot a BB is in, allowing efficient
  // containment query, and allows efficient removal.
  llvm::SmallDenseMap<SILBasicBlock *, unsigned, 32> WorklistMap;
  // Keep track of loop headers - we don't want to jump-thread through them.
  SmallPtrSet<SILBasicBlock *, 32> LoopHeaders;
  // The set of cloned loop headers to avoid infinite loop peeling. Blocks in
  // this set may or may not still be LoopHeaders.
  // (ultimately this can be used to eliminate findLoopHeaders)
  SmallPtrSet<SILBasicBlock *, 4> ClonedLoopHeaders;
  // The cost (~ number of copied instructions) of jump threading per basic
  // block. Used to prevent infinite jump threading loops.
  llvm::SmallDenseMap<SILBasicBlock *, int, 8> JumpThreadingCost;

  // Dominance and post-dominance info for the current function
  DominanceInfo *DT = nullptr;

  ConstantFolder ConstFolder;

  // True if the function has a large amount of blocks. In this case we turn off
  // some expensive optimizations.
  bool isVeryLargeFunction = false;

  void constFoldingCallback(SILInstruction *I) {
    // If a terminal instruction gets constant folded (like cond_br), it
    // enables further simplify-CFG optimizations.
    if (isa<TermInst>(I))
      addToWorklist(I->getParent());
  }

  bool ShouldVerify;
  bool EnableJumpThread;

public:
  SimplifyCFG(SILFunction &Fn, SILFunctionTransform &T, bool Verify,
              bool EnableJumpThread)
      : FuncBuilder(T), Fn(Fn), transform(T), PM(T.getPassManager()),
        ConstFolder(FuncBuilder, PM->getOptions().AssertConfig,
                    /* EnableDiagnostics */ false,
                    [&](SILInstruction *I) { constFoldingCallback(I); }),
        ShouldVerify(Verify), EnableJumpThread(EnableJumpThread) {}

  bool run();

  bool simplifyBlockArgs();
  bool simplifyBlocks();
  bool canonicalizeSwitchEnums();
  bool simplifyThreadedTerminators();
  bool dominatorBasedSimplifications(SILFunction &Fn, DominanceInfo *DT);
  bool dominatorBasedSimplify(DominanceAnalysis *DA);
  bool threadEdge(const ThreadInfo &ti);

  /// Remove the basic block if it has no predecessors. Returns true
  /// If the block was removed.
  bool removeIfDead(SILBasicBlock *BB);

  bool tryJumpThreading(BranchInst *BI);
  bool tailDuplicateObjCMethodCallSuccessorBlocks();
  bool simplifyAfterDroppingPredecessor(SILBasicBlock *BB);

  bool simplifyBranchOperands(OperandValueArrayRef Operands);
  bool simplifyBranchBlock(BranchInst *BI);
  bool simplifyCondBrBlock(CondBranchInst *BI);
  bool simplifyCheckedCastBranchBlock(CheckedCastBranchInst *CCBI);
  bool simplifyCheckedCastAddrBranchBlock(CheckedCastAddrBranchInst *CCABI);
  bool simplifyTryApplyBlock(TryApplyInst *TAI);
  bool simplifySwitchValueBlock(SwitchValueInst *SVI);
  bool simplifyTermWithIdenticalDestBlocks(SILBasicBlock *BB);
  bool simplifySwitchEnumUnreachableBlocks(SwitchEnumInst *SEI);
  bool simplifySwitchEnumBlock(SwitchEnumInst *SEI);
  bool simplifyUnreachableBlock(UnreachableInst *UI);
  bool simplifyProgramTerminationBlock(SILBasicBlock *BB);
  bool simplifyArgument(SILBasicBlock *BB, unsigned i);
  bool simplifyArgs(SILBasicBlock *BB);
  bool simplifySwitchEnumOnObjcClassOptional(SwitchEnumInst *SEI);

private:
  // Called when \p newBlock inherits the former predecessors of \p
  // oldBlock. e.g. if \p oldBlock was a loop header, then newBlock is now a
  // loop header.
  void substitutedBlockPreds(SILBasicBlock *oldBlock, SILBasicBlock *newBlock) {
    if (LoopHeaders.count(oldBlock))
      LoopHeaders.insert(newBlock);
    if (ClonedLoopHeaders.count(oldBlock))
      ClonedLoopHeaders.insert(newBlock);
  }

  void clearWorklist() {
    WorklistMap.clear();
    WorklistList.clear();
  }

  /// popWorklist - Return the next basic block to look at, or null if the
  /// worklist is empty.  This handles skipping over null entries in the
  /// worklist.
  SILBasicBlock *popWorklist() {
    while (!WorklistList.empty())
      if (auto *BB = WorklistList.pop_back_val()) {
        WorklistMap.erase(BB);
        return BB;
      }

    return nullptr;
  }

  /// addToWorklist - Add the specified block to the work list if it isn't
  /// already present.
  void addToWorklist(SILBasicBlock *BB) {
    unsigned &Entry = WorklistMap[BB];
    if (Entry != 0)
      return;
    WorklistList.push_back(BB);
    Entry = WorklistList.size();
  }

  /// removeFromWorklist - Remove the specified block from the worklist if
  /// present.
  void removeFromWorklist(SILBasicBlock *BB) {
    assert(BB && "Cannot add null pointer to the worklist");
    auto It = WorklistMap.find(BB);
    if (It == WorklistMap.end())
      return;

    // If the BB is in the worklist, null out its entry.
    if (It->second) {
      assert(WorklistList[It->second - 1] == BB && "Consistency error");
      WorklistList[It->second - 1] = nullptr;
    }

    // Remove it from the map as well.
    WorklistMap.erase(It);

    if (LoopHeaders.count(BB)) {
      LoopHeaders.erase(BB);
      ClonedLoopHeaders.erase(BB);
    }
  }

  void findLoopHeaders();
  bool addToWorklistAfterSplittingEdges(SILBasicBlock *BB);
};

#endif
