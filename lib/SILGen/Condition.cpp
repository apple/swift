//===--- Condition.cpp - Implements the SILGen Condition class ------------===//
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

#include "Condition.h"
#include "Initialization.h"
#include "ManagedValue.h"
#include "RValue.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILFunction.h"
using namespace swift;
using namespace Lowering;

void Condition::enter(SILGenFunction &SGF, SILBasicBlock *destBB) {
  assert(destBB && "Cannot reenter a finished block.");
  SGF.B.emitBlock(destBB);
}

/// Extract the last SILLocation used in BB.
static SILLocation getContinuationLoc(SILBasicBlock &BB, SILLocation Fallback) {
  for (auto I = BB.rbegin(); I != BB.rend(); ++I)
    if (auto L = I->getLoc())
      return L;
  return Fallback;
}
void Condition::exit(SILGenFunction &SGF, SILBasicBlock *destBB,
                     ArrayRef<SILValue> Args) {
  // If the current point it reachable, branch to the continuation block.
  if (!SGF.B.hasValidInsertionPoint())
    return;
  
  SGF.B.createBranch(getContinuationLoc(*SGF.B.getInsertionBB(), Loc),
                     ContBB, Args);
}

SILBasicBlock *Condition::complete(SILGenFunction &SGF) {
  assert(!TrueBB && "enterTrue is always called.");
  if (FalseBB) {
    assert(ContBB->getNumArguments() == 0 &&
           "block arguments require a non-empty false path.");
    SILGenBuilder(SGF.B, FalseBB).createBranch(Loc, ContBB);
    FalseBB = nullptr;
  }
  SGF.B.emitBlock(ContBB);
  return ContBB;
}

ConditionalValue::ConditionalValue(SILGenFunction &SGF, SGFContext C,
                                   SILLocation loc,
                                   const TypeLowering &valueTL)
  : SGF(SGF), tl(valueTL), contBB(SGF.createBasicBlock()), loc(loc)
{
  if (tl.isAddressOnly()) {
    // If the result type is address-only, get a result buffer for it.
    result = SGF.getBufferForExprResult(loc, tl.getLoweredType(), C);
  } else {
    // Otherwise, add a BB arg to the continuation block to receive loadable
    // result.
    result =
        contBB->createPhiArgument(tl.getLoweredType(), OwnershipKind::Owned);
  }
}

SGFContext ConditionalValue::enterBranch(SILBasicBlock *bb) {
  if (bb) {
    assert(!SGF.B.hasValidInsertionPoint() && "already in a branch");
    SGF.B.emitBlock(bb);
  }
  
  assert(!scope.has_value() && "already have a scope");
  // Start a scope for the current branch.
  scope.emplace(SGF.Cleanups, CleanupLocation(loc));

  // Code emitted in the branch can emit into our buffer for address-only
  // conditionals.
  if (tl.isAddressOnly()) {
    assert(!currentInitialization && "already have an initialization?!");
    currentInitialization = SGF.useBufferAsTemporary(result, tl);
    return SGFContext(currentInitialization.get());
  }

  /// TODO: We might be able to coordinate AllowPlusZero across conditionals
  /// if all branches of the conditional can actually produce a +0 result.
  return SGFContext();
}

void ConditionalValue::exitBranch(RValue &&condResult) {
  assert(scope.has_value() && "no current scope?!");
  if (tl.isAddressOnly()) {
    // Transfer the result into our buffer if it wasn't emitted in-place
    // already.
    assert(currentInitialization && "no current initialization?!");
    std::move(condResult).forwardInto(SGF, loc,
                                      currentInitialization.release());
    scope.reset();
    SGF.B.createBranch(loc, contBB);
  } else {
    SILValue resultVal = std::move(condResult).forwardAsSingleValue(SGF, loc);
    // Branch with the result as a BB argument.
    scope.reset();
    SGF.B.createBranch(loc, contBB, resultVal);
  }
}

ManagedValue ConditionalValue::complete() {
  assert(!SGF.B.hasValidInsertionPoint() && "still in a branch");
  assert(!scope && "still in a branch scope");
  assert(!currentInitialization && "still in a branch initialization");
  SGF.B.emitBlock(contBB);
  return SGF.emitManagedRValueWithCleanup(result);
}
