//===--- OwnedToGuaranteedTransform.cpp -----------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "fso-owned-to-guaranteed-transform"
#include "FunctionSignatureOpts.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/AST/SemanticAttrs.h"
#include "llvm/Support/CommandLine.h"

using namespace swift;

static llvm::cl::opt<bool> FSODisableOwnedToGuaranteed(
    "sil-fso-disable-owned-to-guaranteed",
    llvm::cl::desc("Do not perform owned to guaranteed during FSO. Intended "
                   "only for testing purposes."));

//===----------------------------------------------------------------------===//
//                                 Utilities
//===----------------------------------------------------------------------===//

/// Return the single return value of the function.
static SILValue findReturnValue(SILFunction *F) {
  auto RBB = F->findReturnBB();
  if (RBB == F->end())
    return SILValue();
  auto Term = dyn_cast<ReturnInst>(RBB->getTerminator());
  return Term->getOperand();
}

/// Return the single apply found in this function.
static SILInstruction *findOnlyApply(SILFunction *F) {
  SILInstruction *OnlyApply = nullptr;
  for (auto &B : *F) {
    for (auto &X : B) {
      if (!isa<ApplyInst>(X) && !isa<TryApplyInst>(X))
        continue;
      assert(!OnlyApply && "There are more than 1 function calls");
      OnlyApply = &X;
    }
  }
  assert(OnlyApply && "There is no function calls");
  return OnlyApply;
}

//===----------------------------------------------------------------------===//
//                               Implementation
//===----------------------------------------------------------------------===//

bool FunctionSignatureTransform::OwnedToGuaranteedAnalyzeParameters() {
  SILFunction *F = TransformDescriptor.OriginalFunction;
  auto Args = F->begin()->getSILFunctionArguments();
  // A map from consumed SILArguments to the release associated with an
  // argument.
  //
  // TODO: The return block and throw block should really be abstracted away.
  SILArgumentConvention ArgumentConventions[] = {
      SILArgumentConvention::Direct_Owned, SILArgumentConvention::Indirect_In};
  ConsumedArgToEpilogueReleaseMatcher ArgToReturnReleaseMap(
      RCIA->get(F), F, ArgumentConventions);
  ConsumedArgToEpilogueReleaseMatcher ArgToThrowReleaseMap(
      RCIA->get(F), F, ArgumentConventions,
      ConsumedArgToEpilogueReleaseMatcher::ExitKind::Throw);

  // Did we decide we should optimize any parameter?
  bool SignatureOptimize = false;

  // Analyze the argument information.
  for (unsigned i : indices(Args)) {
    ArgumentDescriptor &A = TransformDescriptor.ArgumentDescList[i];
    if (!A.canOptimizeLiveArg()) {
      continue;
    }

    // See if we can find a ref count equivalent strong_release or release_value
    // at the end of this function if our argument is an @owned parameter.
    // See if we can find a destroy_addr at the end of this function if our
    // argument is an @in parameter.
    if (A.hasConvention(SILArgumentConvention::Direct_Owned) ||
        A.hasConvention(SILArgumentConvention::Indirect_In)) {
      auto Releases = ArgToReturnReleaseMap.getReleasesForArgument(A.Arg);
      if (!Releases.empty()) {
        // If the function has a throw block we must also find a matching
        // release in the throw block.
        auto ReleasesInThrow =
            ArgToThrowReleaseMap.getReleasesForArgument(A.Arg);
        if (!ArgToThrowReleaseMap.hasBlock() || !ReleasesInThrow.empty()) {
          assert(A.CalleeRelease.empty());
          assert(A.CalleeReleaseInThrowBlock.empty());
          llvm::copy(Releases, std::back_inserter(A.CalleeRelease));
          llvm::copy(ReleasesInThrow,
                     std::back_inserter(A.CalleeReleaseInThrowBlock));
          // We can convert this parameter to a @guaranteed.
          A.OwnedToGuaranteed = true;
          SignatureOptimize = true;
        }
      }
    }

    // Modified self argument.
    if (A.OwnedToGuaranteed && Args[i]->isSelf()) {
      TransformDescriptor.shouldModifySelfArgument = true;
    }
  }
  return SignatureOptimize;
}

bool FunctionSignatureTransform::OwnedToGuaranteedAnalyzeResults() {
  SILFunction *F = TransformDescriptor.OriginalFunction;
  auto ResultDescList = TransformDescriptor.ResultDescList;

  auto fnConv = F->getConventions();
  // For now, only do anything if there's a single direct result.
  if (fnConv.getNumDirectSILResults() != 1)
    return false;
  if (!fnConv.getIndirectSILResults().empty())
    return false;

  bool SignatureOptimize = false;
  if (ResultDescList[0].hasConvention(ResultConvention::Owned)) {
    auto RV = findReturnValue(F);
    if (!RV)
      return false;
    auto &RI = ResultDescList[0];
    // We have an @owned return value, find the epilogue retains now.
    auto Retains = EA->get(F)->computeEpilogueARCInstructions(
        EpilogueARCContext::EpilogueARCKind::Retain, RV);
    // We do not need to worry about the throw block, as the return value is
    // only going to be used in the return block/normal block of the try_apply
    // instruction.
    if (!Retains.empty()) {
      RI.CalleeRetain = Retains;
      SignatureOptimize = true;
      RI.OwnedToGuaranteed = true;
    }
  }
  return SignatureOptimize;
}

void
FunctionSignatureTransform::OwnedToGuaranteedTransformFunctionParameters() {
  // And remove all Callee releases that we found and made redundant via owned
  // to guaranteed conversion.
  for (const ArgumentDescriptor &AD : TransformDescriptor.ArgumentDescList) {
    if (!AD.OwnedToGuaranteed)
      continue;
    for (auto &X : AD.CalleeRelease) {
      X->eraseFromParent();
    }
    for (auto &X : AD.CalleeReleaseInThrowBlock) {
      X->eraseFromParent();
    }

    // Now we need to replace the FunctionArgument so that we have the correct
    // ValueOwnershipKind.
    AD.Arg->setOwnershipKind(OwnershipKind::Guaranteed);
  }
}

void FunctionSignatureTransform::OwnedToGuaranteedTransformFunctionResults() {
  // And remove all callee retains that we found and made redundant via owned
  // to unowned conversion.
  for (const ResultDescriptor &RD : TransformDescriptor.ResultDescList) {
    if (!RD.OwnedToGuaranteed)
      continue;
    for (auto &X : RD.CalleeRetain) {
      if (isa<StrongRetainInst>(X) || isa<RetainValueInst>(X)) {
        X->eraseFromParent();
        continue;
      }
      // Create a release to balance it out.
      auto AI = cast<ApplyInst>(X);
      createDecrementBefore(AI, AI->getParent()->getTerminator());
    }
  }
}

void FunctionSignatureTransform::OwnedToGuaranteedFinalizeThunkFunction(
    SILBuilder &Builder, SILFunction *F) {
  // Finish the epilogue work for the argument as well as result.
  for (auto &ArgDesc : TransformDescriptor.ArgumentDescList) {
    OwnedToGuaranteedAddArgumentRelease(ArgDesc, Builder, F);
  }
  for (auto &ResDesc : TransformDescriptor.ResultDescList) {
    OwnedToGuaranteedAddResultRelease(ResDesc, Builder, F);
  }
}

static void createArgumentRelease(SILBuilder &Builder, ArgumentDescriptor &AD) {
  auto &F = Builder.getFunction();
  SILArgument *Arg = F.getArguments()[AD.Index];
  if (Arg->getType().isAddress()) {
    assert(AD.PInfo->getConvention() == ParameterConvention::Indirect_In &&
           F.getConventions().useLoweredAddresses());
    Builder.createDestroyAddr(RegularLocation::getAutoGeneratedLocation(),
                              F.getArguments()[AD.Index]);
    return;
  }
  Builder.createReleaseValue(RegularLocation::getAutoGeneratedLocation(),
                             F.getArguments()[AD.Index],
                             Builder.getDefaultAtomicity());
}

/// Set up epilogue work for the thunk arguments based in the given argument.
/// Default implementation simply passes it through.
void FunctionSignatureTransform::OwnedToGuaranteedAddArgumentRelease(
    ArgumentDescriptor &AD, SILBuilder &Builder, SILFunction *F) {
  // If we have any arguments that were consumed but are now guaranteed,
  // insert a releasing RC instruction.
  if (!AD.OwnedToGuaranteed) {
    return;
  }

  SILInstruction *Call = findOnlyApply(F);
  if (isa<ApplyInst>(Call)) {
    Builder.setInsertionPoint(&*std::next(SILBasicBlock::iterator(Call)));
    createArgumentRelease(Builder, AD);
  } else {
    SILBasicBlock *NormalBB = dyn_cast<TryApplyInst>(Call)->getNormalBB();
    Builder.setInsertionPoint(&*NormalBB->begin());
    createArgumentRelease(Builder, AD);

    SILBasicBlock *ErrorBB = dyn_cast<TryApplyInst>(Call)->getErrorBB();
    Builder.setInsertionPoint(&*ErrorBB->begin());
    createArgumentRelease(Builder, AD);
  }
}

void FunctionSignatureTransform::OwnedToGuaranteedAddResultRelease(
    ResultDescriptor &RD, SILBuilder &Builder, SILFunction *F) {
  // If we have any result that were consumed but are now guaranteed,
  // insert a releasing RC instruction.
  if (!RD.OwnedToGuaranteed) {
    return;
  }

  SILInstruction *Call = findOnlyApply(F);
  if (auto AI = dyn_cast<ApplyInst>(Call)) {
    Builder.setInsertionPoint(&*std::next(SILBasicBlock::iterator(AI)));
    Builder.createRetainValue(RegularLocation::getAutoGeneratedLocation(), AI,
                              Builder.getDefaultAtomicity());
  } else {
    SILBasicBlock *NormalBB = cast<TryApplyInst>(Call)->getNormalBB();
    Builder.setInsertionPoint(&*NormalBB->begin());
    Builder.createRetainValue(RegularLocation::getAutoGeneratedLocation(),
                              NormalBB->getArgument(0),
                              Builder.getDefaultAtomicity());
  }
}

bool FunctionSignatureTransform::OwnedToGuaranteedAnalyze() {
  if (FSODisableOwnedToGuaranteed)
    return false;
  SILFunction *F = TransformDescriptor.OriginalFunction;
  if (F->hasSemanticsAttr(semantics::OPTIMIZE_SIL_SPECIALIZE_OWNED2GUARANTEE_NEVER))
    return false;

  const bool Result = OwnedToGuaranteedAnalyzeResults();
  const bool Params = OwnedToGuaranteedAnalyzeParameters();
  return Params || Result;
}

void FunctionSignatureTransform::OwnedToGuaranteedTransform() {
  OwnedToGuaranteedTransformFunctionResults();
  OwnedToGuaranteedTransformFunctionParameters();
}
