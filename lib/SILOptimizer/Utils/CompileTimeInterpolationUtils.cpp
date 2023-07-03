//===--- CompileTimeInterpolationUtils.cpp -------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SILOptimizer/Utils/CompileTimeInterpolationUtils.h"
#include "swift/AST/ASTContext.h"
#include "swift/SIL/SILFunction.h"

using namespace swift;

bool swift::shouldAttemptEvaluation(SILInstruction *inst) {
  auto *apply = dyn_cast<ApplyInst>(inst);
  if (!apply)
    return true;
  SILFunction *calleeFun = apply->getCalleeFunction();
  if (!calleeFun)
    return false;
  return isConstantEvaluable(calleeFun);
}

std::pair<llvm::Optional<SILBasicBlock::iterator>,
          llvm::Optional<SymbolicValue>>
swift::evaluateOrSkip(ConstExprStepEvaluator &stepEval,
                      SILBasicBlock::iterator instI) {
  SILInstruction *inst = &(*instI);

  // Note that skipping a call conservatively approximates its effects on the
  // interpreter state.
  if (shouldAttemptEvaluation(inst)) {
    return stepEval.tryEvaluateOrElseMakeEffectsNonConstant(instI);
  }
  return stepEval.skipByMakingEffectsNonConstant(instI);
}

void swift::getTransitiveUsers(SILInstructionResultArray values,
                               SmallVectorImpl<SILInstruction *> &users) {
  // Collect the instructions that are data dependent on the value using a
  // fix point iteration.
  SmallPtrSet<SILInstruction *, 16> visited;
  SmallVector<SILValue, 16> worklist;
  llvm::copy(values, std::back_inserter(worklist));
  while (!worklist.empty()) {
    SILValue currVal = worklist.pop_back_val();
    for (Operand *use : currVal->getUses()) {
      SILInstruction *user = use->getUser();
      if (visited.count(user))
        continue;
      visited.insert(user);
      llvm::copy(user->getResults(), std::back_inserter(worklist));
    }
  }
  users.append(visited.begin(), visited.end());
}
