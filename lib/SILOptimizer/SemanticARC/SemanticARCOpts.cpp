//===--- SemanticARCOpts.cpp ----------------------------------------------===//
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

#define DEBUG_TYPE "sil-semantic-arc-opts"

#include "SemanticARCOpts.h"
#include "SemanticARCOptVisitor.h"
#include "Transforms.h"

#include "swift/SILOptimizer/PassManager/Transforms.h"

#include "llvm/Support/CommandLine.h"

using namespace swift;
using namespace swift::semanticarc;

static llvm::cl::list<ARCTransformKind> TransformsToPerform(
    llvm::cl::values(
        clEnumValN(ARCTransformKind::AllPeepholes,
                   "sil-semantic-arc-peepholes-all",
                   "Perform All ARC canonicalizations and peepholes"),
        clEnumValN(ARCTransformKind::LoadCopyToLoadBorrowPeephole,
                   "sil-semantic-arc-peepholes-loadcopy-to-loadborrow",
                   "Perform the load [copy] to load_borrow peephole"),
        clEnumValN(ARCTransformKind::RedundantBorrowScopeElimPeephole,
                   "sil-semantic-arc-peepholes-redundant-borrowscope-elim",
                   "Perform the redundant borrow scope elimination peephole"),
        clEnumValN(ARCTransformKind::RedundantCopyValueElimPeephole,
                   "sil-semantic-arc-peepholes-redundant-copyvalue-elim",
                   "Perform the redundant copy_value peephole"),
        clEnumValN(ARCTransformKind::LifetimeJoiningPeephole,
                   "sil-semantic-arc-peepholes-lifetime-joining",
                   "Perform the join lifetimes peephole"),
        clEnumValN(ARCTransformKind::PhiArgPeepholes,
                   "sil-semantic-arc-peepholes-phi-arg-peepholes",
                   "Perform small peepholes on phi arguments"),
        clEnumValN(ARCTransformKind::OwnedToGuaranteedPhi,
                   "sil-semantic-arc-owned-to-guaranteed-phi",
                   "Perform Owned To Guaranteed Phi. NOTE: Seeded by peephole "
                   "optimizer for compile time saving purposes, so run this "
                   "after running peepholes)")),
    llvm::cl::desc(
        "For testing purposes only run the specified list of semantic arc "
        "optimization. If the list is empty, we run all transforms"));

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

// Even though this is a mandatory pass, it is rerun after deserialization in
// case DiagnosticConstantPropagation exposed anything new in this assert
// configuration.
struct SemanticARCOpts : SILFunctionTransform {
  bool guaranteedOptsOnly;

  SemanticARCOpts(bool guaranteedOptsOnly)
      : guaranteedOptsOnly(guaranteedOptsOnly) {}

#ifndef NDEBUG
  void performCommandlineSpecifiedTransforms(SemanticARCOptVisitor &visitor) {
    for (auto transform : TransformsToPerform) {
      visitor.ctx.transformKind = transform;
      SWIFT_DEFER {
        visitor.ctx.transformKind = ARCTransformKind::Invalid;
        visitor.reset();
      };
      switch (transform) {
      case ARCTransformKind::LifetimeJoiningPeephole:
      case ARCTransformKind::RedundantCopyValueElimPeephole:
      case ARCTransformKind::RedundantBorrowScopeElimPeephole:
      case ARCTransformKind::LoadCopyToLoadBorrowPeephole:
      case ARCTransformKind::PhiArgPeepholes:
      case ARCTransformKind::AllPeepholes:
        // We never assume we are at fixed point when running these transforms.
        if (performPeepholesWithoutFixedPoint(visitor)) {
          invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
        }
        continue;
      case ARCTransformKind::OwnedToGuaranteedPhi:
        if (tryConvertOwnedPhisToGuaranteedPhis(visitor.ctx)) {
          invalidateAnalysis(
              SILAnalysis::InvalidationKind::BranchesAndInstructions);
        }
        continue;
      case ARCTransformKind::All:
      case ARCTransformKind::Invalid:
        llvm_unreachable("unsupported option");
      }
    }
  }
#endif

  void seedWorklist(SemanticARCOptVisitor &visitor) {
    auto &front = getFunction()->front();
    for (auto &block : *getFunction()) {
      if (&block != &front)
        for (auto *arg : block.getArguments())
          if (SemanticARCOptVisitor::shouldVisitArg(arg))
            visitor.worklist.insert(arg);

      for (auto &inst : block) {
        if (SemanticARCOptVisitor::shouldVisitInst(&inst)) {
          for (SILValue v : inst.getResults()) {
            visitor.worklist.insert(v);
          }
        }
      }
    }
  }

  bool performPeepholesWithoutFixedPoint(SemanticARCOptVisitor &visitor) {
    // Add all the results of all instructions that we want to visit to the
    // worklist.
    seedWorklist(visitor);

    // Then process the worklist, performing peepholes.
    return visitor.optimizeWithoutFixedPoint();
  }

  bool performPeepholes(SemanticARCOptVisitor &visitor) {
    // Add all the results of all instructions that we want to visit to the
    // worklist.
    seedWorklist(visitor);

    // Then process the worklist, performing peepholes.
    return visitor.optimize();
  }

  void run() override {
    SILFunction &f = *getFunction();

    // Return early if we are not performing OSSA optimizations or we are not in
    // ownership.
    if (!f.getModule().getOptions().EnableOSSAOptimizations ||
        !f.hasOwnership())
      return;

    // Make sure we are running with ownership verification enabled.
    assert(f.getModule().getOptions().VerifySILOwnership &&
           "Can not perform semantic arc optimization unless ownership "
           "verification is enabled");

    SemanticARCOptVisitor visitor(f, guaranteedOptsOnly);

#ifndef NDEBUG
    // If we are being asked for testing purposes to run a series of transforms
    // expressed on the command line, run that and return.
    if (!TransformsToPerform.empty()) {
      return performCommandlineSpecifiedTransforms(visitor);
    }
#endif

    using InvalidationKind = SILAnalysis::InvalidationKind;
    auto invalidationKind = InvalidationKind::Nothing;

    // Otherwise, perform our standard optimizations.
    if (performPeepholes(visitor))
      invalidationKind = invalidationKind | InvalidationKind::Instructions;

    // Now that we have seeded the map of phis to incoming values that could be
    // converted to guaranteed, ignoring the phi, try convert those phis to be
    // guaranteed.
    if (tryConvertOwnedPhisToGuaranteedPhis(visitor.ctx))
      invalidationKind =
          invalidationKind | InvalidationKind::BranchesAndInstructions;

    if (visitor.deleteDeadArgs())
      invalidationKind = invalidationKind | InvalidationKind::Branches;

    // Then invalidate as we need.
    //
    // NOTE: This is only safe since we are not using any analyses internally to
    // this pass... If we ever do so, we will need to be significantly more
    // careful about this and may need to invalidate above.
    invalidateAnalysis(invalidationKind);
  }
};

} // end anonymous namespace

SILTransform *swift::createSemanticARCOpts() {
  return new SemanticARCOpts(false /*guaranteed*/);
}

SILTransform *swift::createGuaranteedARCOpts() {
  return new SemanticARCOpts(true /*guaranteed*/);
}
