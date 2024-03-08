//===--- LLVMARCContract.cpp ----------------------------------------------===//
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

#define DEBUG_TYPE "swift-arc-contract"
#include "swift/LLVMPasses/Passes.h"
#include "ARCEntryPointBuilder.h"
#include "LLVMARCOpts.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"

using namespace llvm;
using namespace swift;
using swift::SwiftARCContract;

STATISTIC(NumNoopDeleted,
          "Number of no-op swift calls eliminated");
STATISTIC(NumRetainReleasesEliminatedByMergingIntoRetainReleaseN,
          "Number of retain/release eliminated by merging into "
          "retain_n/release_n");
STATISTIC(NumUnknownObjectRetainReleasesEliminatedByMergingIntoRetainReleaseN,
          "Number of retain/release eliminated by merging into "
          "unknownObjectRetain_n/unknownObjectRelease_n");
STATISTIC(NumBridgeRetainReleasesEliminatedByMergingIntoRetainReleaseN,
          "Number of bridge retain/release eliminated by merging into "
          "bridgeRetain_n/bridgeRelease_n");

/// Pimpl implementation of SwiftARCContractPass.
namespace {

struct LocalState {
  TinyPtrVector<CallInst *> RetainList;
  TinyPtrVector<CallInst *> ReleaseList;
  TinyPtrVector<CallInst *> UnknownObjectRetainList;
  TinyPtrVector<CallInst *> UnknownObjectReleaseList;
  TinyPtrVector<CallInst *> BridgeRetainList;
  TinyPtrVector<CallInst *> BridgeReleaseList;
};

/// This implements the very late (just before code generation) lowering
/// processes that we do to expose low level performance optimizations and take
/// advantage of special features of the ABI.  These expansion steps can foil
/// the general mid-level optimizer, so they are done very, very, late.
///
/// Optimizations include:
///
///   - Merging together retain and release calls into retain_n, release_n
///   - calls.
///
/// Coming into this function, we assume that the code is in canonical form:
/// none of these calls have any uses of their return values.
class SwiftARCContractImpl {
  /// Was a change made while running the optimization.
  bool Changed;

  /// Swift RC Identity.
  SwiftRCIdentity RC;

  /// The function that we are processing.
  Function &F;

  /// The entry point builder that is used to construct ARC entry points.
  ARCEntryPointBuilder B;
public:
  SwiftARCContractImpl(Function &InF) : Changed(false), F(InF), B(F) {}

  // The top level run routine of the pass.
  bool run();

private:
  /// Perform the RRN Optimization given the current state that we are
  /// tracking. This is called at the end of BBs and if we run into an unknown
  /// call.
  void
  performRRNOptimization(DenseMap<Value *, LocalState> &PtrToLocalStateMap);
};

} // end anonymous namespace

// FIXME: This method is pretty long since it is actually several smaller
// optimizations that have been copied/pasted over time. This should be split
// into those smaller (currently inline) functions.
void SwiftARCContractImpl::
performRRNOptimization(DenseMap<Value *, LocalState> &PtrToLocalStateMap) {
  // Go through all of our pointers and merge all of the retains with the
  // first retain we saw and all of the releases with the last release we saw.
  llvm::Value *O = nullptr;
  for (auto &P : PtrToLocalStateMap) {
    auto &RetainList = P.second.RetainList;
    if (RetainList.size() > 1) {
      // Create the retainN call right by the first retain.
      B.setInsertPoint(RetainList[0]);
      O = RetainList[0]->getArgOperand(0);
      auto *RI = RetainList[0];
      for (auto R : RetainList) {
        if (B.isAtomic(R)) {
          RI = R;
          break;
        }
      }
      B.createRetainN(RC.getSwiftRCIdentityRoot(O), RetainList.size(), RI);

      // Replace all uses of the retain instructions with our new retainN and
      // then delete them.
      for (auto *Inst : RetainList) {
        Inst->eraseFromParent();
        ++NumRetainReleasesEliminatedByMergingIntoRetainReleaseN;
      }

      --NumRetainReleasesEliminatedByMergingIntoRetainReleaseN;
    }
    RetainList.clear();

    auto &ReleaseList = P.second.ReleaseList;
    if (ReleaseList.size() > 1) {
      // Create the releaseN call right by the last release.
      auto *OldCI = ReleaseList[ReleaseList.size() - 1];
      B.setInsertPoint(OldCI);
      O = OldCI->getArgOperand(0);
      auto *RI = OldCI;
      for (auto R : ReleaseList) {
        if (B.isAtomic(R)) {
          RI = R;
          break;
        }
      }
      B.createReleaseN(RC.getSwiftRCIdentityRoot(O), ReleaseList.size(), RI);

      // Remove all old release instructions.
      for (auto *Inst : ReleaseList) {
        Inst->eraseFromParent();
        ++NumRetainReleasesEliminatedByMergingIntoRetainReleaseN;
      }

      --NumRetainReleasesEliminatedByMergingIntoRetainReleaseN;
    }
    ReleaseList.clear();

    auto &UnknownObjectRetainList = P.second.UnknownObjectRetainList;
    if (UnknownObjectRetainList.size() > 1) {
      // Create the retainN call right by the first retain.
      B.setInsertPoint(UnknownObjectRetainList[0]);
      O = UnknownObjectRetainList[0]->getArgOperand(0);
      auto *RI = UnknownObjectRetainList[0];
      for (auto R : UnknownObjectRetainList) {
        if (B.isAtomic(R)) {
          RI = R;
          break;
        }
      }
      B.createUnknownObjectRetainN(RC.getSwiftRCIdentityRoot(O),
                                   UnknownObjectRetainList.size(), RI);

      // Replace all uses of the retain instructions with our new retainN and
      // then delete them.
      for (auto *Inst : UnknownObjectRetainList) {
        Inst->eraseFromParent();
        ++NumUnknownObjectRetainReleasesEliminatedByMergingIntoRetainReleaseN;
      }

      --NumUnknownObjectRetainReleasesEliminatedByMergingIntoRetainReleaseN;
    }
    UnknownObjectRetainList.clear();

    auto &UnknownObjectReleaseList = P.second.UnknownObjectReleaseList;
    if (UnknownObjectReleaseList.size() > 1) {
      // Create the releaseN call right by the last release.
      auto *OldCI =
          UnknownObjectReleaseList[UnknownObjectReleaseList.size() - 1];
      B.setInsertPoint(OldCI);
      O = OldCI->getArgOperand(0);
      auto *RI = OldCI;
      for (auto R : UnknownObjectReleaseList) {
        if (B.isAtomic(R)) {
          RI = R;
          break;
        }
      }
      B.createUnknownObjectReleaseN(RC.getSwiftRCIdentityRoot(O),
                                    UnknownObjectReleaseList.size(), RI);

      // Remove all old release instructions.
      for (auto *Inst : UnknownObjectReleaseList) {
        Inst->eraseFromParent();
        ++NumUnknownObjectRetainReleasesEliminatedByMergingIntoRetainReleaseN;
      }

      --NumUnknownObjectRetainReleasesEliminatedByMergingIntoRetainReleaseN;
    }
    UnknownObjectReleaseList.clear();

    auto &BridgeRetainList = P.second.BridgeRetainList;
    if (BridgeRetainList.size() > 1) {
      // Create the releaseN call right by the first retain.
      auto *OldCI = BridgeRetainList[0];
      B.setInsertPoint(OldCI);
      O = OldCI->getArgOperand(0);
      auto *RI = OldCI;
      for (auto R : BridgeRetainList) {
        if (B.isAtomic(R)) {
          RI = R;
          break;
        }
      }
      // Bridge retain may modify the input reference before forwarding it.
      auto *I = B.createBridgeRetainN(RC.getSwiftRCIdentityRoot(O),
                                      BridgeRetainList.size(), RI);

      // Remove all old retain instructions.
      for (auto *Inst : BridgeRetainList) {
        // We may need to perform a pointer cast here to ensure that the output
        // type of the retainN matches the output type. This can come up in
        // cases where types have been obfuscated in some way. In such a case,
        // we need the inert point to be at the retain location.
        B.setInsertPoint(Inst);
        Inst->replaceAllUsesWith(B.maybeCast(I, Inst->getType()));
        Inst->eraseFromParent();
        ++NumBridgeRetainReleasesEliminatedByMergingIntoRetainReleaseN;
      }

      --NumBridgeRetainReleasesEliminatedByMergingIntoRetainReleaseN;
    }
    BridgeRetainList.clear();

    auto &BridgeReleaseList = P.second.BridgeReleaseList;
    if (BridgeReleaseList.size() > 1) {
      // Create the releaseN call right by the last release.
      auto *OldCI = BridgeReleaseList[BridgeReleaseList.size() - 1];
      B.setInsertPoint(OldCI);
      O = OldCI->getArgOperand(0);
      auto *RI = OldCI;
      for (auto R : BridgeReleaseList) {
        if (B.isAtomic(R)) {
          RI = R;
          break;
        }
      }
      B.createBridgeReleaseN(RC.getSwiftRCIdentityRoot(O),
                             BridgeReleaseList.size(), RI);

      // Remove all old release instructions.
      for (auto *Inst : BridgeReleaseList) {
        Inst->eraseFromParent();
        ++NumBridgeRetainReleasesEliminatedByMergingIntoRetainReleaseN;
      }

      --NumBridgeRetainReleasesEliminatedByMergingIntoRetainReleaseN;
    }
    BridgeReleaseList.clear();
  }
}


bool SwiftARCContractImpl::run() {
  // intra-BB retain/release merging.
  DenseMap<Value *, LocalState> PtrToLocalStateMap;
  for (BasicBlock &BB : F) {
    for (auto II = BB.begin(), IE = BB.end(); II != IE; ) {
      // Preincrement iterator to avoid iteration issues in the loop.
      Instruction &Inst = *II++;

      auto Kind = classifyInstruction(Inst);
      switch (Kind) {
      case RT_RetainN:
      case RT_UnknownObjectRetainN:
      case RT_BridgeRetainN:
      case RT_ReleaseN:
      case RT_UnknownObjectReleaseN:
      case RT_BridgeReleaseN:
        // These are sometimes explicitly called by user code. Let them be as is
        // for now. Don't try to contract them, but also don't let them prevent
        // nearby contractions.
        continue;
      // Delete all fix lifetime and end borrow instructions. After llvm-ir they
      // have no use and show up as calls in the final binary.
      case RT_FixLifetime:
      case RT_EndBorrow:
        Inst.eraseFromParent();
        ++NumNoopDeleted;
        continue;
      case RT_Retain: {
        auto *CI = cast<CallInst>(&Inst);
        auto *ArgVal = RC.getSwiftRCIdentityRoot(CI->getArgOperand(0));

        LocalState &LocalEntry = PtrToLocalStateMap[ArgVal];
        LocalEntry.RetainList.push_back(CI);
        continue;
      }
      case RT_UnknownObjectRetain: {
        auto *CI = cast<CallInst>(&Inst);
        auto *ArgVal = RC.getSwiftRCIdentityRoot(CI->getArgOperand(0));

        LocalState &LocalEntry = PtrToLocalStateMap[ArgVal];
        LocalEntry.UnknownObjectRetainList.push_back(CI);
        continue;
      }
      case RT_Release: {
        // Stash any releases that we see.
        auto *CI = cast<CallInst>(&Inst);
        auto *ArgVal = RC.getSwiftRCIdentityRoot(CI->getArgOperand(0));

        LocalState &LocalEntry = PtrToLocalStateMap[ArgVal];
        LocalEntry.ReleaseList.push_back(CI);
        continue;
      }
      case RT_UnknownObjectRelease: {
        // Stash any releases that we see.
        auto *CI = cast<CallInst>(&Inst);
        auto *ArgVal = RC.getSwiftRCIdentityRoot(CI->getArgOperand(0));

        LocalState &LocalEntry = PtrToLocalStateMap[ArgVal];
        LocalEntry.UnknownObjectReleaseList.push_back(CI);
        continue;
      }
      case RT_BridgeRetain: {
        auto *CI = cast<CallInst>(&Inst);
        auto *ArgVal = RC.getSwiftRCIdentityRoot(CI->getArgOperand(0));

        LocalState &LocalEntry = PtrToLocalStateMap[ArgVal];
        LocalEntry.BridgeRetainList.push_back(CI);
        continue;
      }
      case RT_BridgeRelease: {
        auto *CI = cast<CallInst>(&Inst);
        auto *ArgVal = RC.getSwiftRCIdentityRoot(CI->getArgOperand(0));

        LocalState &LocalEntry = PtrToLocalStateMap[ArgVal];
        LocalEntry.BridgeReleaseList.push_back(CI);
        continue;
      }
      case RT_Unknown:
      case RT_AllocObject:
      case RT_NoMemoryAccessed:
      case RT_RetainUnowned:
      case RT_CheckUnowned:
      case RT_ObjCRelease:
      case RT_ObjCRetain:
        break;
      }

      if (Kind != RT_Unknown)
        continue;
      
      // If we have an unknown call, we need to create any retainN calls we
      // have seen. The reason why is that we do not want to move retains,
      // releases over isUniquelyReferenced calls. Specifically imagine this:
      //
      // retain(x); unknown(x); release(x); isUniquelyReferenced(x); retain(x);
      //
      // In this case we would with this optimization merge the last retain
      // with the first. This would then create an additional copy. The
      // release side of this is:
      //
      // retain(x); unknown(x); release(x); isUniquelyReferenced(x); release(x);
      //
      // Again in such a case by merging the first release with the second
      // release, we would be introducing an additional copy.
      //
      // Thus if we see an unknown call we merge together all retains and
      // releases before. This could be made more aggressive through
      // appropriate alias analysis and usage of LLVM's function attributes to
      // determine that a function does not touch globals.
      performRRNOptimization(PtrToLocalStateMap);
    }

    // Perform the RRNOptimization.
    performRRNOptimization(PtrToLocalStateMap);
    PtrToLocalStateMap.clear();
  }

  return Changed;
}

bool SwiftARCContract::runOnFunction(Function &F) {
  return SwiftARCContractImpl(F).run();
}

char SwiftARCContract::ID = 0;
INITIALIZE_PASS_BEGIN(SwiftARCContract, "swift-arc-contract",
                      "Swift ARC contraction", false, false)
INITIALIZE_PASS_END(SwiftARCContract,
                    "swift-arc-contract", "Swift ARC contraction",
                    false, false)

llvm::FunctionPass *swift::createSwiftARCContractPass() {
  initializeSwiftARCContractPass(*llvm::PassRegistry::getPassRegistry());
  return new SwiftARCContract();
}

void SwiftARCContract::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.setPreservesCFG();
}

llvm::PreservedAnalyses
SwiftARCContractPass::run(llvm::Function &F,
                          llvm::FunctionAnalysisManager &AM) {
  bool changed = SwiftARCContractImpl(F).run();
  if (!changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
