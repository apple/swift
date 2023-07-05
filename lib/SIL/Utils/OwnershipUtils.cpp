//===--- OwnershipUtils.cpp -----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/OwnershipUtils.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/GraphNodeWorklist.h"
#include "swift/Basic/SmallPtrSetVector.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/LinearLifetimeChecker.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/Test.h"

using namespace swift;

bool swift::findPointerEscape(SILValue original) {
  if (original->getOwnershipKind() != OwnershipKind::Owned &&
      original->getOwnershipKind() != OwnershipKind::Guaranteed) {
    return false;
  }

  ValueWorklist worklist(original->getFunction());
  worklist.push(original);
  if (auto *phi = SILArgument::asPhi(original)) {
    phi->visitTransitiveIncomingPhiOperands([&](auto *phi, auto *operand) {
      worklist.pushIfNotVisited(operand->get());
      return true;
    });
  }

  while (auto value = worklist.pop()) {
    for (auto use : value->getUses()) {
      switch (use->getOperandOwnership()) {
      case OperandOwnership::PointerEscape:
      case OperandOwnership::ForwardingUnowned:
        return true;
      case OperandOwnership::ForwardingConsume: {
        auto *branch = dyn_cast<BranchInst>(use->getUser());
        if (!branch) {
          // Non-phi forwarding consumes end the lifetime of an owned value.
          break;
        }
        auto *phi = branch->getDestBB()->getArgument(use->getOperandNumber());
        worklist.pushIfNotVisited(phi);
        break;
      }
      case OperandOwnership::Borrow: {
        auto borrowOp = BorrowingOperand(use);
        if (auto borrowValue = borrowOp.getBorrowIntroducingUserResult()) {
          worklist.pushIfNotVisited(borrowValue.value);
        }
        break;
      }
      case OperandOwnership::Reborrow: {
        SILArgument *phi = PhiOperand(use).getValue();
        worklist.pushIfNotVisited(phi);
        break;
      }
      case OperandOwnership::GuaranteedForwarding: {
        // This may follow guaranteed phis.
        ForwardingOperand(use).visitForwardedValues([&](SILValue result) {
          // Do not include transitive uses with 'none' ownership
          if (result->getOwnershipKind() == OwnershipKind::None)
            return true;
          worklist.pushIfNotVisited(result);
          return true;
        });
        break;
      }
      case OperandOwnership::InteriorPointer: {
        if (InteriorPointerOperand(use).findTransitiveUses() !=
            AddressUseKind::NonEscaping) {
          return true;
        }
        break;
      }
      default:
        break;
      }
    }
  }
  return false;
}

namespace swift::test {
// Arguments:
// - value: the value to check for escaping
// Dumps:
// - the value
// - whether it has a pointer escape
static FunctionTest OwnershipUtilsHasPointerEscape(
    "has-pointer-escape", [](auto &function, auto &arguments, auto &test) {
      auto value = arguments.takeValue();
      auto has = findPointerEscape(value);
      value->print(llvm::errs());
      auto *boolString = has ? "true" : "false";
      llvm::errs() << boolString << "\n";
    });
} // end namespace swift::test

bool swift::canOpcodeForwardInnerGuaranteedValues(SILValue value) {
  if (auto *inst = value->getDefiningInstructionOrTerminator()) {
    if (auto fwdOp = ForwardingOperation(inst)) {
      return fwdOp.preservesOwnership() &&
             !fwdOp.canForwardOwnedCompatibleValuesOnly();
    }
  }
  return false;
}

bool swift::canOpcodeForwardInnerGuaranteedValues(Operand *use) {
  if (auto fwdOp = ForwardingOperation(use->getUser()))
    return fwdOp.preservesOwnership() &&
           !fwdOp.canForwardOwnedCompatibleValuesOnly();
  return false;
}

bool swift::canOpcodeForwardOwnedValues(SILValue value) {
  if (auto *inst = value->getDefiningInstructionOrTerminator()) {
    if (auto fwdOp = ForwardingOperation(inst)) {
      return fwdOp.preservesOwnership() &&
             !fwdOp.canForwardGuaranteedCompatibleValuesOnly();
    }
  }
  return false;
}

bool swift::canOpcodeForwardOwnedValues(Operand *use) {
  if (auto fwdOp = ForwardingOperation(use->getUser()))
    return fwdOp.preservesOwnership() &&
           !fwdOp.canForwardGuaranteedCompatibleValuesOnly();
  return false;
}

bool swift::computeIsReborrow(SILArgument *arg) {
  if (arg->getOwnershipKind() != OwnershipKind::Guaranteed) {
    return false;
  }
  if (isa<SILFunctionArgument>(arg)) {
    return false;
  }
  return !computeIsGuaranteedForwarding(arg);
}

bool swift::computeIsScoped(SILArgument *arg) {
  if (arg->getOwnershipKind() == OwnershipKind::Owned) {
    return true;
  }
  return computeIsReborrow(arg);
}

// This is the use-def equivalent of use->getOperandOwnership() ==
// OperandOwnership::GuaranteedForwarding.
bool swift::computeIsGuaranteedForwarding(SILValue value) {
  if (value->getOwnershipKind() != OwnershipKind::Guaranteed) {
    return false;
  }
  // NOTE: canOpcodeForwardInnerGuaranteedValues returns true for transformation
  // terminator results.
  if (canOpcodeForwardInnerGuaranteedValues(value) ||
      isa<SILFunctionArgument>(value)) {
    return true;
  }
  // If not a phi, return false
  auto *phi = dyn_cast<SILPhiArgument>(value);
  if (!phi || !phi->isPhi()) {
    return false;
  }
  // For a phi, if we find GuaranteedForwarding phi operand on any incoming
  // path, we return true. Additional verification is added to ensure
  // GuaranteedForwarding phi operands are found on zero or all paths in the
  // OwnershipVerifier.
  bool isGuaranteedForwardingPhi = false;
  phi->visitTransitiveIncomingPhiOperands([&](auto *, auto *op) -> bool {
    auto opValue = op->get();
    assert(opValue->getOwnershipKind().isCompatibleWith(
        OwnershipKind::Guaranteed));
    if (canOpcodeForwardInnerGuaranteedValues(opValue) ||
        isa<SILFunctionArgument>(opValue)) {
      isGuaranteedForwardingPhi = true;
      return false;
    }
    auto *phi = dyn_cast<SILPhiArgument>(opValue);
    if (!phi || !phi->isPhi()) {
      return false;
    }
    return true;
  });
  return isGuaranteedForwardingPhi;
}

//===----------------------------------------------------------------------===//
//                 Guaranteed Use-Point (Lifetime) Discovery
//===----------------------------------------------------------------------===//

// Find all use points of \p guaranteedValue within its borrow scope. All uses
// are naturally dominated by \p guaranteedValue. If a PointerEscape is found,
// then no assumption can be made about \p guaranteedValue's lifetime. Therefore
// the use points are incomplete and this returns false. The escape point that
// was found must still be in \p usePoints to distinguish from dead addresses.
//
// Accumulate results in \p usePoints, ignoring existing elements.
//
// Skip over nested borrow scopes. Their scope-ending instructions are their use
// points. Transitively find all nested scope-ending instructions by looking
// through nested reborrows. Nested reborrows are not use points.
//
// FIXME: handle inner reborrows, which aren't dominated by
// guaranteedValue. Audit all users to handle reborrows.
//
// TODO: Replace this with OwnershipUseVisitor.
bool swift::findInnerTransitiveGuaranteedUses(
  SILValue guaranteedValue, SmallVectorImpl<Operand *> *usePoints) {

  bool foundPointerEscape = false;

  auto leafUse = [&](Operand *use) {
    if (usePoints && use->getOperandOwnership() != OperandOwnership::NonUse) {
      usePoints->push_back(use);
    }
    return true;
  };

  // Push the value's immediate uses.
  //
  // TODO: The worklist can be a simple vector without any a membership check if
  // destructures are changed to be represented as reborrows. Currently a
  // destructure forwards multiple results! This means that the worklist could
  // grow exponentially without the membership check. It's fine to do this
  // membership check locally in this function (within a borrow scope) because
  // it isn't needed for the immediate uses, only the transitive uses.
  GraphNodeWorklist<Operand *, 8> worklist;
  for (Operand *use : guaranteedValue->getUses()) {
    if (use->getOperandOwnership() != OperandOwnership::NonUse)
      worklist.insert(use);
  }

  // --- Transitively follow forwarded uses and look for escapes.

  // usePoints grows in this loop.
  while (Operand *use = worklist.pop()) {
    switch (use->getOperandOwnership()) {
    case OperandOwnership::NonUse:
    case OperandOwnership::TrivialUse:
    case OperandOwnership::ForwardingConsume:
    case OperandOwnership::DestroyingConsume:
      llvm_unreachable("this operand cannot handle an inner guaranteed use");

    case OperandOwnership::ForwardingUnowned:
    case OperandOwnership::PointerEscape:
      leafUse(use);
      foundPointerEscape = true;
      break;

    case OperandOwnership::InstantaneousUse:
    case OperandOwnership::UnownedInstantaneousUse:
    case OperandOwnership::BitwiseEscape:
    // Reborrow only happens when this is called on a value that creates a
    // borrow scope.
    case OperandOwnership::Reborrow:
    // EndBorrow either happens when this is called on a value that creates a
    // borrow scope, or when it is pushed as a use when processing a nested
    // borrow.
    case OperandOwnership::EndBorrow:
      leafUse(use);
      break;

    case OperandOwnership::InteriorPointer:
#if 0 // FIXME!!! Enable in a following commit that fixes RAUW
      // If our base guaranteed value does not have any consuming uses
      // (consider function arguments), we need to be sure to include interior
      // pointer operands since we may not get a use from a end_scope
      // instruction.
      if (InteriorPointerOperand(use).findTransitiveUses(usePoints)
          != AddressUseKind::NonEscaping) {
        foundPointerEscape = true;
      }
#endif
      leafUse(use);
      foundPointerEscape = true;
      break;

    case OperandOwnership::GuaranteedForwarding: {
      bool nonLeaf = false;
      ForwardingOperand(use).visitForwardedValues([&](SILValue result) {
        // Do not include transitive uses with 'none' ownership
        if (result->getOwnershipKind() == OwnershipKind::None)
          return true;

        // Bailout on guaranteed phis because the caller may assume dominance.
        if (SILArgument::asPhi(result)) {
          leafUse(use);
          foundPointerEscape = true;
          return true;
        }
        for (auto *resultUse : result->getUses()) {
          if (resultUse->getOperandOwnership() != OperandOwnership::NonUse) {
            nonLeaf = true;
            worklist.insert(resultUse);
          }
        }
        return true;
      });
      // e.g. A dead forwarded value, e.g. a switch_enum with only trivial uses,
      // must itself be a leaf use.
      if (!nonLeaf) {
        leafUse(use);
      }
      break;
    }
    case OperandOwnership::Borrow:
      // FIXME: Use visitExtendedScopeEndingUses and audit all clients to handle
      // reborrows.
      //
      // FIXME: visit[Extended]ScopeEndingUses can't return false here once dead
      // borrows are disallowed.
      if (!BorrowingOperand(use).visitScopeEndingUses([&](Operand *endUse) {
            if (endUse->getOperandOwnership() == OperandOwnership::Reborrow) {
              foundPointerEscape = true;
            }
            leafUse(endUse);
            return true;
          })) {
        // Special case for dead borrows. This is dangerous because clients
        // don't expect a begin_borrow to be in the use list.
        leafUse(use);
      }
      break;
    }
  }
  return !foundPointerEscape;
}

/// Find all uses in the extended lifetime (i.e. including copies) of a simple
/// (i.e. not reborrowed) borrow scope and its transitive uses.
bool swift::findExtendedUsesOfSimpleBorrowedValue(
    BorrowedValue borrowedValue, SmallVectorImpl<Operand *> *usePoints) {

  auto recordUse = [&](Operand *use) {
    if (usePoints && use->getOperandOwnership() != OperandOwnership::NonUse) {
      usePoints->push_back(use);
    }
  };

  // Push the value's immediate uses.
  //
  // TODO: The worklist can be a simple vector without any a membership check if
  // destructures are changed to be represented as reborrows. Currently a
  // destructure forwards multiple results! This means that the worklist could
  // grow exponentially without the membership check. It's fine to do this
  // membership check locally in this function (within a borrow scope) because
  // it isn't needed for the immediate uses, only the transitive uses.
  GraphNodeWorklist<Operand *, 8> worklist;
  auto addUsesToWorklist = [&worklist](SILValue value) {
    for (Operand *use : value->getUses()) {
      if (use->getOperandOwnership() != OperandOwnership::NonUse)
        worklist.insert(use);
    }
  };

  addUsesToWorklist(borrowedValue.value);

  // --- Transitively follow forwarded uses and look for escapes.

  // usePoints grows in this loop.
  while (Operand *use = worklist.pop()) {
    if (auto *cvi = dyn_cast<CopyValueInst>(use->getUser())) {
      addUsesToWorklist(cvi);
    }
    switch (use->getOperandOwnership()) {
    case OperandOwnership::NonUse:
      break;

    case OperandOwnership::TrivialUse:
    case OperandOwnership::ForwardingConsume:
    case OperandOwnership::DestroyingConsume:
      recordUse(use);
      break;

    case OperandOwnership::ForwardingUnowned:
    case OperandOwnership::PointerEscape:
    case OperandOwnership::Reborrow:
      return false;

    case OperandOwnership::InstantaneousUse:
    case OperandOwnership::UnownedInstantaneousUse:
    case OperandOwnership::BitwiseEscape:
    // EndBorrow either happens when this is called on a value that creates a
    // borrow scope, or when it is pushed as a use when processing a nested
    // borrow.
    case OperandOwnership::EndBorrow:
      recordUse(use);
      break;

    case OperandOwnership::InteriorPointer:
      if (InteriorPointerOperandKind::get(use) ==
          InteriorPointerOperandKind::Invalid)
        return false;
      // If our base guaranteed value does not have any consuming uses (consider
      // function arguments), we need to be sure to include interior pointer
      // operands since we may not get a use from a end_scope instruction.
      if (InteriorPointerOperand(use).findTransitiveUses(usePoints) !=
          AddressUseKind::NonEscaping) {
        return false;
      }
      recordUse(use);
      break;
    case OperandOwnership::GuaranteedForwarding: {
      // Conservatively assume that a forwarding phi is not dominated by the
      // initial borrowed value and bailout.
      if (PhiOperand(use)) {
        return false;
      }
      ForwardingOperand(use).visitForwardedValues([&](SILValue result) {
        // Do not include transitive uses with 'none' ownership
        if (result->getOwnershipKind() == OwnershipKind::None)
          return true;
        for (auto *resultUse : result->getUses()) {
          if (resultUse->getOperandOwnership() != OperandOwnership::NonUse) {
            worklist.insert(resultUse);
          }
        }
        return true;
      });
      recordUse(use);
      break;
    }
    case OperandOwnership::Borrow:
      // FIXME: visitExtendedScopeEndingUses can't return false here once dead
      // borrows are disallowed.
      if (!BorrowingOperand(use).visitExtendedScopeEndingUses(
            [&](Operand *endUse) {
              recordUse(endUse);
              return true;
            })) {
        // Special case for dead borrows. This is dangerous because clients
        // don't expect a begin_borrow to be in the use list.
        recordUse(use);
      }
      break;
    }
  }
  return true;
}

// TODO: refactor this with SSAPrunedLiveness::computeLiveness.
bool swift::findUsesOfSimpleValue(SILValue value,
                                  SmallVectorImpl<Operand *> *usePoints) {
  for (auto *use : value->getUses()) {
    switch (use->getOperandOwnership()) {
    case OperandOwnership::PointerEscape:
      return false;
    case OperandOwnership::Borrow:
      if (!BorrowingOperand(use).visitScopeEndingUses([&](Operand *end) {
        if (end->getOperandOwnership() == OperandOwnership::Reborrow) {
          return false;
        }
        usePoints->push_back(end);
        return true;
      })) {
        return false;
      }
      break;
    default:
      break;
    }
    usePoints->push_back(use);
  }
  return true;
}

bool swift::visitGuaranteedForwardingPhisForSSAValue(
    SILValue value, function_ref<bool(Operand *)> visitor) {
  assert(isa<BeginBorrowInst>(value) || isa<LoadBorrowInst>(value) ||
         (isa<SILPhiArgument>(value) &&
          value->getOwnershipKind() == OwnershipKind::Guaranteed));
  // guaranteedForwardingOps is a collection of all transitive
  // GuaranteedForwarding uses of \p value. It is a set, to avoid repeated
  // processing of structs and tuples which are GuaranteedForwarding.
  SmallSetVector<Operand *, 4> guaranteedForwardingOps;
  // Collect first-level GuaranteedForwarding uses, and call the visitor on any
  // GuaranteedForwardingPhi uses.
  for (auto *use : value->getUses()) {
    if (use->getOperandOwnership() == OperandOwnership::GuaranteedForwarding) {
      if (PhiOperand(use)) {
        if (!visitor(use)) {
          return false;
        }
      }
      guaranteedForwardingOps.insert(use);
    }
  }

  // Transitively, collect GuaranteedForwarding uses.
  for (unsigned i = 0; i < guaranteedForwardingOps.size(); i++) {
    for (auto val : guaranteedForwardingOps[i]->getUser()->getResults()) {
      for (auto *valUse : val->getUses()) {
        if (valUse->getOperandOwnership() ==
            OperandOwnership::GuaranteedForwarding) {
          if (PhiOperand(valUse)) {
            if (!visitor(valUse)) {
              return false;
            }
          }
          guaranteedForwardingOps.insert(valUse);
        }
      }
    }
  }
  return true;
}

// Find all use points of \p guaranteedValue within its borrow scope. All use
// points will be dominated by \p guaranteedValue.
//
// Record (non-nested) reborrows as uses.
//
// BorrowedValues (which introduce a borrow scope) are fundamentally different
// than "inner" guaranteed values. Their only use points are their scope-ending
// uses. There is no need to transitively process uses. However, unlike inner
// guaranteed values, they can have reborrows. To transitively process
// reborrows, use findExtendedTransitiveBorrowedUses.
bool swift::findTransitiveGuaranteedUses(
    SILValue guaranteedValue, SmallVectorImpl<Operand *> &usePoints,
    function_ref<void(Operand *)> visitReborrow) {

  // Handle local borrow introducers without following uses.
  // SILFunctionArguments are *not* borrow introducers in this context--we're
  // trying to find lifetime of values within a function.
  if (auto borrowedValue = BorrowedValue(guaranteedValue)) {
    if (borrowedValue.isLocalScope()) {
      borrowedValue.visitLocalScopeEndingUses([&](Operand *scopeEnd) {
        // Initially push the reborrow as a use point. visitReborrow may pop it
        // if it only wants to compute the extended lifetime's use points.
        usePoints.push_back(scopeEnd);
        if (scopeEnd->getOperandOwnership() == OperandOwnership::Reborrow)
          visitReborrow(scopeEnd);
        return true;
      });
    }
    return true;
  }
  return findInnerTransitiveGuaranteedUses(guaranteedValue, &usePoints);
}

// Find all use points of \p guaranteedValue within its borrow scope. If the
// guaranteed value introduces a borrow scope, then this includes the extended
// borrow scope by following reborrows.
bool swift::
findExtendedTransitiveGuaranteedUses(SILValue guaranteedValue,
                                     SmallVectorImpl<Operand *> &usePoints) {
  // Multiple paths may reach the same reborrows, and reborrow may even be
  // recursive, so the working set requires a membership check.
  SmallPtrSetVector<SILValue, 4> reborrows;
  auto visitReborrow = [&](Operand *reborrow) {
    // Pop the reborrow. It should not appear in the use points of the
    // extend lifetime.
    assert(reborrow == usePoints.back());
    usePoints.pop_back();
    auto borrowedPhi =
      BorrowingOperand(reborrow).getBorrowIntroducingUserResult();
    reborrows.insert(borrowedPhi.value);
  };
  if (!findTransitiveGuaranteedUses(guaranteedValue, usePoints, visitReborrow))
    return false;

  // For guaranteed values that do not introduce a borrow scope, reborrows will
  // be empty at this point.
  for (unsigned idx = 0; idx < reborrows.size(); ++idx) {
    bool result =
      findTransitiveGuaranteedUses(reborrows[idx], usePoints, visitReborrow);
    // It is impossible to find a Pointer escape while traversing reborrows.
    assert(result && "visiting reborrows always succeeds");
    (void)result;
  }
  return true;
}

//===----------------------------------------------------------------------===//
//                           Borrowing Operand
//===----------------------------------------------------------------------===//

void BorrowingOperandKind::print(llvm::raw_ostream &os) const {
  switch (value) {
  case Kind::Invalid:
    llvm_unreachable("Using an unreachable?!");
  case Kind::BeginBorrow:
    os << "BeginBorrow";
    return;
  case Kind::BeginApply:
    os << "BeginApply";
    return;
  case Kind::Branch:
    os << "Branch";
    return;
  case Kind::Apply:
    os << "Apply";
    return;
  case Kind::TryApply:
    os << "TryApply";
    return;
  case Kind::Yield:
    os << "Yield";
    return;
  case Kind::PartialApplyStack:
    os << "PartialApply [stack]";
    return;
  case Kind::BeginAsyncLet:
    os << "BeginAsyncLet";
    return;
  }
  llvm_unreachable("Covered switch isn't covered?!");
}

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &os,
                                     BorrowingOperandKind kind) {
  kind.print(os);
  return os;
}

void BorrowingOperand::print(llvm::raw_ostream &os) const {
  os << "BorrowScopeOperand:\n"
        "Kind: " << kind << "\n"
        "Value: " << op->get()
     << "User: " << *op->getUser();
}

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &os,
                                     const BorrowingOperand &operand) {
  operand.print(os);
  return os;
}

bool BorrowingOperand::hasEmptyRequiredEndingUses() const {
  switch (kind) {
  case BorrowingOperandKind::Invalid:
    llvm_unreachable("Using invalid case");
  case BorrowingOperandKind::BeginBorrow:
  case BorrowingOperandKind::BeginApply:
  case BorrowingOperandKind::BeginAsyncLet:
  case BorrowingOperandKind::PartialApplyStack: {
    return op->getUser()->hasUsesOfAnyResult();
  }
  case BorrowingOperandKind::Branch: {
    auto *br = cast<BranchInst>(op->getUser());
    return br->getArgForOperand(op)->use_empty();
  }
  // These are instantaneous borrow scopes so there aren't any special end
  // scope instructions.
  case BorrowingOperandKind::Apply:
  case BorrowingOperandKind::TryApply:
  case BorrowingOperandKind::Yield:
    return false;
  }
  llvm_unreachable("Covered switch isn't covered");
}

bool BorrowingOperand::visitScopeEndingUses(
    function_ref<bool(Operand *)> func) const {
  switch (kind) {
  case BorrowingOperandKind::Invalid:
    llvm_unreachable("Using invalid case");
  case BorrowingOperandKind::BeginBorrow: {
    bool deadBorrow = true;
    for (auto *use : cast<BeginBorrowInst>(op->getUser())->getUses()) {
      if (use->isLifetimeEnding()) {
        deadBorrow = false;
        if (!func(use))
          return false;
      }
    }
    // FIXME: special case for dead borrows. This is dangerous because clients
    // only expect visitScopeEndingUses to return false if the visitor returned
    // false.
    return !deadBorrow;
  }
  case BorrowingOperandKind::BeginApply: {
    bool deadApply = true;
    auto *user = cast<BeginApplyInst>(op->getUser());
    for (auto *use : user->getTokenResult()->getUses()) {
      deadApply = false;
      if (!func(use))
        return false;
    }
    return !deadApply;
  }
  case BorrowingOperandKind::PartialApplyStack: {
    auto user = cast<PartialApplyInst>(op->getUser());
    assert(user->isOnStack() && "escaping closures can't borrow");
    // The closure's borrow lifetimes end when the closure itself ends its
    // lifetime. That may happen transitively through conversions that forward
    // ownership of the closure.
    return user->visitOnStackLifetimeEnds(func);
  }
  case BorrowingOperandKind::BeginAsyncLet: {
    auto user = cast<BuiltinInst>(op->getUser());
    // The async let ends its borrow when the task is ended.
    bool dead = true;
    for (auto *use : user->getUses()) {
      dead = false;
      auto builtinUser = dyn_cast<BuiltinInst>(use->getUser());
      if (!builtinUser
          || builtinUser->getBuiltinKind() != BuiltinValueKind::EndAsyncLetLifetime)
        continue;

      if (!func(use)) {
        return false;
      }
    }
    return !dead;
  }
  // These are instantaneous borrow scopes so there aren't any special end
  // scope instructions.
  case BorrowingOperandKind::Apply:
  case BorrowingOperandKind::TryApply:
  case BorrowingOperandKind::Yield:
    return true;
  case BorrowingOperandKind::Branch: {
    bool deadBranch = true;
    auto *br = cast<BranchInst>(op->getUser());
    for (auto *use : br->getArgForOperand(op)->getUses()) {
      if (use->isLifetimeEnding()) {
        deadBranch = false;
        if (!func(use))
          return false;
      }
    }
    return !deadBranch;
  }
  }
  llvm_unreachable("Covered switch isn't covered");
}

bool BorrowingOperand::visitExtendedScopeEndingUses(
    function_ref<bool(Operand *)> visitor) const {

  if (hasBorrowIntroducingUser()) {
    auto borrowedValue = getBorrowIntroducingUserResult();
    return borrowedValue.visitExtendedScopeEndingUses(visitor);
  }
  return visitScopeEndingUses(visitor);
}

BorrowedValue BorrowingOperand::getBorrowIntroducingUserResult() const {
  switch (kind) {
  case BorrowingOperandKind::Invalid:
  case BorrowingOperandKind::Apply:
  case BorrowingOperandKind::TryApply:
  case BorrowingOperandKind::BeginApply:
  case BorrowingOperandKind::Yield:
  case BorrowingOperandKind::PartialApplyStack:
  case BorrowingOperandKind::BeginAsyncLet:
    return BorrowedValue();

  case BorrowingOperandKind::BeginBorrow: {
    auto value = BorrowedValue(cast<BeginBorrowInst>(op->getUser()));
    assert(value);
    return value;
  }
  case BorrowingOperandKind::Branch: {
    auto *bi = cast<BranchInst>(op->getUser());
    auto value =
        BorrowedValue(bi->getDestBB()->getArgument(op->getOperandNumber()));
    assert(value && "guaranteed-to-unowned conversion not allowed on branches");
    return value;
  }
  }
  llvm_unreachable("covered switch");
}

SILValue BorrowingOperand::getScopeIntroducingUserResult() {
  switch (kind) {
  case BorrowingOperandKind::Invalid:
  case BorrowingOperandKind::Yield:
  case BorrowingOperandKind::Apply:
  case BorrowingOperandKind::TryApply:
    return SILValue();

  case BorrowingOperandKind::BeginAsyncLet:
  case BorrowingOperandKind::PartialApplyStack:
  case BorrowingOperandKind::BeginBorrow:
    return cast<SingleValueInstruction>(op->getUser());

  case BorrowingOperandKind::BeginApply:
    return cast<BeginApplyInst>(op->getUser())->getTokenResult();

  case BorrowingOperandKind::Branch: {
    PhiOperand phiOp(op);
    return phiOp.getValue();
  }
  }
  llvm_unreachable("covered switch");
}

void BorrowingOperand::getImplicitUses(
    SmallVectorImpl<Operand *> &foundUses) const {
  // FIXME: this visitScopeEndingUses should never return false once dead
  // borrows are disallowed.
  if (!visitScopeEndingUses([&](Operand *endOp) {
    foundUses.push_back(endOp);
    return true;
  })) {
    // Special-case for dead borrows.
    foundUses.push_back(op);
  }
}

//===----------------------------------------------------------------------===//
//                             Borrow Introducers
//===----------------------------------------------------------------------===//

void BorrowedValueKind::print(llvm::raw_ostream &os) const {
  switch (value) {
  case BorrowedValueKind::Invalid:
    llvm_unreachable("Using invalid case?!");
  case BorrowedValueKind::SILFunctionArgument:
    os << "SILFunctionArgument";
    return;
  case BorrowedValueKind::BeginBorrow:
    os << "BeginBorrowInst";
    return;
  case BorrowedValueKind::LoadBorrow:
    os << "LoadBorrowInst";
    return;
  case BorrowedValueKind::Phi:
    os << "Phi";
    return;
  }
  llvm_unreachable("Covered switch isn't covered?!");
}

void BorrowedValue::print(llvm::raw_ostream &os) const {
  os << "BorrowScopeIntroducingValue:\n"
    "Kind: " << kind << "\n"
    "Value: " << value;
}

void BorrowedValue::getLocalScopeEndingInstructions(
    SmallVectorImpl<SILInstruction *> &scopeEndingInsts) const {
  assert(isLocalScope() && "Should only call this given a local scope");

  switch (kind) {
  case BorrowedValueKind::Invalid:
    llvm_unreachable("Using invalid case?!");
  case BorrowedValueKind::SILFunctionArgument:
    llvm_unreachable("Should only call this with a local scope");
  case BorrowedValueKind::BeginBorrow:
  case BorrowedValueKind::LoadBorrow:
  case BorrowedValueKind::Phi:
    for (auto *use : value->getUses()) {
      if (use->isLifetimeEnding()) {
        scopeEndingInsts.push_back(use->getUser());
      }
    }
    return;
  }
  llvm_unreachable("Covered switch isn't covered?!");
}

// Note: BorrowedLifetimeExtender assumes no intermediate values between a
// borrow introducer and its reborrow. The borrowed value must be an operand of
// the reborrow.
bool BorrowedValue::visitLocalScopeEndingUses(
    function_ref<bool(Operand *)> visitor) const {
  assert(isLocalScope() && "Should only call this given a local scope");
  switch (kind) {
  case BorrowedValueKind::Invalid:
    llvm_unreachable("Using invalid case?!");
  case BorrowedValueKind::SILFunctionArgument:
    llvm_unreachable("Should only call this with a local scope");
  case BorrowedValueKind::LoadBorrow:
  case BorrowedValueKind::BeginBorrow:
  case BorrowedValueKind::Phi:
    for (auto *use : value->getUses()) {
      if (use->isLifetimeEnding()) {
        if (!visitor(use))
          return false;
      }
    }
    return true;
  }
  llvm_unreachable("Covered switch isn't covered?!");
}

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &os,
                                     BorrowedValueKind kind) {
  kind.print(os);
  return os;
}

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &os,
                                     const BorrowedValue &value) {
  value.print(os);
  return os;
}

/// Add this scopes live blocks into the PrunedLiveness result.
void BorrowedValue::
computeTransitiveLiveness(MultiDefPrunedLiveness &liveness) const {
  liveness.initializeDef(value);
  visitTransitiveLifetimeEndingUses([&](Operand *endOp) {
    if (endOp->getOperandOwnership() == OperandOwnership::EndBorrow) {
      liveness.updateForUse(endOp->getUser(), /*lifetimeEnding*/ true);
      return true;
    }
    assert(endOp->getOperandOwnership() == OperandOwnership::Reborrow);
    PhiOperand phiOper(endOp);
    liveness.initializeDef(phiOper.getValue());
    liveness.updateForUse(endOp->getUser(), /*lifetimeEnding*/ false);
    return true;
  });
}

bool BorrowedValue::areUsesWithinExtendedScope(
    ArrayRef<Operand *> uses, DeadEndBlocks *deadEndBlocks) const {
  // First make sure that we actually have a local scope. If we have a non-local
  // scope, then we have something (like a SILFunctionArgument) where a larger
  // semantic construct (in the case of SILFunctionArgument, the function
  // itself) acts as the scope. So we already know that our passed in
  // instructions must be in the same scope.
  if (!isLocalScope())
    return true;

  // Compute the local scope's liveness.
  MultiDefPrunedLiveness liveness(value->getFunction());
  computeTransitiveLiveness(liveness);
  return liveness.areUsesWithinBoundary(uses, deadEndBlocks);
}

// The visitor \p func is only called on final scope-ending uses, not reborrows.
bool BorrowedValue::visitExtendedScopeEndingUses(
    function_ref<bool(Operand *)> visitor) const {
  assert(isLocalScope());

  SmallPtrSetVector<SILValue, 4> reborrows;

  auto visitEnd = [&](Operand *scopeEndingUse) {
    if (scopeEndingUse->getOperandOwnership() == OperandOwnership::Reborrow) {
      auto borrowedValue =
          BorrowingOperand(scopeEndingUse).getBorrowIntroducingUserResult();
      reborrows.insert(borrowedValue.value);
      return true;
    }
    return visitor(scopeEndingUse);
  };

  if (!visitLocalScopeEndingUses(visitEnd))
    return false;

  // reborrows grows in this loop.
  for (unsigned idx = 0; idx < reborrows.size(); ++idx) {
    if (!BorrowedValue(reborrows[idx]).visitLocalScopeEndingUses(visitEnd))
      return false;
  }
  return true;
}

bool BorrowedValue::visitTransitiveLifetimeEndingUses(
    function_ref<bool(Operand *)> visitor) const {
  assert(isLocalScope());

  SmallPtrSetVector<SILValue, 4> reborrows;

  auto visitEnd = [&](Operand *scopeEndingUse) {
    if (scopeEndingUse->getOperandOwnership() == OperandOwnership::Reborrow) {
      auto borrowedValue =
          BorrowingOperand(scopeEndingUse).getBorrowIntroducingUserResult();
      reborrows.insert(borrowedValue.value);
      // visitor on the reborrow
      return visitor(scopeEndingUse);
    }
    // visitor on the end_borrow
    return visitor(scopeEndingUse);
  };

  if (!visitLocalScopeEndingUses(visitEnd))
    return false;

  // reborrows grows in this loop.
  for (unsigned idx = 0; idx < reborrows.size(); ++idx) {
    if (!BorrowedValue(reborrows[idx]).visitLocalScopeEndingUses(visitEnd))
      return false;
  }

  return true;
}

bool BorrowedValue::visitInteriorPointerOperandHelper(
    function_ref<void(InteriorPointerOperand)> func,
    BorrowedValue::InteriorPointerOperandVisitorKind kind) const {
  using Kind = BorrowedValue::InteriorPointerOperandVisitorKind;

  SmallVector<Operand *, 32> worklist(value->getUses());
  while (!worklist.empty()) {
    auto *op = worklist.pop_back_val();

    if (auto interiorPointer = InteriorPointerOperand(op)) {
      func(interiorPointer);
      continue;
    }

    if (auto borrowingOperand = BorrowingOperand(op)) {
      switch (kind) {
      case Kind::NoNestedNoReborrows:
        // We do not look through nested things and or reborrows, so just
        // continue.
        continue;
      case Kind::YesNestedNoReborrows:
        // We only look through nested borrowing operands, we never look through
        // reborrows though.
        if (borrowingOperand.isReborrow())
          continue;
        break;
      case Kind::YesNestedYesReborrows:
        // Look through everything!
        break;
      }

      auto bv = borrowingOperand.getBorrowIntroducingUserResult();
      for (auto *use : bv->getUses()) {
        if (auto intPtrOperand = InteriorPointerOperand(use)) {
          func(intPtrOperand);
          continue;
        }
        worklist.push_back(use);
      }
      continue;
    }

    auto *user = op->getUser();
    if (isa<DebugValueInst>(user) || isa<SuperMethodInst>(user) ||
        isa<ClassMethodInst>(user) || isa<CopyValueInst>(user) ||
        isa<EndBorrowInst>(user) || isa<ApplyInst>(user) ||
        isa<StoreInst>(user) || isa<PartialApplyInst>(user) ||
        isa<UnmanagedRetainValueInst>(user) ||
        isa<UnmanagedReleaseValueInst>(user) ||
        isa<UnmanagedAutoreleaseValueInst>(user)) {
      continue;
    }

    // These are interior pointers that have not had support yet added for them.
    if (isa<ProjectExistentialBoxInst>(user)) {
      continue;
    }

    // Look through object.
    if (auto *svi = dyn_cast<SingleValueInstruction>(user)) {
      if (Projection::isObjectProjection(svi)) {
        for (SILValue result : user->getResults()) {
          llvm::copy(result->getUses(), std::back_inserter(worklist));
        }
        continue;
      }
    }

    return false;
  }

  return true;
}

//===----------------------------------------------------------------------===//
//                              AddressOwnership
//===----------------------------------------------------------------------===//

bool AddressOwnership::areUsesWithinLifetime(
    ArrayRef<Operand *> uses, DeadEndBlocks &deadEndBlocks) const {
  if (!base.hasLocalOwnershipLifetime())
    return true;

  SILValue root = base.getOwnershipReferenceRoot();
  BorrowedValue borrow(root);
  if (borrow)
    return borrow.areUsesWithinExtendedScope(uses, &deadEndBlocks);

  // --- A reference with no borrow scope! Currently happens for project_box.

  // Compute the reference value's liveness.
  SSAPrunedLiveness liveness(root->getFunction());
  liveness.initializeDef(root);
  LiveRangeSummary summary = liveness.computeSimple();
  // Conservatively ignore InnerBorrowKind::Reborrowed and
  // AddressUseKind::PointerEscape and Reborrowed. The resulting liveness at
  // least covers the known uses.
  (void)summary;

  // FIXME (implicit borrow): handle reborrows transitively just like above so
  // we don't bail out if a uses is within the reborrowed scope.
  return liveness.areUsesWithinBoundary(uses, &deadEndBlocks);
}

//===----------------------------------------------------------------------===//
//                          Owned Value Introducers
//===----------------------------------------------------------------------===//

void OwnedValueIntroducerKind::print(llvm::raw_ostream &os) const {
  switch (value) {
  case OwnedValueIntroducerKind::Invalid:
    llvm_unreachable("Using invalid case?!");
  case OwnedValueIntroducerKind::Apply:
    os << "Apply";
    return;
  case OwnedValueIntroducerKind::BeginApply:
    os << "BeginApply";
    return;
  case OwnedValueIntroducerKind::TryApply:
    os << "TryApply";
    return;
  case OwnedValueIntroducerKind::Copy:
    os << "Copy";
    return;
  case OwnedValueIntroducerKind::LoadCopy:
    os << "LoadCopy";
    return;
  case OwnedValueIntroducerKind::LoadTake:
    os << "LoadTake";
    return;
  case OwnedValueIntroducerKind::Move:
    os << "Move";
    return;
  case OwnedValueIntroducerKind::Phi:
    os << "Phi";
    return;
  case OwnedValueIntroducerKind::Struct:
    os << "Struct";
    return;
  case OwnedValueIntroducerKind::Tuple:
    os << "Tuple";
    return;
  case OwnedValueIntroducerKind::FunctionArgument:
    os << "FunctionArgument";
    return;
  case OwnedValueIntroducerKind::PartialApplyInit:
    os << "PartialApplyInit";
    return;
  case OwnedValueIntroducerKind::AllocBoxInit:
    os << "AllocBoxInit";
    return;
  case OwnedValueIntroducerKind::AllocRefInit:
    os << "AllocRefInit";
    return;
  }
  llvm_unreachable("Covered switch isn't covered");
}

//===----------------------------------------------------------------------===//
//                       Introducer Searching Routines
//===----------------------------------------------------------------------===//

bool swift::getAllBorrowIntroducingValues(SILValue inputValue,
                                          SmallVectorImpl<BorrowedValue> &out) {
  if (inputValue->getOwnershipKind() != OwnershipKind::Guaranteed)
    return false;

  SmallSetVector<SILValue, 32> worklist;
  worklist.insert(inputValue);

  // worklist grows in this loop.
  for (unsigned idx = 0; idx < worklist.size(); idx++) {
    SILValue value = worklist[idx];

    // First check if v is an introducer. If so, stash it and continue.
    if (auto scopeIntroducer = BorrowedValue(value)) {
      out.push_back(scopeIntroducer);
      continue;
    }

    // If v produces .none ownership, then we can ignore it. It is important
    // that we put this before checking for guaranteed forwarding instructions,
    // since we want to ignore guaranteed forwarding instructions that in this
    // specific case produce a .none value.
    if (value->getOwnershipKind() == OwnershipKind::None)
      continue;

    // Otherwise if v is an ownership forwarding value, add its defining
    // instruction
    if (value->isGuaranteedForwarding()) {
      if (auto *i = value->getDefiningInstruction()) {
        for (SILValue opValue : i->getNonTypeDependentOperandValues()) {
          worklist.insert(opValue);
        }
        continue;
      }

      // Otherwise, we should have a block argument that is defined by a single
      // predecessor terminator.
      auto *arg = cast<SILPhiArgument>(value);
      if (arg->isTerminatorResult()) {
        if (auto *forwardedOper = arg->forwardedTerminatorResultOperand()) {
          worklist.insert(forwardedOper->get());
          continue;
        }
      }
      arg->visitIncomingPhiOperands([&](auto *operand) {
        worklist.insert(operand->get());
        return true;
      });
    }

    // Otherwise, this is an introducer we do not understand. Bail and return
    // false.
    return false;
  }

  return true;
}

// FIXME: replace this logic with AccessBase::findOwnershipReferenceRoot.
BorrowedValue swift::getSingleBorrowIntroducingValue(SILValue inputValue) {
  if (inputValue->getOwnershipKind() != OwnershipKind::Guaranteed)
    return {};

  SILValue currentValue = inputValue;
  while (true) {
    // First check if our initial value is an introducer. If we have one, just
    // return it.
    if (auto scopeIntroducer = BorrowedValue(currentValue)) {
      return scopeIntroducer;
    }

    if (currentValue->getOwnershipKind() == OwnershipKind::None)
      return {};

    // Otherwise if v is an ownership forwarding value, add its defining
    // instruction
    if (currentValue->isGuaranteedForwarding()) {
      if (auto *i = currentValue->getDefiningInstructionOrTerminator()) {
        auto instOps = i->getNonTypeDependentOperandValues();
        // If we have multiple incoming values, return .None. We can't handle
        // this.
        auto begin = instOps.begin();
        if (std::next(begin) != instOps.end()) {
          return {};
        }
        // Otherwise, set currentOp to the single operand and continue.
        currentValue = *begin;
        continue;
      }
    }

    // Otherwise, this is an introducer we do not understand. Bail and return
    // None.
    return {};
  }

  llvm_unreachable("Should never hit this");
}

bool swift::getAllOwnedValueIntroducers(
    SILValue inputValue, SmallVectorImpl<OwnedValueIntroducer> &out) {
  if (inputValue->getOwnershipKind() != OwnershipKind::Owned)
    return false;

  SmallVector<SILValue, 32> worklist;
  worklist.emplace_back(inputValue);

  while (!worklist.empty()) {
    SILValue value = worklist.pop_back_val();

    // First check if v is an introducer. If so, stash it and continue.
    if (auto introducer = OwnedValueIntroducer::get(value)) {
      out.push_back(introducer);
      continue;
    }

    // If v produces .none ownership, then we can ignore it. It is important
    // that we put this before checking for guaranteed forwarding instructions,
    // since we want to ignore guaranteed forwarding instructions that in this
    // specific case produce a .none value.
    if (value->getOwnershipKind() == OwnershipKind::None)
      continue;

    // Otherwise if v is an ownership forwarding value, add its defining
    // instruction
    if (isForwardingConsume(value)) {
      if (auto *i = value->getDefiningInstructionOrTerminator()) {
        llvm::copy(i->getNonTypeDependentOperandValues(),
                   std::back_inserter(worklist));
        continue;
      }
    }

    // Otherwise, this is an introducer we do not understand. Bail and return
    // false.
    return false;
  }

  return true;
}

OwnedValueIntroducer swift::getSingleOwnedValueIntroducer(SILValue inputValue) {
  if (inputValue->getOwnershipKind() != OwnershipKind::Owned)
    return {};

  SILValue currentValue = inputValue;
  while (true) {
    // First check if our initial value is an introducer. If we have one, just
    // return it.
    if (auto introducer = OwnedValueIntroducer::get(currentValue)) {
      return introducer;
    }

    // Otherwise if v is an ownership forwarding value, add its defining
    // instruction
    if (isForwardingConsume(currentValue)) {
      if (auto *i = currentValue->getDefiningInstructionOrTerminator()) {
        auto instOps = i->getNonTypeDependentOperandValues();
        // If we have multiple incoming values, return .None. We can't handle
        // this.
        auto begin = instOps.begin();
        if (std::next(begin) != instOps.end()) {
          return {};
        }
        // Otherwise, set currentOp to the single operand and continue.
        currentValue = *begin;
        continue;
      }
    }

    // Otherwise, this is an introducer we do not understand. Bail and return
    // None.
    return {};
  }

  llvm_unreachable("Should never hit this");
}

//===----------------------------------------------------------------------===//
//                             Forwarding Operand
//===----------------------------------------------------------------------===//

ForwardingOperand::ForwardingOperand(Operand *use) {
  if (use->isTypeDependent())
    return;

  switch (use->getOperandOwnership()) {
  case OperandOwnership::ForwardingUnowned:
  case OperandOwnership::ForwardingConsume:
  case OperandOwnership::GuaranteedForwarding:
    this->use = use;
    break;
  default:
    this->use = nullptr;
    return;
  }
}

ValueOwnershipKind ForwardingOperand::getForwardingOwnershipKind() const {
  auto *user = use->getUser();

  // NOTE: This if chain is meant to be a covered switch, so make sure to return
  // in each if itself since we have an unreachable at the bottom to ensure if a
  // new subclass of OwnershipForwardingInst is added
  if (auto *ofsvi = dyn_cast<OwnershipForwardingSingleValueInstruction>(user))
    return ofsvi->getForwardingOwnershipKind();

  if (auto *ofmvi =
          dyn_cast<OwnershipForwardingMultipleValueInstruction>(user)) {
    assert(ofmvi->getNumOperands() == 1);
    return ofmvi->getForwardingOwnershipKind();
  }

  if (auto *ofti = dyn_cast<OwnershipForwardingTermInst>(user)) {
    assert(ofti->getNumOperands() == 1);
    return ofti->getForwardingOwnershipKind();
  }

  llvm_unreachable("Unhandled forwarding inst?!");
}

void ForwardingOperand::setForwardingOwnershipKind(
    ValueOwnershipKind newKind) const {
  auto *user = use->getUser();
  // NOTE: This if chain is meant to be a covered switch, so make sure to return
  // in each if itself since we have an unreachable at the bottom to ensure if a
  // new subclass of OwnershipForwardingInst is added
  if (auto *ofsvi = dyn_cast<OwnershipForwardingSingleValueInstruction>(user))
    return ofsvi->setForwardingOwnershipKind(newKind);
  if (auto *ofmvi = dyn_cast<OwnershipForwardingMultipleValueInstruction>(user)) {
    assert(ofmvi->getNumOperands() == 1);
    if (!ofmvi->getOperand(0)->getType().isTrivial(*ofmvi->getFunction())) {
      ofmvi->setForwardingOwnershipKind(newKind);
      // TODO: Refactor this better.
      if (auto *dsi = dyn_cast<DestructureStructInst>(ofmvi)) {
        for (auto &result : dsi->getAllResultsBuffer()) {
          if (result.getType().isTrivial(*dsi->getFunction()))
            continue;
          result.setOwnershipKind(newKind);
        }
      } else {
        auto *dti = cast<DestructureTupleInst>(ofmvi);
        for (auto &result : dti->getAllResultsBuffer()) {
          if (result.getType().isTrivial(*dti->getFunction()))
            continue;
          result.setOwnershipKind(newKind);
        }
      }
    }
    return;
  }

  if (auto *ofti = dyn_cast<OwnershipForwardingTermInst>(user)) {
    assert(ofti->getNumOperands() == 1);
    if (!ofti->getOperand()->getType().isTrivial(*ofti->getFunction())) {
      ofti->setForwardingOwnershipKind(newKind);

      // Then convert all of its incoming values that are owned to be guaranteed.
      for (auto &succ : ofti->getSuccessors()) {
        auto *succBlock = succ.getBB();

        // If we do not have any arguments, then continue.
        if (succBlock->args_empty())
          continue;

        for (auto *succArg : succBlock->getSILPhiArguments()) {
          // If we have an any value, just continue.
          if (!succArg->getType().isTrivial(*ofti->getFunction()))
            continue;
          succArg->setOwnershipKind(newKind);
        }
      }
    }
    return;
  }

  assert(
      !isa<MoveOnlyWrapperToCopyableValueInst>(user) &&
      "MoveOnlyWrapperToCopyableValueInst can not have its ownership changed");

  llvm_unreachable("Out of sync with OperandOwnership");
}

void ForwardingOperand::replaceOwnershipKind(ValueOwnershipKind oldKind,
                                             ValueOwnershipKind newKind) const {
  auto *user = use->getUser();

  if (auto *fInst = dyn_cast<OwnershipForwardingSingleValueInstruction>(user))
    if (fInst->getForwardingOwnershipKind() == oldKind)
      return fInst->setForwardingOwnershipKind(newKind);

  if (auto *ofmvi = dyn_cast<OwnershipForwardingMultipleValueInstruction>(user)) {
    if (ofmvi->getForwardingOwnershipKind() == oldKind) {
      ofmvi->setForwardingOwnershipKind(newKind);
    }
    // TODO: Refactor this better.
    if (auto *dsi = dyn_cast<DestructureStructInst>(ofmvi)) {
      for (auto &result : dsi->getAllResultsBuffer()) {
        if (result.getOwnershipKind() != oldKind)
          continue;
        result.setOwnershipKind(newKind);
      }
    } else {
      auto *dti = cast<DestructureTupleInst>(ofmvi);
      for (auto &result : dti->getAllResultsBuffer()) {
        if (result.getOwnershipKind() != oldKind)
          continue;
        result.setOwnershipKind(newKind);
      }
    }
    return;
  }

  if (auto *ofti = dyn_cast<OwnershipForwardingTermInst>(user)) {
    if (ofti->getForwardingOwnershipKind() == oldKind) {
      ofti->setForwardingOwnershipKind(newKind);
      // Then convert all of its incoming values that are owned to be guaranteed.
      for (auto &succ : ofti->getSuccessors()) {
        auto *succBlock = succ.getBB();

        // If we do not have any arguments, then continue.
        if (succBlock->args_empty())
          continue;

        for (auto *succArg : succBlock->getSILPhiArguments()) {
          // If we have an any value, just continue.
          if (succArg->getOwnershipKind() == oldKind) {
            succArg->setOwnershipKind(newKind);
          }
        }
      }
    }
    return;
  }

  assert(
      !isa<MoveOnlyWrapperToCopyableValueInst>(user) &&
      "MoveOnlyWrapperToCopyableValueInst can not have its ownership changed");

  llvm_unreachable("Missing Case! Out of sync with OperandOwnership");
}

SILValue ForwardingOperand::getSingleForwardedValue() const {
  if (auto *svi = dyn_cast<SingleValueInstruction>(use->getUser()))
    return svi;
  return SILValue();
}

bool ForwardingOperand::visitForwardedValues(
    function_ref<bool(SILValue)> visitor) {
  auto *user = use->getUser();

  // See if we have a single value instruction... if we do that is always the
  // transitive result.
  if (auto *svi = dyn_cast<SingleValueInstruction>(user)) {
    return visitor(svi);
  }

  if (auto *mvri = dyn_cast<MultipleValueInstruction>(user)) {
    return llvm::all_of(mvri->getResults(), [&](SILValue value) {
      if (value->getOwnershipKind() == OwnershipKind::None)
        return true;
      return visitor(value);
    });
  }

  // This is an instruction like switch_enum and checked_cast_br that are
  // "transforming terminators"... We know that this means that we should at
  // most have a single phi argument.
  auto *ti = cast<TermInst>(user);
  if (ti->mayHaveTerminatorResult()) {
    return llvm::all_of(
        ti->getSuccessorBlocks(), [&](SILBasicBlock *succBlock) {
          // If we do not have any arguments, then continue.
          if (succBlock->args_empty())
            return true;

          auto args = succBlock->getSILPhiArguments();
          assert(args.size() == 1 &&
                 "Transforming terminator with multiple args?!");
          return visitor(args[0]);
        });
  }

  // If our terminator is function exiting, we do not have a value to visit, so
  // just return.
  if (ti->isFunctionExiting())
    return true;

  auto *succArg = PhiOperand(use).getValue();
  return visitor(succArg);
}

void swift::visitExtendedReborrowPhiBaseValuePairs(
    BeginBorrowInst *borrowInst, function_ref<void(SILPhiArgument *, SILValue)>
                                     visitReborrowPhiBaseValuePair) {
  // A Reborrow can have different base values on different control flow
  // paths.
  // For that reason, worklist stores (reborrow, base value) pairs.
  // We need a SetVector to make sure we don't revisit the same pair again.
  SmallSetVector<std::tuple<PhiOperand, SILValue>, 4> worklist;

  // Find all reborrows of value and insert the (reborrow, base value) pair into
  // the worklist.
  auto collectReborrows = [&](SILValue value, SILValue baseValue) {
    BorrowedValue(value).visitLocalScopeEndingUses([&](Operand *op) {
      if (op->getOperandOwnership() == OperandOwnership::Reborrow) {
        worklist.insert(std::make_tuple(PhiOperand(op), baseValue));
      }
      return true;
    });
  };

  // Initialize the worklist.
  collectReborrows(borrowInst, borrowInst->getOperand());

  // For every (reborrow, base value) pair in the worklist:
  // - Find phi value and new base value
  // - Call the visitor on the phi value and new base value pair
  // - Populate the worklist with pairs of reborrows of phi value and the new
  // base.
  for (unsigned idx = 0; idx < worklist.size(); idx++) {
    PhiOperand phiOp;
    SILValue currentBaseValue;
    std::tie(phiOp, currentBaseValue) = worklist[idx];

    auto *phiValue = phiOp.getValue();
    SILValue newBaseValue = currentBaseValue;

    // If the previous base value was also passed as a phi operand along with
    // the reborrow, its phi value will be the new base value.
    for (auto &op : phiOp.getBranch()->getAllOperands()) {
      PhiOperand otherPhiOp(&op);
      if (otherPhiOp.getSource() != currentBaseValue) {
        continue;
      }
      newBaseValue = otherPhiOp.getValue();
    }

    // Call the visitor function
    visitReborrowPhiBaseValuePair(phiValue, newBaseValue);

    collectReborrows(phiValue, newBaseValue);
  }
}

void swift::visitExtendedGuaranteedForwardingPhiBaseValuePairs(
    BorrowedValue borrow, function_ref<void(SILPhiArgument *, SILValue)>
                              visitGuaranteedForwardingPhiBaseValuePair) {
  assert(borrow.kind == BorrowedValueKind::BeginBorrow ||
         borrow.kind == BorrowedValueKind::LoadBorrow);
  // A GuaranteedForwardingPhi can have different base values on different
  // control flow paths.
  // For that reason, worklist stores (GuaranteedForwardingPhi operand, base
  // value) pairs. We need a SetVector to make sure we don't revisit the same
  // pair again.
  SmallSetVector<std::tuple<PhiOperand, SILValue>, 4> worklist;

  auto collectGuaranteedForwardingPhis = [&](SILValue value,
                                             SILValue baseValue) {
    visitGuaranteedForwardingPhisForSSAValue(value, [&](Operand *op) {
      worklist.insert(std::make_tuple(PhiOperand(op), baseValue));
      return true;
    });
  };

  // Collect all GuaranteedForwardingPhis
  collectGuaranteedForwardingPhis(borrow.value, borrow.value);
  borrow.visitTransitiveLifetimeEndingUses([&](Operand *endUse) {
    if (endUse->getOperandOwnership() == OperandOwnership::Reborrow) {
      auto *phiValue = PhiOperand(endUse).getValue();
      collectGuaranteedForwardingPhis(phiValue, phiValue);
    }
    return true;
  });
  // For every (GuaranteedForwardingPhi operand, base value) pair in the
  // worklist:
  // - Find phi value and new base value
  // - Call the visitor on the phi value and new base value pair
  // - Populate the worklist with pairs of GuaranteedForwardingPhi ops of phi
  // value and the new base.
  for (unsigned idx = 0; idx < worklist.size(); idx++) {
    PhiOperand phiOp;
    SILValue currentBaseValue;
    std::tie(phiOp, currentBaseValue) = worklist[idx];

    auto *phiValue = phiOp.getValue();
    SILValue newBaseValue = currentBaseValue;

    // If an adjacent reborrow is found in the same block as the guaranteed phi,
    // then set newBaseValue to the reborrow.
    for (auto &op : phiOp.getBranch()->getAllOperands()) {
      PhiOperand otherPhiOp(&op);
      if (otherPhiOp.getSource() != currentBaseValue) {
        continue;
      }
      newBaseValue = otherPhiOp.getValue();
    }

    // Call the visitor function
    visitGuaranteedForwardingPhiBaseValuePair(phiValue, newBaseValue);

    collectGuaranteedForwardingPhis(phiValue, newBaseValue);
  }
}

/// If \p instruction forwards guaranteed values to its results, visit each
/// forwarded operand. The visitor must check whether the forwarded value is
/// guaranteed.
///
/// Return true \p visitOperand was called at least once.
///
/// \p visitOperand should always recheck for Guaranteed owernship if it
/// matters, in case a cast forwards a trivial type to a nontrivial type.
///
/// This intentionally does not handle phis, which require recursive traversal
/// to determine `isGuaranteedForwardingPhi`.
bool swift::visitForwardedGuaranteedOperands(
  SILValue value, function_ref<void(Operand *)> visitOperand) {

  assert(!SILArgument::asPhi(value) && "phis are handled separately");

  if (auto *termResult = SILArgument::isTerminatorResult(value)) {
    if (auto *oper = termResult->forwardedTerminatorResultOperand()) {
      visitOperand(oper);
      return true;
    }
    return false;
  }
  auto *inst = value->getDefiningInstruction();
  if (!inst)
    return false;

  // Bypass conversions that produce a guarantee value out of thin air.
  if (inst->getNumRealOperands() == 0) {
    return false;
  }

  auto fwdOp = ForwardingOperation(inst);
  if (!fwdOp) {
    return false;
  }

  for (auto &operand : fwdOp.getForwardedOperands()) {
    visitOperand(&operand);
  }
  return true;
}

namespace {

// Find the definitions of the scopes that enclose guaranteed values, handling
// all combinations of aggregation, guaranteed forwarding phis, and reborrows.
class FindEnclosingDefs {
  // A separately allocated set-vector is used for each level of recursion
  // across block boundaries (NodeSet cannot be used recursively).
  using LocalValueSetVector = SmallPtrSetVector<SILValue, 8>;

  SILFunction *function;
  ValueSet visitedPhis;

public:
  FindEnclosingDefs(SILFunction *function) : function(function),
                                             visitedPhis(function) {}

  // Visit each definition of a scope that immediately encloses a guaranteed
  // value. The guaranteed value effectively keeps these scopes alive.
  //
  // This means something different depending on whether \p value is itself a
  // borrow introducer vs. a forwarded guaranteed value. If \p value is an
  // introducer, then this disovers the enclosing borrow scope and visits all
  // introducers of that scope. If \p value is a forwarded value, then this
  // visits the introducers of the current borrow scope.
  bool visitEnclosingDefs(SILValue value,
                          function_ref<bool(SILValue)> visitor) && {
    if (value->getOwnershipKind() != OwnershipKind::Guaranteed)
      return true;

    if (auto borrowedValue = BorrowedValue(value)) {
      switch (borrowedValue.kind) {
      case BorrowedValueKind::Invalid:
        llvm_unreachable("checked above");

      case BorrowedValueKind::Phi: {
        StackList<SILValue> enclosingDefs(function);
        recursivelyFindDefsOfReborrow(SILArgument::asPhi(value), enclosingDefs);
        for (SILValue def : enclosingDefs) {
          if (!visitor(def))
            return false;
        }
        return true;
      }
      case BorrowedValueKind::BeginBorrow:
        return std::move(*this).visitBorrowIntroducers(
            cast<BeginBorrowInst>(value)->getOperand(), visitor);

      case BorrowedValueKind::LoadBorrow:
      case BorrowedValueKind::SILFunctionArgument:
        // There is no enclosing def on this path.
        return true;
      }
    }
    // Handle forwarded guaranteed values.
    return std::move(*this).visitBorrowIntroducers(value, visitor);
  }

  // Visit the values that introduce the borrow scopes that includes \p
  // value. If value is owned, or introduces a borrow scope, then this only
  // visits \p value.
  bool visitBorrowIntroducers(SILValue value,
                              function_ref<bool(SILValue)> visitor) && {
    StackList<SILValue> introducers(function);
    LocalValueSetVector visitedValues;
    recursivelyFindBorrowIntroducers(value, introducers, visitedValues);
    for (SILValue introducer : introducers) {
      if (!visitor(introducer))
        return false;
    }
    return true;
  }

protected:
  // This is the identity function (i.e. just adds \p value to \p introducers)
  // when:
  // - \p value is owned
  // - \p value introduces a borrow scope (begin_borrow, load_borrow, reborrow)
  //
  // Otherwise recurse up the use-def chain to find all introducers.
  //
  // Returns false if \p forwardingPhi was already encountered, either because
  // of a phi cycle or because of reconvergent control flow. Similarly, return
  // false if all incoming values were encountered.
  bool recursivelyFindBorrowIntroducers(SILValue value,
                                        StackList<SILValue> &introducers,
                                        LocalValueSetVector &visitedValues) {
    // Check if this value's introducers have already been added to
    // 'introducers' to avoid duplicates and avoid exponential recursion on
    // aggregates.
    if (!visitedValues.insert(value))
      return false;

    switch (value->getOwnershipKind()) {
    case OwnershipKind::Any:
    case OwnershipKind::None:
    case OwnershipKind::Unowned:
      return false;

    case OwnershipKind::Owned:
      introducers.push_back(value);
      return true;

    case OwnershipKind::Guaranteed:
      break;
    }
    // BorrowedValue handles the initial scope introducers: begin_borrow,
    // load_borrow, & reborrow.
    if (BorrowedValue(value)) {
      introducers.push_back(value);
      return true;
    }
    bool foundNewIntroducer = false;
    // Handle forwarding phis.
    if (auto *phi = SILArgument::asPhi(value)) {
      foundNewIntroducer = recursivelyFindForwardingPhiIntroducers(
          phi, introducers, visitedValues);
    } else {
      // Recurse through guaranteed forwarding instructions.
      visitForwardedGuaranteedOperands(value, [&](Operand *operand) {
        SILValue forwardedVal = operand->get();
        if (forwardedVal->getOwnershipKind() == OwnershipKind::Guaranteed) {
          foundNewIntroducer |=
            recursivelyFindBorrowIntroducers(forwardedVal, introducers,
                                             visitedValues);
        }
      });
    }
    return foundNewIntroducer;
  }

  // Given the enclosing definition on a predecessor path, identify the
  // enclosing definitions on the successor block. Each enclosing predecessor
  // def is either used by an outer-adjacent phi in the successor block, or it
  // must dominate the successor block.
  static SILValue findSuccessorDefFromPredDef(SILBasicBlock *predecessor,
                                              SILValue enclosingPredDef) {

    SILBasicBlock *successor = predecessor->getSingleSuccessorBlock();
    assert(successor && "phi predecessor must have a single successor in OSSA");

    for (auto *candidatePhi : successor->getArguments()) {
      SILValue candidateValue =
        candidatePhi->getIncomingPhiValue(predecessor);

      // Find the outer adjacent phi in the successor block.
      // the 'enclosingDef' from the 'pred' block.
      if (candidateValue == enclosingPredDef)
        return candidatePhi;
    }
    // No candidates phi are outer-adjacent phis. The incoming enclosingDef
    // must dominate the current guaranteed phi. So it remains the enclosing
    // scope.
    return enclosingPredDef;
  }

  // Given the enclosing definitions on a predecessor path, identify the
  // enclosing definitions on the successor block.
  void findSuccessorDefsFromPredDefs(
      SILBasicBlock *predecessor, const StackList<SILValue> &predDefs,
      StackList<SILValue> &successorDefs,
      LocalValueSetVector &visitedSuccessorValues) {

    // Gather the new introducers for the successor block.
    for (SILValue predDef : predDefs) {
      SILValue succDef = findSuccessorDefFromPredDef(predecessor, predDef);
      if (visitedSuccessorValues.insert(succDef))
        successorDefs.push_back(succDef);
    }
  }

  // Find the introducers of a forwarding phi's borrow scope. The introducers
  // are either dominating values, or reborrows in the same block as the
  // forwarding phi.
  //
  // Recurse along the use-def phi web until a begin_borrow is reached. At each
  // level, find the outer-adjacent phi, if one exists, otherwise return the
  // dominating definition.
  //
  // Returns false if \p forwardingPhi was already encountered, either because
  // of a phi cycle or because of reconvergent control flow. Similarly, returns
  // false if all incoming values were encountered.
  //
  //     one(%reborrow_1 : @guaranteed)
  //         %field = struct_extract %reborrow_1
  //         br two(%reborrow_1, %field)
  //     two(%reborrow_2 : @guaranteed, %forward_2 : @guaranteed)
  //         end_borrow %reborrow_2
  //
  // Calling recursivelyFindForwardingPhiIntroducers(%forward_2)
  // recursively computes these introducers:
  //
  //    %field is the only value incoming to %forward_2.
  //
  //    %field is introduced by %reborrow_1 via
  //    recursivelyFindBorrowIntroducers(%field).
  //
  //    %reborrow_1 is introduced by %reborrow_2 in block "two" via
  //    findSuccessorDefsFromPredDefs(%reborrow_1)).
  //
  //    %reborrow_2 is returned.
  //
  bool
  recursivelyFindForwardingPhiIntroducers(SILPhiArgument *forwardingPhi,
                                          StackList<SILValue> &introducers,
                                          LocalValueSetVector &visitedValues) {
    // Phi cycles are skipped. They cannot contribute any new enclosing defs.
    if (!visitedPhis.insert(forwardingPhi))
      return false;

    bool foundIntroducer = false;
    SILBasicBlock *block = forwardingPhi->getParent();
    for (auto *pred : block->getPredecessorBlocks()) {
      SILValue incomingValue = forwardingPhi->getIncomingPhiValue(pred);

      // Each phi operand requires a new introducer list and visited values
      // set. These values will be remapped to successor phis before adding them
      // to the caller's introducer list. It may be necessary to revisit a value
      // that was already visited by the caller before remapping to phis.
      StackList<SILValue> incomingIntroducers(function);
      LocalValueSetVector incomingVisitedValues;
      if (!recursivelyFindBorrowIntroducers(incomingValue, incomingIntroducers,
                                            incomingVisitedValues))
        continue;

      foundIntroducer = true;
      findSuccessorDefsFromPredDefs(pred, incomingIntroducers, introducers,
                                    visitedValues);
    }
    return foundIntroducer;
  }

  // Given a reborrow operand's incoming value, find the enclosing definition.
  void recursivelyFindDefsOfReborrowOperand(
    SILValue incomingValue,
    StackList<SILValue> &enclosingDefs) {

    if (incomingValue->getOwnershipKind() == OwnershipKind::None)
      return;

    assert(incomingValue->getOwnershipKind() == OwnershipKind::Guaranteed);

    // Avoid repeatedly constructing BorrowedValue during use-def
    // traversal. That would be quadratic if it checks all uses for reborrows.
    if (auto *predPhi = dyn_cast<SILPhiArgument>(incomingValue)) {
      recursivelyFindDefsOfReborrow(predPhi, enclosingDefs);
      return;
    }

    // Handle non-phi borrow introducers.
    BorrowedValue borrowedValue(incomingValue);

    switch (borrowedValue.kind) {
    case BorrowedValueKind::Phi:
      llvm_unreachable("phis are short-curcuited above");
    case BorrowedValueKind::Invalid:
      llvm_unreachable("A reborrow immediate operand must be a BorrowedValue.");
    case BorrowedValueKind::BeginBorrow: {
      LocalValueSetVector visitedValues;
      recursivelyFindBorrowIntroducers(
        cast<BeginBorrowInst>(incomingValue)->getOperand(), enclosingDefs,
        visitedValues);
      break;
    }
    case BorrowedValueKind::LoadBorrow:
    case BorrowedValueKind::SILFunctionArgument:
      // There is no enclosing def on this path.
      break;
    }
  }

  // Given a reborrow, find the definitions of the enclosing borrow scopes. Each
  // enclosing borrow scope is represented by one of the following cases, which
  // refer to the example below:
  //
  // dominating owned value -> %value encloses %reborrow_1
  // owned outer-adjacent phi -> %phi_3 encloses %reborrow_3
  // dominating outer borrow introducer -> %outerBorrowB encloses %reborrow
  // outer-adjacent reborrow -> %outerReborrow encloses %reborrow
  //
  // Recurse along the use-def phi web until a begin_borrow is reached. Then
  // find all introducers of the begin_borrow's operand. At each level, find
  // the outer adjacent phi, if one exists, otherwise return the most recently
  // found dominating definition.
  //
  // If \p reborrow was already encountered because of a phi cycle, then no
  // enclosingDefs are added.
  //
  // Example:
  //
  //         %value = ...
  //         %borrow = begin_borrow %value
  //         br one(%borrow)
  //     one(%reborrow_1 : @guaranteed)
  //         br two(%value, %reborrow_1)
  //     two(%phi_2 : @owned, %reborrow_2 : @guaranteed)
  //         br three(%value, %reborrow_1)
  //     three(%phi_3 : @owned, %reborrow_3 : @guaranteed)
  //         end_borrow %reborrow_3
  //         destroy_value %phi_3
  //
  // recursivelyFindDefsOfReborrow(%reborrow_3) returns %phi_3 by
  // computing enclosing defs (inner -> outer) in this order:
  //
  //     %reborrow_1 -> %value
  //     %reborrow_2 -> %phi_2
  //     %reborrow_3 -> %phi_3
  //
  // Example:
  //
  //         %outerBorrowA = begin_borrow
  //         %outerBorrowB = begin_borrow
  //         %struct = struct (%outerBorrowA, outerBorrowB)
  //         %borrow = begin_borrow %struct
  //         br one(%outerBorrowA, %borrow)
  //     one(%outerReborrow : @guaranteed, %reborrow : @guaranteed)
  //
  // recursivelyFindDefsOfReborrow(%reborrow) returns
  // (%outerReborrow, %outerBorrowB).
  //
  void recursivelyFindDefsOfReborrow(SILPhiArgument *reborrow,
                                     StackList<SILValue> &enclosingDefs) {
    assert(enclosingDefs.empty());
    LocalValueSetVector visitedDefs;

    // phi cycles can be skipped. They cannot contribute any new enclosing defs.
    if (!visitedPhis.insert(reborrow))
      return;

    SILBasicBlock *block = reborrow->getParent();
    for (auto *pred : block->getPredecessorBlocks()) {
      SILValue incomingValue = reborrow->getIncomingPhiValue(pred);

      // Each phi operand requires a new enclosing def list. These values will
      // be remapped to successor phis before adding them to the caller's
      // enclosing def list. It may be necessary to revisit a value that was
      // already visited by the caller before remapping to phis.
      StackList<SILValue> enclosingPredDefs(function);
      recursivelyFindDefsOfReborrowOperand(incomingValue, enclosingPredDefs);
      findSuccessorDefsFromPredDefs(pred, enclosingPredDefs, enclosingDefs,
                                    visitedDefs);
    }
  }
};

} // end namespace

bool swift::visitEnclosingDefs(SILValue value,
                               function_ref<bool(SILValue)> visitor) {
  if (isa<SILUndef>(value))
    return true;
  return FindEnclosingDefs(value->getFunction())
    .visitEnclosingDefs(value, visitor);
}

namespace swift::test {
// Arguments:
// - SILValue: value
// Dumps:
// - function
// - the enclosing defs
static FunctionTest FindEnclosingDefsTest(
    "find-enclosing-defs", [](auto &function, auto &arguments, auto &test) {
      function.dump();
      llvm::dbgs() << "Enclosing Defs:\n";
      visitEnclosingDefs(arguments.takeValue(), [](SILValue def) {
        def->dump();
        return true;
      });
    });
} // end namespace swift::test

bool swift::visitBorrowIntroducers(SILValue value,
                                   function_ref<bool(SILValue)> visitor) {
  if (isa<SILUndef>(value))
    return true;
  return FindEnclosingDefs(value->getFunction())
    .visitBorrowIntroducers(value, visitor);
}

namespace swift::test {
// Arguments:
// - SILValue: value
// Dumps:
// - function
// - the borrow introducers
static FunctionTest FindBorrowIntroducers(
    "find-borrow-introducers", [](auto &function, auto &arguments, auto &test) {
      function.dump();
      llvm::dbgs() << "Introducers:\n";
      visitBorrowIntroducers(arguments.takeValue(), [](SILValue def) {
        def->dump();
        return true;
      });
    });
} // end namespace swift::test

/// Return true of the lifetime of \p innerPhiVal depends on \p outerPhiVal.
///
/// This handles SIL values with nested lifetimes that cross a control flow
/// merge.
///
/// When an owned value is passed to a phi, it is consumed. So any
/// "inner" scope borrowing that owned value must end no later than that
/// branch instruction. Either such a borrow scope ends before the branch that
/// represents the owned phi operand:
///     %lifetime = begin_borrow %value
///     ...
///     end_borrow %lifetime    <-- borrow scope ends here
///     br block(%value)        <-- owned value consumed here
/// or the borrow scope ends in another phi in the same block as (adjacent to)
/// the owned phi:
///     %lifetime = begin_borrow %value
///     ...
///     end_borrow %lifetime
///     br block(%value, %lifetime)         <-- borrow scope ends here
///                                         <-- adjacent to the consume
/// A phi corresponding to a value nested within another phi's lifetime is an
/// "inner adjacent phi".
///
/// A guaranteed phi that ends a borrow scope is a special kind of phi called a
/// "reborrow". In the above example, the reborrow is an inner adjacent to the
/// owned phi and the owned phi is outer adjacent to the reborrow.
///
/// Note that an inner lifetime cannot extend beyond the outer lifetime's scope,
/// even of the outer value is forwarded. In particular, the following is
/// invalid:
///         %lifetime = begin_borrow %value
///         ...
///         br block(%value)
///     block(%value_2 : @owned):
///         end_borrow %lifetime
///         destroy_value %value_2
/// because %lifetime depends on %value but %value is consumed at `br two`.
///
/// Similarly, a reborrow ends its borrow scope and begins a new borrow
/// scope. So any open nested borrow of the original outer borrow must end no
/// later than in that branch instruction.
///
/// This extends to guaranteed forwarding phis, whose lifetimes are nested
/// within a borrow scope.
///
/// Currently, an owned phi's inner adjacent phi must be a reborrow. A
/// reborrow's adjacent phi may be either a nested reborrow, or a guaranteed
/// forwarding phi. In the future, we remove the requirement that all guaranteed
/// values have borrow scopes; then an owned phi's inner adjacent phi may be a
/// guaranteed forwarding phi.
///
/// Given a phi, 'outerPhi', it can be determined to have an inner adjacent phi,
/// 'innerPhi' if and only if: on any path, the operand of 'outerPhi' is the
/// enclosing definition of the operand of 'innerPhi' on the same path.
///      
bool swift::isInnerAdjacentPhi(SILArgument *innerPhiVal,
                               SILArgument *outerPhiVal) {
  auto innerPhi = PhiValue(innerPhiVal);
  auto outerPhi = PhiValue(outerPhiVal);
  assert(innerPhi.phiBlock == outerPhi.phiBlock && "precondition");

  for (SILBasicBlock *predBlock : innerPhi.phiBlock->getPredecessorBlocks()) {
    SILValue innerValue = innerPhi.getOperand(predBlock)->get();
    SILValue outerValue = outerPhi.getOperand(predBlock)->get();
    // Visitor returns false to stop visiting when a match is found.
    if (!visitEnclosingDefs(innerValue, [&](SILValue def) {
      // If innerValue's enclosing 'def' is 'outerValue', then we found an inner
      // adjacent phi.
      return def != outerValue;
    })) {
      // outerPhi ends the lifetime of an enclosing def for this predecessor.
      return true;
    }
  }
  return false;
}

/// Visit the phis in the same block as \p phi whose lifetime depends on \p
/// phi.
///
/// See isInnerAdjacentPhi() comments.
///
/// If the visitor returns false, stops visiting and returns false.  Otherwise,
/// returns true.
bool swift::visitInnerAdjacentPhis(SILArgument *phi,
                                   function_ref<bool(SILArgument *)> visitor) {
  SILBasicBlock *block = phi->getParentBlock();
  if (block->pred_empty())
    return true;

  for (auto *adjacentPhi : block->getArguments()) {
    if (adjacentPhi == phi)
      continue;

    if (isInnerAdjacentPhi(adjacentPhi, phi)) {
      if (!visitor(adjacentPhi))
        return false;
    }
  }
  return true;
}

namespace swift::test {
// Arguments:
// - SILValue: phi
// Dumps:
// - function
// - the adjacent phis
static FunctionTest VisitInnerAdjacentPhisTest(
    "visit-inner-adjacent-phis",
    [](auto &function, auto &arguments, auto &test) {
      function.dump();
      visitInnerAdjacentPhis(cast<SILPhiArgument>(arguments.takeValue()),
                             [](auto *argument) -> bool {
                               argument->dump();
                               return true;
                             });
    });
} // end namespace swift::test

void swift::visitTransitiveEndBorrows(
    SILValue value,
    function_ref<void(EndBorrowInst *)> visitEndBorrow) {
  GraphNodeWorklist<SILValue, 4> worklist;
  worklist.insert(value);

  while (!worklist.empty()) {
    auto val = worklist.pop();
    for (auto *consumingUse : val->getConsumingUses()) {
      auto *consumingUser = consumingUse->getUser();
      if (auto *branch = dyn_cast<BranchInst>(consumingUser)) {
        auto *succBlock = branch->getSingleSuccessorBlock();
        auto *phiArg = cast<SILPhiArgument>(
            succBlock->getArgument(consumingUse->getOperandNumber()));
        worklist.insert(phiArg);
      } else {
        visitEndBorrow(cast<EndBorrowInst>(consumingUser));
      }
    }
  }
}

/// Whether the specified lexical begin_borrow instruction is nested.
///
/// A begin_borrow [lexical] is nested if the borrowed value's lifetime is
/// guaranteed by another lexical scope.  That happens if:
/// - the non-guaranteed borrowee's value is lexical
/// - the guaranteed borrowee's value's reference roots are lexical
///   - for example, the borrowee is itself a begin_borrow [lexical]
bool swift::isNestedLexicalBeginBorrow(BeginBorrowInst *bbi) {
  assert(bbi->isLexical());
  auto value = bbi->getOperand();
  if (value->getOwnershipKind() != OwnershipKind::Guaranteed) {
    return value->isLexical();
  }
  SmallVector<SILValue, 8> roots;
  findGuaranteedReferenceRoots(value, /*lookThroughNestedBorrows=*/false,
                               roots);
  return llvm::all_of(roots, [](auto root) {
    if (auto *outerBBI = dyn_cast<BeginBorrowInst>(root)) {
      return outerBBI->isLexical();
    }
    if (auto *arg = dyn_cast<SILFunctionArgument>(root)) {
      return arg->getOwnershipKind() == OwnershipKind::Guaranteed;
    }
    return false;
  });
}

bool swift::isRedundantMoveValue(MoveValueInst *mvi) {
  // Given: %moved_to_value = move_value %original_value
  //
  // Check whether the original value's lifetime and the moved-to value's
  // lifetime have the same (1) ownership, (2) lexicality, and (3) escaping.
  //
  // Along the way, also check for cases where they have different values for
  // those characteristics but it doesn't matter because of how limited the uses
  // of the original value are (for now, whether the move is the only consuming
  // use).

  auto original = mvi->getOperand();

  // (1) Ownership matches?
  // (The new value always has owned ownership.)
  if (original->getOwnershipKind() != OwnershipKind::Owned) {
    return false;
  }

  // (2) Lexicality matches?
  if (mvi->isLexical() != original->isLexical()) {
    return false;
  }

  // The move doesn't alter constraints: ownership and lexicality match.

  // Before checking whether escaping matches, check whether the move_value is
  // redundant regardless on account of how its uses are limited.
  //
  // At this point, ownership and lexicality are known to match.  If the
  // original value doesn't escape, then merging the two lifetimes won't make
  // it harder to optimize the portion of the merged lifetime corresponding to
  // the moved-to value.  If the original's only consuming use is the
  // move_value, then the original value's lifetime couldn't be shortened
  // anyway.
  //
  // Summary: !escaping(original)
  //       && singleConsumingUser(original) == move
  //       => redundant(mvi)
  //
  // Check this in two ways, one cheaper than the other.

  // First, avoid calling findPointerEscape(original).
  //
  // If the original value is not a phi (a phi's incoming values might have
  // escaping uses) and its only user is the move, then it doesn't escape. Also
  // if its only user is the move, then its only _consuming_ user is the move.
  auto *singleUser =
      original->getSingleUse() ? original->getSingleUse()->getUser() : nullptr;
  if (mvi == singleUser && !SILArgument::asPhi(original)) {
    assert(!findPointerEscape(original));
    assert(original->getSingleConsumingUse()->getUser() == mvi);
    // - !escaping(original)
    // - singleConsumingUser(original) == move
    return true;
  }

  // Second, call findPointerEscape(original).
  //
  // Explicitly check both
  // - !escaping(original)
  // - singleConsumingUser(original) == move
  auto originalHasEscape = findPointerEscape(original);
  auto *singleConsumingUser = original->getSingleConsumingUse()
                                  ? original->getSingleConsumingUse()->getUser()
                                  : nullptr;
  if (mvi == singleConsumingUser && !originalHasEscape) {
    return true;
  }

  // (3) Escaping matches?  (Expensive check, saved for last.)
  auto moveHasEscape = findPointerEscape(mvi);
  return moveHasEscape == originalHasEscape;
}
