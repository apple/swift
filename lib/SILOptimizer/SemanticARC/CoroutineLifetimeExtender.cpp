//===--- CoroutineLifetimeExtender.cpp ------------------------------------===//
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
///
/// \file
///
/// This pass contains a simple transform that extends the lifetime of a
/// coroutine with a trivial resume to transitive uses of copies of yielded
/// values from the coroutine and then eliminates those copies.
///
//===----------------------------------------------------------------------===//

#include "Context.h"
#include "OwnershipLiveRange.h"
#include "Transforms.h"

#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"

using namespace swift;
using namespace swift::semanticarc;

//===----------------------------------------------------------------------===//
//                        Process Candidate Functions
//===----------------------------------------------------------------------===//

static bool processCandidates(Context &ctx,
                              ArrayRef<FullApplySite> applySiteList) {
  assert(!applySiteList.empty() && "Must have at least one candidate");

  SmallVector<SILInstruction *, 32> scratchBuffer;
  SmallVector<SILInstruction *, 32> frontier;
  SmallVector<SILInstruction *, 32> destroyInsts;
  bool madeChange = false;

  // Now optimize each apply site that we can (converting to guaranteed) and
  // then after we are done, expand the lifetime of the coroutine over those
  // points.
  for (auto fas : applySiteList) {
    // Use RAII to ensure in the face of future code that we properly clear
    // scratchBuffer, frontier on every iteration.
    SWIFT_DEFER {
      scratchBuffer.clear();
      frontier.clear();
      destroyInsts.clear();
    };

    // NOTE: This needs to be rewritten to use FullApplySite methods if we add
    // additional coroutine call sites.
    auto *bai = cast<BeginApplyInst>(fas);
    bool didOptimizeCopy = false;

    // Now we know that we have a simple enough coroutine that we can process,
    // try to gather up copies that we /could/ eliminate if we lifetime extend.
    // Return true if we found something and false otherwise.
    for (SILValue yieldedValue : bai->getYieldedValues()) {
      if (yieldedValue.getOwnershipKind() != ValueOwnershipKind::Guaranteed) {
        continue;
      }

      bool result = visitGuaranteedValueUsesIgnoringForwarding(
          yieldedValue, [&](Operand *use) {
            auto *cvi = dyn_cast<CopyValueInst>(use->getUser());
            if (!cvi) {
              return true; // Continue processing.
            }

            OwnershipLiveRange lr(cvi);

            // For now if we have any unknown consuming uses bail. Eventually we
            // should try to handle phis here and should track this
            // info/post-process.
            if (bool(lr.hasUnknownConsumingUse())) {
              return true; // Continue processing.
            }

            llvm::copy(lr.getAllConsumingInsts(),
                       std::back_inserter(scratchBuffer));
            llvm::copy(lr.getDestroyingInsts(),
                       std::back_inserter(destroyInsts));

            // Now do a RAUW and convert forwarding insts to be guaranteed. The
            // destroys are still around since we need them as liveness points.
            cvi->replaceAllUsesWith(cvi->getOperand());
            ctx.instModCallbacks.deleteInst(cvi);
            std::move(lr).convertOwnedGeneralForwardingUsesToGuaranteed();
            didOptimizeCopy = true;
            madeChange = true;
            return true; // Continue processing.
          });
      (void)result;
      assert(result);
    }

    // If we did not optimize any actual copy, continue.
    if (!didOptimizeCopy) {
      continue;
    }

    // Otherwise, we know that we need to lifetime extend our coroutine over the
    // transitive closure consisting of the original coroutine end_applies and
    // the consuming uses of any copies that we eliminated. To do so, first add
    // to our scratchBuffer our end applies. We stash the original scratch
    // buffer size so we can just pop off the end_apply after we compute the
    // ValueLifetime of the coroutine and eliminate them without needing a
    // separate array.
    unsigned originalScratchBufferSize = scratchBuffer.size();
    SILValue tokenResult = bai->getTokenResult();

    for (auto *eai : tokenResult->getUsersOfType<EndApplyInst>()) {
      scratchBuffer.push_back(eai);
    }

    ValueLifetimeAnalysis analysis(bai, scratchBuffer);
    bool success = analysis.computeFrontier(
        frontier, ValueLifetimeAnalysis::Mode::DontModifyCFG,
        &ctx.getDeadEndBlocks());
    (void)success;
    assert(success);

    // Now that we have our frontier, delete off the end of our scratch buffer
    // the end_apply.
    while (scratchBuffer.size() != originalScratchBufferSize) {
      // We use cast<> here just as a sanity check.
      auto *eai = cast<EndApplyInst>(scratchBuffer.pop_back_val());
      ctx.instModCallbacks.deleteInst(eai);
    }
    // And then clear the rest of the scratch buffer to prepare for the next
    // iteration.
    scratchBuffer.clear();

    // Then add new end_apply along the new lifetime frontier of the coroutine.
    while (!frontier.empty()) {
      auto *insertionPoint = frontier.pop_back_val();
      SILBuilderWithScope builder(insertionPoint);
      auto *newInst =
          builder.createEndApply(insertionPoint->getLoc(), tokenResult);
      ctx.instModCallbacks.createdNewInst(newInst);
    }

    // And then delete all of the destroys.
    while (!destroyInsts.empty()) {
      ctx.instModCallbacks.deleteInst(destroyInsts.pop_back_val());
    }
  }

  return madeChange;
}

//===----------------------------------------------------------------------===//
//                             Gather Candidates
//===----------------------------------------------------------------------===//

// TODO: Make this stronger.
static bool isSafeInstruction(SILInstruction *i) {
  if (isa<EndBorrowInst>(i))
    return true;
  return !i->mayHaveSideEffects();
}

/// Returns true if \p fn is a coroutine such that both the yield and unwind
/// edges both contain only instructions without side-effects that immediately
/// end the function.
static bool isSimpleCoroutine(SILFunction *fn) {
  YieldInst *yi = nullptr;
  for (auto &block : *fn) {
    for (auto &inst : block) {
      if (auto *newYI = dyn_cast<YieldInst>(&inst)) {
        // Two yields!
        if (yi) {
          return false;
        }
        yi = newYI;
      }
    }
  }

  // No yields found!
  if (!yi) {
    return false;
  }

  // Ok, we found a single yield. Lets make sure that our unwind and resume
  // blocks do not have side-effects and are function exiting.
  //
  // TODO: Make this stronger.
  auto *resumeBlock = yi->getResumeBB();
  auto *unwindBlock = yi->getUnwindBB();

  if (!resumeBlock->getTerminator()->isFunctionExiting() ||
      !unwindBlock->getTerminator()->isFunctionExiting()) {
    return false;
  }

  for (auto &inst : *yi->getResumeBB()) {
    if (!isSafeInstruction(&inst)) {
      return false;
    }
  }

  for (auto &inst : *yi->getUnwindBB()) {
    if (!isSafeInstruction(&inst)) {
      return false;
    }
  }

  // Ok, we passed all of our tests! Return true!
  return true;
}

static bool
isEligibleApplySite(FullApplySite fas,
                    SmallVectorImpl<FullApplySite> &accumulatedCandidates) {
  // First check that we have a coroutine whose callee function has a single
  // block resume that only contains instructions without side-effects.
  if (!fas.beginsCoroutineEvaluation()) {
    return false;
  }

  auto *callee = fas.getCalleeFunction();
  if (!callee || !isSimpleCoroutine(callee)) {
    return false;
  }

  // We only consider beginApply that never abort for simplicity.
  //
  // NOTE: This is assumed at the end of the function by our usage of
  // getUsersOfType(). If this code is changed, we must change that other code
  // as well.
  //
  // TODO: Can we handle abort?
  auto *bai = cast<BeginApplyInst>(fas);
  SILValue tokenResult = bai->getTokenResult();
  for (auto *op : tokenResult->getUses()) {
    if (!isa<EndApplyInst>(op->getUser())) {
      return false;
    }
  }

  // Now we know that we have a simple enough coroutine that we can process, try
  // to gather up copies that we /could/ eliminate if we lifetime extend. Return
  // true if we found something and false otherwise.
  for (SILValue yieldedValue : bai->getYieldedValues()) {
    if (yieldedValue.getOwnershipKind() == ValueOwnershipKind::Guaranteed) {
      accumulatedCandidates.push_back(fas);
      return true;
    }
  }

  return false;
}

static bool
gatherCandidates(SILFunction *fn,
                 SmallVectorImpl<FullApplySite> &resultingCandidates) {
  bool foundCandidate = false;
  for (auto &block : *fn) {
    for (auto &inst : block) {
      if (auto fas = FullApplySite::isa(&inst)) {
        if (isEligibleApplySite(fas, resultingCandidates)) {
          foundCandidate = true;
        }
      }
    }
  }
  return foundCandidate;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

bool swift::semanticarc::
    tryEliminatingCopiesByLifetimeExtendingTrivialCoroutines(Context &ctx) {
  // We do not perform this optimization when running as a guarantee
  // optimization.
  if (ctx.onlyGuaranteedOpts) {
    return false;
  }

  // Gather up our candidate apply sites.
  SmallVector<FullApplySite, 32> candidates;
  if (!gatherCandidates(&ctx.fn, candidates)) {
    // If we didn't find any candidates, bail.
    return false;
  }

  // Then try to eliminate copies!
  return processCandidates(ctx, candidates);
}
