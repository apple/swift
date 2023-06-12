//===--- MoveOnlyDeinitDevirtualization.cpp --------------------------------------===//
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
/// \file
///
/// This pass runs after move only checking has occurred and transforms last
/// destroy_value of move only types into a call to the move only types deinit.
///
/// TODO: This pass is disabled because it hides bugs in the common case in
/// which optimization passes incorrectly remove the deinit, for example, by
/// destructuring the aggregate rather than destroying it as a whole. Consider
/// reeabling this pass later in the pipeline, after all other OSSA function
/// passes have run. Also consider removing bailouts from this pass. If it's
/// possible to devirtualize, then it should do it.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-move-only-checker"

#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockData.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/Consumption.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PostOrder.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILArgumentConvention.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILMoveOnlyDeinit.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;

static bool performTransform(SILFunction &fn) {
  bool changed = false;

  auto &mod = fn.getModule();
  for (auto &block : fn) {
    for (auto ii = block.begin(), ie = block.end(); ii != ie;) {
      auto *inst = &*ii;
      ++ii;

      if (auto *dvi = dyn_cast<DestroyValueInst>(inst)) {
        auto destroyType = dvi->getOperand()->getType();
        if (destroyType.isMoveOnlyNominalType() &&
            !isa<DropDeinitInst>(lookThroughOwnershipInsts(dvi->getOperand()))) {
          LLVM_DEBUG(llvm::dbgs() << "Handling: " << *dvi);
          auto *nom = destroyType.getNominalOrBoundGenericNominal();
          assert(nom);
          auto *deinitFunc = mod.lookUpMoveOnlyDeinitFunction(nom);
          if (!deinitFunc) {
            LLVM_DEBUG(llvm::dbgs()
                       << "Failed to find deinit func for type! Skipping!\n");
            continue;
          }

          auto astType = destroyType.getASTType();
          auto subMap =
              astType->getContextSubstitutionMap(nom->getModuleContext(), nom);
          SILBuilderWithScope builder(dvi);

          SILValue value = dvi->getOperand();
          auto conv = deinitFunc->getConventionsInContext();
          if (conv.getSILArgumentConvention(conv.getSILArgIndexOfSelf())
                  .isIndirectConvention()) {
            auto *asi =
                builder.createAllocStack(dvi->getLoc(), value->getType());
            builder.emitStoreValueOperation(dvi->getLoc(), value, asi,
                                            StoreOwnershipQualifier::Init);
            value = asi;
          }
          auto *funcRef = builder.createFunctionRef(dvi->getLoc(), deinitFunc);
          builder.createApply(dvi->getLoc(), funcRef, subMap, value);
          if (isa<AllocStackInst>(value))
            builder.createDeallocStack(dvi->getLoc(), value);
          dvi->eraseFromParent();
          changed = true;
          continue;
        }
      }

      if (auto *dai = dyn_cast<DestroyAddrInst>(inst)) {
        auto destroyType = dai->getOperand()->getType();
        if (destroyType.isLoadable(fn) && destroyType.isMoveOnlyNominalType() &&
            !isa<DropDeinitInst>(dai->getOperand())) {
          LLVM_DEBUG(llvm::dbgs() << "Handling: " << *dai);
          auto *nom = destroyType.getNominalOrBoundGenericNominal();
          assert(nom);
          auto *deinitFunc = mod.lookUpMoveOnlyDeinitFunction(nom);
          if (!deinitFunc) {
            LLVM_DEBUG(llvm::dbgs()
                       << "Failed to find deinit func for type! Skipping!\n");
            continue;
          }

          SILBuilderWithScope builder(dai);
          auto *funcRef = builder.createFunctionRef(dai->getLoc(), deinitFunc);
          auto subMap = destroyType.getASTType()->getContextSubstitutionMap(
              nom->getModuleContext(), nom);

          auto conv = deinitFunc->getConventionsInContext();
          auto argConv =
              conv.getSILArgumentConvention(conv.getSILArgIndexOfSelf());
          SILValue value = dai->getOperand();
          if (!argConv.isIndirectConvention())
            value = builder.emitLoadValueOperation(
                dai->getLoc(), dai->getOperand(), LoadOwnershipQualifier::Take);
          builder.createApply(dai->getLoc(), funcRef, subMap, value);
          dai->eraseFromParent();
          changed = true;
          continue;
        }
      }
    }
  }

  return changed;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class SILMoveOnlyDeinitDevirtualizationPass : public SILFunctionTransform {
  void run() override {
    auto *fn = getFunction();

    // Don't rerun diagnostics on deserialized functions.
    if (getFunction()->wasDeserializedCanonical())
      return;

    assert(fn->getModule().getStage() == SILStage::Raw &&
           "Should only run on Raw SIL");
    LLVM_DEBUG(llvm::dbgs() << "===> MoveOnly Deinit Devirtualization. Visiting: "
                            << fn->getName() << '\n');
    if (performTransform(*fn)) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::CallsAndInstructions);
    }
  }
};

} // anonymous namespace

SILTransform *swift::createMoveOnlyDeinitDevirtualization() {
  return new SILMoveOnlyDeinitDevirtualizationPass();
}
