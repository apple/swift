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

#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/OwnershipUtils.h"

using namespace swift;
using namespace swift::semanticarc;

OwnershipLiveRange::OwnershipLiveRange(SILValue value)
    : introducer(OwnedValueIntroducer::get(value)), destroyingUses(),
      ownershipForwardingUses(), unknownConsumingUses() {
  assert(introducer);
  assert(introducer.value->getOwnershipKind() == OwnershipKind::Owned);

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
    assert(op->get()->getOwnershipKind() == OwnershipKind::Owned &&
           "Added non-owned value to worklist?!");

    auto *user = op->getUser();

    // Ok, this constraint can take something owned as live. Assert that it
    // can also accept something that is guaranteed. Any non-consuming use of
    // an owned value should be able to take a guaranteed parameter as well
    // (modulo bugs). We assert to catch these.
    if (!op->isLifetimeEnding()) {
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
    if ((ti && !ti->mayHaveTerminatorResult()) ||
        !canOpcodeForwardInnerGuaranteedValues(op) ||
        1 !=
            count_if(user->getNonTypeDependentOperandValues(), [&](SILValue v) {
              return v->getOwnershipKind() == OwnershipKind::Owned;
            })) {
      tmpUnknownConsumingUses.push_back(op);
      continue;
    }

    // If we have a subclass of ForwardingInstruction that doesnt directly
    // forward its operand to the result, treat the use as an unknown consuming
    // use.
    //
    // If we do not directly forward and we have an owned value (which we do
    // here), we could get back a different value. Thus we can not transform
    // such a thing from owned to guaranteed.
    if (auto *i = ForwardingInstruction::get(op->getUser())) {
      if (!i->preservesOwnership()) {
        tmpUnknownConsumingUses.push_back(op);
        continue;
      }
    }

    // Ok, this is a forwarding instruction whose ownership we can flip from
    // owned -> guaranteed.
    tmpForwardingConsumingUses.push_back(op);

    // If we have a non-terminator, just visit its users recursively to see if
    // the users force the live range to be alive.
    if (!ti) {
      for (SILValue v : user->getResults()) {
        if (v->getOwnershipKind() != OwnershipKind::Owned)
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
        // Owned values can get transformed to None values, currently we bail
        // out computing OwnershipLiveRange in this case, because it can lead to
        // incorrect results in the presence of dead edges on the non-trivial
        // paths of switch_enum.
        if (succArg->getOwnershipKind() == OwnershipKind::None) {
          tmpUnknownConsumingUses.push_back(op);
          continue;
        }

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

    // Do not insert end_borrow if the block insert point is a dead end block.
    //
    // DISCUSSION: This is important to do since otherwise, we may be implicitly
    // reducing the lifetime of a value which we can not do yet since we do not
    // require all interior pointer instructions to be guarded by borrows
    // (yet). Once that constraint is in place, we will not have this problem.
    //
    // Consider a situation where one has a @owned switch_enum on an
    // indirect box case which is post-dominated by an unreachable that we want
    // to convert to @guaranteed:
    //
    //   enum MyEnum {
    //     indirect case FirstCase(Int)
    //     ...
    //   }
    //
    //   bb0(%in_guaranteed_addr : $*MyEnum):
    //     ...
    //     %0 = load [copy] %in_guaranteed_addr : $*MyEnum
    //     switch_enum %0 : $MyEnum, case #MyEnum.FirstCase: bb1, ...
    //
    //   bb1(%1 : @owned ${ var Int }):
    //     %2 = project_box %1 : ${ var Int }, 0
    //     %3 = load [trivial] %2 : $*Int
    //     apply %log(%3) : $@convention(thin) (Int) -> ()
    //     unreachable
    //
    // In this case, we will not have a destroy_value on the box, but we may
    // have a project_box on the box. This is ok since we are going to leak the
    // value. But since we are using all consuming uses to determine the
    // lifetime, we will want to insert an end_borrow at the head of the
    // switch_enum dest block like follows:
    //
    //   bb0(%in_guaranteed_addr : $*MyEnum):
    //     ...
    //     %0 = load_borrow %in_guaranteed_addr : $*MyEnum
    //     switch_enum %0 : $MyEnum, case #MyEnum.FirstCase: bb1, ...
    //
    //   bb1(%1 : @guaranteed ${ var Int }):
    //     end_borrow %1 : ${ var Int }
    //     %2 = project_box %1 : ${ var Int }, 0
    //     %3 = load [trivial] %2 : $*Int
    //     apply %log(%3) : $@convention(thin) (Int) -> ()
    //     unreachable
    //
    // which would violate ownership invariants. Instead, we need to realize
    // that %1 is dominated by a dead end block so we may not have a
    // destroy_value upon it meaning we should just not insert the end_borrow
    // here. If we have a destroy_value upon it (since we did not get rid of a
    // destroy_value), then we will still get rid of the destroy_value if we are
    // going to optimize this, so we are still correct.
    if (deadEndBlocks.isDeadEnd(insertPoint->getParent()))
      continue;

    SILBuilderWithScope builder(insertPoint);
    builder.createEndBorrow(loc, newGuaranteedValue);
  }
}

void OwnershipLiveRange::convertOwnedGeneralForwardingUsesToGuaranteed() && {
  while (!ownershipForwardingUses.empty()) {
    auto *use = ownershipForwardingUses.back();
    ownershipForwardingUses = ownershipForwardingUses.drop_back();
    ForwardingOperand operand(use);
    operand.replaceOwnershipKind(OwnershipKind::Owned,
                                 OwnershipKind::Guaranteed);
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
  case OwnedValueIntroducerKind::Invalid:
    llvm_unreachable("Using invalid case?!");
  case OwnedValueIntroducerKind::Phi: {
    auto *phiArg = cast<SILPhiArgument>(introducer.value);
    phiArg->setOwnershipKind(OwnershipKind::Guaranteed);
    phiArg->setReborrow(computeIsReborrow(phiArg));
    return phiArg;
  }
  case OwnedValueIntroducerKind::Struct: {
    auto *si = cast<StructInst>(introducer.value);
    si->setForwardingOwnershipKind(OwnershipKind::Guaranteed);
    return si;
  }
  case OwnedValueIntroducerKind::Tuple: {
    auto *ti = cast<TupleInst>(introducer.value);
    ti->setForwardingOwnershipKind(OwnershipKind::Guaranteed);
    return ti;
  }
  case OwnedValueIntroducerKind::Copy:
  case OwnedValueIntroducerKind::LoadCopy:
  case OwnedValueIntroducerKind::Apply:
  case OwnedValueIntroducerKind::BeginApply:
  case OwnedValueIntroducerKind::TryApply:
  case OwnedValueIntroducerKind::LoadTake:
  case OwnedValueIntroducerKind::Move:
  case OwnedValueIntroducerKind::FunctionArgument:
  case OwnedValueIntroducerKind::PartialApplyInit:
  case OwnedValueIntroducerKind::AllocBoxInit:
  case OwnedValueIntroducerKind::AllocRefInit:
    return SILValue();
  }
  llvm_unreachable("covered switch");
}

void OwnershipLiveRange::convertJoinedLiveRangePhiToGuaranteed(
    DeadEndBlocks &deadEndBlocks, ValueLifetimeAnalysis::Frontier &scratch,
    InstModCallbacks callbacks) && {

  // First convert the phi value itself to be guaranteed.
  convertIntroducerToGuaranteed(introducer);

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
