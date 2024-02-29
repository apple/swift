//===--- OwnedToGuaranteedPhiOpt.cpp --------------------------------------===//
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
/// Late optimization that eliminates webs of owned phi nodes.
///
//===----------------------------------------------------------------------===//

#include "Context.h"
#include "OwnershipPhiOperand.h"
#include "Transforms.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/STLExtras.h"
#include "swift/SILOptimizer/Utils/OwnershipOptUtils.h"

using namespace swift;
using namespace swift::semanticarc;

namespace {
using ConsumingOperandState = Context::ConsumingOperandState;
} // anonymous namespace

template <typename OperandRangeTy>
static bool canEliminatePhi(
    OperandRangeTy optimizableIntroducerRange,
    ArrayRef<OwnershipPhiOperand> incomingValueOperandList,
    SmallVectorImpl<OwnedValueIntroducer> &ownedValueIntroducerAccumulator) {
  for (auto incomingValueOperand : incomingValueOperandList) {
    SILValue incomingValue = incomingValueOperand.getValue();

    // Before we do anything, see if we have an incoming value with trivial
    // ownership. This can occur in the case where we are working with enums due
    // to trivial non-payloaded cases. Skip that.
    if (incomingValue->getOwnershipKind() == OwnershipKind::None) {
      continue;
    }

    // Then see if this is an introducer that we actually saw as able to be
    // optimized if we could flip this joined live range.
    //
    // NOTE: If this linear search is too slow, we can change the multimap to
    // sort the mapped to list by pointer instead of insertion order. In such a
    // case, we could then bisect.
    if (llvm::find(optimizableIntroducerRange,
                   incomingValueOperand.getOperand()) ==
        optimizableIntroducerRange.end()) {
      return false;
    }

    // Now that we know it is an owned value that we saw before, check for
    // introducers of the owned value which are the copies that we may be able
    // to eliminate. Since we do not look through joined live ranges, we must
    // only have a single introducer. So look for that one and if not, bail.
    auto singleIntroducer = getSingleOwnedValueIntroducer(incomingValue);
    if (!singleIntroducer) {
      return false;
    }

    // Then make sure that our owned value introducer is able to be converted to
    // guaranteed and that we found it to have a LiveRange that we could have
    // eliminated /if/ we were to get rid of this phi.
    if (!singleIntroducer.isConvertableToGuaranteed()) {
      return false;
    }

    // Otherwise, add the introducer to our result array.
    ownedValueIntroducerAccumulator.push_back(singleIntroducer);
  }

#ifndef NDEBUG
  // Other parts of the pass ensure that we only add values to the list if their
  // owned value introducer is not used by multiple live ranges. That being
  // said, lets assert that.
  {
    SmallVector<OwnedValueIntroducer, 32> uniqueCheck;
    llvm::copy(ownedValueIntroducerAccumulator,
               std::back_inserter(uniqueCheck));
    sortUnique(uniqueCheck);
    assert(
        uniqueCheck.size() == ownedValueIntroducerAccumulator.size() &&
        "multiple joined live range operands are from the same live range?!");
  }
#endif

  return true;
}

static bool getIncomingJoinedLiveRangeOperands(
    SILValue joinedLiveRange,
    SmallVectorImpl<OwnershipPhiOperand> &resultingOperands) {
  if (auto *phi = dyn_cast<SILPhiArgument>(joinedLiveRange)) {
    return phi->visitIncomingPhiOperands([&](Operand *op) {
      if (auto phiOp = OwnershipPhiOperand::get(op)) {
        resultingOperands.push_back(*phiOp);
        return true;
      }
      return false;
    });
  }

  if (auto *svi = dyn_cast<SingleValueInstruction>(joinedLiveRange)) {
    return llvm::all_of(svi->getAllOperands(), [&](const Operand &op) {
      // skip type dependent operands.
      if (op.isTypeDependent())
        return true;

      auto phiOp = OwnershipPhiOperand::get(&op);
      if (!phiOp)
        return false;
      resultingOperands.push_back(*phiOp);
      return true;
    });
  }

  llvm_unreachable("Unhandled joined live range?!");
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

// This needs `SemanticARCOptVisitor::performGuaranteedCopyValueOptimization` to
// run before so that joinedOwnedIntroducerToConsumedOperands is populated.
bool swift::semanticarc::tryConvertOwnedPhisToGuaranteedPhis(Context &ctx) {
  bool madeChange = false;

  // First freeze our multi-map so we can use it for map queries. Also, setup a
  // defer of the reset so we do not forget to reset the map when we are done.
  ctx.joinedOwnedIntroducerToConsumedOperands.setFrozen();
  SWIFT_DEFER { ctx.joinedOwnedIntroducerToConsumedOperands.reset(); };

  // Now for each phi argument that we have in our multi-map...
  SmallVector<OwnershipPhiOperand, 4> incomingValueOperandList;
  SmallVector<OwnedValueIntroducer, 4> ownedValueIntroducers;
  for (auto pair : ctx.joinedOwnedIntroducerToConsumedOperands.getRange()) {
    SWIFT_DEFER {
      incomingValueOperandList.clear();
      ownedValueIntroducers.clear();
    };

    // First compute the LiveRange for ownershipPhi value. For simplicity, we
    // only handle cases now where the result does not have any additional
    // ownershipPhi uses.
    SILValue joinedIntroducer = pair.first;
    OwnershipLiveRange joinedLiveRange(joinedIntroducer);
    if (bool(joinedLiveRange.hasUnknownConsumingUse())) {
      continue;
    }

    // Ok, we know that our phi argument /could/ be converted to guaranteed if
    // our incoming values are able to be converted to guaranteed. Now for each
    // incoming value, compute the incoming values ownership roots and see if
    // all of the ownership roots are in our owned incoming value array.
    if (!getIncomingJoinedLiveRangeOperands(joinedIntroducer,
                                            incomingValueOperandList)) {
      continue;
    }

    // Grab our list of introducer values paired with this SILArgument. See if
    // all of these introducer values were ones that /could/ have been
    // eliminated if it was not for the given phi. If all of them are, we can
    // optimize!
    {
      std::function<Operand *(const Context::ConsumingOperandState)> lambda =
        [&](const Context::ConsumingOperandState &state) -> Operand * {
            unsigned opNum = state.operandNumber;
            if (state.parent.is<SILBasicBlock *>()) {
              SILBasicBlock *block = state.parent.get<SILBasicBlock *>();
              return &block->getTerminator()->getAllOperands()[opNum];
            }
            SILInstruction *inst = state.parent.get<SILInstruction *>();
            return &inst->getAllOperands()[opNum];
        };
      auto operandsTransformed = makeTransformRange(pair.second, lambda);
      if (!canEliminatePhi(operandsTransformed, incomingValueOperandList,
                           ownedValueIntroducers)) {
        continue;
      }
    }

    // Ok, at this point we know that we can eliminate this phi. First go
    // through the list of incomingValueOperandList and stash the value/set the
    // operand's stored value to undef. We will hook them back up later.
    SmallVector<SILValue, 8> originalIncomingValues;
    for (auto &incomingValueOperand : incomingValueOperandList) {
      originalIncomingValues.push_back(incomingValueOperand.getValue());
      incomingValueOperand.markUndef();
    }

    // Then go through all of our owned value introducers, compute their live
    // ranges, and eliminate them. We know it is safe to remove them from our
    // previous proofs.
    //
    // NOTE: If our introducer is a copy_value that is one of our
    // originalIncomingValues, we need to update the originalIncomingValue array
    // with that value since we are going to delete the copy_value here. This
    // creates a complication since we want to binary_search on
    // originalIncomingValues to detect this same condition! So, we create a
    // list of updates that we apply after we no longer need to perform
    // binary_search, but before we start RAUWing things.
    SmallVector<std::pair<SILValue, unsigned>, 8> incomingValueUpdates;
    for (auto introducer : ownedValueIntroducers) {
      SILValue v = introducer.value;
      OwnershipLiveRange lr(v);

      // For now, we only handle copy_value for simplicity.
      //
      // TODO: Add support for load [copy].
      if (introducer.kind == OwnedValueIntroducerKind::Copy) {
        auto *cvi = cast<CopyValueInst>(v);
        // Before we convert from owned to guaranteed, we need to first see if
        // cvi is one of our originalIncomingValues. If so, we need to set
        // originalIncomingValues to be cvi->getOperand(). Otherwise, weirdness
        // results since we are deleting one of our stashed values.
        auto iter = find(originalIncomingValues, cvi);
        if (iter != originalIncomingValues.end()) {
          // We use an auxillary array here so we can continue to bisect on
          // original incoming values. Once we are done processing here, we will
          // not need that property anymore.
          unsigned updateOffset =
              std::distance(originalIncomingValues.begin(), iter);
          incomingValueUpdates.emplace_back(cvi->getOperand(), updateOffset);
        }
        std::move(lr).convertToGuaranteedAndRAUW(cvi->getOperand(),
                                                 ctx.instModCallbacks);
        continue;
      }
      llvm_unreachable("Unhandled optimizable introducer!");
    }

    // Now go through and update our original incoming value array now that we
    // do not need it to be sorted for bisection purposes.
    while (!incomingValueUpdates.empty()) {
      auto pair = incomingValueUpdates.pop_back_val();
      originalIncomingValues[pair.second] = pair.first;
    }

    // Now if our phi operand consumes/forwards its guaranteed input, insert a
    // begin_borrow along the incoming value edges. We have to do this after
    // converting the incoming values to be guaranteed to avoid tripping
    // SILBuilder checks around simple ownership invariants (namely that def/use
    // line up) when creating instructions.
    assert(incomingValueOperandList.size() == originalIncomingValues.size());
    while (!incomingValueOperandList.empty()) {
      auto incomingValueOperand = incomingValueOperandList.pop_back_val();
      SILValue originalValue = originalIncomingValues.pop_back_val();
      incomingValueOperand.getOperand()->set(originalValue);
    }

    // Then convert the phi's live range to be guaranteed.
    std::move(joinedLiveRange)
        .convertJoinedLiveRangePhiToGuaranteed(
            ctx.getDeadEndBlocks(), ctx.lifetimeFrontier, ctx.instModCallbacks);

    madeChange = true;
    ctx.verify();
  }

  if (madeChange)
    updateBorrowedFrom(ctx.pm, &ctx.fn);

  return madeChange;
}
