//===--- RCStateTransitionVisitors.cpp ------------------------------------===//
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

#define DEBUG_TYPE "arc-sequence-opts"
#include "RCStateTransitionVisitors.h"
#include "ARCBBState.h"
#include "swift/Basic/Assertions.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "llvm/Support/Debug.h"

using namespace swift;

namespace {

using ARCBBState = ARCSequenceDataflowEvaluator::ARCBBState;

} // end anonymous namespace


//===----------------------------------------------------------------------===//
//                             Utilities 
//===----------------------------------------------------------------------===//

/// Return true if this instruction is the epilogue release for the \p Arg.
/// false otherwise.
static bool isOwnedArgumentEpilogueRelease(SILInstruction *I, SILValue Arg,
                                           EpilogueARCFunctionInfo *EAFI) {
  auto Releases = 
    EAFI->computeEpilogueARCInstructions(
            EpilogueARCContext::EpilogueARCKind::Release, Arg);
  return Releases.size() && Releases.count(I); 
}

static bool isGuaranteedSafetyByEpilogueRelease(SILInstruction *I, SILValue Arg,
                                                EpilogueARCFunctionInfo *EAFI) {
  auto Releases = 
    EAFI->computeEpilogueARCInstructions(
            EpilogueARCContext::EpilogueARCKind::Release, Arg);
  return Releases.size() && !Releases.count(I);
}

//===----------------------------------------------------------------------===//
//                      BottomUpRCStateTransitionVisitor
//===----------------------------------------------------------------------===//

template <class ARCState>
BottomUpDataflowRCStateVisitor<ARCState>::BottomUpDataflowRCStateVisitor(
    RCIdentityFunctionInfo *RCFI, EpilogueARCFunctionInfo *EAFI,
    ARCState &State, bool FreezeOwnedArgEpilogueReleases,
    IncToDecStateMapTy &IncToDecStateMap,
    ImmutablePointerSetFactory<SILInstruction *> &SetFactory)
    : RCFI(RCFI), EAFI(EAFI), DataflowState(State),
      FreezeOwnedArgEpilogueReleases(FreezeOwnedArgEpilogueReleases),
      IncToDecStateMap(IncToDecStateMap), SetFactory(SetFactory) {}

template <class ARCState>
typename BottomUpDataflowRCStateVisitor<ARCState>::DataflowResult
BottomUpDataflowRCStateVisitor<ARCState>::
visitAutoreleasePoolCall(SILNode *N) {
  DataflowState.clear();

  // We just cleared our BB State so we have no more possible effects.
  return DataflowResult(RCStateTransitionDataflowResultKind::NoEffects);
}

// private helper method since C++ does not have extensions... *sigh*.
//
// TODO: This needs a better name.
template <class ARCState>
static bool isKnownSafe(BottomUpDataflowRCStateVisitor<ARCState> *State,
                        SILInstruction *I, SILValue Op) {
  // If we are running with 'frozen' owned arg releases, check if we have a
  // frozen use in the side table. If so, this release must be known safe.
  if (State->FreezeOwnedArgEpilogueReleases)
    if (isGuaranteedSafetyByEpilogueRelease(I, Op, State->EAFI))
      return true;

  // A guaranteed function argument is guaranteed to outlive the function we are
  // processing. So bottom up for such a parameter, we are always known safe.
  if (auto *Arg = dyn_cast<SILFunctionArgument>(Op)) {
    if (Arg->hasConvention(SILArgumentConvention::Direct_Guaranteed)) {
      return true;
    }
  }

  // If Op is a load from an in_guaranteed parameter, it is guaranteed as well.
  if (auto *LI = dyn_cast<LoadInst>(Op)) {
    SILValue RCIdentity = State->RCFI->getRCIdentityRoot(LI->getOperand());
    if (auto *Arg = dyn_cast<SILFunctionArgument>(RCIdentity)) {
      if (Arg->hasConvention(SILArgumentConvention::Indirect_In_Guaranteed)) {
        return true;
      }
    }
  }

  return false;
}

template <class ARCState>
typename BottomUpDataflowRCStateVisitor<ARCState>::DataflowResult
BottomUpDataflowRCStateVisitor<ARCState>::visitStrongDecrement(SILNode *N) {
  auto *I = dyn_cast<SILInstruction>(N);
  if (!I)
    return DataflowResult();

  SILValue Op = RCFI->getRCIdentityRoot(I->getOperand(0));

  // If this instruction is a post dominating release, skip it so we don't pair
  // it up with anything. Do make sure that it does not effect any other
  // instructions.
  if (FreezeOwnedArgEpilogueReleases && isOwnedArgumentEpilogueRelease(I, Op, EAFI))
    return DataflowResult(Op);

  BottomUpRefCountState &State = DataflowState.getBottomUpRefCountState(Op);
  bool NestingDetected = State.initWithMutatorInst(SetFactory.get(I), RCFI);

  if (isKnownSafe(this, I, Op)) {
    State.updateKnownSafe(true);
  }

  LLVM_DEBUG(llvm::dbgs() << "    REF COUNT DECREMENT! Known Safe: "
                          << (State.isKnownSafe() ? "yes" : "no") << "\n");

  // Continue on to see if our reference decrement could potentially affect
  // any other pointers via a use or a decrement.
  return DataflowResult(Op, NestingDetected);
}

template <class ARCState>
typename BottomUpDataflowRCStateVisitor<ARCState>::DataflowResult
BottomUpDataflowRCStateVisitor<ARCState>::visitStrongIncrement(SILNode *N) {
  auto *I = dyn_cast<SILInstruction>(N);
  if (!I)
    return DataflowResult();

  // Look up the state associated with its operand...
  SILValue Op = RCFI->getRCIdentityRoot(I->getOperand(0));
  auto &RefCountState = DataflowState.getBottomUpRefCountState(Op);

  LLVM_DEBUG(llvm::dbgs() << "    REF COUNT INCREMENT!\n");

  // If we find a state initialized with a matching increment, pair this
  // decrement with a copy of the ref count state and then clear the ref
  // count state in preparation for any future pairs we may see on the same
  // pointer.
  if (RefCountState.isRefCountInstMatchedToTrackedInstruction(I)) {
    // Copy the current value of ref count state into the result map.
    IncToDecStateMap[I] = RefCountState;
    LLVM_DEBUG(llvm::dbgs() << "    MATCHING DECREMENT:"
                            << RefCountState.getRCRoot());

    // Clear the ref count state so it can be used for future pairs we may
    // see.
    RefCountState.clear();
  }
#ifndef NDEBUG
  else {
    if (RefCountState.isTrackingRefCountInst()) {
      LLVM_DEBUG(llvm::dbgs() << "    FAILED MATCH DECREMENT:"
                              << RefCountState.getRCRoot());
    } else {
      LLVM_DEBUG(llvm::dbgs() << "    FAILED MATCH DECREMENT. Not tracking a "
                                 "decrement.\n");
    }
  }
#endif
  return DataflowResult(Op);
}

//===----------------------------------------------------------------------===//
//                       TopDownDataflowRCStateVisitor
//===----------------------------------------------------------------------===//

template <class ARCState>
TopDownDataflowRCStateVisitor<ARCState>::TopDownDataflowRCStateVisitor(
    RCIdentityFunctionInfo *RCFI, ARCState &DataflowState,
    DecToIncStateMapTy &DecToIncStateMap,
    ImmutablePointerSetFactory<SILInstruction *> &SetFactory)
    : RCFI(RCFI), DataflowState(DataflowState),
      DecToIncStateMap(DecToIncStateMap), SetFactory(SetFactory) {}

template <class ARCState>
typename TopDownDataflowRCStateVisitor<ARCState>::DataflowResult
TopDownDataflowRCStateVisitor<ARCState>::
visitAutoreleasePoolCall(SILNode *N) {
  DataflowState.clear();
  // We just cleared our BB State so we have no more possible effects.
  return DataflowResult(RCStateTransitionDataflowResultKind::NoEffects);
}

template <class ARCState>
typename TopDownDataflowRCStateVisitor<ARCState>::DataflowResult
TopDownDataflowRCStateVisitor<ARCState>::visitStrongDecrement(SILNode *N) {
  auto *I = dyn_cast<SILInstruction>(N);
  if (!I)
    return DataflowResult();

  // Look up the state associated with I's operand...
  SILValue Op = RCFI->getRCIdentityRoot(I->getOperand(0));
  auto &RefCountState = DataflowState.getTopDownRefCountState(Op);

  LLVM_DEBUG(llvm::dbgs() << "    REF COUNT DECREMENT!\n");

  // If we are tracking an increment on the ref count root associated with
  // the decrement and the decrement matches, pair this decrement with a
  // copy of the increment state and then clear the original increment state
  // so that we are ready to process further values.
  if (RefCountState.isRefCountInstMatchedToTrackedInstruction(I)) {
    // Copy the current value of ref count state into the result map.
    DecToIncStateMap[I] = RefCountState;
    LLVM_DEBUG(llvm::dbgs() << "    MATCHING INCREMENT:\n"
                            << RefCountState.getRCRoot());

    // Clear the ref count state in preparation for more pairs.
    RefCountState.clear();
  }
#if NDEBUG
  else {
    if (RefCountState.isTrackingRefCountInst()) {
      LLVM_DEBUG(llvm::dbgs() << "    FAILED MATCH INCREMENT:\n"
                              << RefCountState.getValue());
    } else {
      LLVM_DEBUG(llvm::dbgs() << "    FAILED MATCH. NO INCREMENT.\n");
    }
  }
#endif

  // Otherwise we continue processing the reference count decrement to see if
  // the decrement can affect any other pointers that we are tracking.
  return DataflowResult(Op);
}

template <class ARCState>
typename TopDownDataflowRCStateVisitor<ARCState>::DataflowResult
TopDownDataflowRCStateVisitor<ARCState>::visitStrongIncrement(SILNode *N) {
  auto *I = dyn_cast<SILInstruction>(N);
  if (!I)
    return DataflowResult();

  // Map the increment's operand to a newly initialized or reinitialized ref
  // count state and continue...
  SILValue Op = RCFI->getRCIdentityRoot(I->getOperand(0));
  auto &State = DataflowState.getTopDownRefCountState(Op);
  bool NestingDetected = State.initWithMutatorInst(SetFactory.get(I), RCFI);

  LLVM_DEBUG(llvm::dbgs() << "    REF COUNT INCREMENT! Known Safe: "
                          << (State.isKnownSafe() ? "yes" : "no") << "\n");

  // Continue processing in case this increment could be a CanUse for a
  // different pointer.
  return DataflowResult(Op, NestingDetected);
}

template <class ARCState>
typename TopDownDataflowRCStateVisitor<ARCState>::DataflowResult
TopDownDataflowRCStateVisitor<ARCState>::visitStrongEntranceArgument(
    SILFunctionArgument *Arg) {
  LLVM_DEBUG(llvm::dbgs() << "VISITING ENTRANCE ARGUMENT: " << *Arg);

  if (!Arg->hasConvention(SILArgumentConvention::Direct_Owned)) {
    LLVM_DEBUG(llvm::dbgs() << "    Not owned! Bailing!\n");
    return DataflowResult();
  }

  LLVM_DEBUG(llvm::dbgs() << "    Initializing state.\n");

  auto &State = DataflowState.getTopDownRefCountState(Arg);
  State.initWithArg(Arg);

  return DataflowResult();
}

template <class ARCState>
typename TopDownDataflowRCStateVisitor<ARCState>::DataflowResult
TopDownDataflowRCStateVisitor<ARCState>::
visitStrongEntranceApply(ApplyInst *AI) {
  LLVM_DEBUG(llvm::dbgs() << "VISITING ENTRANCE APPLY: " << *AI);

  // We should have checked earlier that AI has an owned result value. To
  // prevent mistakes, assert that here.
#ifndef NDEBUG
  bool hasOwnedResult = false;
  for (auto result : AI->getSubstCalleeConv().getDirectSILResults()) {
    if (result.getConvention() == ResultConvention::Owned)
      hasOwnedResult = true;
  }
  assert(hasOwnedResult && "Expected AI to be Owned here");
#endif

  // Otherwise, return a dataflow result containing a +1.
  LLVM_DEBUG(llvm::dbgs() << "    Initializing state.\n");

  auto &State = DataflowState.getTopDownRefCountState(AI);
  State.initWithEntranceInst(SetFactory.get(AI), AI);

  return DataflowResult(AI);
}

template <class ARCState>
typename TopDownDataflowRCStateVisitor<ARCState>::DataflowResult
TopDownDataflowRCStateVisitor<ARCState>::visitStrongEntrancePartialApply(
    PartialApplyInst *PAI) {
  LLVM_DEBUG(llvm::dbgs() << "VISITING ENTRANCE PARTIAL APPLY: " << *PAI);

  // Rreturn a dataflow result containing a +1.
  LLVM_DEBUG(llvm::dbgs() << "    Initializing state.\n");

  auto &State = DataflowState.getTopDownRefCountState(PAI);
  State.initWithEntranceInst(SetFactory.get(PAI), PAI);

  return DataflowResult(PAI);
}

template <class ARCState>
typename TopDownDataflowRCStateVisitor<ARCState>::DataflowResult
TopDownDataflowRCStateVisitor<ARCState>::
visitStrongEntranceAllocRef(AllocRefInst *ARI) {
  // Alloc refs always introduce new references at +1.
  TopDownRefCountState &State = DataflowState.getTopDownRefCountState(ARI);
  State.initWithEntranceInst(SetFactory.get(ARI), ARI);

  return DataflowResult(ARI);
}

template <class ARCState>
typename TopDownDataflowRCStateVisitor<ARCState>::DataflowResult
TopDownDataflowRCStateVisitor<ARCState>::
visitStrongEntranceAllocRefDynamic(AllocRefDynamicInst *ARI) {
  // Alloc ref dynamic always introduce references at +1.
  auto &State = DataflowState.getTopDownRefCountState(ARI);
  State.initWithEntranceInst(SetFactory.get(ARI), ARI);

  return DataflowResult(ARI);
}

template <class ARCState>
typename TopDownDataflowRCStateVisitor<ARCState>::DataflowResult
TopDownDataflowRCStateVisitor<ARCState>::
visitStrongAllocBox(AllocBoxInst *ABI) {
  // Alloc box introduces a ref count of +1 on its container.
  auto &State = DataflowState.getTopDownRefCountState(ABI);
  State.initWithEntranceInst(SetFactory.get(ABI), ABI);
  return DataflowResult(ABI);
}

template <class ARCState>
typename TopDownDataflowRCStateVisitor<ARCState>::DataflowResult
TopDownDataflowRCStateVisitor<ARCState>::
visitStrongEntrance(SILNode *N) {
  if (auto *Arg = dyn_cast<SILFunctionArgument>(N))
    return visitStrongEntranceArgument(Arg);

  if (auto *AI = dyn_cast<ApplyInst>(N))
    return visitStrongEntranceApply(AI);

  if (auto *ARI = dyn_cast<AllocRefInst>(N))
    return visitStrongEntranceAllocRef(ARI);

  if (auto *ARI = dyn_cast<AllocRefDynamicInst>(N))
    return visitStrongEntranceAllocRefDynamic(ARI);

  if (auto *ABI = dyn_cast<AllocBoxInst>(N))
    return visitStrongAllocBox(ABI);

  if (auto *PAI = dyn_cast<PartialApplyInst>(N))
    return visitStrongEntrancePartialApply(PAI);

  return DataflowResult();
}

//===----------------------------------------------------------------------===//
//                           Template Instantiation
//===----------------------------------------------------------------------===//

namespace swift {

template class BottomUpDataflowRCStateVisitor<ARCBBState>;
template class BottomUpDataflowRCStateVisitor<ARCRegionState>;
template class TopDownDataflowRCStateVisitor<ARCBBState>;
template class TopDownDataflowRCStateVisitor<ARCRegionState>;

} // namespace swift
