//===--- ConstantPropagation.cpp - Constant fold and diagnose overflows ---===//
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

#define DEBUG_TYPE "constant-propagation"
#include "TFConstExpr.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/ConstantFolding.h"
#include "llvm/Support/CommandLine.h"

using namespace swift;

static llvm::cl::opt<bool> ConstantPropagationUseNewFolder(
    "constant-propagation-use-new-folder", llvm::cl::init(false),
    llvm::cl::desc("Use new folder in ConstantPropagation passes"));

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {

class ConstantPropagation : public SILFunctionTransform {
  bool EnableDiagnostics;

public:
  ConstantPropagation(bool EnableDiagnostics) :
    EnableDiagnostics(EnableDiagnostics) {}

private:
  /// The entry point to the transformation.
  void run() override {
    SILAnalysis::InvalidationKind Invalidation;

    if (ConstantPropagationUseNewFolder) {
      tf::ConstExprEvaluator evaluator(getFunction()->getModule());
      Invalidation =
          evaluator.propagateConstants(*getFunction(), EnableDiagnostics);
    } else {
      ConstantFolder Folder(getOptions().AssertConfig, EnableDiagnostics);
      Folder.initializeWorklist(*getFunction());
      Invalidation = Folder.processWorkList();
    }

    if (Invalidation != SILAnalysis::InvalidationKind::Nothing) {
      invalidateAnalysis(Invalidation);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createDiagnosticConstantPropagation() {
  // Diagostic propagation is rerun on deserialized SIL because it is sensitive
  // to assert configuration.
  return new ConstantPropagation(true /*enable diagnostics*/);
}

SILTransform *swift::createPerformanceConstantPropagation() {
  return new ConstantPropagation(false /*disable diagnostics*/);
}
