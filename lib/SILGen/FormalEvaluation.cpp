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

using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//
//                             Formal Evaluation
//===----------------------------------------------------------------------===//

void FormalEvaluation::_anchor() {}

//===----------------------------------------------------------------------===//
//                      Shared Borrow Formal Evaluation
//===----------------------------------------------------------------------===//

void SharedBorrowFormalEvaluation::finish(SILGenFunction &gen) {
  gen.B.createEndBorrow(CleanupLocation::get(loc), borrowedValue,
                        originalValue);
}

//===----------------------------------------------------------------------===//
//                          Formal Evaluation Scope
//===----------------------------------------------------------------------===//

FormalEvaluationScope::FormalEvaluationScope(SILGenFunction &gen)
    : gen(gen), savedDepth(gen.FormalEvalContext.stable_begin()),
      wasInWritebackScope(gen.InWritebackScope) {
  if (gen.InInOutConversionScope) {
    savedDepth.reset();
    return;
  }
  gen.InWritebackScope = true;
}

FormalEvaluationScope::FormalEvaluationScope(FormalEvaluationScope &&o)
    : gen(o.gen), savedDepth(o.savedDepth),
      wasInWritebackScope(o.wasInWritebackScope) {
  o.savedDepth.reset();
}

void FormalEvaluationScope::popImpl() {
  // Pop the InWritebackScope bit.
  gen.InWritebackScope = wasInWritebackScope;

  // Check to see if there is anything going on here.

  auto &context = gen.FormalEvalContext;
  using iterator = FormalEvaluationContext::iterator;
  using stable_iterator = FormalEvaluationContext::stable_iterator;

  iterator unwrappedSavedDepth = context.find(savedDepth.getValue());
  iterator iter = context.begin();
  if (iter == unwrappedSavedDepth)
    return;

  // Save our start point to make sure that we are not adding any new cleanups
  // to the front of the stack.
  stable_iterator originalBegin = context.stable_begin();

  // Then working down the stack until we visit unwrappedSavedDepth...
  for (; iter != unwrappedSavedDepth; ++iter) {
    // Grab the next evaluation...
    FormalEvaluation &evaluation = *iter;

    // and deactivate the cleanup.
    gen.Cleanups.setCleanupState(evaluation.getCleanup(), CleanupState::Dead);

    // Attempt to diagnose problems where obvious aliasing introduces illegal
    // code. We do a simple N^2 comparison here to detect this because it is
    // extremely unlikely more than a few writebacks are active at once.
    if (evaluation.getKind() == FormalEvaluation::Exclusive) {
      iterator j = iter;
      ++j;

      for (; j != unwrappedSavedDepth; ++j) {
        FormalEvaluation &other = *j;
        if (other.getKind() != FormalEvaluation::Exclusive)
          continue;
        auto &lhs = static_cast<LValueWriteback &>(evaluation);
        auto &rhs = static_cast<LValueWriteback &>(other);
        lhs.diagnoseConflict(rhs, gen);
      }
    }

    // Claim the address of each and then perform the writeback from the
    // temporary allocation to the source we copied from.
    //
    // This evaluates arbitrary code, so it's best to be paranoid
    // about iterators on the context.
    evaluation.finish(gen);
  }

  // Then check that we did not add any additional cleanups to the beginning of
  // the stack...
  assert(originalBegin == context.stable_begin() &&
         "more writebacks placed onto context during writeback scope pop?!");

  // And then pop off all stack elements until we reach the savedDepth.
  context.pop(savedDepth.getValue());
}
