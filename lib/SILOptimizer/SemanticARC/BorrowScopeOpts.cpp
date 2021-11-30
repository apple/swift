//===--- BorrowScopeOpts.cpp ----------------------------------------------===//
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
/// Optimizations that attempt to simplify and or eliminate borrow scopes. Today
/// we only eliminate scopes, but we could also eliminate redundant scopes by
/// converting struct_extract operations to use destructure operations.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-semantic-arc-opts"

#include "Context.h"
#include "SemanticARCOptVisitor.h"
#include "llvm/ADT/iterator_range.h"

using namespace swift;
using namespace swift::semanticarc;

bool SemanticARCOptVisitor::visitBeginBorrowInst(BeginBorrowInst *bbi) {
  using OwnershipMergeInst = AllArgOwnershipForwardingSingleValueInst;

  // Quickly check if we are supposed to perform this transformation.
  if (!ctx.shouldPerform(ARCTransformKind::RedundantBorrowScopeElimPeephole))
    return false;

  // Lexical borrow scopes must remain in order to ensure that value lifetimes
  // are not observably shortened.
  if (bbi->isLexical())
    return false;

  LLVM_DEBUG(llvm::dbgs() << "Visiting: " << *bbi);

  auto kind = bbi->getOperand().getOwnershipKind();
  SmallVector<Operand *, 8> initialAggregatesUseList;
  SmallVector<EndBorrowInst *, 16> endBorrows;
  for (auto *op : bbi->getUses()) {
    if (!op->isLifetimeEnding()) {
      // Make sure that this operand can accept our arguments kind.
      if (op->canAcceptKind(kind))
        continue;

      if (auto *mergeInst = dyn_cast<OwnershipMergeInst>(op->getUser())) {
        if (mergeInst->hasOneUse() &&
            mergeInst->forwardsSingleNonTrivialArgument()) {
          initialAggregatesUseList.push_back(op);
          continue;
        }
      }

      // If we can't accept our argument and we don't have a simple mergeInst
      // that we might be able to hoist a copy out of.
      return false;
    }

    // Otherwise, this borrow is being consumed. See if our consuming inst is an
    // end_borrow. If it isn't, then return false, this scope is
    // needed. Otherwise, add the end_borrow to our list of end borrows.
    auto *ebi = dyn_cast<EndBorrowInst>(op->getUser());
    if (!ebi) {
      return false;
    }
    endBorrows.push_back(ebi);
  }

  if (initialAggregatesUseList.empty()) {
    LLVM_DEBUG(llvm::dbgs() << "    Eliminating!\n");
    // At this point, we know that the begin_borrow's operand can be
    // used as an argument to all non-end borrow uses. Eliminate the
    // begin borrow and end borrows.
    while (!endBorrows.empty()) {
      auto *ebi = endBorrows.pop_back_val();
      eraseInstruction(ebi);
    }

    eraseAndRAUWSingleValueInstruction(bbi, bbi->getOperand());
    return true;
  }

  // See if we can eliminate all of our uses that cannot escape. This currently
  // just supports single forwarding operations. We would need a larger, non
  // peephole optimization to handle more complex cases. But at least this
  // allows for arbitrary single non-trivial element construction.
  SmallVector<OwnershipMergeInst *, 32> simpleAggregateInstsToFix;
  SmallVector<CopyValueInst *, 32> copiesToRemove;

  // We already checked earlier while we populated initialAggregatesUseList has
  // one use and forwards single non trivial argument, so we do not need to
  // check it here again.
  SmallVector<Operand *, 32> worklist;
  for (auto *use : initialAggregatesUseList) {
    bool foundCopyUse = false;
    worklist.push_back(use);
    while (!worklist.empty()) {
      auto *use = worklist.pop_back_val();

      // If we have an aggregate that forwards our value that is only consumed
      // once, look through it. This from an ownership perspective creates a
      // linear chain that we can fix up.
      if (auto *aggregate = dyn_cast<OwnershipMergeInst>(use->getUser())) {
        if (auto *oneUse = aggregate->getSingleUse()) {
          if (aggregate->forwardsSingleNonTrivialArgument()) {
            simpleAggregateInstsToFix.push_back(aggregate);
            worklist.push_back(oneUse);
            continue;
          }
        }
      }

      // Otherwise, see if we have a single copy_value. We support this
      // case. Otherwise, return false. We are being very conservative on
      // purpose so we handle a very specific type of pattern.
      if (auto *cvi = dyn_cast<CopyValueInst>(use->getUser())) {
        if (foundCopyUse)
          return false;
        foundCopyUse = true;
        copiesToRemove.push_back(cvi);
        continue;
      }

      return false;
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "    Eliminating!\n");

  for (auto *use : initialAggregatesUseList) {
    auto *user = use->getUser();
    SILBuilderWithScope builder(user->getIterator());
    auto *cvi = builder.createCopyValue(user->getLoc(), bbi->getOperand());
    getCallbacks().setUseValue(use, cvi);
    cast<OwnershipMergeInst>(user)->setForwardingOwnershipKind(
        OwnershipKind::Owned);
  }

  while (!simpleAggregateInstsToFix.empty()) {
    auto *inst = simpleAggregateInstsToFix.pop_back_val();
    inst->setForwardingOwnershipKind(OwnershipKind::Owned);
  }

  while (!copiesToRemove.empty()) {
    auto *cvi = copiesToRemove.pop_back_val();
    eraseAndRAUWSingleValueInstruction(cvi, cvi->getOperand());
  }

  // At this point, we know that the begin_borrow's operand can be used as an
  // argument to all non-end borrow uses. We have also handled all of the cases
  // of copy aggregate formation if it was possible. Now it is safe to eliminate
  // the begin borrow and end borrows.
  while (!endBorrows.empty()) {
    auto *ebi = endBorrows.pop_back_val();
    eraseInstruction(ebi);
  }

  eraseAndRAUWSingleValueInstruction(bbi, bbi->getOperand());

  return true;
}
