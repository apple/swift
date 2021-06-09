//===--- MergeCondFail.cpp -  Merge cond_fail instructions ----------------===//
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

#define DEBUG_TYPE "merge-cond_fail"

#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"

#include "llvm/Support/Debug.h"

using namespace swift;

/// Return true if the operand of the cond_fail instruction looks like
/// the overflow bit of an arithmetic instruction.
static bool hasOverflowConditionOperand(CondFailInst *CFI) {
  if (auto *TEI = dyn_cast<TupleExtractInst>(CFI->getOperand()))
    if (isa<BuiltinInst>(TEI->getOperand()))
      return true;
  return false;
}

namespace {
/// Merge cond_fail instructions.
///
/// We can merge cond_fail instructions if there is no side-effect or memory
/// write in between them.
/// This pass merges cond_fail instructions by building the disjunction of
/// their operands.
class MergeCondFailInsts : public SILFunctionTransform {
public:
  MergeCondFailInsts() {}

  void run() override {
    bool Changed = false;
    auto *F = getFunction();
    // Merge cond_fail instructions if there is no side-effect or read in
    // between them.
    for (auto &BB : *F) {
      // Per basic block list of cond_fails to merge.
      SmallVector<CondFailInst *, 16> CondFailToMerge;
      for (auto InstIt = BB.begin(), End = BB.end(); InstIt != End;) {
        auto *CurInst = &*InstIt;
        ++InstIt;
        auto *CFI = dyn_cast<CondFailInst>(CurInst);

        // Stop merging at side-effects or reads from memory.
        if (!CFI && (CurInst->mayHaveSideEffects() ||
                     CurInst->mayReadFromMemory())) {
          // Merge cond_fail.
          if (CondFailToMerge.size() > 1) {
            Changed |= mergeCondFails(CondFailToMerge);
            CondFailToMerge.clear();
            continue;
          }
        }

        // Do not process arithmetic overflow checks. We typically generate more
        // efficient code with separate jump-on-overflow.
        if (CFI && !hasOverflowConditionOperand(CFI) &&
            (CondFailToMerge.empty() ||
             CFI->getMessage() == CondFailToMerge.front()->getMessage()))
          CondFailToMerge.push_back(CFI);

      }
      // Process any remaining cond_fail instructions in the current basic
      // block.
      if (CondFailToMerge.size() > 1)
        Changed |= mergeCondFails(CondFailToMerge);
    }
    
    if (Changed) {
      PM->invalidateAnalysis(F, SILAnalysis::InvalidationKind::Instructions);
    }
  }

  /// Try to merge the cond_fail instructions. Returns true if any could
  /// be merge.
  bool mergeCondFails(SmallVectorImpl<CondFailInst *> &CondFailToMerge) {
    assert(CondFailToMerge.size() > 1 &&
           "Need at least two cond_fail instructions");

    if (CondFailToMerge.size() < 2)
      return false;

    SILValue MergedCond;
    auto *LastCFI = CondFailToMerge.back();
    auto InsertPt = ++SILBasicBlock::iterator(LastCFI);
    SILBuilderWithScope Builder(InsertPt);
    SILLocation Loc = LastCFI->getLoc();

    // Merge conditions and remove the merged cond_fail instructions.
    for (unsigned I = 0, E = CondFailToMerge.size(); I != E; ++I) {
      auto CurCond = CondFailToMerge[I]->getOperand();
      if (MergedCond) {
        CurCond = Builder.createBuiltinBinaryFunction(Loc, "or",
                                                      CurCond->getType(),
                                                      CurCond->getType(),
                                                      {MergedCond, CurCond});
      }

      MergedCond = CurCond;
    }

    // Create a new cond_fail using the merged condition.
    Builder.createCondFail(Loc, MergedCond, LastCFI->getMessage());

    for (CondFailInst *CFI : CondFailToMerge) {
      CFI->eraseFromParent();
    }
    return true;
  }
};
} // end anonymous namespace

SILTransform *swift::createLegacyMergeCondFails() {
  return new MergeCondFailInsts();
}
