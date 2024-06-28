//===--- OwnershipVerifierStateDumper.cpp ---------------------------------===//
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
/// This is a simple utility pass that verifies the ownership of all SILValue in
/// a module with a special flag set that causes the verifier to emit textual
/// errors instead of asserting. This is done one function at a time so that we
/// can number the errors as we emit them so in FileCheck tests, we can be 100%
/// sure we exactly matched the number of errors.
///
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class OwnershipVerifierTextualErrorDumper : public SILFunctionTransform {
  void run() override {
    SILFunction *f = getFunction();
    auto *deBlocksAnalysis = getAnalysis<DeadEndBlocksAnalysis>();
    f->verifyOwnership(f->getModule().getOptions().OSSAVerifyComplete
                           ? nullptr
                           : deBlocksAnalysis->get(f));
  }
};

} // end anonymous namespace

SILTransform *swift::createOwnershipVerifierTextualErrorDumper() {
  return new OwnershipVerifierTextualErrorDumper();
}
