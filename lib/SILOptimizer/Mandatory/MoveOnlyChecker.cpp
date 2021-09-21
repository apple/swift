//===--- MoveOnlyChecker.cpp ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Defer.h"
#define DEBUG_TYPE "sil-move-only-checker"

#include "swift/AST/DiagnosticsSIL.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SILOptimizer/Analysis/ClosureScope.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalOSSALifetime.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                            Diagnostic Utilities
//===----------------------------------------------------------------------===//

template <typename... T, typename... U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
                     U &&...args) {
  Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

//===----------------------------------------------------------------------===//
//                                 Main Pass
//===----------------------------------------------------------------------===//

namespace {

struct MoveOnlyChecker {
  SILFunction *fn;

  MoveOnlyChecker(SILFunction *fn) : fn(fn) {}
  bool check(NonLocalAccessBlockAnalysis *accessBlockAnalysis,
             DominanceInfo *domTree);
};

} // namespace

bool MoveOnlyChecker::check(NonLocalAccessBlockAnalysis *accessBlockAnalysis,
                            DominanceInfo *domTree) {
  SmallSetVector<SILValue, 32> moveIntroducersToProcess;

  // First add all of the arguments.
  for (auto *arg : fn->front().getArguments()) {
    moveIntroducersToProcess.insert(arg);
  }

  for (auto &block : *fn) {
    for (auto &ii : block) {
      if (auto *mvi = dyn_cast<MoveValueInst>(&ii)) {
        moveIntroducersToProcess.insert(mvi);
      }

      if (auto ai = FullApplySite::isa(&ii)) {
        for (auto result : ii.getResults()) {
          if (result->getType().isMoveOnly())
            moveIntroducersToProcess.insert(result);
        }
      }
    }
  }

  auto callbacks =
      InstModCallbacks().onDelete([&](SILInstruction *instToDelete) {
        if (auto *mvi = dyn_cast<MoveValueInst>(instToDelete))
          moveIntroducersToProcess.remove(mvi);
        instToDelete->eraseFromParent();
      });
  InstructionDeleter deleter(std::move(callbacks));
  bool changed = false;

  SmallVector<Operand *, 32> consumingUsesNeedingCopy;
  auto foundConsumingUseNeedingCopy = [&](Operand *use) {
    consumingUsesNeedingCopy.push_back(use);
  };
  SmallVector<Operand *, 32> consumingUsesNotNeedingCopy;
  auto foundConsumingUseNotNeedingCopy = [&](Operand *use) {
    consumingUsesNotNeedingCopy.push_back(use);
  };

  CanonicalizeOSSALifetime canonicalizer(
      false /*pruneDebugMode*/, false /*poisonRefsMode*/, accessBlockAnalysis,
      domTree, deleter, foundConsumingUseNeedingCopy,
      foundConsumingUseNotNeedingCopy);
  auto &astContext = fn->getASTContext();
  while (moveIntroducersToProcess.size()) {
    SWIFT_DEFER {
      consumingUsesNeedingCopy.clear();
      consumingUsesNotNeedingCopy.clear();
    };

    SILValue movedValue = moveIntroducersToProcess.pop_back_val();
    LLVM_DEBUG(llvm::dbgs() << "Visiting: " << *movedValue);
    changed |= canonicalizer.canonicalizeValueLifetime(movedValue);

    if (consumingUsesNeedingCopy.empty())
      continue;

    StringRef varName = "unknown";
    if (auto *use = getSingleDebugUse(movedValue)) {
      DebugVarCarryingInst debugVar(use->getUser());
      if (auto varInfo = debugVar.getVarInfo()) {
        varName = varInfo->Name;
      } else {
        if (auto *decl = debugVar.getDecl()) {
          varName = decl->getBaseName().userFacingName();
        }
      }
    }

    diagnose(astContext,
             movedValue->getDefiningInstruction()->getLoc().getSourceLoc(),
             diag::sil_moveonlychecker_value_consumed_more_than_once, varName);

    while (consumingUsesNeedingCopy.size()) {
      auto *consumingUse = consumingUsesNeedingCopy.pop_back_val();
      diagnose(astContext, consumingUse->getUser()->getLoc().getSourceLoc(),
               diag::sil_moveonlychecker_consuming_use_here);
    }

    while (consumingUsesNotNeedingCopy.size()) {
      auto *consumingUse = consumingUsesNotNeedingCopy.pop_back_val();
      diagnose(astContext, consumingUse->getUser()->getLoc().getSourceLoc(),
               diag::sil_moveonlychecker_consuming_use_here);
    }
  }

  return changed;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class MoveOnlyCheckerPass : public SILFunctionTransform {
  void run() override {
    auto *fn = getFunction();

    // Don't rerun diagnostics on deserialized functions.
    if (getFunction()->wasDeserializedCanonical())
      return;

    assert(fn->getModule().getStage() == SILStage::Raw &&
           "Should only run on Raw SIL");

    auto *accessBlockAnalysis = getAnalysis<NonLocalAccessBlockAnalysis>();
    auto *dominanceAnalysis = getAnalysis<DominanceAnalysis>();
    DominanceInfo *domTree = dominanceAnalysis->get(fn);

    if (MoveOnlyChecker(getFunction()).check(accessBlockAnalysis, domTree)) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }
  }
};

} // anonymous namespace

SILTransform *swift::createMoveOnlyChecker() {
  return new MoveOnlyCheckerPass();
}
