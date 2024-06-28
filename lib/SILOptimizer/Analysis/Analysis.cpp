//===--- Analysis.cpp - Swift Analysis ------------------------------------===//
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

#define DEBUG_TYPE "sil-analysis"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/AST/Module.h"
#include "swift/AST/SILOptions.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/Analysis/ClassHierarchyAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/IVAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/Analysis/ProtocolConformanceAnalysis.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"

using namespace swift;

void SILAnalysis::verifyFunction(SILFunction *F) {
  // Only functions with bodies can be analyzed by the analysis.
  assert(F->isDefinition() && "Can't analyze external functions");
}

SILAnalysis *swift::createDominanceAnalysis(SILModule *) {
  return new DominanceAnalysis();
}

SILAnalysis *swift::createPostDominanceAnalysis(SILModule *) {
  return new PostDominanceAnalysis();
}

SILAnalysis *swift::createInductionVariableAnalysis(SILModule *M) {
  return new IVAnalysis(M);
}

SILAnalysis *swift::createPostOrderAnalysis(SILModule *M) {
  return new PostOrderAnalysis();
}

SILAnalysis *swift::createClassHierarchyAnalysis(SILModule *M) {
  return new ClassHierarchyAnalysis(M);
}

SILAnalysis *swift::createBasicCalleeAnalysis(SILModule *M) {
  return new BasicCalleeAnalysis(M);
}

SILAnalysis *swift::createProtocolConformanceAnalysis(SILModule *M) {
  return new ProtocolConformanceAnalysis(M);
}

SILAnalysis *swift::createNonLocalAccessBlockAnalysis(SILModule *M) {
  return new NonLocalAccessBlockAnalysis();
}
