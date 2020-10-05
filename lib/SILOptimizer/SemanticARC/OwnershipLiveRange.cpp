//===--- OwnershipLiveRange.cpp -------------------------------------------===//
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

#include "OwnershipLiveRange.h"
#include "OwnershipPhiOperand.h"

using namespace swift;
using namespace swift::semanticarc;

OwnershipLiveRange::OwnershipLiveRange(SILValue value)
    : consumingUses(), introducer(*OwnedValueIntroducer::get(value)),
      destroyingUses(), ownershipForwardingUses(), unknownConsumingUses() {
  assert(introducer.value.getOwnershipKind() == ValueOwnershipKind::Owned);

  SmallVector<Operand *, 32> tmpDestroyingUses;
  SmallVector<Operand *, 32> tmpForwardingConsumingUses;
  SmallVector<Operand *, 32> tmpUnknownConsumingUses;

  // We know that our silvalue produces an @owned value. Look through all of our
  // uses and classify them as either consuming or not.
  SmallVector<Operand *, 32> worklist(introducer.value->getUses());
  while (!worklist.empty()) {
    auto *op = worklist.pop_back_val();

    // Skip type dependent operands.
    if (op->isTypeDependent())
      continue;

    // Do a quick check that we did not add ValueOwnershipKind that are not
    // owned to the worklist.
    assert(op->get().getOwnershipKind() == ValueOwnershipKind::Owned &&
           "Added non-owned value to worklist?!");

    auto *user = op->getUser();

    // Ok, this constraint can take something owned as live. Assert that it
    // can also accept something that is guaranteed. Any non-consuming use of
    // an owned value should be able to take a guaranteed parameter as well
    // (modulo bugs). We assert to catch these.
    if (!op->isConsumingUse()) {
      continue;
    }

    // Ok, we know now that we have a consuming use. See if we have a destroy
    // value, quickly up front. If we do have one, stash it and continue.
    if (isa<DestroyValueInst>(user)) {
      tmpDestroyingUses.push_back(op);
      continue;
    }

    // Otherwise, see if we have a forwarding value that has a single
    // non-trivial operand that can accept a guaranteed value. If not, we can
    // not recursively process it, so be conservative and assume that we /may
    // consume/ the value, so the live range must not be eliminated.
    //
    // DISCUSSION: For now we do not support forwarding instructions with
    // multiple non-trivial arguments since we would need to optimize all of
    // the non-trivial arguments at the same time.
    //
    // NOTE: Today we do not support TermInsts for simplicity... we /could/
    // support it though if we need to.
    auto *ti = dyn_cast<TermInst>(user);
    if ((ti && !ti->isTransformationTerminator()) ||
        !isGuaranteedForwardingInst(user) ||
        1 != count_if(user->getOperandValues(
                          true /*ignore type dependent operands*/),
                      [&](SILValue v) {
                        return v.getOwnershipKind() ==
                               ValueOwnershipKind::Owned;
                      })) {
      tmpUnknownConsumingUses.push_back(op);
      continue;
    }

    // Ok, this is a forwarding instruction whose ownership we can flip from
    // owned -> guaranteed.
    tmpForwardingConsumingUses.push_back(op);

    // If we have a non-terminator, just visit its users recursively to see if
    // the the users force the live range to be alive.
    if (!ti) {
      for (SILValue v : user->getResults()) {
        if (v.getOwnershipKind() != ValueOwnershipKind::Owned)
          continue;
        llvm::copy(v->getUses(), std::back_inserter(worklist));
      }
      continue;
    }

    // Otherwise, we know that we have no only a terminator, but a
    // transformation terminator, so we should add the users of its results to
    // the worklist.
    for (auto &succ : ti->getSuccessors()) {
      auto *succBlock = succ.getBB();

      // If we do not have any arguments, then continue.
      if (succBlock->args_empty())
        continue;

      for (auto *succArg : succBlock->getSILPhiArguments()) {
        // If we have an any value, just continue.
        if (succArg->getOwnershipKind() == ValueOwnershipKind::None)
          continue;

        // Otherwise add all users of this BBArg to the worklist to visit
        // recursively.
        llvm::copy(succArg->getUses(), std::back_inserter(worklist));
      }
    }
  }

  // The order in which we append these to consumingUses matters since we assume
  // their order as an invariant. This is done to ensure that we can pass off
  // all of our uses or individual sub-arrays of our users without needing to
  // move around memory.
  llvm::copy(tmpDestroyingUses, std::back_inserter(consumingUses));
  llvm::copy(tmpForwardingConsumingUses, std::back_inserter(consumingUses));
  llvm::copy(tmpUnknownConsumingUses, std::back_inserter(consumingUses));

  auto cUseArrayRef = llvm::makeArrayRef(consumingUses);
  destroyingUses = cUseArrayRef.take_front(tmpDestroyingUses.size());
  ownershipForwardingUses = cUseArrayRef.slice(
      tmpDestroyingUses.size(), tmpForwardingConsumingUses.size());
  unknownConsumingUses = cUseArrayRef.take_back(tmpUnknownConsumingUses.size());
}

void OwnershipLiveRange::insertEndBorrowsAtDestroys(
    SILValue newGuaranteedValue, DeadEndBlocks &deadEndBlocks,
    ValueLifetimeAnalysis::Frontier &scratch) {
  assert(scratch.empty() && "Expected scratch to be initially empty?!");

  // Since we are looking through forwarding uses that can accept guaranteed
  // parameters, we can have multiple destroy_value along the same path. We need
  // to find the post-dominating block set of these destroy value to ensure that
  // we do not insert multiple end_borrow.
  //
  // TODO: Hoist this out?
  SILInstruction *inst = introducer.value->getDefiningInstruction();
  Optional<ValueLifetimeAnalysis> analysis;
  if (!inst) {
    analysis.emplace(cast<SILArgument>(introducer.value),
                     getAllConsumingInsts());
  } else {
    analysis.emplace(inst, getAllConsumingInsts());
  }

  // Use all consuming uses in our value lifetime analysis to ensure correctness
  // in the face of unreachable code.
  bool foundCriticalEdges = !analysis->computeFrontier(
      scratch, ValueLifetimeAnalysis::DontModifyCFG, &deadEndBlocks);
  (void)foundCriticalEdges;
  assert(!foundCriticalEdges);
  auto loc = RegularLocation::getAutoGeneratedLocation();
  while (!scratch.empty()) {
    auto *insertPoint = scratch.pop_back_val();
    SILBuilderWithScope builder(insertPoint);
    builder.createEndBorrow(loc, newGuaranteedValue);
  }
}

static void convertInstructionOwnership(SILInstruction *i,
                                        ValueOwnershipKind oldOwnership,
                                        ValueOwnershipKind newOwnership) {
  // If this is a term inst, just convert all of its incoming values that are
  // owned to be guaranteed.
  if (auto *ti = dyn_cast<TermInst>(i)) {
    for (auto &succ : ti->getSuccessors()) {
      auto *succBlock = succ.getBB();

      // If we do not have any arguments, then continue.
      if (succBlock->args_empty())
        continue;

      for (auto *succArg : succBlock->getSILPhiArguments()) {
        // If we have an any value, just continue.
        if (succArg->getOwnershipKind() == oldOwnership) {
          succArg->setOwnershipKind(newOwnership);
        }
      }
    }
    return;
  }

  assert(i->hasResults());
  for (SILValue result : i->getResults()) {
    if (auto *svi = dyn_cast<OwnershipForwardingSingleValueInst>(result)) {
      if (svi->getOwnershipKind() == oldOwnership) {
        svi->setOwnershipKind(newOwnership);
      }
      continue;
    }

    if (auto *ofci = dyn_cast<OwnershipForwardingConversionInst>(result)) {
      if (ofci->getOwnershipKind() == oldOwnership) {
        ofci->setOwnershipKind(newOwnership);
      }
      continue;
    }

    if (auto *sei = dyn_cast<OwnershipForwardingSelectEnumInstBase>(result)) {
      if (sei->getOwnershipKind() == oldOwnership) {
        sei->setOwnershipKind(newOwnership);
      }
      continue;
    }

    if (auto *mvir = dyn_cast<MultipleValueInstructionResult>(result)) {
      if (mvir->getOwnershipKind() == oldOwnership) {
        mvir->setOwnershipKind(newOwnership);
      }
      continue;
    }

    llvm_unreachable("unhandled forwarding instruction?!");
  }
}

void OwnershipLiveRange::convertOwnedGeneralForwardingUsesToGuaranteed() && {
  while (!ownershipForwardingUses.empty()) {
    auto *i = ownershipForwardingUses.back()->getUser();
    ownershipForwardingUses = ownershipForwardingUses.drop_back();
    convertInstructionOwnership(i, ValueOwnershipKind::Owned,
                                ValueOwnershipKind::Guaranteed);
  }
}

void OwnershipLiveRange::convertToGuaranteedAndRAUW(
    SILValue newGuaranteedValue, InstModCallbacks callbacks) && {
  auto *value = cast<SingleValueInstruction>(introducer.value);
  while (!destroyingUses.empty()) {
    auto *d = destroyingUses.back();
    destroyingUses = destroyingUses.drop_back();
    callbacks.deleteInst(d->getUser());
  }

  callbacks.eraseAndRAUWSingleValueInst(value, newGuaranteedValue);

  // Then change all of our guaranteed forwarding insts to have guaranteed
  // ownership kind instead of what ever they previously had (ignoring trivial
  // results);
  std::move(*this).convertOwnedGeneralForwardingUsesToGuaranteed();
}

// TODO: If this is useful, move onto OwnedValueIntroducer itself?
static SILValue convertIntroducerToGuaranteed(OwnedValueIntroducer introducer) {
  switch (introducer.kind) {
  case OwnedValueIntroducerKind::Phi: {
    auto *phiArg = cast<SILPhiArgument>(introducer.value);
    phiArg->setOwnershipKind(ValueOwnershipKind::Guaranteed);
    return phiArg;
  }
  case OwnedValueIntroducerKind::Struct: {
    auto *si = cast<StructInst>(introducer.value);
    si->setOwnershipKind(ValueOwnershipKind::Guaranteed);
    return si;
  }
  case OwnedValueIntroducerKind::Tuple: {
    auto *ti = cast<TupleInst>(introducer.value);
    ti->setOwnershipKind(ValueOwnershipKind::Guaranteed);
    return ti;
  }
  case OwnedValueIntroducerKind::Copy:
  case OwnedValueIntroducerKind::LoadCopy:
  case OwnedValueIntroducerKind::Apply:
  case OwnedValueIntroducerKind::BeginApply:
  case OwnedValueIntroducerKind::TryApply:
  case OwnedValueIntroducerKind::LoadTake:
  case OwnedValueIntroducerKind::FunctionArgument:
  case OwnedValueIntroducerKind::PartialApplyInit:
  case OwnedValueIntroducerKind::AllocBoxInit:
  case OwnedValueIntroducerKind::AllocRefInit:
    return SILValue();
  }
}

void OwnershipLiveRange::convertJoinedLiveRangePhiToGuaranteed(
    DeadEndBlocks &deadEndBlocks, ValueLifetimeAnalysis::Frontier &scratch,
    InstModCallbacks callbacks) && {

  // First convert the phi value itself to be guaranteed.
  SILValue phiValue = convertIntroducerToGuaranteed(introducer);

  // Then insert end_borrows at each of our destroys if we are consuming. We
  // have to convert the phi to guaranteed first since otherwise, the ownership
  // check when we create the end_borrows will trigger.
  if (introducer.hasConsumingGuaranteedOperands()) {
    insertEndBorrowsAtDestroys(phiValue, deadEndBlocks, scratch);
  }

  // Then eliminate all of the destroys...
  while (!destroyingUses.empty()) {
    auto *d = destroyingUses.back();
    destroyingUses = destroyingUses.drop_back();
    callbacks.deleteInst(d->getUser());
  }

  // and change all of our guaranteed forwarding insts to have guaranteed
  // ownership kind instead of what ever they previously had (ignoring trivial
  // results);
  std::move(*this).convertOwnedGeneralForwardingUsesToGuaranteed();
}

OwnershipLiveRange::HasConsumingUse_t
OwnershipLiveRange::hasUnknownConsumingUse(bool assumingAtFixPoint) const {
  // First do a quick check if we have /any/ unknown consuming
  // uses. If we do not have any, return false early.
  if (unknownConsumingUses.empty()) {
    return HasConsumingUse_t::No;
  }

  // Ok, we do have some unknown consuming uses. If we aren't assuming we are at
  // the fixed point yet, just bail.
  if (!assumingAtFixPoint) {
    return HasConsumingUse_t::Yes;
  }

  // We do not know how to handle yet cases where an owned value is used by
  // multiple phi nodes. So we bail early if unknown consuming uses is > 1.
  //
  // TODO: Build up phi node web.
  auto *op = getSingleUnknownConsumingUse();
  if (!op) {
    return HasConsumingUse_t::Yes;
  }

  // Make sure our single unknown consuming use is a branch inst. If not, bail,
  // this is a /real/ unknown consuming use.
  if (!OwnershipPhiOperand::get(op)) {
    return HasConsumingUse_t::Yes;
  }

  // Otherwise, setup the phi to incoming value map mapping the block arguments
  // to our introducer.
  return HasConsumingUse_t::YesButAllPhiArgs;
}

OwnershipLiveRange::DestroyingInstsRange
OwnershipLiveRange::getDestroyingInsts() const {
  return DestroyingInstsRange(getDestroyingUses(), OperandToUser());
}

OwnershipLiveRange::ConsumingInstsRange
OwnershipLiveRange::getAllConsumingInsts() const {
  return ConsumingInstsRange(consumingUses, OperandToUser());
}
