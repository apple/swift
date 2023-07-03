//===--- MoveOnlyTempAllocationFromLetTester.cpp --------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file A simple tester for the utility function
/// eliminateTemporaryAllocationsFromLet that allows us to write separate SIL
/// test cases for the utility.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-move-only-checker"

#include "MoveOnlyAddressCheckerUtils.h"
#include "MoveOnlyDiagnostics.h"
#include "MoveOnlyUtils.h"

#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;
using namespace swift::siloptimizer;

namespace {

struct MoveOnlyTempAllocationFromLetTester : SILFunctionTransform {
  void run() override {
    auto *fn = getFunction();

    // Only run this pass if the move only language feature is enabled.
    if (!fn->getASTContext().supportsMoveOnlyTypes())
      return;

    // Don't rerun diagnostics on deserialized functions.
    if (getFunction()->wasDeserializedCanonical())
      return;

    assert(fn->getModule().getStage() == SILStage::Raw &&
           "Should only run on Raw SIL");

    LLVM_DEBUG(llvm::dbgs()
               << "===> MoveOnlyTempAllocationFromLetTester. Visiting: "
               << fn->getName() << '\n');

    SmallSetVector<MarkMustCheckInst *, 32> moveIntroducersToProcess;
    DiagnosticEmitter diagnosticEmitter(getFunction());

    unsigned diagCount = diagnosticEmitter.getDiagnosticCount();
    searchForCandidateAddressMarkMustChecks(
        getFunction(), moveIntroducersToProcess, diagnosticEmitter);

    // Return early if we emitted a diagnostic.
    if (diagCount != diagnosticEmitter.getDiagnosticCount())
      return;

    bool madeChange = false;
    while (!moveIntroducersToProcess.empty()) {
      auto *next = moveIntroducersToProcess.pop_back_val();
      madeChange |= eliminateTemporaryAllocationsFromLet(next);
    }

    if (madeChange)
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
};

} // namespace

SILTransform *swift::createMoveOnlyTempAllocationFromLetTester() {
  return new MoveOnlyTempAllocationFromLetTester();
}
