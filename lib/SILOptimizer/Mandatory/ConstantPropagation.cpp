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
#include "swift/Basic/Assertions.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/ConstantFolding.h"

using namespace swift;

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
    SILOptFunctionBuilder FuncBuilder(*this);
    ConstantFolder Folder(FuncBuilder, getOptions().AssertConfig,
                          EnableDiagnostics);
    Folder.initializeWorklist(*getFunction());
    auto Invalidation = Folder.processWorkList();

    if (Invalidation != SILAnalysis::InvalidationKind::Nothing) {
      invalidateAnalysis(Invalidation);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createDiagnosticConstantPropagation() {
  // Diagnostic propagation is rerun on deserialized SIL because it is sensitive
  // to assert configuration.
  return new ConstantPropagation(true /*enable diagnostics*/);
}

SILTransform *swift::createPerformanceConstantPropagation() {
  return new ConstantPropagation(false /*disable diagnostics*/);
}
