//===--- UsePrespecialized.cpp - use pre-specialized functions ------------===//
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

#define DEBUG_TYPE "use-prespecialized"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/Generics.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/SpecializationMangler.h"
#include "llvm/Support/Debug.h"

using namespace swift;

namespace {



static void collectApplyInst(SILFunction &F,
                             llvm::SmallVectorImpl<ApplySite> &NewApplies) {
  // Scan all of the instructions in this function in search of ApplyInsts.
  for (auto &BB : F)
    for (auto &I : BB)
      if (ApplySite AI = ApplySite::isa(&I))
        NewApplies.push_back(AI);
}

/// A simple pass which replaces each apply of a generic function by an apply
/// of the corresponding pre-specialized function, if such a pre-specialization
/// exists.
class UsePrespecialized: public SILModuleTransform {
  ~UsePrespecialized() override { }

  void run() override {
    auto &M = *getModule();
    for (auto &F : M) {
      if (replaceByPrespecialized(F)) {
        invalidateAnalysis(&F, SILAnalysis::InvalidationKind::FunctionBody);
      }
    }
  }

  bool replaceByPrespecialized(SILFunction &F);
};

} // end anonymous namespace

// Analyze the function and replace each apply of
// a generic function by an apply of the corresponding
// pre-specialized function, if such a pre-specialization exists.
bool UsePrespecialized::replaceByPrespecialized(SILFunction &F) {
  bool Changed = false;
  auto &M = F.getModule();

  llvm::SmallVector<ApplySite, 16> NewApplies;
  collectApplyInst(F, NewApplies);

  for (auto &AI : NewApplies) {
    auto *ReferencedF = AI.getReferencedFunctionOrNull();
    if (!ReferencedF)
      continue;

    LLVM_DEBUG(llvm::dbgs() << "Trying to use specialized function for:\n";
               AI.getInstruction()->dumpInContext());

    // Check if it is a call of a generic function.
    // If this is the case, check if there is a specialization
    // available for it already and use this specialization
    // instead of the generic version.
    if (!AI.hasSubstitutions())
      continue;

    SubstitutionMap Subs = AI.getSubstitutionMap();

    // Bail if any generic type parameters are unbound.
    // TODO: Remove this limitation once public partial specializations
    // are supported and can be provided by other modules.
    if (Subs.hasArchetypes())
      continue;

    ReabstractionInfo ReInfo(M.getSwiftModule(), M.isWholeModule(), AI,
                             ReferencedF, Subs, IsNotSerialized,
                             /*ConvertIndirectToDirect=*/ true,
                             /*dropMetatypeArgs=*/ false);

    if (!ReInfo.canBeSpecialized())
      continue;

    auto SpecType = ReInfo.getSpecializedType();
    // Bail if any generic types parameters of the concrete type
    // are unbound.
    if (SpecType->hasArchetype())
      continue;

    // Create a name of the specialization. All external pre-specializations
    // are serialized without bodies. Thus use IsNotSerialized here.
    Mangle::GenericSpecializationMangler NewGenericMangler(ReferencedF,
                                                           IsNotSerialized);
    std::string ClonedName = NewGenericMangler.mangleReabstracted(Subs,
       ReInfo.needAlternativeMangling());
      
    SILFunction *NewF = nullptr;
    // If we already have this specialization, reuse it.
    auto PrevF = M.lookUpFunction(ClonedName);
    if (PrevF) {
      LLVM_DEBUG(llvm::dbgs() << "Found a specialization: " << ClonedName
                              << "\n");
      NewF = PrevF;
    }

    if (!PrevF || !NewF) {
      // Check for the existence of this function in another module without
      // loading the function body.
      PrevF = lookupPrespecializedSymbol(M, ClonedName);
      LLVM_DEBUG(llvm::dbgs() << "Checked if there is a specialization in a "
                                 "different module: "
                              << PrevF << "\n");
      if (!PrevF)
        continue;
      assert(PrevF->isExternalDeclaration() &&
             "Prespecialized function should be an external declaration");
      NewF = PrevF;
    }

    if (!NewF)
      continue;

    // An existing specialization was found.
    LLVM_DEBUG(llvm::dbgs() << "Found a specialization of "
                            << ReferencedF->getName()
                            << " : " << NewF->getName() << "\n");

    auto NewAI = replaceWithSpecializedFunction(AI, NewF, ReInfo);
    switch (AI.getKind()) {
    case ApplySiteKind::ApplyInst:
      cast<ApplyInst>(AI)->replaceAllUsesWith(cast<ApplyInst>(NewAI));
      break;
    case ApplySiteKind::PartialApplyInst:
      cast<PartialApplyInst>(AI)->replaceAllUsesWith(
          cast<PartialApplyInst>(NewAI));
      break;
    case ApplySiteKind::TryApplyInst:
    case ApplySiteKind::BeginApplyInst:
      break;
    }
    recursivelyDeleteTriviallyDeadInstructions(AI.getInstruction(), true);
    Changed = true;
  }

  return Changed;
}


SILTransform *swift::createUsePrespecialized() {
  return new UsePrespecialized();
}
