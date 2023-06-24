//===--- CopyValueOpts.cpp ------------------------------------------------===//
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
/// Contains optimizations that eliminate redundant copy values.
///
/// FIXME: CanonicalizeOSSALifetime likely replaces everything this file.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-semantic-arc-opts"

#include "OwnershipPhiOperand.h"
#include "SemanticARCOptVisitor.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/LinearLifetimeChecker.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/Projection.h"

using namespace swift;
using namespace swift::semanticarc;

//===----------------------------------------------------------------------===//
//                     Guaranteed Copy Value Optimization
//===----------------------------------------------------------------------===//

// Eliminate a copy of a borrowed value, if:
//
// 1. All of the copies users do not consume the copy (and thus can accept a
//    borrowed value instead).
// 2. The copies's non-destroy_value users are strictly contained within the
//    scope of the borrowed value.
//
// Example:
//
//   %0 = @guaranteed (argument or instruction)
//   %1 = copy_value %0
//   apply %f(%1) : $@convention(thin) (@guaranteed ...) ...
//   other_non_consuming_use %1
//   destroy_value %1
//   end_borrow %0 (if an instruction)
//
// =>
//
//   %0 = @guaranteed (argument or instruction)
//   apply %f(%0) : $@convention(thin) (@guaranteed ...) ...
//   other_non_consuming_use %0
//   end_borrow %0 (if an instruction)
//
// NOTE: This means that the destroy_value technically can be after the
// end_borrow. In practice, this will not be the case but we use this to avoid
// having to reason about the ordering of the end_borrow and destroy_value.
//
// NOTE: Today we only perform this for guaranteed parameters since this enables
// us to avoid doing the linear lifetime check to make sure that all destroys
// are within the borrow scope.
//
// TODO: This needs a better name.
bool SemanticARCOptVisitor::performGuaranteedCopyValueOptimization(
    CopyValueInst *cvi) {
  // All mandatory copy optimization is handled by CanonicalizeOSSALifetime,
  // which knows how to preserve lifetimes for debugging.
  if (ctx.onlyMandatoryOpts)
    return false;

  SmallVector<BorrowedValue, 4> borrowScopeIntroducers;

  // Find all borrow introducers for our copy operand. If we are unable to find
  // all of the introducers (due to pattern matching failure), conservatively
  // return false. We can not optimize.
  //
  // NOTE: We can get multiple introducers if our copy_value's operand
  // value runs through a phi or an aggregate forming instruction.
  if (!getAllBorrowIntroducingValues(cvi->getOperand(), borrowScopeIntroducers))
    return false;

  // Then go over all of our uses and see if the value returned by our copy
  // value forms a dead live range or a live range that would be dead if it was
  // not consumed by phi nodes. If we do not have such a live range, there must
  // be some consuming use that we either do not understand is /actually/
  // forwarding or a user that truly represents a necessary consume of the value
  // (e.x. storing into memory).
  OwnershipLiveRange lr(cvi);
  auto hasUnknownConsumingUseState =
      lr.hasUnknownConsumingUse(ctx.assumingAtFixedPoint);
  if (hasUnknownConsumingUseState ==
      OwnershipLiveRange::HasConsumingUse_t::Yes) {
    return false;
  }

  // Next check if we do not have any destroys of our copy_value and are
  // processing a local borrow scope. In such a case, due to the way we ignore
  // dead end blocks, we may eliminate the copy_value, creating a use of the
  // borrowed value after the end_borrow. To avoid this, in such cases we
  // bail. In contrast, a non-local borrow scope does not have any end scope
  // instructions, implying we can avoid this hazard and still optimize in such
  // a case.
  //
  // DISCUSSION: Consider the following SIL:
  //
  // ```
  //   %1 = begin_borrow %0 : $KlassPair                            (1)
  //   %2 = struct_extract %1 : $KlassPair, #KlassPair.firstKlass
  //   %3 = copy_value %2 : $Klass
  //   ...
  //   end_borrow %1 : $LintCommand                                 (2)
  //   cond_br ..., bb1, bb2
  //
  //   ...
  //
  //   bbN:
  //     // Never return type implies dead end block.
  //     apply %f(%3) : $@convention(thin) (@guaranteed Klass) -> Never (3)
  //     unreachable
  // ```
  //
  // For simplicity, note that if bbN post-dominates %3, given that when we
  // compute linear lifetime errors we ignore dead end blocks, we would not
  // register that the copy_values only use is outside of the begin_borrow
  // region defined by (1), (2) and thus would eliminate the copy. This would
  // result in %2 being used by %f, causing the linear lifetime checker to
  // error.
  //
  // Naively one may assume that the solution to this is to just check if %3 has
  // /any/ destroy_values at all and if it doesn't have any reachable
  // destroy_values, then we are in this case. But is this correct in
  // general. We prove this below:
  //
  // The only paths along which the copy_value can not be destroyed or consumed
  // is along paths to dead end blocks. Trivially, we know that such a dead end
  // block, can not be reachable from the end_borrow since by their nature dead
  // end blocks end in unreachables.
  //
  // So we know that we can only run into this bug if we have a dead end block
  // reachable from the end_borrow, meaning that the bug can not occur if we
  // branch before the end_borrow since in that case, the borrow scope would
  // last over the dead end block's no return meaning that we will not use the
  // borrowed value after its lifetime is ended by the end_borrow.
  //
  // With that in hand, we note again that if we have exactly one consumed,
  // destroy_value /after/ the end_borrow we will not optimize here. This means
  // that this bug can only occur if the copy_value is only post-dominated by
  // dead end blocks that use the value in a non-consuming way.
  //
  // TODO: There may be some way of sinking this into the loop below.
  //
  // FIXME: The haveAnyLocalScopes and destroy.empty() checks are relics of
  // attempts to handle dead end blocks during areUsesWithinExtendedScope. If
  // we don't use dead end blocks at all, they should not be relevant.
  bool haveAnyLocalScopes =
      llvm::any_of(borrowScopeIntroducers, [](BorrowedValue borrowScope) {
        return borrowScope.isLocalScope();
      });

  auto destroys = lr.getDestroyingUses();
  if (destroys.empty() && haveAnyLocalScopes) {
    return false;
  }

  // If we reached this point, then we know that all of our users can accept a
  // guaranteed value and our owned value is destroyed only by a set of
  // destroy_values. Check if:
  //
  // 1. All of our destroys are joint post-dominated by our end borrow scope
  //    set. If they are not, then the copy_value is lifetime extending the
  //    guaranteed value, we can not eliminate it.
  //
  // 2. If all of our destroy_values are dead end. In such a case, the linear
  //    lifetime checker will not perform any checks since it assumes that dead
  //    end destroys can be ignored. Since we are going to end the program
  //    anyways, we want to be conservative here and optimize only if we do not
  //    need to insert an end_borrow since all of our borrow introducers are
  //    non-local scopes.
  //
  // The call to areUsesWithinExtendedScope cannot consider dead-end blocks. A
  // local borrow scope requires all its inner uses to be inside the borrow
  // scope, regardless of whether the end of the scope is inside a dead-end
  // block.
  {
    if (llvm::any_of(borrowScopeIntroducers, [&](BorrowedValue borrowScope) {
          return !borrowScope.areUsesWithinExtendedScope(
              lr.getAllConsumingUses(), nullptr);
        })) {
      return false;
    }
  }

  // Otherwise, we know that our copy_value/destroy_values are all completely
  // within the guaranteed value scope. So we /could/ optimize it. Now check if
  // we were truly dead or if we are dead if we can eliminate phi arg uses. If
  // we need to handle the phi arg uses, we bail. After we reach a fixed point,
  // we will try to eliminate this value then if we can find a complete set of
  // all incoming values to our phi argument.
  if (hasUnknownConsumingUseState ==
      OwnershipLiveRange::HasConsumingUse_t::YesButAllPhiArgs) {
    auto opPhi = *OwnershipPhiOperand::get(lr.getSingleUnknownConsumingUse());
    SmallVector<Operand *, 8> scratchSpace;

    bool canOptimizePhi = opPhi.visitResults([&](SILValue value) {
      SWIFT_DEFER {
        scratchSpace.clear();
      };

      OwnershipLiveRange phiArgLR(value);
      if (bool(phiArgLR.hasUnknownConsumingUse())) {
        return false;
      }

      // Replacing owned phi operands with a local borrow introducer can
      // introduce reborrows. Since lifetime adjustment is not implemented for
      // this case, disable here.
      // Returning false here will make sure so this isn't populated in
      // joinedOwnedIntroducerToConsumedOperands which is used by
      // semanticarc::tryConvertOwnedPhisToGuaranteedPhis for transforming owned
      // phi to guaranteed.
      if (haveAnyLocalScopes) {
        return false;
      }

      if (llvm::any_of(borrowScopeIntroducers, [&](BorrowedValue borrowScope) {
            return !borrowScope.areUsesWithinExtendedScope(
                phiArgLR.getAllConsumingUses(), nullptr);
          })) {
        return false;
      }

      return true;
    });

    if (canOptimizePhi) {
      Context::ConsumingOperandState state(opPhi);
      opPhi.visitResults([&](SILValue value) {
        ctx.joinedOwnedIntroducerToConsumedOperands.insert(value, state);
        return true;
      });
    }

    return false;
  }

  // Otherwise, our copy must truly not be needed, o RAUW and convert to
  // guaranteed!
  LLVM_DEBUG(llvm::dbgs() << "Replace copy with guaranteed source: " << *cvi);
  std::move(lr).convertToGuaranteedAndRAUW(cvi->getOperand(), getCallbacks());
  return true;
}

//===----------------------------------------------------------------------===//
//                       Trivial Live Range Elimination
//===----------------------------------------------------------------------===//

/// If cvi only has destroy value users, then cvi is a dead live range. Lets
/// eliminate all such dead live ranges.
///
/// FIXME: CanonicalizeOSSALifetime replaces this.
bool SemanticARCOptVisitor::eliminateDeadLiveRangeCopyValue(
    CopyValueInst *cvi) {
  // This is a cheap optimization generally.

  // See if we are lucky and have a simple case.
  if (auto *op = cvi->getSingleUse()) {
    if (auto *dvi = dyn_cast<DestroyValueInst>(op->getUser())) {
      LLVM_DEBUG(llvm::dbgs() << "Erasing single-use copy: " << *cvi);
      eraseInstruction(dvi);
      eraseInstructionAndAddOperandsToWorklist(cvi);
      return true;
    }
  }

  // If all of our copy_value users are destroy_value, zap all of the
  // instructions. We begin by performing that check and gathering up our
  // destroy_value.
  SmallVector<DestroyValueInst *, 16> destroys;
  if (!all_of(cvi->getUses(), [&](Operand *op) {
        auto *dvi = dyn_cast<DestroyValueInst>(op->getUser());
        if (!dvi)
          return false;

        // Stash dvi in destroys so we can easily eliminate it later.
        destroys.push_back(dvi);
        return true;
      })) {
    return false;
  }

  // Now that we have a truly dead live range copy value, eliminate it!
  LLVM_DEBUG(llvm::dbgs() << "Eliminate dead copy: " << *cvi);
  while (!destroys.empty()) {
    eraseInstruction(destroys.pop_back_val());
  }
  eraseInstructionAndAddOperandsToWorklist(cvi);
  return true;
}

//===----------------------------------------------------------------------===//
//                             Live Range Joining
//===----------------------------------------------------------------------===//

/// Given that our copy_value and destroy_value are in different blocks
/// determine if we can eliminate the copy/destroy.
///
/// We assume that our copy_value \p cvi has a single consuming use
/// (\p cviConsumingUse) and that the destroy_value \p cviOperandDestroy is the
/// only destroy of the copy_value's operand.
static bool canJoinIfCopyDiesInFunctionExitingBlock(
    SILValue cviOperand, DestroyValueInst *cviOperandDestroy,
    CopyValueInst *cvi, Operand *cviConsumingUse) {
  // This is a simple optimization, so at least handle hand-offs at returns.
  //
  // First if our copy_value's consuming use is a return inst, then we know that
  // the copy_value is live over the destroy_value \p cviOperandDestroy so we
  // can eliminate the two safely.
  auto cviConsumerIter = cviConsumingUse->getUser()->getIterator();
  if (isa<ReturnInst>(cviConsumerIter)) {
    return true;
  }

  // Then see if our cviConsumer is in the same block as a return inst and the
  // destroy_value is not. In that case, we know that the cviConsumer must
  // post-dominate the destroy_value.
  auto *cviConsumingBlock = cviConsumerIter->getParent();
  if (isa<ReturnInst>(cviConsumingBlock->getTerminator()) &&
      cviConsumingBlock != cviOperandDestroy->getParent()) {
    return true;
  }

  return false;
}

static Operand *lookThroughSingleForwardingUse(Operand *use) {
  ForwardingOperand forwardingOperand(use);
  if (!forwardingOperand)
    return nullptr;
  auto forwardedValue = forwardingOperand.getSingleForwardedValue();
  if (!forwardedValue)
    return nullptr;
  auto *singleConsumingUse = forwardedValue->getSingleConsumingUse();
  if (!singleConsumingUse)
    return nullptr;
  return singleConsumingUse;
}

/// Walk from inst to the end of the inst->getParent() looking for \p use's
/// user. Every instruction that we visit that is not said user is added to
/// foundInsts if foundInsts is not nullptr. We do not include \p inst in \p
/// foundInsts.
static bool isUseBetweenInstAndBlockEnd(
    SILInstruction *inst, Operand *use,
    SmallPtrSetImpl<SILInstruction *> *foundInsts = nullptr) {
  auto userOfUse = use->getUser();
  auto instRegion = llvm::make_range(std::next(inst->getIterator()),
                                     inst->getParent()->end());
  for (auto &i : instRegion) {
    if (&i == userOfUse)
      return true;
    if (foundInsts)
      foundInsts->insert(&i);
  }
  return false;
}

/// Optimize assuming that \p singleCVIConsumingUse and \p dvi are in the same
/// block.
///
/// Importantly since \p singleCVIConsumingUse and \p dvi are in the same block,
/// we know that \p cvi must be post-dominated by dvi since its only consuming
/// use is single cvi consuming use by assumption.
static bool tryJoinIfDestroyConsumingUseInSameBlock(
    SemanticARCOptVisitor &ctx, CopyValueInst *cvi, DestroyValueInst *dvi,
    SILValue operand, Operand *singleCVIConsumingUse) {
  // First see if our destroy_value is in between singleCVIConsumingUse and the
  // end of block. If this is not true, then we know the destroy_value must be
  // /before/ our singleCVIConsumingUse meaning that by joining the lifetimes,
  // we are not going to shrink the overall composite lifetime.
  SmallPtrSet<SILInstruction *, 8> visitedInsts;
  if (!isUseBetweenInstAndBlockEnd(singleCVIConsumingUse->getUser(),
                                   &dvi->getAllOperands()[0], &visitedInsts)) {
    LLVM_DEBUG(llvm::dbgs()
               << "Eliminate copy with useless lifetime: " << *cvi);
    ctx.eraseInstruction(dvi);
    ctx.eraseAndRAUWSingleValueInstruction(cvi, operand);
    return true;
  }

  // The lifetime of the original ends after the lifetime of the copy. If the
  // original is lexical, its lifetime must not be shortened through deinit
  // barriers.
  if (cvi->getOperand()->isLexical()) {
    // At this point, visitedInsts contains all the instructions between the
    // consuming use of the copy and the destroy.  If any of those instructions
    // is a deinit barrier, it would be illegal to shorten the original lexical
    // value's lifetime to end at that consuming use.  Bail if any are.
    if (llvm::any_of(visitedInsts, [](auto *inst) {
          return mayBeDeinitBarrierNotConsideringSideEffects(inst);
        }))
      return false;
  }

  // If we reached this point, isUseBetweenInstAndBlockEnd succeeded implying
  // that we found destroy_value to be after our consuming use. Noting that
  // additionally, the routine places all instructions in between consuming use
  // and destroy_value into visitedInsts for our use, we may still be able to
  // optimize if:
  //
  // 1. singleCVIConsumingUse is actually a forwarding user and forms the head
  //    of a chain of same-block forwarding uses the last of which is /after/
  //    the destroy_value.
  //
  // 2. Our copy_value's operand does not have any direct uses or live dependent
  //    borrow scopes in between the first forwarding use and the
  //    destroy_value. This ensures that we do not need to deal with splitting
  //    borrow scopes or having to deal with "shape"-mismatches in between uses
  //    of the copy_value's operand and the current running forwarded value.
  //
  // This choice of optimization was just an attempt to be pragmatic given we
  // want to be able to run this optimization at -Onone.
  //
  // With that in mind, lets first check 1.
  Operand *currentForwardingUse = singleCVIConsumingUse;
  while (auto *op = lookThroughSingleForwardingUse(currentForwardingUse)) {
    // Visited insts contain all instructions in between singleCVIConsumingUse
    // and the destroy_value, so if our forwarding inst is not in VisitedInsts,
    // it must not be in the region and currentForwardingUse must be the last
    // use.
    if (!visitedInsts.count(op->getUser()))
      break;
    currentForwardingUse = op;
  }

  // Ok now see if we were able to find a forwarding inst that was later than
  // destroy_value...
  if (currentForwardingUse == singleCVIConsumingUse ||
      visitedInsts.count(currentForwardingUse->getUser())) {
    // If not, see if this use did have a forwardedValue but that forwardedValue
    // has multiple end lifetime uses. In that case, we can optimize if there
    // aren't any uses/etc
    ForwardingOperand forwardingOperand(currentForwardingUse);
    if (!forwardingOperand)
      return false;
    auto forwardedValue = forwardingOperand.getSingleForwardedValue();
    if (!forwardedValue)
      return false;

    // If our forwarding value has a single consuming use and that use is in the
    // same block as our destroy_value, bail if the single consuming use is
    // before our destroy_value.
    if (auto *singleConsumingUse = forwardedValue->getSingleConsumingUse()) {
      if (singleConsumingUse->getParentBlock() == dvi->getParentBlock() &&
          !isUseBetweenInstAndBlockEnd(dvi, singleConsumingUse)) {
        return false;
      }
    }

    // If our forwarded value has multiple lifetime ending uses or a single
    // consuming use that is after the destroy_value, we still need to perform
    // our safety check below to know if we can optimized.
  }

  // Otherwise, we looked through at least one forwarded use and our final use
  // was past dvi in the current block! So we can optimize!
  //
  // As one last safety check, make sure that our copy_value operand does not
  // have any uses in our code region. If it does, we would need to rewrite
  // forwarded values so that the types match up, which is more than this humble
  // optimization is trying to do here given we want to run this at -Onone.
  //
  // TODO: Can we make this more aggressive and by how much? E.x.: Can we allow
  // debug_value users but move them to before our singleCVIConsumingUse?
  for (auto *use : operand->getUses()) {
    auto *user = use->getUser();

    // First if our user is dvi, just continue.
    if (user == dvi)
      continue;

    // Then see if the user itself is a visitedInst. If so, we have a use that
    // may require us to do some sort of transform, we can't optimize.
    if (visitedInsts.count(use->getUser()))
      return false;

    // Ok, we have a use that isn't in our visitedInsts region. That being said,
    // we may still have a use that introduces a new BorrowScope onto our
    // copy_value's operand that overlaps with our forwarding value region. In
    // such a case, we can not optimize.
    //
    // To prove this since we know that any such scope must end at our
    // destroy_value (since that is when the copy_value's operand is destroyed),
    // we need to only find scopes that end within the region in between the
    // singleConsumingUse (the original forwarded use) and the destroy_value. In
    // such a case, we must bail!
    if (auto operand = BorrowingOperand(use))
      if (!operand.visitScopeEndingUses([&](Operand *endScopeUse) {
            // Return false if we did see the relevant end scope instruction
            // in the block. That means that we are going to exit early and
            // return false.
            return !visitedInsts.count(endScopeUse->getUser());
          }))
        return false;
  }
  // Check whether the uses considered immediately above are all effectively
  // instantaneous uses. Pointer escapes propagate values ways that may not be
  // discoverable.
  if (findPointerEscape(operand)) {
    return false;
  }

  // Ok, we now know that we can eliminate this value.
  LLVM_DEBUG(llvm::dbgs()
             << "Eliminate borrowed copy with useless lifetime: " << *cvi);
  ctx.eraseInstruction(dvi);
  ctx.eraseAndRAUWSingleValueInstruction(cvi, operand);
  return true;
}

/// Given that:
///
/// 1. Our copy_value's operand has a single consuming use (and that use is a
///    destroy_value).
/// 2. Our copy_value has a single consuming use.
///
/// try and perform various optimizations to eliminate our copy_value,
/// destroy_value. Example:
///
/// ```
/// %1 = copy_value %0 // in some block
/// ...
///
/// bbN:
///   destroy_value %0
///   br bbFunctionExistingBlock
///
/// bbFunctionExistingBlock:
///   consumingUse %1
///   return
/// ```
///
/// will be optimized to:
///
/// ```
/// ...
///
/// bbN:
///   br bbFunctionExistingBlock
///
/// bbFunctionExistingBlock:
///   consumingUse %0
///   return
/// ```
static bool tryJoiningIfCopyOperandHasSingleDestroyValue(
    SemanticARCOptVisitor &ctx, CopyValueInst *cvi, SILValue operand) {
  // First perform our quick checks to see if our operand has a single
  // destroy_value and our copy_value has a single consuming use. If either are
  // false, we can not optimize so bail early.
  auto *dvi = operand->getSingleConsumingUserOfType<DestroyValueInst>();
  if (!dvi)
    return false;

  auto *singleCVIConsumingUse = cvi->getSingleConsumingUse();
  if (!singleCVIConsumingUse)
    return false;

  // Otherwise, first check to see if our operand's consuming use is a return
  // inst or is in a function exiting block and dvi is not. With this
  // information, we can conclude in both cases that singleCviConsumingUse must
  // post-dominate destroy_value and can eliminate the hand off traffic.
  if (canJoinIfCopyDiesInFunctionExitingBlock(operand, dvi, cvi,
                                              singleCVIConsumingUse)) {
    LLVM_DEBUG(llvm::dbgs() << "Eliminate returned copy: " << *cvi);
    ctx.eraseInstruction(dvi);
    ctx.eraseAndRAUWSingleValueInstruction(cvi, operand);
    return true;
  }

  // Otherwise, try to prove that dvi and singleCVIConsumingUse are not in the same block.
  // block with dvi being strictly before singleCVIConsumingUse, that is:
  //
  // %operand = ...
  // ...
  // %copiedOperand = cvi %operand
  // ...
  // dvi %operand
  // cviConsumer %copiedOperand
  //
  // In such a case, all we know is that dvi and cviConsumer are in the same
  // block. Since dvi is the only destroy of %operand, we know that dvi must
  // post-dominate %copiedOperand and %operand.
  if (dvi->getParent() != singleCVIConsumingUse->getParentBlock())
    return false;

  // First see if our initial use is after dvi. Then we do not need to do any
  // more complex work. We actually check here if we find our destroy_value in
  // between our consuming use and the end block. The reason why we do this is
  // so that if we fail, visitedInsts will contain all instructions in between
  // the consuming use and the destroy_value.
  return tryJoinIfDestroyConsumingUseInSameBlock(ctx, cvi, dvi, operand,
                                                 singleCVIConsumingUse);
}

// # The Problem We Are Solving
//
// The main idea here is that we are trying to eliminate the simplest, easiest
// form of live range joining. Consider the following SIL:
//
//   ```
//   %cviOperand = ...                // @owned value
//   %cvi = copy_value %cviOperand    // copy of @owned value
//   ...
//   destroy_value %cviOperandDestroy // destruction of @owned value
//   ...
//   apply %consumingUser(%cvi)       // destruction of copy of @owned value
//   ```
//
// We want to reduce reference count traffic by eliminating the middle
// copy/destroy yielding:
//
//   ```
//   %cviOperand = ...                // @owned value
//   // *eliminated copy_value*
//   ...
//   // *eliminated destroy_value*
//   ...
//   apply %consumingUser(%cviOperand)       // destruction of copy of @owned
//   value
//   ```
//
// # Safety
//
// In order to do this safely, we need to take the union of the two objects
// lifetimes since we are only joining lifetimes. This ensures that we can rely
// on SILGen's correctness on inserting safe lifetimes. To keep this simple
// today we only optimize if the destroy_value and consuming user are in the
// same block and the consuming user is later in the block than the
// destroy_value.
//
// DISCUSSION: The reason why we do not shrink lifetimes today is that all
// interior pointers (e.x. project_box) are properly guarded by
// begin_borrow. Because of that we can not shrink lifetimes and instead rely on
// SILGen's correctness.
//
// FIXME: CanonicalizeOSSALifetime replaces this.
bool SemanticARCOptVisitor::tryJoiningCopyValueLiveRangeWithOperand(
    CopyValueInst *cvi) {
  // First do a quick check if our operand is owned. If it is not owned, we can
  // not join live ranges.
  SILValue operand = cvi->getOperand();
  if (operand->getOwnershipKind() != OwnershipKind::Owned) {
    return false;
  }

  // Then we handle two different use cases:
  //
  // 1. First we optimize a special case where our copy_value has a single
  //    consuming use and our copy_value's operand has a single consuming use
  //    and that single use is a destroy_value.
  //
  // 2. The second is a more general optimization where our copy_value has
  //    multiple destroy_value, but we know that our copy_value is in the same
  //    block as one of those destroy_value.
  if (tryJoiningIfCopyOperandHasSingleDestroyValue(*this, cvi, operand))
    return true;

  // Otherwise, use a more conservative analysis that requires our copy_value
  // and destroy_value, but is looser about how we handle the consuming use:
  //
  // 1. Since our copy_value and destroy_value are in the same block, if our
  //    copy_value has multiple consuming uses, we know those consuming uses
  //    must be outside of our current block and must be dominated by the
  //    copy_value, destroy_value. So we can immediately optimize.
  //
  // 2. Otherwise, if we have a single consuming use and it is in the same block
  //    as our copy_value, destroy_value, we attempt to prove that the consuming
  //    use (after looking through a forwarding use chain) is later in the
  //    current block than the destroy_value. We use the last forwarding
  //    instruction in a chain of SIL instructions that end in the current
  //    block. Since we are looking through forwarding uses, we need to create
  //    new-borrow scopes at each forwarding instruction as we clone if we have
  //    any guaranteed elements in between our destroy_value and final
  //    forwarding use.
  auto *singleCVIConsumingUse = cvi->getSingleConsumingUse();
  for (auto *use : operand->getConsumingUses()) {
    auto *dvi = dyn_cast<DestroyValueInst>(use->getUser());
    if (!dvi)
      continue;

    // First setup our condition... We only optimize if our copy_value and
    // destroy_value are in the same block. Additionally since our destroy_value
    // is destroying the operand of the copy_value, we must have that cvi is
    // strictly before dvi in the block.
    if (dvi->getParent() != cvi->getParent()) {
      continue;
    }

    // If we had multiple consuming uses of our copy_value, then we know that
    // the copy_value must be live out of the current block implying that we
    // can optimize without any further analysis since we know we will not be
    // shrinking lifetimes of owned values.
    if (singleCVIConsumingUse == nullptr) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Eliminate multiply consumed live-out copy: " << *cvi);
      eraseInstruction(dvi);
      eraseAndRAUWSingleValueInstruction(cvi, operand);
      return true;
    }

    // Then note that if our copy_value has a single consuming use, if that use
    // is not in the same block as our copy_value/destroy_value, it must be live
    // out of the block and thus we are not shrinking any lifetimes.
    if (singleCVIConsumingUse->getParentBlock() != cvi->getParent()) {
      LLVM_DEBUG(llvm::dbgs() << "Eliminate non-local live-out copy: " << *cvi);
      eraseInstruction(dvi);
      eraseAndRAUWSingleValueInstruction(cvi, operand);
      return true;
    }

    // Ok, we know that all of the following instructions are in the same block
    // together:
    //
    // 1. our copy_value (cvi).
    // 2. The consumer of our copy_value (singleCVIConsumingUse).
    // 3. A destroy_value of the copy_value's operand (dvi).
    //
    // So call our subroutine that optimizes given the destroy_value, consume
    // are in the same block and that the copy_value is post-dominated by the
    // destroy_value.
    if (tryJoinIfDestroyConsumingUseInSameBlock(*this, cvi, dvi, operand,
                                                singleCVIConsumingUse))
      return true;
  }

  // Otherwise, we couldn't handle this case, so return false.
  //
  // NOTE: We would generally do a more complex analysis here to handle the more
  // general case. That would most likely /not/ be a guaranteed optimization
  // until we investigate/measure.
  return false;
}

//===----------------------------------------------------------------------===//
//                       Owned Copy Value Optimizations
//===----------------------------------------------------------------------===//

/// Given an owned value that is completely enclosed within its parent owned
/// value and is not consumed, eliminate the copy.
bool SemanticARCOptVisitor::tryPerformOwnedCopyValueOptimization(
    CopyValueInst *cvi) {
  // All mandatory copy optimization is handled by CanonicalizeOSSALifetime,
  // which knows how to preserve lifetimes for debugging.
  if (ctx.onlyMandatoryOpts)
    return false;

  auto originalValue = cvi->getOperand();
  if (originalValue->getOwnershipKind() != OwnershipKind::Owned)
    return false;

  // TODO: Add support for forwarding insts here.
  SmallVector<DestroyValueInst *, 8> destroyingUses;
  SmallVector<Operand *, 32> allCopyUses;
  for (auto *use : cvi->getUses()) {
    // First just stash our use so we have /all uses/.
    allCopyUses.push_back(use);

    // Then if we are not a lifetime ending use, just continue.
    if (!use->isLifetimeEnding()) {
      continue;
    }

    // Otherwise, if we have a destroy value lifetime ending use, stash that.
    if (auto *dvi = dyn_cast<DestroyValueInst>(use->getUser())) {
      destroyingUses.push_back(dvi);
      continue;
    }

    // Otherwise, just bail for now.
    return false;
  }

  // NOTE: We do not actually care if the parent's lifetime ends with
  // destroy_values. All we care is that it is lifetime ending and the use isn't
  // a forwarding instruction.
  SmallVector<Operand *, 8> parentLifetimeEndingUses;
  for (auto *origValueUse : originalValue->getUses())
    if (origValueUse->isLifetimeEnding() &&
        !ForwardingInstruction::isa(origValueUse->getUser()))
      parentLifetimeEndingUses.push_back(origValueUse);

  // Ok, we have an owned value. If we do not have any non-destroying consuming
  // uses, see if all of our uses (ignoring destroying uses) are within our
  // parent owned value's lifetime.
  LinearLifetimeChecker checker(&ctx.getDeadEndBlocks());
  if (!checker.validateLifetime(originalValue, parentLifetimeEndingUses,
                                allCopyUses))
    return false;

  // Ok, we can perform our transform. Eliminate all of our destroy value insts,
  // and then RAUW our copy value with our parent value.
  while (!destroyingUses.empty())
    eraseInstruction(destroyingUses.pop_back_val());
  eraseAndRAUWSingleValueInstruction(cvi, cvi->getOperand());
  return true;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

bool SemanticARCOptVisitor::visitCopyValueInst(CopyValueInst *cvi) {
  // If our copy value inst has only destroy_value users, it is a dead live
  // range. Try to eliminate them.
  if (ctx.shouldPerform(ARCTransformKind::RedundantCopyValueElimPeephole) &&
      eliminateDeadLiveRangeCopyValue(cvi)) {
    return true;
  }

  // Then see if copy_value operand's lifetime ends after our copy_value via a
  // destroy_value. If so, we can join their lifetimes.
  if (ctx.shouldPerform(ARCTransformKind::LifetimeJoiningPeephole) &&
      tryJoiningCopyValueLiveRangeWithOperand(cvi)) {
    return true;
  }

  // Then try to perform the guaranteed copy value optimization.
  if (ctx.shouldPerform(ARCTransformKind::RedundantCopyValueElimPeephole) &&
      performGuaranteedCopyValueOptimization(cvi)) {
    return true;
  }

  if (ctx.shouldPerform(ARCTransformKind::RedundantCopyValueElimPeephole) &&
      tryPerformOwnedCopyValueOptimization(cvi)) {
    return true;
  }

  return false;
}
