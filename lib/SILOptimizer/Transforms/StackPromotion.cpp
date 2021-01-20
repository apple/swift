//===--- StackPromotion.cpp - Promotes allocations to the stack -----------===//
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
#include "swift/SIL/CFG.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/Analysis/EscapeAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/StackNesting.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"
#include "llvm/ADT/Statistic.h"

#define DEBUG_TYPE "stack-promotion"

STATISTIC(NumStackPromoted, "Number of objects promoted to the stack");

using namespace swift;

namespace {

/// Promotes heap allocated objects to the stack.
///
/// It handles alloc_ref instructions of native swift classes: if promoted,
/// the [stack] attribute is set in the alloc_ref and a dealloc_ref [stack] is
/// inserted at the end of the object's lifetime.
class StackPromotion : public SILFunctionTransform {

public:
  StackPromotion() {}

private:
  /// The entry point to the transformation.
  void run() override;

  /// Promotes allocations in \p BB.
  bool promoteInBlock(SILBasicBlock *BB, EscapeAnalysis *EA,
                      DeadEndBlocks &DEBlocks);

  /// Tries to promote the allocation \p ARI.
  bool tryPromoteAlloc(AllocRefInst *ARI, EscapeAnalysis *EA,
                       DeadEndBlocks &DEBlocks);
};

void StackPromotion::run() {
  SILFunction *F = getFunction();
  // FIXME: We should be able to support ownership.
  if (F->hasOwnership())
    return;

  LLVM_DEBUG(llvm::dbgs() << "** StackPromotion in " << F->getName() << " **\n");

  auto *EA = getAnalysis<EscapeAnalysis>();
  auto *DEBA = getAnalysis<DeadEndBlocksAnalysis>();
  auto &DEBlocks = *DEBA->get(F);
  bool Changed = false;

  // Search the whole function for stack promotable allocations.
  for (SILBasicBlock &BB : *F) {
    Changed |= promoteInBlock(&BB, EA, DEBlocks);
  }
  if (!Changed)
    return;

  // Make sure that all stack allocating instructions are nested correctly.
  StackNesting SN;
  if (SN.correctStackNesting(F) == StackNesting::Changes::CFG) {
    invalidateAnalysis(SILAnalysis::InvalidationKind::BranchesAndInstructions);
  } else {
    invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
}

bool StackPromotion::promoteInBlock(SILBasicBlock *BB, EscapeAnalysis *EA,
                                    DeadEndBlocks &DEBlocks) {
  bool Changed = false;
  for (auto Iter = BB->begin(); Iter != BB->end();) {
    // The allocation instruction may be moved, so increment Iter prior to
    // doing the optimization.
    SILInstruction *I = &*Iter++;
    if (auto *ARI = dyn_cast<AllocRefInst>(I)) {
      // Don't stack promote any allocation inside a code region which ends up
      // in a no-return block. Such allocations may missing their final release.
      // We would insert the deallocation too early, which may result in a
      // use-after-free problem.
      if (DEBlocks.isDeadEnd(BB))
        return false;

      Changed |= tryPromoteAlloc(ARI, EA, DEBlocks);
    }
  }
  return Changed;
}

bool StackPromotion::tryPromoteAlloc(AllocRefInst *ARI, EscapeAnalysis *EA,
                                     DeadEndBlocks &DEBlocks) {
  if (ARI->isObjC() || ARI->canAllocOnStack())
    return false;

  auto *ConGraph = EA->getConnectionGraph(ARI->getFunction());
  auto *contentNode = ConGraph->getValueContent(ARI);
  if (!contentNode)
    return false;

  // The most important check: does the object escape the current function?
  if (contentNode->escapes())
    return false;

  LLVM_DEBUG(llvm::dbgs() << "Promote " << *ARI);

  // Collect all use-points of the allocation. These are refcount instructions
  // and apply instructions.
  llvm::SmallVector<SILInstruction *, 8> UsePoints;
  ConGraph->getUsePoints(contentNode, UsePoints);

  ValueLifetimeAnalysis VLA(ARI, UsePoints);
  // Check if there is a use point before the allocation (this can happen e.g.
  // if the allocated object is stored into another object, which is already
  // alive at the allocation point).
  // In such a case the value lifetime extends up to the function entry.
  if (VLA.isAliveAtBeginOfBlock(ARI->getFunction()->getEntryBlock())) {
    LLVM_DEBUG(llvm::dbgs() << "  use before allocation -> don't promote");
    return false;
  }

  // Compute the places where the lifetime of the object ends.
  ValueLifetimeAnalysis::Frontier Frontier;
  if (!VLA.computeFrontier(Frontier, ValueLifetimeAnalysis::UsersMustPostDomDef,
                           &DEBlocks)) {
    LLVM_DEBUG(llvm::dbgs() << "  uses don't post-dom allocation -> don't promote");
    return false;
  }
  ++NumStackPromoted;

  // We set the [stack] attribute in the alloc_ref.
  ARI->setStackAllocatable();

  /// And create dealloc_ref [stack] at the end of the object's lifetime.
  for (SILInstruction *FrontierInst : Frontier) {
    SILBuilder B(FrontierInst);
    B.createDeallocRef(ARI->getLoc(), ARI, true);
  }
  return true;
}

} // end anonymous namespace

SILTransform *swift::createStackPromotion() {
  return new StackPromotion();
}
