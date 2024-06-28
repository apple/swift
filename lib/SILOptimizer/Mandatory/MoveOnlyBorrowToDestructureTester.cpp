//===--- MoveOnlyBorrowToDestructureTester.cpp ----------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file This is a pass that converts the borrow + gep pattern to destructures
/// or emits an error if it cannot be done. It is assumed that it runs
/// immediately before move checking of objects runs. This ensures that the move
/// checker does not need to worry about this problem and instead can just check
/// that the newly inserted destructures do not cause move only errors.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-move-only-checker"

#include "MoveOnlyBorrowToDestructureUtils.h"
#include "MoveOnlyDiagnostics.h"
#include "MoveOnlyObjectCheckerUtils.h"
#include "MoveOnlyUtils.h"

#include "swift/Basic/Assertions.h"
#include "swift/Basic/BlotSetVector.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "llvm/ADT/ArrayRef.h"

using namespace swift;
using namespace swift::siloptimizer;
using namespace swift::siloptimizer::borrowtodestructure;

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

static bool runTransform(
    SILFunction *fn,
    ArrayRef<MarkUnresolvedNonCopyableValueInst *> moveIntroducersToProcess,
    PostOrderAnalysis *poa, DiagnosticEmitter &diagnosticEmitter) {
  IntervalMapAllocator allocator;
  bool madeChange = false;
  while (!moveIntroducersToProcess.empty()) {
    auto *mmci = moveIntroducersToProcess.back();
    moveIntroducersToProcess = moveIntroducersToProcess.drop_back();

    BorrowToDestructureTransform transform(allocator, mmci, mmci,
                                           diagnosticEmitter, poa);
    transform.transform();
    madeChange = true;
  }

  return madeChange;
}

namespace {

class MoveOnlyBorrowToDestructureTransformPass : public SILFunctionTransform {
  void run() override {
    auto *fn = getFunction();

    // Don't rerun diagnostics on deserialized functions.
    if (getFunction()->wasDeserializedCanonical())
      return;

    assert(fn->getModule().getStage() == SILStage::Raw &&
           "Should only run on Raw SIL");

    LLVM_DEBUG(llvm::dbgs()
               << "===> MoveOnlyBorrowToDestructureTransform. Visiting: "
               << fn->getName() << '\n');

    auto *postOrderAnalysis = getAnalysis<PostOrderAnalysis>();

    llvm::SmallSetVector<MarkUnresolvedNonCopyableValueInst *, 32>
        moveIntroducersToProcess;
    DiagnosticEmitter diagnosticEmitter(getFunction());

    unsigned diagCount = diagnosticEmitter.getDiagnosticCount();
    bool madeChange =
        searchForCandidateObjectMarkUnresolvedNonCopyableValueInsts(
            getFunction(), moveIntroducersToProcess, diagnosticEmitter);
    if (madeChange) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }

    if (diagCount != diagnosticEmitter.getDiagnosticCount()) {
      if (cleanupNonCopyableCopiesAfterEmittingDiagnostic(fn))
        invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
      return;
    }

    diagCount = diagnosticEmitter.getDiagnosticCount();
    auto introducers = llvm::ArrayRef(moveIntroducersToProcess.begin(),
                                      moveIntroducersToProcess.end());
    if (runTransform(fn, introducers, postOrderAnalysis, diagnosticEmitter)) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }

    if (diagCount != diagnosticEmitter.getDiagnosticCount()) {
      if (cleanupNonCopyableCopiesAfterEmittingDiagnostic(fn))
        invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }
  }
};

} // namespace

SILTransform *swift::createMoveOnlyBorrowToDestructureTransform() {
  return new MoveOnlyBorrowToDestructureTransformPass();
}
