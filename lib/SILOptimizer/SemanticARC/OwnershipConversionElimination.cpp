//===--- OwnershipConversionElimination.cpp -------------------------------===//
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

#include "SemanticARC/SemanticARCOpts.h"
#include "SemanticARCOptVisitor.h"
#include "swift/SIL/LinearLifetimeChecker.h"

using namespace swift;
using namespace semanticarc;

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

bool SemanticARCOptVisitor::visitUncheckedOwnershipConversionInst(
    UncheckedOwnershipConversionInst *uoci) {
  // Return false if we are supposed to only be running guaranteed opts.
  if (ctx.onlyMandatoryOpts)
    return false;

  // Then check if we are running tests and shouldn't perform this optimization
  // since we are testing something else.
  if (!ctx.shouldPerform(ARCTransformKind::OwnershipConversionElimPeephole))
    return false;

  // Otherwise, shrink our state space so that we only consider conversions from
  // owned or guaranteed to unowned. These are always legal and it is sometimes
  // convenient to insert them to avoid forwarding issues when RAUWing
  // values. So we eliminate them here.
  if (uoci->getConversionOwnershipKind() != OwnershipKind::Unowned)
    return false;

  auto op = uoci->getOperand();
  auto opKind = op.getOwnershipKind();
  if (opKind != OwnershipKind::Owned && opKind != OwnershipKind::Guaranteed)
    return false;

  // Ok, we can perform our optimization. First go through all of the uses of
  // uoci and see if they can accept our operand without any changes and that
  // they do not consume values.
  SmallVector<Operand *, 8> newUses;
  for (auto *use : uoci->getUses()) {
    if (use->isLifetimeEnding() || !use->canAcceptKind(opKind))
      return false;
    newUses.push_back(use);
  }

  // Ok, now we need to perform our lifetime check.
  SmallVector<Operand *, 8> consumingUses(op->getConsumingUses());
  LinearLifetimeChecker checker(ctx.getDeadEndBlocks());
  if (!checker.validateLifetime(op, consumingUses, newUses))
    return false;

  // Otherwise, we can perform our rauw.
  eraseAndRAUWSingleValueInstruction(uoci, op);
  return true;
}
