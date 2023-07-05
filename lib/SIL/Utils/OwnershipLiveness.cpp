//===--- OwnershipLiveness.cpp --------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/OwnershipLiveness.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/LLVM.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/Test.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

void OSSALiveness::print(llvm::raw_ostream &OS) const { liveness.print(OS); }

void OSSALiveness::dump() const { print(llvm::dbgs()); }

struct LinearLivenessVisitor :
  public OwnershipUseVisitor<LinearLivenessVisitor> {

  LinearLiveness &linearLiveness;

  LinearLivenessVisitor(LinearLiveness &linearLiveness):
    linearLiveness(linearLiveness){}

  bool handleUsePoint(Operand *use, UseLifetimeConstraint useConstraint) {
    linearLiveness.liveness.updateForUse(
      use->getUser(), useConstraint == UseLifetimeConstraint::LifetimeEnding);
    return true;
  }

  bool handlePointerEscape(Operand *use) {
    llvm_unreachable("a pointer escape cannot end a linear lifetime");
  }

  // handleOwnedPhi and handleOuterReborrow ends the linear lifetime.
  // By default, they are treated like a normal lifetime-ending use.

  bool handleGuaranteedForwardingPhi(Operand *use) {
    llvm_unreachable("guaranteed forwarding phi cannot end a linear lifetime");
  }

  bool handleInnerBorrow(BorrowingOperand borrowingOperand) {
    llvm_unreachable("an inner borrow cannot end a linear lifetime");
  }

  bool handleInnerAdjacentReborrow(SILArgument *reborrow) {
    llvm_unreachable("inner adjacent reborrows are not visited");
  }

  bool handleInnerReborrow(BorrowingOperand borrowingOperand) {
    llvm_unreachable("an inner borrow cannot end a linear lifetime");
  }

  bool handleScopedAddress(ScopedAddressValue scopedAddress) {
    llvm_unreachable("an scoped address cannot end a linear lifetime");
  }
};

LinearLiveness::LinearLiveness(SILValue def): OSSALiveness(def) {
  if (def->getOwnershipKind() != OwnershipKind::Owned) {
    BorrowedValue borrowedValue(def);
    assert(borrowedValue && borrowedValue.isLocalScope());
    (void)borrowedValue;
  }
}

void LinearLiveness::compute() {
  liveness.initializeDef(ownershipDef);
  LinearLivenessVisitor(*this).visitLifetimeEndingUses(ownershipDef);
}

struct InteriorLivenessVisitor :
  public OwnershipUseVisitor<InteriorLivenessVisitor> {

  InteriorLiveness &interiorLiveness;

  // If domInfo is nullptr, then InteriorLiveness never assumes dominance. As a
  // result it may report extra unenclosedPhis. In that case, any attempt to
  // create a new phi would result in an immediately redundant phi.
  const DominanceInfo *domInfo = nullptr;

  /// handleInnerScopeCallback may add uses to the inner scope, but it may not
  /// modify the use-list containing \p borrowingOperand. This callback can be
  /// used to ensure that the inner scope is complete before visiting its scope
  /// ending operands.
  ///
  /// An inner scope encapsulates any pointer escapes so visiting its interior
  /// uses is not necessary when visiting the outer scope's interior uses.
  InteriorLiveness::InnerScopeHandlerRef handleInnerScopeCallback;

  // State local to an invocation of
  // OwnershipUseVisitor::visitOwnershipUses().
  NodeSet visited;

  InteriorLivenessVisitor(
    InteriorLiveness &interiorLiveness,
    const DominanceInfo *domInfo,
    InteriorLiveness::InnerScopeHandlerRef handleInnerScope)
    : interiorLiveness(interiorLiveness),
      domInfo(domInfo),
      handleInnerScopeCallback(handleInnerScope),
      visited(interiorLiveness.ownershipDef->getFunction()) {}

  bool handleUsePoint(Operand *use, UseLifetimeConstraint useConstraint) {
    interiorLiveness.liveness.updateForUse(
      use->getUser(), useConstraint == UseLifetimeConstraint::LifetimeEnding);
    return true;
  }

  bool handlePointerEscape(Operand *use) {
    interiorLiveness.addressUseKind = AddressUseKind::PointerEscape;
    return true;
  }

  // handleOwnedPhi and handleOuterReborrow ends the linear lifetime.
  // By default, they are treated like a normal lifetime-ending use.

  bool handleGuaranteedForwardingPhi(Operand *use) {
    recursivelyVisitInnerGuaranteedPhi(PhiOperand(use), /*reborrow*/false);
    return true;
  }

  /// After this returns true, handleUsePoint will be called on the scope
  /// ending operands.
  ///
  /// Handles begin_borrow, load_borrow, store_borrow, begin_apply.
  bool handleInnerBorrow(BorrowingOperand borrowingOperand) {
    if (handleInnerScopeCallback) {
      auto value = borrowingOperand.getScopeIntroducingUserResult();
      if (value) {
        handleInnerScopeCallback(value);
      }
    }
    return true;
  }

  bool handleInnerAdjacentReborrow(SILArgument *reborrow) {
    if (handleInnerScopeCallback) {
      handleInnerScopeCallback(reborrow);
    }
    return true;
  }

  bool handleInnerReborrow(Operand *phiOper) {
    recursivelyVisitInnerGuaranteedPhi(PhiOperand(phiOper), /*reborrow*/true);
    return true;
  }

  /// After this returns true, handleUsePoint will be called on the scope
  /// ending operands.
  ///
  /// Handles store_borrow, begin_access.
  bool handleScopedAddress(ScopedAddressValue scopedAddress) {
    if (handleInnerScopeCallback) {
      handleInnerScopeCallback(scopedAddress.value);
    }
    return true;
  }

  void recursivelyVisitInnerGuaranteedPhi(PhiOperand phiOper, bool isReborrow);
};

// Dominating ownershipDef example: handleReborrow must continue visiting phi
// uses:
//
// bb0:
//  d1 = ...
//  cond_br bb1, bb2
// bb1:
//   b1 = borrow d1
//   br bb3(b1)
// bb2:
//   b2 = borrow d1
//   br bb3(b2)
// bb3(reborrow):
//   u1 = d1
//   u2 = reborrow
//   // can't move destroy above u2
//   destroy_value d1
//
// Dominating ownershipDef example: handleGuaranteedForwardingPhi must continue
// visiting phi uses:
//
// bb0:
//  b1 = borrow d1
//  cond_br bb1, bb2
// bb1:
//   p1 = projection b1
//   br bb3(p1)
// bb2:
//   p1 = projection b1
//   br bb3(p2)
// bb3(forwardingPhi):
//   u1 = b1
//   u2 = forwardingPhi
//   // can't move end_borrow above u2
//   end_borrow b1
//
// TODO: when phi's have a reborrow flag, remove \p isReborrow.
void InteriorLivenessVisitor::
recursivelyVisitInnerGuaranteedPhi(PhiOperand phiOper, bool isReborrow) {
  SILValue phiValue = phiOper.getValue();
  if (!visited.insert(phiValue))
    return;

  if (!visitEnclosingDefs(phiValue, [this](SILValue enclosingDef){
    // If the enclosing def is \p ownershipDef, return false to check
    // dominance.
    if (enclosingDef == interiorLiveness.ownershipDef)
      return false;

    // Otherwise, phiValue is enclosed by an outer adjacent phi, so its scope
    // does not contribute to the outer liveness. This phi will be recorded as a
    // regular use by the visitor, and this enclosing def will be visited as
    // separate lifetime-ending-use use. Return true to continue checking if any
    // other enclosing defs do not have an outer adjacent reborrow.
    return true;
  })) {
    // TODO: instead of relying on Dominance, we can reformulate this algorithm
    // to detect redundant phis, similar to the SSAUpdater.
    //
    // At least one enclosing def is ownershipDef. If ownershipDef dominates
    // phiValue, then this is consistent with a well-formed linear lifetime, and
    // the phi's uses directly contribute to ownershipDef's liveness.
    if (domInfo &&
        domInfo->dominates(interiorLiveness.ownershipDef->getParentBlock(),
                           phiValue->getParentBlock())) {
      if (isReborrow) {
        visitInnerBorrow(phiOper.getOperand());
      } else {
        visitInteriorUses(phiValue);
      }
      return;
    }
    // ownershipDef does not dominate this phi. Record it so the liveness
    // client can use this information to insert the missing outer adjacent phi.
    interiorLiveness.unenclosedPhis.push_back(phiValue);
  }
}

void InteriorLiveness::compute(const DominanceInfo *domInfo, InnerScopeHandlerRef handleInnerScope) {
  liveness.initializeDef(ownershipDef);
  addressUseKind = AddressUseKind::NonEscaping;
  InteriorLivenessVisitor(*this, domInfo, handleInnerScope)
    .visitInteriorUses(ownershipDef);
}

void InteriorLiveness::print(llvm::raw_ostream &OS) const {
  OSSALiveness::print(OS);

  switch (getAddressUseKind()) {
  case AddressUseKind::NonEscaping:
    OS << "Complete liveness\n";
    break;
  case AddressUseKind::PointerEscape:
    OS << "Incomplete liveness: Escaping address\n";
    break;
  case AddressUseKind::Unknown:
    OS << "Incomplete liveness: Unknown address use\n";
    break;
  }
  OS << "Unenclosed phis {\n";
  for (SILValue phi : getUnenclosedPhis()) {
    OS << "  " << phi;
  }
  OS << "}\n";
}

void InteriorLiveness::dump() const { print(llvm::dbgs()); }

// =============================================================================
// ExtendedLinearLiveness
// =============================================================================
  
struct ExtendedLinearLivenessVisitor
    : public OwnershipUseVisitor<ExtendedLinearLivenessVisitor> {

  ExtendedLinearLiveness &extendedLiveness;

  // State local to an invocation of
  // OwnershipUseVisitor::visitOwnershipUses().
  InstructionSet visited;

  ExtendedLinearLivenessVisitor(ExtendedLinearLiveness &extendedLiveness)
    : extendedLiveness(extendedLiveness),
      visited(extendedLiveness.ownershipDef->getFunction()) {}

  bool handleUsePoint(Operand *use, UseLifetimeConstraint useConstraint) {
    extendedLiveness.liveness.updateForUse(
      use->getUser(), useConstraint == UseLifetimeConstraint::LifetimeEnding);
    return true;
  }

  bool handlePointerEscape(Operand *use) {
    llvm_unreachable("a pointer escape cannot end a linear lifetime");
  }

  bool handleOwnedPhi(Operand *phiOper) {
    extendedLiveness.liveness.initializeDef(PhiOperand(phiOper).getValue());
    return true;
  }

  bool handleOuterReborrow(Operand *phiOper) {
    extendedLiveness.liveness.initializeDef(PhiOperand(phiOper).getValue());
    return true;
  }

  bool handleGuaranteedForwardingPhi(Operand *use) {
    llvm_unreachable("guaranteed forwarding phi cannot end a linear lifetime");
  }

  bool handleInnerBorrow(BorrowingOperand borrowingOperand) {
    llvm_unreachable("an inner borrow cannot end a linear lifetime");
  }

  bool handleInnerAdjacentReborrow(SILArgument *reborrow) {
    llvm_unreachable("inner adjacent reborrows are not visited");
  }

  bool handleInnerReborrow(BorrowingOperand borrowingOperand) {
    llvm_unreachable("an inner borrow cannot end a linear lifetime");
  }

  bool handleScopedAddress(ScopedAddressValue scopedAddress) {
    llvm_unreachable("an scoped address cannot end a linear lifetime");
  }
};

ExtendedLinearLiveness::ExtendedLinearLiveness(SILValue def)
  : ownershipDef(def), liveness(def->getFunction(), &discoveredBlocks) {
  if (def->getOwnershipKind() != OwnershipKind::Owned) {
    BorrowedValue borrowedValue(def);
    assert(borrowedValue && borrowedValue.isLocalScope());
    (void)borrowedValue;
  }
}

void ExtendedLinearLiveness::compute() {
  liveness.initializeDef(ownershipDef);
  for (auto defIter = liveness.defBegin(); defIter != liveness.defEnd();
       ++defIter) {
    auto *def = cast<ValueBase>(*defIter);
    ExtendedLinearLivenessVisitor(*this).visitLifetimeEndingUses(def);
  }
}

void ExtendedLinearLiveness::print(llvm::raw_ostream &OS) const {
  liveness.print(OS);
}

void ExtendedLinearLiveness::dump() const { print(llvm::dbgs()); }

} // namespace swift

namespace swift::test {
// Arguments:
// - SILValue: value
// Dumps:
// - function
// - the computed pruned liveness
// - the liveness boundary
static FunctionTest LinearLivenessTest("linear-liveness", [](auto &function,
                                                             auto &arguments,
                                                             auto &test) {
  SILValue value = arguments.takeValue();
  function.dump();
  llvm::dbgs() << "Linear liveness: " << value;
  LinearLiveness liveness(value);
  liveness.compute();
  liveness.print(llvm::outs());

  PrunedLivenessBoundary boundary;
  liveness.getLiveness().computeBoundary(boundary);
  boundary.print(llvm::outs());
});

// Arguments:
// - SILValue: value
// Dumps:
// - function
// - the computed pruned liveness
// - the liveness boundary
static FunctionTest
    InteriorLivenessTest("interior-liveness",
                         [](auto &function, auto &arguments, auto &test) {
                           SILValue value = arguments.takeValue();
                           function.dump();
                           llvm::dbgs() << "Interior liveness: " << value;
                           auto *domTree = test.getDominanceInfo();
                           InteriorLiveness liveness(value);
                           auto handleInnerScope = [](SILValue innerBorrow) {
                             llvm::outs() << "Inner scope: " << innerBorrow;
                           };
                           liveness.compute(domTree, handleInnerScope);
                           liveness.print(llvm::outs());

                           PrunedLivenessBoundary boundary;
                           liveness.getLiveness().computeBoundary(boundary);
                           boundary.print(llvm::outs());
                         });

// Arguments:
// - SILValue: value
// Dumps:
// - function
// - the computed pruned liveness
// - the liveness boundary
static FunctionTest ExtendedLinearLivenessTest(
    "extended-liveness", [](auto &function, auto &arguments, auto &test) {
      SILValue value = arguments.takeValue();
      function.dump();
      llvm::dbgs() << "Extended liveness: " << value;
      ExtendedLinearLiveness liveness(value);
      liveness.compute();
      liveness.print(llvm::outs());

      PrunedLivenessBoundary boundary;
      liveness.getLiveness().computeBoundary(boundary);
      boundary.print(llvm::outs());
    });
} // end namespace swift::test
