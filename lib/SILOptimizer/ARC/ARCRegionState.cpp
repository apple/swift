//===--- ARCRegionState.cpp -----------------------------------------------===//
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
#include "ARCRegionState.h"
#include "ARCSequenceOptUtils.h"
#include "RCStateTransitionVisitors.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Range.h"
#include "swift/SILOptimizer/Analysis/LoopRegionAnalysis.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;

llvm::cl::opt<bool> verifyARCLoopSummary(
    "verify-arc-loop-summary", llvm::cl::init(false),
    llvm::cl::desc("Verify if loop summary is correct in ARCLoopsOpts"));

//===----------------------------------------------------------------------===//
//                               ARCRegionState
//===----------------------------------------------------------------------===//

ARCRegionState::ARCRegionState(LoopRegion *R, bool AllowsLeaks)
    : Region(R), PtrToTopDownState(), PtrToBottomUpState(),
      AllowsLeaks(AllowsLeaks) {}

//===---
// Bottom Up Merge
//

/// Initialize this Region with the state of the successor region. This is
/// called on a region's state and then any other successors states are merged
/// in.
void ARCRegionState::initSuccBottomUp(ARCRegionState &SuccRegionState) {
  PtrToBottomUpState = SuccRegionState.PtrToBottomUpState;
}

/// Merge in the state of the successor basic block. Returns true if after the
/// merge operation the region is tracking any state. Returns false otherwise.
///
/// The return value enables an analysis to bail early.
///
/// This is an intersection operation.
void ARCRegionState::mergeSuccBottomUp(ARCRegionState &SuccRegionState) {
  // Otherwise for each [(SILValue, BottomUpState)] that we are tracking...
  for (auto &Pair : getBottomupStates()) {
    if (!Pair.has_value())
      continue;

    SILValue RefCountedValue = Pair->first;

    // If our SILValue was blotted, skip it. This will be ignored for the rest
    // of the ARC optimization.
    if (!RefCountedValue)
      continue;

    // Then attempt to lookup the corresponding (SILValue, BottomUpState) from
    // SuccRegion. If we fail to do so, blot this SILValue and continue.
    //
    // Since we are already initialized by initSuccBottomUp(), this has the
    // effect of an intersection.
    auto Other = SuccRegionState.PtrToBottomUpState.find(RefCountedValue);
    if (Other == SuccRegionState.PtrToBottomUpState.end()) {
      PtrToBottomUpState.erase(RefCountedValue);
      continue;
    }

    SILValue OtherRefCountedValue = (*Other)->first;

    // If the other ref count value was blotted, blot our value and continue.
    // This has the effect of an intersection since we already checked earlier
    // that RefCountedValue was not blotted.
    if (!OtherRefCountedValue) {
      PtrToBottomUpState.erase(RefCountedValue);
      continue;
    }

    BottomUpRefCountState &RefCountState = Pair->second;
    BottomUpRefCountState &OtherRefCountState = (*Other)->second;

    // Ok, now we know that the merged set can safely represent a set of
    // of instructions which together semantically act as one ref count
    // increment. Merge the two states together.
    if (!RefCountState.merge(OtherRefCountState)) {
      PtrToBottomUpState.erase(RefCountedValue);
    }
  }
}

//===---
// Top Down Merge
//

/// Initialize the state for this Region with the state of its predecessor
/// Region. Used to create an initial state before we merge in other
/// predecessors.
void ARCRegionState::initPredTopDown(ARCRegionState &PredRegionState) {
  PtrToTopDownState = PredRegionState.PtrToTopDownState;
}

/// Merge in the state of the predecessor basic block.
void ARCRegionState::mergePredTopDown(ARCRegionState &PredRegionState) {
  // For each [(SILValue, TopDownState)] that we are tracking...
  for (auto &Pair : getTopDownStates()) {
    if (!Pair.has_value())
      continue;
    SILValue RefCountedValue = Pair->first;

    // If our SILValue was blotted, skip it. This will be ignored in the rest of
    // the optimizer.
    if (!RefCountedValue)
      continue;

    // Then attempt to lookup the corresponding (SILValue, TopDownState) from
    // PredRegion. If we fail to do so, blot this SILValue and continue.
    //
    // Since we are already initialized by initPredTopDown(), this has the
    // effect of an intersection.
    auto Other = PredRegionState.PtrToTopDownState.find(RefCountedValue);
    if (Other == PredRegionState.PtrToTopDownState.end()) {
      PtrToTopDownState.erase(RefCountedValue);
      continue;
    }

    SILValue OtherRefCountedValue = (*Other)->first;

    // If the other ref count value was blotted, blot our value and continue.
    // This has the effect of an intersection.
    if (!OtherRefCountedValue) {
      PtrToTopDownState.erase(RefCountedValue);
      continue;
    }

    // Ok, so now we know that the ref counted value we are tracking was not
    // blotted on either side. Grab the states.
    TopDownRefCountState &RefCountState = Pair->second;
    TopDownRefCountState &OtherRefCountState = (*Other)->second;

    // Attempt to merge Other into this ref count state. If we fail, blot this
    // ref counted value and continue.
    if (!RefCountState.merge(OtherRefCountState)) {
      LLVM_DEBUG(llvm::dbgs() << "Failed to merge!\n");
      PtrToTopDownState.erase(RefCountedValue);
      continue;
    }
  }
}

//===---
// Bottom Up Dataflow
//

bool ARCRegionState::processBlockBottomUp(
    const LoopRegion *R, AliasAnalysis *AA, RCIdentityFunctionInfo *RCIA,
    EpilogueARCFunctionInfo *EAFI, LoopRegionFunctionInfo *LRFI,
    bool FreezeOwnedArgEpilogueReleases,
    BlotMapVector<SILInstruction *, BottomUpRefCountState> &IncToDecStateMap,
    ImmutablePointerSetFactory<SILInstruction *> &SetFactory) {
  LLVM_DEBUG(llvm::dbgs() << ">>>> Bottom Up!\n");

  SILBasicBlock &BB = *R->getBlock();
  BottomUpDataflowRCStateVisitor<ARCRegionState> DataflowVisitor(
      RCIA, EAFI, *this, FreezeOwnedArgEpilogueReleases, IncToDecStateMap,
      SetFactory);

  auto II = summarizedinterestinginsts_rbegin();
  auto IE = summarizedinterestinginsts_rend();

  // If we do not have any interesting instructions, bail and return false since
  // we can not have any nested instructions.
  if (II == IE)
    return false;

  // If II is not an arc significant terminator, skip it.
  if (*II == BB.getTerminator() &&
      !isARCSignificantTerminator(cast<TermInst>(*II)))
    ++II;

  bool NestingDetected = false;
  while (II != IE) {
    SILInstruction *I = *II;
    ++II;

    LLVM_DEBUG(llvm::dbgs() << "VISITING:\n    " << *I);

    auto Result = DataflowVisitor.visit(I->asSILNode());

    // If this instruction can have no further effects on another instructions,
    // continue. This happens for instance if we have cleared all of the state
    // we are tracking.
    if (Result.Kind == RCStateTransitionDataflowResultKind::NoEffects)
      continue;

    // Make sure that we propagate out whether or not nesting was detected.
    NestingDetected |= Result.NestingDetected;

    // This SILValue may be null if we were unable to find a specific RCIdentity
    // that the instruction "visits".
    SILValue Op = Result.RCIdentity;

    std::function<bool(SILInstruction *)> checkIfRefCountInstIsMatched =
        [&IncToDecStateMap](SILInstruction *Inst) {
          assert(isa<StrongRetainInst>(Inst) || isa<RetainValueInst>(Inst));
          return IncToDecStateMap.find(Inst) != IncToDecStateMap.end();
        };

    // For all other (reference counted value, ref count state) we are
    // tracking...
    for (auto &OtherState : getBottomupStates()) {
      // If the other state's value is blotted, skip it.
      if (!OtherState.has_value())
        continue;

      // If this is the state associated with the instruction that we are
      // currently visiting, bail.
      if (Op && OtherState->first == Op)
        continue;

      OtherState->second.updateForSameLoopInst(I, AA);
      OtherState->second.checkAndResetKnownSafety(
          I, OtherState->first, checkIfRefCountInstIsMatched, RCIA, AA);
    }
  }

  return NestingDetected;
}

// Returns true if any of the non-local successors of the region are leaking
// blocks. We currently do not handle early exits, but do handle trapping
// blocks. Returns false if otherwise
static bool hasEarlyExits(
    const LoopRegion *R, LoopRegionFunctionInfo *LRFI,
    llvm::DenseMap<const LoopRegion *, ARCRegionState *> &RegionStateInfo) {
  assert(R->isLoop() && "Expected a loop region that is representing a loop");

  // Go through all of our non local successors. If any of them cannot be
  // ignored, we bail for simplicity. This means that for now we do not handle
  // early exits.
  if (any_of(R->getNonLocalSuccs(), [&](unsigned SuccID) -> bool {
        return !RegionStateInfo[LRFI->getRegion(SuccID)]->allowsLeaks();
      })) {
    return true;
  }

  return false;
}

bool ARCRegionState::processLoopBottomUp(
    const LoopRegion *R, AliasAnalysis *AA, LoopRegionFunctionInfo *LRFI,
    RCIdentityFunctionInfo *RCIA,
    llvm::DenseMap<const LoopRegion *, ARCRegionState *> &RegionStateInfo,
    llvm::DenseSet<SILInstruction *> &UnmatchedRefCountInsts) {
  ARCRegionState *State = RegionStateInfo[R];

  // If we find that we have non-leaking early exits, clear state
  // and bail. We do not handle these for now.
  if (hasEarlyExits(R, LRFI, RegionStateInfo)) {
    clearBottomUpState();
    return false;
  }

  std::function<bool(SILInstruction *)> checkIfRefCountInstIsMatched =
      [&UnmatchedRefCountInsts](SILInstruction *Inst) {
        assert(isa<StrongRetainInst>(Inst) || isa<RetainValueInst>(Inst));
        return UnmatchedRefCountInsts.find(Inst) == UnmatchedRefCountInsts.end();
      };

  // For each state that we are currently tracking, apply our summarized
  // instructions to it.
  for (auto &OtherState : getBottomupStates()) {
    if (!OtherState.has_value())
      continue;

    for (auto *I : State->getSummarizedInterestingInsts()) {
      OtherState->second.updateForDifferentLoopInst(I, AA);
      OtherState->second.checkAndResetKnownSafety(
          I, OtherState->first, checkIfRefCountInstIsMatched, RCIA, AA);
    }
#ifndef NDEBUG
    // Verify updateForDifferentLoopInst is conservative enough that the flow
    // sensitive native of the loop summarized instructions does not matter.
    if (verifyARCLoopSummary) {
      auto NewRefCountState = OtherState->second;
      for (auto *I : State->getSummarizedInterestingInsts()) {
        NewRefCountState.updateForDifferentLoopInst(I, AA);
      }
      assert(NewRefCountState.getLatticeState() ==
             OtherState->second.getLatticeState());
    }
#endif
  }

  return false;
}

bool ARCRegionState::processBottomUp(
    AliasAnalysis *AA, RCIdentityFunctionInfo *RCIA,
    EpilogueARCFunctionInfo *EAFI, LoopRegionFunctionInfo *LRFI,
    bool FreezeOwnedArgEpilogueReleases,
    llvm::DenseSet<SILInstruction *> &UnmatchedRefCountInsts,
    BlotMapVector<SILInstruction *, BottomUpRefCountState> &IncToDecStateMap,
    llvm::DenseMap<const LoopRegion *, ARCRegionState *> &RegionStateInfo,
    ImmutablePointerSetFactory<SILInstruction *> &SetFactory) {
  const LoopRegion *R = getRegion();

  // We only process basic blocks for now. This ensures that we always propagate
  // the empty set from loops.
  if (!R->isBlock())
    return processLoopBottomUp(R, AA, LRFI, RCIA, RegionStateInfo,
                               UnmatchedRefCountInsts);

  return processBlockBottomUp(R, AA, RCIA, EAFI, LRFI, FreezeOwnedArgEpilogueReleases,
                              IncToDecStateMap, SetFactory);
}

//===---
// Top Down Dataflow
//

bool ARCRegionState::processBlockTopDown(
    SILBasicBlock &BB, AliasAnalysis *AA, RCIdentityFunctionInfo *RCIA,
    BlotMapVector<SILInstruction *, TopDownRefCountState> &DecToIncStateMap,
    ImmutablePointerSetFactory<SILInstruction *> &SetFactory) {
  LLVM_DEBUG(llvm::dbgs() << ">>>> Top Down!\n");

  bool NestingDetected = false;

  TopDownDataflowRCStateVisitor<ARCRegionState> DataflowVisitor(
      RCIA, *this, DecToIncStateMap, SetFactory);

  // If the current BB is the entry BB, initialize a state corresponding to each
  // of its owned parameters. This enables us to know that if we see a retain
  // before any decrements that the retain is known safe.
  //
  // We do not handle guaranteed parameters here since those are handled in the
  // code in GlobalARCPairingAnalysis. This is because even if we don't do
  // anything, we will still pair the retain, releases and then the guaranteed
  // parameter will ensure it is known safe to remove them.
  if (BB.isEntry()) {
    auto Args = BB.getArguments();
    for (unsigned i = 0, e = Args.size(); i != e; ++i) {
      DataflowVisitor.visit(Args[i]);
    }
  }

  // For each instruction I in BB...
  for (auto *I : SummarizedInterestingInsts) {

    LLVM_DEBUG(llvm::dbgs() << "VISITING:\n    " << *I);

    auto Result = DataflowVisitor.visit(I->asSILNode());

    // If this instruction can have no further effects on another instructions,
    // continue. This happens for instance if we have cleared all of the state
    // we are tracking.
    if (Result.Kind == RCStateTransitionDataflowResultKind::NoEffects)
      continue;

    // Make sure that we propagate out whether or not nesting was detected.
    NestingDetected |= Result.NestingDetected;

    // This SILValue may be null if we were unable to find a specific RCIdentity
    // that the instruction "visits".
    SILValue Op = Result.RCIdentity;

    std::function<bool(SILInstruction *)> checkIfRefCountInstIsMatched =
        [&DecToIncStateMap](SILInstruction *Inst) {
          assert(isa<StrongReleaseInst>(Inst) || isa<ReleaseValueInst>(Inst));
          return DecToIncStateMap.find(Inst) != DecToIncStateMap.end();
        };

    // For all other [(SILValue, TopDownState)] we are tracking...
    for (auto &OtherState : getTopDownStates()) {
      // If the other state's value is blotted, skip it.
      if (!OtherState.has_value())
        continue;

      // If we visited an increment or decrement successfully (and thus Op is
      // set), if this is the state for this operand, skip it. We already
      // processed it.
      if (Op && OtherState->first == Op)
        continue;

      OtherState->second.updateForSameLoopInst(I, AA);
      OtherState->second.checkAndResetKnownSafety(
          I, OtherState->first, checkIfRefCountInstIsMatched, RCIA, AA);
    }
  }

  return NestingDetected;
}

bool ARCRegionState::processLoopTopDown(
    const LoopRegion *R, ARCRegionState *State, AliasAnalysis *AA,
    LoopRegionFunctionInfo *LRFI, RCIdentityFunctionInfo *RCIA,
    llvm::DenseSet<SILInstruction *> &UnmatchedRefCountInsts) {

  assert(R->isLoop() && "We assume we are processing a loop");

  // If we have more than 2 predecessors, we do not have a pre-header. We do not
  // support this case since canonicalization failed.
  if (R->pred_size() != 1) {
    clearTopDownState();
    return false;
  }

  auto *PredRegion = LRFI->getRegion(*R->pred_begin());
  (void) PredRegion;
  assert(PredRegion->isBlock() && "Expected the predecessor region to be a "
                                  "block");

  std::function<bool(SILInstruction *)> checkIfRefCountInstIsMatched =
      [&UnmatchedRefCountInsts](SILInstruction *Inst) {
        assert(isa<StrongReleaseInst>(Inst) || isa<ReleaseValueInst>(Inst));
        return UnmatchedRefCountInsts.find(Inst) == UnmatchedRefCountInsts.end();
      };

  // For each state that we are currently tracking, apply our summarized
  // instructions to it.
  for (auto &OtherState : getTopDownStates()) {
    if (!OtherState.has_value())
      continue;

    for (auto *I : State->getSummarizedInterestingInsts()) {
      OtherState->second.updateForDifferentLoopInst(I, AA);
      OtherState->second.checkAndResetKnownSafety(
          I, OtherState->first, checkIfRefCountInstIsMatched, RCIA, AA);
    }
#ifndef NDEBUG
    // Verify updateForDifferentLoopInst is conservative enough that the flow
    // sensitive native of the loop summarized instructions does not matter.
    if (verifyARCLoopSummary) {
      auto NewRefCountState = OtherState->second;
      for (auto *I : State->getSummarizedInterestingInsts()) {
        NewRefCountState.updateForDifferentLoopInst(I, AA);
      }
      assert(NewRefCountState.getLatticeState() ==
             OtherState->second.getLatticeState());
    }
#endif
  }

  return false;
}

bool ARCRegionState::processTopDown(
    AliasAnalysis *AA, RCIdentityFunctionInfo *RCIA,
    LoopRegionFunctionInfo *LRFI,
    llvm::DenseSet<SILInstruction *> &UnmatchedRefCountInsts,
    BlotMapVector<SILInstruction *, TopDownRefCountState> &DecToIncStateMap,
    llvm::DenseMap<const LoopRegion *, ARCRegionState *> &RegionStateInfo,
    ImmutablePointerSetFactory<SILInstruction *> &SetFactory) {
  const LoopRegion *R = getRegion();

  // We only process basic blocks for now. This ensures that we always propagate
  // the empty set from loops.
  if (!R->isBlock())
    return processLoopTopDown(R, RegionStateInfo[R], AA, LRFI, RCIA,
                              UnmatchedRefCountInsts);

  return processBlockTopDown(*R->getBlock(), AA, RCIA, DecToIncStateMap,
                             SetFactory);
}

//===---
// Summary
//

static bool isStrongEntranceInstruction(const SILInstruction &I) {
  switch (I.getKind()) {
  case SILInstructionKind::AllocRefInst:
  case SILInstructionKind::AllocRefDynamicInst:
  case SILInstructionKind::AllocBoxInst:
    return true;
  default:
    return false;
  }
}

void ARCRegionState::summarizeBlock(SILBasicBlock *BB) {
  SummarizedInterestingInsts.clear();

  for (auto &I : *BB)
    // FIXME: mayReleaseOrReadRefCount should be a strict subset of
    // canUseObject. If not, there is a bug in canUseObject.
    if (canUseObject(&I) || I.mayReleaseOrReadRefCount() ||
        isStrongEntranceInstruction(I))
      SummarizedInterestingInsts.push_back(&I);
}

void ARCRegionState::summarizeLoop(
    const LoopRegion *R, LoopRegionFunctionInfo *LRFI,
    llvm::DenseMap<const LoopRegion *, ARCRegionState *> &RegionStateInfo) {
  SummarizedInterestingInsts.clear();
  for (unsigned SubregionID : R->getSubregions()) {
    LoopRegion *Subregion = LRFI->getRegion(SubregionID);
    ARCRegionState *SubregionState = RegionStateInfo[Subregion];
    std::copy(SubregionState->summarizedinterestinginsts_begin(),
              SubregionState->summarizedinterestinginsts_end(),
              std::back_inserter(SummarizedInterestingInsts));
  }
}

void ARCRegionState::summarize(
    LoopRegionFunctionInfo *LRFI,
    llvm::DenseMap<const LoopRegion *, ARCRegionState *> &RegionStateInfo) {
  const LoopRegion *R = getRegion();

  // We do not need to summarize a function since it is the outermost loop.
  if (R->isFunction())
    return;

  assert(R->isLoop() && "Expected to be called on a loop");
  // We know that all of our sub blocks have the correct interesting insts since
  // we did one scan at the beginning and are updating our interesting inst list
  // as we move around retains/releases. Additionally since we are going through
  // the loop nest bottom up, all of our subloops have already been
  // summarized. Thus all we need to do is gather up the interesting
  // instructions from our subregions.
  summarizeLoop(R, LRFI, RegionStateInfo);
}

void ARCRegionState::addInterestingInst(SILInstruction *TargetInst) {
  // Insert I into its location in the interesting instruction list.
  SILBasicBlock *BB = getRegion()->getBlock();
  assert(TargetInst->getParent() == BB);

  auto II = BB->begin();
  auto IE = BB->end();
  assert(II != IE && "I can not be an element of an empty block");

  auto SI = SummarizedInterestingInsts.begin();
  auto SE = SummarizedInterestingInsts.end();

  while (II != IE) {
    if (SI == SE) {
      // Ok, TargetInst is after all of the interesting insts. Append it to the
      // list.
      SummarizedInterestingInsts.push_back(TargetInst);
      return;
    }

    // Move II down the block until it hits TargetInst or the first
    // SummarizedInterestingInst.
    while (&*II != *SI && &*II != TargetInst) {
      ++II;
    }

    // If II == SI and TargetInst == II then there is nothing further to do.
    if (&*II == TargetInst) {
      assert(&*II != *SI);
      SummarizedInterestingInsts.insert(SI, TargetInst);
      return;
    }

    // If we reach this point, then we know that II == SI and we have not found
    // TargetInst yet. So we move to the next II, SI.
    ++II;
    ++SI;
  }

  llvm_unreachable("Could not find Inst in the block?!");
}

void ARCRegionState::removeInterestingInst(SILInstruction *I) {
  SummarizedInterestingInsts.erase(
      std::remove(SummarizedInterestingInsts.begin(),
                  SummarizedInterestingInsts.end(), I),
      SummarizedInterestingInsts.end());
}
