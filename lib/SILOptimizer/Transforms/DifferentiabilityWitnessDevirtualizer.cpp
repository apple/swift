//===--- DifferentiabilityWitnessDevirtualizer.cpp ------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Devirtualizes `differentiability_witness_function` instructions into
// `function_ref` instructions for differentiability witness definitions.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;

namespace {
class DifferentiabilityWitnessDevirtualizer : public SILFunctionTransform {

  /// Returns true if any changes were made.
  bool devirtualizeDifferentiabilityWitnessesInFunction(SILFunction &f);

  /// The entry point to the transformation.
  void run() override {
    if (devirtualizeDifferentiabilityWitnessesInFunction(*getFunction()))
      invalidateAnalysis(SILAnalysis::InvalidationKind::CallsAndInstructions);
  }
};
} // end anonymous namespace

bool DifferentiabilityWitnessDevirtualizer::
    devirtualizeDifferentiabilityWitnessesInFunction(SILFunction &f) {
  bool changed = false;
  llvm::SmallVector<DifferentiabilityWitnessFunctionInst *, 8> insts;
  for (auto &bb : f) {
    for (auto &inst : bb) {
      auto *dfwi = dyn_cast<DifferentiabilityWitnessFunctionInst>(&inst);
      if (!dfwi)
        continue;
      insts.push_back(dfwi);
    }
  }
  for (auto *inst : insts) {
    auto *witness = inst->getWitness();
    if (witness->isDeclaration())
      f.getModule().loadDifferentiabilityWitness(witness);
    if (witness->isDeclaration())
      continue;
    changed = true;
    SILBuilderWithScope builder(inst);
    auto kind = inst->getWitnessKind().getAsDerivativeFunctionKind();
    assert(kind.has_value());
    auto *newInst = builder.createFunctionRefFor(inst->getLoc(),
                                                 witness->getDerivative(*kind));
    inst->replaceAllUsesWith(newInst);
    inst->getParent()->erase(inst);
  }
  return changed;
}

SILTransform *swift::createDifferentiabilityWitnessDevirtualizer() {
  return new DifferentiabilityWitnessDevirtualizer();
}
