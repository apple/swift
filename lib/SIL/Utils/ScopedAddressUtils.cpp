//===--- ScopedAddressUtils.cpp -------------------------------------------===//
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

#include "swift/SIL/ScopedAddressUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/Test.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "swift/SILOptimizer/Utils/OwnershipOptUtils.h"

using namespace swift;

void ScopedAddressValueKind::print(llvm::raw_ostream &os) const {
  switch (value) {
  case ScopedAddressValueKind::Invalid:
    llvm_unreachable("Using invalid case?!");
  case ScopedAddressValueKind::StoreBorrow:
    os << "StoreBorrow";
    return;
  case ScopedAddressValueKind::BeginAccess:
    os << "BeginAccess";
    return;
  }
  llvm_unreachable("Covered switch isn't covered?!");
}

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &os,
                                     ScopedAddressValueKind kind) {
  kind.print(os);
  return os;
}

bool ScopedAddressValue::isScopeEndingUse(Operand *op) const {
  switch (kind) {
  case ScopedAddressValueKind::Invalid:
    llvm_unreachable("Using invalid case?!");
  case ScopedAddressValueKind::StoreBorrow: {
    if (auto *endBorrow = dyn_cast<EndBorrowInst>(op->getUser())) {
      return endBorrow->getOperand() == value;
    }
    return false;
  }
  case ScopedAddressValueKind::BeginAccess: {
    if (auto *endAccess = dyn_cast<EndAccessInst>(op->getUser())) {
      return endAccess->getOperand() == value;
    }
    return false;
  }
  }
}

bool ScopedAddressValue::visitScopeEndingUses(
    function_ref<bool(Operand *)> visitor) const {
  switch (kind) {
  case ScopedAddressValueKind::Invalid:
    llvm_unreachable("Using invalid case?!");
  case ScopedAddressValueKind::StoreBorrow: {
    for (auto *use : value->getUses()) {
      if (isa<EndBorrowInst>(use->getUser())) {
        if (!visitor(use))
          return false;
      }
    }
    return true;
  }
  case ScopedAddressValueKind::BeginAccess: {
    for (auto *use : value->getUses()) {
      if (isa<EndAccessInst>(use->getUser())) {
        if (!visitor(use))
          return false;
      }
    }
    return true;
  }
  }
}

// Note: This is used to fixup an incomplete address scope, so cannot assume the
// scope's lifetime is already complete. Therefore, it needs to transitively
// process all address uses.
//
// FIXME: users of this should use the standard recursive lifetime completion
// utility. Otherwise dealing with nested incomplete lifetimes becomes
// expensive and complex. e.g.
//
//   %storeBorrow = store_borrow %_ to %adr
//   %loadBorrow  = load_borrow %storeBorrow
//   apply %f(%loadBorrow) : $@convention(thin) (...) -> Never
//   unreachable
//
AddressUseKind ScopedAddressValue::computeTransitiveLiveness(
    SSAPrunedLiveness &liveness) const {
  liveness.initializeDef(value);
  return updateTransitiveLiveness(liveness);
}

namespace swift::test {
// Arguments:
// - SILValue: value to a analyze
// Dumps:
// - the liveness result and boundary
static FunctionTest ScopedAddressLivenessTest(
    "scoped-address-liveness", [](auto &function, auto &arguments, auto &test) {
      auto value = arguments.takeValue();
      assert(!arguments.hasUntaken());
      llvm::outs() << "Scoped address analysis: " << value;

      ScopedAddressValue scopedAddress(value);
      assert(scopedAddress);

      SmallVector<SILBasicBlock *, 8> discoveredBlocks;
      SSAPrunedLiveness liveness(value->getFunction(), &discoveredBlocks);
      scopedAddress.computeTransitiveLiveness(liveness);
      liveness.print(llvm::outs());

      PrunedLivenessBoundary boundary;
      liveness.computeBoundary(boundary);
      boundary.print(llvm::outs());
    });
} // end namespace swift::test

AddressUseKind ScopedAddressValue::updateTransitiveLiveness(
    SSAPrunedLiveness &liveness) const {
  SmallVector<Operand *, 4> uses;
  // Collect all uses that need to be enclosed by the scope.
  auto addressKind = findTransitiveUsesForAddress(value, &uses);
  for (auto *use : uses) {
    if (isScopeEndingUse(use))
      continue;

    // Update all collected uses as non-lifetime ending.
    liveness.updateForUse(use->getUser(), /* lifetimeEnding */ false);
  }
  visitScopeEndingUses([&](Operand *endOp) {
    liveness.updateForUse(endOp->getUser(), /* isLifetimeEnding */ true);
    return true;
  });
  return addressKind;
}

void ScopedAddressValue::createScopeEnd(SILBasicBlock::iterator insertPt,
                                        SILLocation loc) const {
  switch (kind) {
  case ScopedAddressValueKind::StoreBorrow: {
    SILBuilderWithScope(insertPt).createEndBorrow(loc, value);
    return;
  }
  case ScopedAddressValueKind::BeginAccess: {
    SILBuilderWithScope(insertPt).createEndAccess(loc, value, false);
    return;
  }
  case ScopedAddressValueKind::Invalid:
    llvm_unreachable("Using invalid case?!");
  }
}

void ScopedAddressValue::endScopeAtLivenessBoundary(
    SSAPrunedLiveness *liveness) const {
  // If no users exist, create scope ending instruction immediately after the
  // scoped address value.
  if (liveness->empty()) {
    createScopeEnd(value->getNextInstruction()->getIterator(),
                   RegularLocation::getAutoGeneratedLocation());
    return;
  }

  PrunedLivenessBoundary scopedAddressBoundary;
  liveness->computeBoundary(scopedAddressBoundary);
  // Go over the boundary and create scope ending instructions.
  scopedAddressBoundary.visitInsertionPoints(
      [&](SILBasicBlock::iterator insertPt) {
        createScopeEnd(insertPt, RegularLocation::getAutoGeneratedLocation());
      });
}

bool swift::hasOtherStoreBorrowsInLifetime(StoreBorrowInst *storeBorrow,
                                           SSAPrunedLiveness *liveness,
                                           DeadEndBlocks *deadEndBlocks) {
  SmallVector<StoreBorrowInst *, 4> otherStoreBorrows;
  // Collect all other store_borrows to the destination of \p storeBorrow
  for (auto *destUse : storeBorrow->getDest()->getUses()) {
    if (auto *user = dyn_cast<StoreBorrowInst>(destUse->getUser())) {
      if (user == storeBorrow) {
        continue;
      }
      otherStoreBorrows.push_back(user);
    }
  }

  for (auto *otherStoreBorrow : otherStoreBorrows) {
    // Return true, if otherStoreBorrow was in \p storeBorrow's scope
    if (liveness->isWithinBoundary(otherStoreBorrow)) {
      return true;
    }
  }
  return false;
}

void ScopedAddressValue::print(llvm::raw_ostream &os) const {
  os << "ScopedAddressIntroducingValue:\n"
        "Kind: "
     << kind
     << "\n"
        "Value: "
     << value;
}

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &os,
                                     const ScopedAddressValue &value) {
  value.print(os);
  return os;
}
