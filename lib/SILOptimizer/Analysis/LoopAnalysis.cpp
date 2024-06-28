//===--- LoopAnalysis.cpp - SIL Loop Analysis -----------------------------===//
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

#include "swift/Basic/Assertions.h"
#include "swift/SIL/Dominance.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/LoopAnalysis.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "llvm/Support/Debug.h"

using namespace swift;

std::unique_ptr<SILLoopInfo>
SILLoopAnalysis::newFunctionAnalysis(SILFunction *F) {
  assert(DA != nullptr && "Expect a valid dominance analysis");
  DominanceInfo *DT = DA->get(F);
  assert(DT != nullptr && "Expect a valid dominance information");
  return std::make_unique<SILLoopInfo>(F, DT);
}

void SILLoopAnalysis::initialize(SILPassManager *PM) {
  DA = PM->getAnalysis<DominanceAnalysis>();
}

SILAnalysis *swift::createLoopAnalysis(SILModule *M) {
  return new SILLoopAnalysis(M);
}
