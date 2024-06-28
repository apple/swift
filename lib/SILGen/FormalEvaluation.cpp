//===--- FormalEvaluation.cpp ---------------------------------------------===//
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

#include "FormalEvaluation.h"
#include "LValue.h"
#include "SILGenFunction.h"
#include "swift/Basic/Assertions.h"

using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//
//                               Formal Access
//===----------------------------------------------------------------------===//

void FormalAccess::_anchor() {}

void FormalAccess::verify(SILGenFunction &SGF) const {
#ifndef NDEBUG
  // If this access was already finished, continue. This can happen if an
  // owned formal access was forwarded.
  if (isFinished()) {
    assert(getKind() == FormalAccess::Owned &&
           "Only owned formal accesses should be forwarded.");
    // We can not check that our cleanup is actually dead since the cleanup
    // may have been popped at this point and the stack may have new values.
    return;
  }

  assert(!isFinished() && "Can not finish a formal access cleanup "
         "twice");

  // Now try to look up the cleanup handle of the formal access.
  SGF.Cleanups.checkIterator(getCleanup());
#endif
}

//===----------------------------------------------------------------------===//
//                      Shared Borrow Formal Evaluation
//===----------------------------------------------------------------------===//

void SharedBorrowFormalAccess::finishImpl(SILGenFunction &SGF) {
  SGF.B.createEndBorrow(CleanupLocation(loc), borrowedValue);
}

//===----------------------------------------------------------------------===//
//                             OwnedFormalAccess
//===----------------------------------------------------------------------===//

void OwnedFormalAccess::finishImpl(SILGenFunction &SGF) {
  auto cleanupLoc = CleanupLocation(loc);
  if (value->getType().isAddress())
    SGF.B.createDestroyAddr(cleanupLoc, value);
  else
    SGF.B.emitDestroyValueOperation(cleanupLoc, value);
}

//===----------------------------------------------------------------------===//
//                          Formal Evaluation Scope
//===----------------------------------------------------------------------===//

FormalEvaluationScope::FormalEvaluationScope(SILGenFunction &SGF)
    : SGF(SGF), savedDepth(SGF.FormalEvalContext.stable_begin()),
      previous(SGF.FormalEvalContext.innermostScope),
      wasInInOutConversionScope(SGF.InInOutConversionScope) {
  if (wasInInOutConversionScope) {
    savedDepth.reset();
    assert(isPopped());
    return;
  }
  SGF.FormalEvalContext.innermostScope = this;
}

FormalEvaluationScope::FormalEvaluationScope(FormalEvaluationScope &&o)
    : SGF(o.SGF), savedDepth(o.savedDepth), previous(o.previous),
      wasInInOutConversionScope(o.wasInInOutConversionScope) {

  // Replace the scope in the active-scope chain if it's present.
  if (!o.isPopped()) {
    for (auto c = &SGF.FormalEvalContext.innermostScope; ; c = &(*c)->previous){
      if (*c == &o) {
        *c = this;
        break;
      }
    }
  }

  o.savedDepth.reset();
  assert(o.isPopped());
}

void FormalEvaluationScope::popImpl() {
  auto &context = SGF.FormalEvalContext;

  // Remove the innermost scope from the chain.
  assert(context.innermostScope == this &&
         "popping formal-evaluation scopes out of order");
  context.innermostScope = previous;

  auto endDepth = *savedDepth;

  // Check to see if there is anything going on here.
  if (endDepth == context.stable_begin())
    return;

#ifndef NDEBUG
  // Verify that all the accesses are valid.
  for (auto i = context.begin(), e = context.find(endDepth); i != e; ++i) {
    i->verify(SGF);
  }
#endif

  // Save our start point to make sure that we are not adding any new cleanups
  // to the front of the stack.
  auto originalBegin = context.stable_begin();

  // Then working down the stack until we visit unwrappedSavedDepth...
  auto i = originalBegin;
  do {
    // Grab the next evaluation.
    FormalAccess &access = context.findAndAdvance(i);

    // If this access was already finished, continue. This can happen if an
    // owned formal access was forwarded.
    if (access.isFinished()) {
      assert(access.getKind() == FormalAccess::Owned &&
             "Only owned formal accesses should be forwarded.");
      // We can not check that our cleanup is actually dead since the cleanup
      // may have been popped at this point and the stack may have new values.
      continue;
    }

    assert(!access.isFinished() && "Can not finish a formal access cleanup "
                                   "twice");

    // Set the finished bit to appease various invariants.
    access.setFinished();

    // Deactivate the cleanup.
    if (SGF.B.hasValidInsertionPoint()) {
      SGF.Cleanups.setCleanupState(access.getCleanup(), CleanupState::Dead);
    }

    // Attempt to diagnose problems where obvious aliasing introduces illegal
    // code. We do a simple N^2 comparison here to detect this because it is
    // extremely unlikely more than a few writebacks are active at once.
    if (access.getKind() == FormalAccess::Exclusive) {
      // Note that we already advanced 'iter' above, so we can just start
      // iterating from there.  Also, this doesn't invalidate the iterators.
      for (auto j = context.find(i), je = context.find(endDepth); j != je; ++j){
        FormalAccess &other = *j;
        if (other.getKind() != FormalAccess::Exclusive)
          continue;
        auto &lhs = static_cast<ExclusiveBorrowFormalAccess &>(access);
        auto &rhs = static_cast<ExclusiveBorrowFormalAccess &>(other);
        lhs.diagnoseConflict(rhs, SGF);
      }
    }

    // Claim the address of each and then perform the writeback from the
    // temporary allocation to the source we copied from.
    //
    // This evaluates arbitrary code, so it's best to be paranoid
    // about iterators on the context.
    if (SGF.B.hasValidInsertionPoint()) {
      DiverseValueBuffer<FormalAccess> copiedAccess(access);
      copiedAccess.getCopy().finish(SGF);
    }

  } while (i != endDepth);

  // Then check that we did not add any additional cleanups to the beginning of
  // the stack...
  assert(originalBegin == context.stable_begin() &&
         "pushed more formal evaluations while popping formal evaluations?!");

  // And then pop off all stack elements until we reach the savedDepth.
  context.pop(endDepth);
}

void FormalEvaluationScope::verify() const {
  // Walk up the stack to the saved depth.
  auto &context = SGF.FormalEvalContext;
  for (auto i = context.begin(), e = context.find(*savedDepth); i != e; ++i) {
    i->verify(SGF);
  }
}

//===----------------------------------------------------------------------===//
//                         Formal Evaluation Context
//===----------------------------------------------------------------------===//

void FormalEvaluationContext::dump(SILGenFunction &SGF) {
  for (auto II = begin(), IE = end(); II != IE; ++II) {
    FormalAccess &access = *II;
    SGF.Cleanups.dump(access.getCleanup());
  }
}
