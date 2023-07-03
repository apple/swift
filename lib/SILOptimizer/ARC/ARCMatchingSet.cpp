//===--- ARCMatchingSet.cpp - ARC Matching Set Builder --------------------===//
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

#include "ARCMatchingSet.h"
#include "RefCountState.h"
#include "swift/Basic/BlotMapVector.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                          ARC Matching Set Builder
//===----------------------------------------------------------------------===//

/// Match retains to releases and return whether or not all of the releases
/// were known safe.
llvm::Optional<MatchingSetFlags>
ARCMatchingSetBuilder::matchIncrementsToDecrements() {
  MatchingSetFlags Flags = {true, true};

  // For each increment in our list of new increments.
  //
  // FIXME: Refactor this into its own function.
  for (SILInstruction *Increment : NewIncrements) {
    LLVM_DEBUG(llvm::dbgs() << "    Looking up state for increment: "
                            << *Increment);

    auto BURefCountState = BUMap.find(Increment);
    assert(BURefCountState != BUMap.end() && "Catch this on debug builds.");
    if (BURefCountState == BUMap.end()) {
      LLVM_DEBUG(llvm::dbgs() << "        FAILURE! Could not find state for "
                                 "increment!\n");
      return llvm::None;
    }

    LLVM_DEBUG(llvm::dbgs() << "        SUCCESS! Found state for increment.\n");

    // If we are not tracking a ref count inst for this increment, there is
    // nothing we can pair it with implying we should skip it.
    if (!(*BURefCountState)->second.isTrackingRefCountInst()) {
      LLVM_DEBUG(llvm::dbgs() << "    SKIPPING INCREMENT! State not tracking "
                                 "any instruction.\n");
      continue;
    }

    bool BUIsKnownSafe = (*BURefCountState)->second.isKnownSafe();
    LLVM_DEBUG(llvm::dbgs() << "        BOTTOM UP KNOWNSAFE: "
                            << (BUIsKnownSafe ? "true" : "false") << "\n");
    Flags.KnownSafe &= BUIsKnownSafe;

    bool BUCodeMotionSafe = (*BURefCountState)->second.isCodeMotionSafe();
    LLVM_DEBUG(llvm::dbgs() << "        BOTTOM UP CODEMOTIONSAFE: "
                            << (BUCodeMotionSafe ? "true" : "false") << "\n");
    Flags.CodeMotionSafe &= BUCodeMotionSafe;

    // Now that we know we have an inst, grab the decrement.
    for (auto DecIter : (*BURefCountState)->second.getInstructions()) {
      SILInstruction *Decrement = DecIter;
      LLVM_DEBUG(llvm::dbgs() << "        Decrement: " << *Decrement);

      // Now grab the increment matched up with the decrement from the bottom up
      // map.
      // If we can't find it, bail we can't match this increment up with
      // anything.
      auto TDRefCountState = TDMap.find(Decrement);
      if (TDRefCountState == TDMap.end()) {
        LLVM_DEBUG(llvm::dbgs() << "            FAILURE! Could not find state "
                                   "for decrement.\n");
        return llvm::None;
      }
      LLVM_DEBUG(llvm::dbgs() << "            SUCCESS! Found state for "
                                 "decrement.\n");

      // Make sure the increment we are looking at is also matched to our
      // decrement. Otherwise bail.
      if (!(*TDRefCountState)->second.isTrackingRefCountInst() ||
          !(*TDRefCountState)->second.containsInstruction(Increment)) {
        LLVM_DEBUG(
            llvm::dbgs() << "            FAILURE! Not tracking instruction or "
                            "found increment that did not match.\n");
        return llvm::None;
      }

      // Add the decrement to the decrement to move set. If we don't insert
      // anything, just continue.
      if (!MatchSet.Decrements.insert(Decrement)) {
        LLVM_DEBUG(llvm::dbgs()
                   << "        SKIPPING! Already processed this decrement\n");
        continue;
      }

      NewDecrements.push_back(Decrement);
    }
  }

  return Flags;
}

llvm::Optional<MatchingSetFlags>
ARCMatchingSetBuilder::matchDecrementsToIncrements() {
  MatchingSetFlags Flags = {true, true};

  // For each increment in our list of new increments.
  //
  // FIXME: Refactor this into its own function.
  for (SILInstruction *Decrement : NewDecrements) {
    LLVM_DEBUG(llvm::dbgs() << "    Looking up state for decrement: "
                            << *Decrement);

    auto TDRefCountState = TDMap.find(Decrement);
    assert(TDRefCountState != TDMap.end() && "Catch this on debug builds.");
    if (TDRefCountState == TDMap.end()) {
      LLVM_DEBUG(llvm::dbgs() << "        FAILURE! Could not find state for "
                                 "increment!\n");
      return llvm::None;
    }

    LLVM_DEBUG(llvm::dbgs() << "        SUCCESS! Found state for decrement.\n");

    // If we are not tracking a ref count inst for this increment, there is
    // nothing we can pair it with implying we should skip it.
    if (!(*TDRefCountState)->second.isTrackingRefCountInst()) {
      LLVM_DEBUG(llvm::dbgs() << "    SKIPPING DECREMENT! State not tracking "
                                 "any instruction.\n");
      continue;
    }

    bool TDIsKnownSafe = (*TDRefCountState)->second.isKnownSafe();
    LLVM_DEBUG(llvm::dbgs() << "        TOP DOWN KNOWNSAFE: "
                            << (TDIsKnownSafe ? "true" : "false") << "\n");
    Flags.KnownSafe &= TDIsKnownSafe;

    bool TDCodeMotionSafe = (*TDRefCountState)->second.isCodeMotionSafe();
    LLVM_DEBUG(llvm::dbgs() << "        TOP DOWN CODEMOTIONSAFE: "
                            << (TDCodeMotionSafe ? "true" : "false") << "\n");
    Flags.CodeMotionSafe &= TDCodeMotionSafe;

    // Now that we know we have an inst, grab the decrement.
    for (auto IncIter : (*TDRefCountState)->second.getInstructions()) {
      SILInstruction *Increment = IncIter;
      LLVM_DEBUG(llvm::dbgs() << "        Increment: " << *Increment);

      // Now grab the increment matched up with the decrement from the bottom up
      // map.
      // If we can't find it, bail we can't match this increment up with
      // anything.
      auto BURefCountState = BUMap.find(Increment);
      if (BURefCountState == BUMap.end()) {
        LLVM_DEBUG(llvm::dbgs() << "            FAILURE! Could not find state "
                                   "for increment.\n");
        return llvm::None;
      }

      LLVM_DEBUG(
          llvm::dbgs() << "            SUCCESS! Found state for increment.\n");

      // Make sure the increment we are looking at is also matched to our
      // decrement.
      // Otherwise bail.
      if (!(*BURefCountState)->second.isTrackingRefCountInst() ||
          !(*BURefCountState)->second.containsInstruction(Decrement)) {
        LLVM_DEBUG(
            llvm::dbgs() << "            FAILURE! Not tracking instruction or "
                            "found increment that did not match.\n");
        return llvm::None;
      }

      // Add the decrement to the decrement to move set. If we don't insert
      // anything, just continue.
      if (!MatchSet.Increments.insert(Increment)) {
        LLVM_DEBUG(llvm::dbgs()
                   << "    SKIPPING! Already processed this increment.\n");
        continue;
      }

      NewIncrements.push_back(Increment);
    }
  }

  return Flags;
}

/// Visit each retain/release that is matched up to our operand over and over
/// again until we converge by not adding any more to the set which we can move.
/// If we find a situation that we cannot handle, we bail and return false. If
/// we succeed and it is safe to move increment/releases, we return true.
bool ARCMatchingSetBuilder::matchUpIncDecSetsForPtr() {
  bool KnownSafeTD = true;
  bool KnownSafeBU = true;
  bool CodeMotionSafeTD = true;
  bool CodeMotionSafeBU = true;

  while (true) {
    LLVM_DEBUG(llvm::dbgs() << "Attempting to match up increments -> "
               "decrements:\n");
    // For each increment in our list of new increments, attempt to match them
    // up with decrements.
    auto Result = matchIncrementsToDecrements();
    if (!Result) {
      LLVM_DEBUG(llvm::dbgs() << "    FAILED TO MATCH INCREMENTS -> "
                 "DECREMENTS!\n");
      return false;
    }
    if (!Result->KnownSafe) {
      LLVM_DEBUG(llvm::dbgs() << "    NOT KNOWN SAFE!\n");
      KnownSafeTD = false;
    }
    if (!Result->CodeMotionSafe) {
      LLVM_DEBUG(llvm::dbgs() << "    NOT CODE MOTION SAFE!\n");
      CodeMotionSafeTD = false;
    }
      
    NewIncrements.clear();

    // If we do not have any decrements to attempt to match up with, bail.
    if (NewDecrements.empty())
      break;

    LLVM_DEBUG(llvm::dbgs() << "Attempting to match up decrements -> "
               "increments:\n");
    Result = matchDecrementsToIncrements();
    if (!Result) {
      LLVM_DEBUG(llvm::dbgs() << "    FAILED TO MATCH DECREMENTS -> "
                 "INCREMENTS!\n");
      return false;
    }
    if (!Result->KnownSafe) {
      LLVM_DEBUG(llvm::dbgs() << "    NOT KNOWN SAFE!\n");
      KnownSafeBU = false;
    }
    if (!Result->CodeMotionSafe) {
      LLVM_DEBUG(llvm::dbgs() << "    NOT CODE MOTION SAFE!\n");
      CodeMotionSafeBU = false;
    }
    NewDecrements.clear();

    // If we do not have any increment to attempt to match up with again, bail.
    if (NewIncrements.empty())
      break;
  }

  // There is no way we can get a top-down code motion but not a bottom-up, or vice
  // versa.
  assert(CodeMotionSafeTD == CodeMotionSafeBU && "Asymmetric code motion safety");

  bool UnconditionallySafe = (KnownSafeTD && KnownSafeBU);
  bool CodeMotionSafe = (CodeMotionSafeTD && CodeMotionSafeBU);
  if (UnconditionallySafe || CodeMotionSafe) {
    LLVM_DEBUG(llvm::dbgs() << "UNCONDITIONALLY OR CODE MOTION SAFE! "
               "DELETING INSTS.\n");
  } else {
    LLVM_DEBUG(llvm::dbgs() << "NOT UNCONDITIONALLY SAFE AND CODE MOTION "
               "BLOCKED!\n");
    return false;
  }

  // Make sure we always have increments and decrements in the match set.
  assert(MatchSet.Increments.empty() == MatchSet.Decrements.empty() &&
         "Match set without increments or decrements");

  if (!MatchSet.Increments.empty())
    MatchedPair = true;

  // Success!
  LLVM_DEBUG(llvm::dbgs() << "SUCCESS! We can remove things.\n");
  return true;
}
