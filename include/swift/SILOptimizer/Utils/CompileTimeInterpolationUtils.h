//===--- CompileTimeInterpolationUtils.h ----------------------------------===//
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

// Utilities for the compile-time string interpolation approach used by the
// OSLogOptimization pass.

#ifndef SWIFT_SILOPTIMIZER_COMPILE_TIME_INTERPOLATION_H
#define SWIFT_SILOPTIMIZER_COMPILE_TIME_INTERPOLATION_H

#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILConstants.h"
#include "swift/SILOptimizer/Utils/ConstExpr.h"

namespace swift {

/// Decide if the given instruction (which could possibly be a call) should
/// be constant evaluated.
///
/// \returns true iff the given instruction is not a call or if it is, it calls
/// a known constant-evaluable function such as string append etc., or calls
/// a function annotate as "constant_evaluable".
bool shouldAttemptEvaluation(SILInstruction *inst);

/// Skip or evaluate the given instruction based on the evaluation policy and
/// handle errors. The policy is to evaluate all non-apply instructions as well
/// as apply instructions that are marked as "constant_evaluable".
std::pair<llvm::Optional<SILBasicBlock::iterator>,
          llvm::Optional<SymbolicValue>>
evaluateOrSkip(ConstExprStepEvaluator &stepEval, SILBasicBlock::iterator instI);

/// Given a vector of SILValues \p worklist, compute the set of transitive
/// users of these values (excluding the worklist values) by following the
/// use-def chain starting at value. Note that this function does not follow
/// use-def chains though branches.
void getTransitiveUsers(SILInstructionResultArray values,
                        SmallVectorImpl<SILInstruction *> &users);
} // end namespace swift
#endif
