//===- BasicCalleeAnalysis.h - Determine callees per call site --*- C++ -*-===//
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

#ifndef SWIFT_SILOPTIMIZER_ANALYSIS_BASICCALLEEANALYSIS_H
#define SWIFT_SILOPTIMIZER_ANALYSIS_BASICCALLEEANALYSIS_H

#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SIL/CalleeCache.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/SILModule.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Allocator.h"

namespace swift {

class BasicCalleeAnalysis : public SILAnalysis {
  SILModule &M;
  std::unique_ptr<CalleeCache> Cache;

public:

  BasicCalleeAnalysis(SILModule *M)
      : SILAnalysis(SILAnalysisKind::BasicCallee), M(*M), Cache(nullptr) {}

  ~BasicCalleeAnalysis() {}

  static bool classof(const SILAnalysis *S) {
    return S->getKind() == SILAnalysisKind::BasicCallee;
  }

  /// Invalidate all information in this analysis.
  virtual void invalidate() override {
    Cache.reset();
  }

  /// Invalidate all of the information for a specific function.
  virtual void invalidate(SILFunction *F, InvalidationKind K) override {
    // No invalidation needed because the analysis does not cache anything
    // per call-site in functions.
  }

  /// Notify the analysis about a newly created function.
  virtual void notifyAddedOrModifiedFunction(SILFunction *F) override {
    // Nothing to be done because the analysis does not cache anything
    // per call-site in functions.
  }

  /// Notify the analysis about a function which will be deleted from the
  /// module.
  virtual void notifyWillDeleteFunction(SILFunction *F) override {
    // No invalidation needed because the analysis does not cache anything per
    // call-site in functions.
  }

  /// Notify the analysis about changed witness or vtables.
  virtual void invalidateFunctionTables() override {
    Cache.reset();
  }

  SWIFT_DEBUG_DUMP;

  void print(llvm::raw_ostream &os) const;

  void updateCache() {
    if (!Cache)
      Cache = std::make_unique<CalleeCache>(M);
  }

  CalleeList getCalleeList(FullApplySite FAS) {
    updateCache();
    return Cache->getCalleeList(FAS);
  }

  CalleeList getCalleeListOfValue(SILValue callee) {
    updateCache();
    return Cache->getCalleeListOfValue(callee);
  }

  CalleeList getDestructors(SILType type, bool isExactType) {
    updateCache();
    return Cache->getDestructors(type, isExactType);
  }

  MemoryBehavior getMemoryBehavior(ApplySite as, bool observeRetains);

  CalleeCache *getCalleeCache() { return Cache.get(); }
};

bool isDeinitBarrier(SILInstruction *const instruction,
                     BasicCalleeAnalysis *bca);

} // end namespace swift

#endif
