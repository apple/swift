//===--- RefCountState.cpp ------------------------------------------------===//
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
#include "RefCountState.h"
#include "RCStateTransition.h"
#include "swift/Basic/Assertions.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                           Lattice State Merging
//===----------------------------------------------------------------------===//

static inline BottomUpRefCountState::LatticeState
MergeBottomUpLatticeStates(BottomUpRefCountState::LatticeState L1,
                           BottomUpRefCountState::LatticeState L2) {
  using LatticeState = BottomUpRefCountState::LatticeState;
  // If both states equal, return the first.
  if (L1 == L2)
    return L1;

  // If either are none, return None.
  if (L1 == LatticeState::None || L2 == LatticeState::None)
    return LatticeState::None;

  // Canonicalize.
  if (unsigned(L1) > unsigned(L2))
    std::swap(L1, L2);

  // Choose the side further along in the sequence.
  if ((L1 == LatticeState::Decremented || L1 == LatticeState::MightBeUsed) ||
      (L2 == LatticeState::MightBeUsed ||
       L2 == LatticeState::MightBeDecremented))
    return L2;

  // Otherwise, we don't know what happened, be conservative and return none.
  return LatticeState::None;
}

static inline TopDownRefCountState::LatticeState
MergeTopDownLatticeStates(TopDownRefCountState::LatticeState L1,
                          TopDownRefCountState::LatticeState L2) {
  using LatticeState = TopDownRefCountState::LatticeState;
  // If both states equal, return the first.
  if (L1 == L2)
    return L1;

  // If either are none, return None.
  if (L1 == LatticeState::None || L2 == LatticeState::None)
    return LatticeState::None;

  // Canonicalize.
  if (unsigned(L1) > unsigned(L2))
    std::swap(L1, L2);

  // Choose the side further along in the sequence.
  if ((L1 == LatticeState::Incremented ||
       L1 == LatticeState::MightBeDecremented) ||
      (L2 == LatticeState::MightBeDecremented ||
       L2 == LatticeState::MightBeUsed))
    return L2;

  // Otherwise, we don't know what happened, return none.
  return LatticeState::None;
}

//===----------------------------------------------------------------------===//
//                         Bottom Up Ref Count State
//===----------------------------------------------------------------------===//

/// Initializes/reinitialized the state for I. If we reinitialize we return
/// true.
bool BottomUpRefCountState::initWithMutatorInst(
    ImmutablePointerSet<SILInstruction *> *I, RCIdentityFunctionInfo *RCFI) {
  assert(I->size() == 1);
  SILInstruction *Inst = *I->begin();
  assert((isa<StrongReleaseInst>(Inst) || isa<ReleaseValueInst>(Inst)) &&
         "strong_release and release_value are only supported.");
  (void) Inst;

  bool NestingDetected = SuperTy::initWithMutatorInst(I, RCFI);

  // If we know that there is another decrement on the same pointer that has
  // not been matched up to an increment, then the pointer must have a
  // reference count of at least 2 before this decrement. This implies it is
  // known safe.
  KnownSafe = NestingDetected;

  // If we saw a non arc user that will keep this value alive, set known safe
  // since we will not move non-arc instructions.
  KnownSafe |= FoundNonARCUser;

  // Set our lattice state to be decremented.
  LatState = LatticeState::Decremented;

  return NestingDetected;
}

/// Return true if we *might* remove this instruction.
///
/// This is a conservative query given the information we know, so as we
/// perform the dataflow it may change value.
bool BottomUpRefCountState::mightRemoveMutators() {
  if (LatState == LatticeState::None)
    return false;

  // We will not remove mutators if we have a might be decremented value that
  // is not known safe.
  return isCodeMotionSafe() || isKnownSafe();
}

/// Uninitialize the current state.
void BottomUpRefCountState::clear() {
  // If we cannot conservatively prove that the given RefCountState will not
  // be removed, be conservative and clear the transition state, so we do not
  // propagate KnownSafety forward.
  if (mightRemoveMutators())
    Transition = RCStateTransition();
  LatState = LatticeState::None;
  SuperTy::clear();
}

/// If advance the state's sequence appropriately for a decrement. If we do
/// advance return true. Otherwise return false.
bool BottomUpRefCountState::isRefCountStateModified() const {
  switch (LatState) {
  case LatticeState::Decremented:
    return true;
  case LatticeState::None:
  case LatticeState::MightBeDecremented:
  case LatticeState::MightBeUsed:
    return false;
  }

  llvm_unreachable("Unhandled TermKind in switch.");
}

/// Returns true if given the current lattice state, do we care if the value
/// we are tracking is decremented.
bool BottomUpRefCountState::valueCanBeDecrementedGivenLatticeState() const {
  switch (LatState) {
  case LatticeState::MightBeUsed:
    return true;
  case LatticeState::None:
  case LatticeState::MightBeDecremented:
  case LatticeState::Decremented:
    return false;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

/// If advance the state's sequence appropriately for a decrement. If we do
/// advance return true. Otherwise return false.
bool BottomUpRefCountState::handleDecrement() {
  switch (LatState) {
  case LatticeState::MightBeUsed:
    LatState = LatticeState::MightBeDecremented;
    return true;
  case LatticeState::None:
  case LatticeState::MightBeDecremented:
  case LatticeState::Decremented:
    return false;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

/// Returns true if given the current lattice state, do we care if the value we
/// are tracking is used.
bool BottomUpRefCountState::valueCanBeUsedGivenLatticeState() const {
  switch (LatState) {
  case LatticeState::Decremented:
    return true;
  case LatticeState::None:
  case LatticeState::MightBeDecremented:
  case LatticeState::MightBeUsed:
    return false;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

/// Given the current lattice state, if we have seen a use, advance the
/// lattice state. Return true if we do so and false otherwise.
bool BottomUpRefCountState::handleUser() {
  assert(valueCanBeUsedGivenLatticeState() &&
         "Must be able to be used at this point of the lattice.");

  // Advance the sequence...
  switch (LatState) {
  case LatticeState::Decremented:
    LatState = LatticeState::MightBeUsed;
    return true;
  case LatticeState::MightBeUsed:
  case LatticeState::MightBeDecremented:
  case LatticeState::None:
    return false;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

/// Returns true if given the current lattice state, do we care if the value
/// we are tracking is used.
bool BottomUpRefCountState::
valueCanBeGuaranteedUsedGivenLatticeState() const {
  switch (LatState) {
  case LatticeState::None:
  case LatticeState::MightBeDecremented:
    return false;
  case LatticeState::Decremented:
  case LatticeState::MightBeUsed:
    return true;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

/// Given the current lattice state, if we have seen a use, advance the
/// lattice state. Return true if we do so and false otherwise.
bool BottomUpRefCountState::handleGuaranteedUser() {
  assert(valueCanBeGuaranteedUsedGivenLatticeState() &&
         "Must be able to be used at this point of the lattice.");

  // Advance the sequence...
  switch (LatState) {
  case LatticeState::Decremented: {
    LatState = LatticeState::MightBeDecremented;
    return true;
  }
  case LatticeState::MightBeUsed:
    LatState = LatticeState::MightBeDecremented;
    return true;
  case LatticeState::MightBeDecremented:
  case LatticeState::None:
    return false;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

// Returns true if the passed in ref count inst matches the ref count inst
// we are tracking. This handles generically retains/release.
bool BottomUpRefCountState::isRefCountInstMatchedToTrackedInstruction(
    SILInstruction *RefCountInst) {
  // If we are not tracking any state transitions bail.
  if (!Transition.isValid())
    return false;

  // Otherwise, ask the transition state if this instruction causes a
  // transition that can be matched with the transition in order to eliminate
  // the transition.
  if (!Transition.matchingInst(RefCountInst))
    return false;

  return handleRefCountInstMatch();
}

/// We have a matching ref count inst. Return true if we advance the sequence
/// and false otherwise.
bool BottomUpRefCountState::handleRefCountInstMatch() {
  // Otherwise modify the state appropriately in preparation for removing the
  // increment, decrement pair.
  switch (LatState) {
  case LatticeState::None:
    return false;
  case LatticeState::Decremented:
  case LatticeState::MightBeUsed:
  case LatticeState::MightBeDecremented:
    return true;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

bool BottomUpRefCountState::merge(const BottomUpRefCountState &Other) {

  auto NewState = MergeBottomUpLatticeStates(LatState, Other.LatState);
  LLVM_DEBUG(llvm::dbgs() << "            Performing BottomUp Merge.\n");
  LLVM_DEBUG(llvm::dbgs() << "                Left: " << LatState << "; Right: "
                          << Other.LatState << "; Result: " << NewState <<"\n");
  LLVM_DEBUG(llvm::dbgs() << "                V: ";
             if (hasRCRoot())
               getRCRoot()->dump();
             else
               llvm::dbgs() << "\n";
             llvm::dbgs() << "                OtherV: ";
             if (Other.hasRCRoot())
               Other.getRCRoot()->dump();
             else
               llvm::dbgs() << "\n");

  LatState = NewState;
  KnownSafe &= Other.KnownSafe;
  FoundNonARCUser &= Other.FoundNonARCUser;

  // If we're doing a merge on a path that's previously seen a partial merge,
  // conservatively drop the sequence, to avoid doing partial RR elimination. If
  // the branch predicates for the two merge differ, mixing them is unsafe since
  // they are not control dependent.
  //
  // TODO: Add support for working around control dependence issues.
  if (LatState == BottomUpRefCountState::LatticeState::None) {
    LLVM_DEBUG(llvm::dbgs() << "            Found LatticeState::None. "
                               "Clearing State!\n");
    clear();
    return false;
  }

  if (!Transition.isValid() || !Other.Transition.isValid() ||
      !Transition.merge(Other.Transition)) {
    LLVM_DEBUG(llvm::dbgs() << "            Failed merge!\n");
    clear();
    return false;
  }

  return true;
}

// Check if PotentialGuaranteedUser can use the reference count associated with
// the value we are tracking. If so advance the state's sequence appropriately
// and return true. Otherwise return false.
bool BottomUpRefCountState::handlePotentialGuaranteedUser(
    SILInstruction *PotentialGuaranteedUser, AliasAnalysis *AA) {
  // If we are not tracking a ref count, just return false.
  if (!isTrackingRefCount())
    return false;

  // If at the current lattice state, we don't care if the value we are
  // tracking can be decremented or used, just return false.
  //
  // This causes us to only perform alias queries when we are at a lattice
  // state where the alias queries will actually be used.
  if (!valueCanBeGuaranteedUsedGivenLatticeState())
    return false;

  // If we can prove that Other cannot use the pointer we are tracking,
  // return...
  if (!mayGuaranteedUseValue(PotentialGuaranteedUser, getRCRoot(), AA))
    return false;

  // If we can prove that the pointer we are tracking cannot be decremented,
  // return. On return, BottomUpRefCountState::handlePotentialUser can correctly
  // handle transition of refcount state. It transitions from a Decrement
  // refcount state to a MightBeUsed refcount state
  if (!mayDecrementRefCount(PotentialGuaranteedUser, getRCRoot(), AA)) {
    return false;
   }

  // Instructions that we do not recognize (and thus will not move) and that
  // *must* use RCIdentity, implies we are always known safe as long as meet
  // over all path constraints are satisfied.
  if (isRCStateTransitionUnknown(PotentialGuaranteedUser->asSILNode()))
    if (mustUseValue(PotentialGuaranteedUser, getRCRoot(), AA))
      FoundNonARCUser = true;

  // Otherwise, update the ref count state given the guaranteed user.
  return handleGuaranteedUser();
}

/// Check if PotentialDecrement can decrement the reference count associated
/// with the value we are tracking. If so advance the state's sequence
/// appropriately and return true. Otherwise return false.
bool BottomUpRefCountState::handlePotentialDecrement(
    SILInstruction *PotentialDecrement, AliasAnalysis *AA) {
  // If we are not tracking a ref count, just return false.
  if (!isTrackingRefCount())
    return false;

  // If at the current lattice state, we don't care if the value we are
  // tracking can be decremented, just return false.
  //
  // This causes us to only perform alias queries when we are at a lattice
  // state where the alias queries will actually be used.
  if (!valueCanBeDecrementedGivenLatticeState())
    return false;

  // If we can prove that Other cannot use the pointer we are tracking,
  // return...
  if (!mayDecrementRefCount(PotentialDecrement, getRCRoot(), AA))
    return false;

  // Otherwise, allow the CRTP substruct to update itself given we have a
  // potential decrement.
  return handleDecrement();
}

// Check if PotentialUser could be a use of the reference counted value that
// requires user to be alive. If so advance the state's sequence
// appropriately and return true. Otherwise return false.
bool BottomUpRefCountState::handlePotentialUser(SILInstruction *PotentialUser,
                                                AliasAnalysis *AA) {

  // If we are not tracking a ref count, just return false.
  if (!isTrackingRefCount())
    return false;

  // If at the current lattice state, we don't care if the value we are
  // tracking can be used, just return false.
  //
  // This causes us to only perform alias queries when we are at a lattice
  // state where the alias queries will actually be used.
  if (!valueCanBeUsedGivenLatticeState())
    return false;

  if (!mayHaveSymmetricInterference(PotentialUser, getRCRoot(), AA))
    return false;

  // Instructions that we do not recognize (and thus will not move) and that
  // *must* use RCIdentity, implies we are always known safe as long as meet
  // over all path constraints are satisfied.
  if (isRCStateTransitionUnknown(PotentialUser->asSILNode()))
    if (mustUseValue(PotentialUser, getRCRoot(), AA))
      FoundNonARCUser = true;

  return handleUser();
}

void BottomUpRefCountState::updateForSameLoopInst(SILInstruction *I,
                                                  AliasAnalysis *AA) {
  // If this state is not tracking anything, there is nothing to update.
  if (!isTrackingRefCount())
    return;

  // Check if the instruction we are visiting could potentially use our
  // instruction in a way that requires us to guarantee the lifetime of the
  // pointer up to this point. This has the effect of performing a use and a
  // decrement.
  if (handlePotentialGuaranteedUser(I, AA)) {
    LLVM_DEBUG(llvm::dbgs() << "    Found Potential Guaranteed Use:\n        "
                            << getRCRoot());
    return;
  }

  // Check if the instruction we are visiting could potentially decrement
  // the reference counted value we are tracking... in a manner that could
  // cause us to change states. If we do change states continue...
  if (handlePotentialDecrement(I, AA)) {
    LLVM_DEBUG(llvm::dbgs() << "    Found Potential Decrement:\n        "
                            << getRCRoot());
    return;
  }

  // Otherwise check if the reference counted value we are tracking
  // could be used by the given instruction.
  if (!handlePotentialUser(I, AA))
    return;
  LLVM_DEBUG(llvm::dbgs() << "    Found Potential Use:\n        "
                          << getRCRoot());
}

// Remove "KnownSafe" on the BottomUpRefCountState. If we find another unmatched
// retain instruction with a different aliasing RCIdentity or the same
// RCIdentity in the child region in the loop case.
void BottomUpRefCountState::checkAndResetKnownSafety(
    SILInstruction *I, SILValue VisitedRC,
    std::function<bool(SILInstruction *)> checkIfRefCountInstIsMatched,
    RCIdentityFunctionInfo *RCIA, AliasAnalysis *AA) {
  assert(VisitedRC);
  // If the RefCountState was not marked "KnownSafe", there is nothing to do.
  if (!isKnownSafe())
    return;
  assert(Transition.getKind() == RCStateTransitionKind::StrongDecrement);
  // We only care about retain instructions that can potentially pair with a
  // previously visited release.
  if (!(isa<StrongRetainInst>(I) || isa<RetainValueInst>(I)))
    return;
  SILValue VisitingRC = RCIA->getRCIdentityRoot(I->getOperand(0));
  assert(VisitingRC);
  // If the visiting retain instruction was not already pair with a release
  // instruction, return.
  if (checkIfRefCountInstIsMatched(I))
    return;
  // If the VisitingRC and VisitedRC do not alias, they cannot be incorrectly
  // paired.
  if (AA->isNoAlias(VisitingRC, VisitedRC))
    return;
  LLVM_DEBUG(llvm::dbgs() << "Clearing KnownSafe for: ");
  LLVM_DEBUG(VisitedRC->dump());
  clearKnownSafe();
}

// This function is conservative enough that the flow sensitive nature of
// loop summarized instructions does not matter.
void BottomUpRefCountState::updateForDifferentLoopInst(SILInstruction *I,
                                                       AliasAnalysis *AA) {
  // If we are not tracking anything, bail.
  if (!isTrackingRefCount())
    return;

  if (valueCanBeGuaranteedUsedGivenLatticeState()) {
    // Any instruction that may need guaranteed use or may decrement the
    // refcount will turn off CodeMotionSafety
    if (mayGuaranteedUseValue(I, getRCRoot(), AA) ||
        mayDecrementRefCount(I, getRCRoot(), AA)) {
      LLVM_DEBUG(llvm::dbgs() << "    Found potential guaranteed use:\n        "
                              << getRCRoot());
      handleGuaranteedUser();
      return;
    }
  }

  // We can just handle potential users normally, since if we handle the user we
  // already saw a decrement implying that we will treat this like a guaranteed
  // use.
  if (!handlePotentialUser(I, AA))
    return;
  LLVM_DEBUG(llvm::dbgs() << "    Found Potential Use:\n        "
                          << getRCRoot());
}

//===----------------------------------------------------------------------===//
//                          Top Down Ref Count State
//===----------------------------------------------------------------------===//

/// Initializes/reinitialized the state for I. If we reinitialize we return
/// true.
bool TopDownRefCountState::initWithMutatorInst(
    ImmutablePointerSet<SILInstruction *> *I, RCIdentityFunctionInfo *RCFI) {
  assert(I->size() == 1);
  SILInstruction *Inst = *I->begin();
  (void)Inst;
  assert((isa<StrongRetainInst>(Inst) || isa<RetainValueInst>(Inst)) &&
         "strong_retain and retain_value are only supported.");

  bool NestingDetected = SuperTy::initWithMutatorInst(I, RCFI);

  // This retain is known safe if the operand we are tracking was already
  // known incremented previously. This occurs when you have nested
  // increments.
  KnownSafe = isRefCountStateModified();

  // Set our lattice state to be incremented.
  LatState = LatticeState::Incremented;

  return NestingDetected;
}

/// Initialize this ref count state with the @owned Arg at +1.
void TopDownRefCountState::initWithArg(SILFunctionArgument *Arg) {
  LatState = LatticeState::Incremented;
  Transition = RCStateTransition(Arg);
  assert(Transition.getKind() == RCStateTransitionKind::StrongEntrance &&
         "Expected a strong entrance here");
  RCRoot = Arg;
  KnownSafe = false;
}

/// Initialize this RefCountState with an instruction which introduces a new
/// ref count at +1.
void TopDownRefCountState::initWithEntranceInst(
    ImmutablePointerSet<SILInstruction *> *I, SILValue RCIdentity) {
  LatState = LatticeState::Incremented;
  Transition = RCStateTransition(I);
  assert(Transition.getKind() == RCStateTransitionKind::StrongEntrance &&
         "Expected a strong entrance here");
  RCRoot = RCIdentity;
  KnownSafe = false;
}

/// Uninitialize the current state.
void TopDownRefCountState::clear() {
  Transition = RCStateTransition();
  LatState = LatticeState::None;
  SuperTy::clear();
}

/// Can we guarantee that the given reference counted value has been modified?
bool TopDownRefCountState::isRefCountStateModified() const {
  switch (LatState) {
  case LatticeState::Incremented:
    return true;
  case LatticeState::None:
  case LatticeState::MightBeDecremented:
  case LatticeState::MightBeUsed:
    return false;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

/// Returns true if given the current lattice state, do we care if the value
/// we are tracking is decremented.
bool TopDownRefCountState::valueCanBeDecrementedGivenLatticeState() const {
  switch (LatState) {
  case LatticeState::Incremented:
    return true;
  case LatticeState::None:
  case LatticeState::MightBeDecremented:
  case LatticeState::MightBeUsed:
    return false;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

/// If advance the state's sequence appropriately for a decrement. If we do
/// advance return true. Otherwise return false.
bool TopDownRefCountState::handleDecrement() {
  switch (LatState) {
  case LatticeState::Incremented:
    LatState = LatticeState::MightBeDecremented;
    return true;
  case LatticeState::None:
  case LatticeState::MightBeDecremented:
  case LatticeState::MightBeUsed:
    return false;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

/// Returns true if given the current lattice state, do we care if the value
/// we are tracking is used.
bool TopDownRefCountState::valueCanBeUsedGivenLatticeState() const {
  switch (LatState) {
  case LatticeState::MightBeDecremented:
    return true;
  case LatticeState::None:
  case LatticeState::Incremented:
  case LatticeState::MightBeUsed:
    return false;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

/// Given the current lattice state, if we have seen a use, advance the
/// lattice state. Return true if we do so and false otherwise.
bool TopDownRefCountState::handleUser() {
  assert(valueCanBeUsedGivenLatticeState() &&
         "Must be able to be used at this point of the lattice.");

  // Otherwise advance the sequence...
  switch (LatState) {
  case LatticeState::MightBeDecremented:
    LatState = LatticeState::MightBeUsed;
    return true;
  case LatticeState::Incremented:
  case LatticeState::None:
  case LatticeState::MightBeUsed:
    return false;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

/// Returns true if given the current lattice state, do we care if the value
/// we are tracking is used.
bool
TopDownRefCountState::
valueCanBeGuaranteedUsedGivenLatticeState() const {
  switch (LatState) {
  case LatticeState::None:
  case LatticeState::MightBeUsed:
    return false;
  case LatticeState::Incremented:
  case LatticeState::MightBeDecremented:
    return true;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

/// Given the current lattice state, if we have seen a use, advance the
/// lattice state. Return true if we do so and false otherwise.
bool TopDownRefCountState::handleGuaranteedUser() {
  assert(valueCanBeGuaranteedUsedGivenLatticeState() &&
         "Must be able to be used at this point of the lattice.");
  // Advance the sequence...
  switch (LatState) {
  case LatticeState::Incremented: {
    LatState = LatticeState::MightBeUsed;
    return true;
  }
  case LatticeState::MightBeDecremented:
    LatState = LatticeState::MightBeUsed;
    return true;
  case LatticeState::MightBeUsed:
  case LatticeState::None:
    return false;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

// Returns true if the passed in ref count inst matches the ref count inst
// we are tracking. This handles generically retains/release.
bool TopDownRefCountState::isRefCountInstMatchedToTrackedInstruction(
    SILInstruction *RefCountInst) {
  // If we are not tracking any state transitions bail.
  if (!Transition.isValid())
    return false;

  // Otherwise, ask the transition state if this instruction causes a
  // transition that can be matched with the transition in order to eliminate
  // the transition.
  if (!Transition.matchingInst(RefCountInst))
    return false;

  return handleRefCountInstMatch();
}

/// We have a matching ref count inst. Return true if we advance the sequence
/// and false otherwise.
bool TopDownRefCountState::handleRefCountInstMatch() {
  // Otherwise modify the state appropriately in preparation for removing the
  // increment, decrement pair.
  switch (LatState) {
  case LatticeState::None:
    return false;
  case LatticeState::Incremented:
  case LatticeState::MightBeDecremented:
  case LatticeState::MightBeUsed:
    return true;
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

bool TopDownRefCountState::merge(const TopDownRefCountState &Other) {
  auto NewState = MergeTopDownLatticeStates(LatState, Other.LatState);
  LLVM_DEBUG(llvm::dbgs() << "        Performing TopDown Merge.\n");
  LLVM_DEBUG(llvm::dbgs() << "            Left: " << LatState << "; Right: "
                          << Other.LatState << "; Result: "
                          << NewState << "\n");
  LLVM_DEBUG(llvm::dbgs() << "            V: ";
             if (hasRCRoot())
               getRCRoot()->dump();
             else
               llvm::dbgs() << "\n";
             llvm::dbgs() << "            OtherV: ";
             if (Other.hasRCRoot())
               Other.getRCRoot()->dump();
             else
               llvm::dbgs() << "\n");

  LatState = NewState;
  KnownSafe &= Other.KnownSafe;

  // If we're doing a merge on a path that's previously seen a partial merge,
  // conservatively drop the sequence, to avoid doing partial RR elimination. If
  // the branch predicates for the two merge differ, mixing them is unsafe since
  // they are not control dependent.
  //
  // TODO: Add support for determining control dependence.
  if (LatState == TopDownRefCountState::LatticeState::None) {
    clear();
    LLVM_DEBUG(llvm::dbgs() << "            Found LatticeState::None. "
                               "Clearing State!\n");
    return false;
  }

  if (!Transition.isValid() || !Other.Transition.isValid() ||
      !Transition.merge(Other.Transition)) {
    LLVM_DEBUG(llvm::dbgs() << "            Failed merge!\n");
    clear();
    return false;
  }

  return true;
}

// Check if PotentialGuaranteedUser can use the reference count associated with
// the value we are tracking. If so advance the state's sequence appropriately
// and return true. Otherwise return false.
bool TopDownRefCountState::handlePotentialGuaranteedUser(
    SILInstruction *PotentialGuaranteedUser, AliasAnalysis *AA) {
  // If we are not tracking a ref count, just return false.
  if (!isTrackingRefCount())
    return false;

  // If at the current lattice state, we don't care if the value we are
  // tracking can be decremented or used, just return false.
  //
  // This causes us to only perform alias queries when we are at a lattice
  // state where the alias queries will actually be used.
  if (!valueCanBeGuaranteedUsedGivenLatticeState())
    return false;

  // If we can prove that Other cannot use the pointer we are tracking,
  // return...
  if (!mayGuaranteedUseValue(PotentialGuaranteedUser, getRCRoot(), AA))
    return false;

  // If we can prove that the pointer we are tracking cannot be decremented,
  // return. On return, TopDownRefCountState::handlePotentialUser can correctly
  // handle transition of refcount state.
  if (!mayDecrementRefCount(PotentialGuaranteedUser, getRCRoot(), AA)) {
    return false;
   }

  // Otherwise, update our step given that we have a potential decrement.
   return handleGuaranteedUser();
}

// Check if PotentialDecrement can decrement the reference count associated with
// the value we are tracking. If so advance the state's sequence appropriately
// and return true. Otherwise return false.
bool TopDownRefCountState::handlePotentialDecrement(
    SILInstruction *PotentialDecrement, AliasAnalysis *AA) {
  // If we are not tracking a ref count, just return false.
  if (!isTrackingRefCount())
    return false;

  // If at the current lattice state, we don't care if the value we are
  // tracking can be decremented, just return false.
  //
  // This causes us to only perform alias queries when we are at a lattice
  // state where the alias queries will actually be used.
  if (!valueCanBeDecrementedGivenLatticeState())
    return false;

  // If we can prove that Other cannot use the pointer we are tracking,
  // return...
  if (!mayDecrementRefCount(PotentialDecrement, getRCRoot(), AA))
    return false;

  // Otherwise, update our state given the potential decrement.
  return handleDecrement();
}

// Check if PotentialUser could be a use of the reference counted value that
// requires user to be alive. If so advance the state's sequence appropriately
// and return true. Otherwise return false.
bool TopDownRefCountState::handlePotentialUser(SILInstruction *PotentialUser,
                                               AliasAnalysis *AA) {
  // If we are not tracking a ref count, just return false.
  if (!isTrackingRefCount())
    return false;

  // If at the current lattice state, we don't care if the value we are
  // tracking can be used, just return false.
  //
  // This causes us to only perform alias queries when we are at a lattice
  // state where the alias queries will actually be used.
  if (!valueCanBeUsedGivenLatticeState())
    return false;

  if (!mayHaveSymmetricInterference(PotentialUser, getRCRoot(), AA))
    return false;

  return handleUser();
}

void TopDownRefCountState::updateForSameLoopInst(SILInstruction *I,
                                                 AliasAnalysis *AA) {
  // If we are not tracking anything, bail.
  if (!isTrackingRefCount())
    return;

  // Check if the instruction we are visiting could potentially use our
  // instruction in a way that requires us to guarantee the lifetime of the
  // pointer up to this point. This has the effect of performing a use and a
  // decrement.
  if (handlePotentialGuaranteedUser(I, AA)) {
    LLVM_DEBUG(llvm::dbgs() << "    Found Potential Guaranteed Use:\n        "
                            << getRCRoot());
    return;
  }

  // Check if the instruction we are visiting could potentially decrement
  // the reference counted value we are tracking in a manner that could
  // cause us to change states. If we do change states continue...
  if (handlePotentialDecrement(I, AA)) {
    LLVM_DEBUG(llvm::dbgs() << "    Found Potential Decrement:\n        "
                            << getRCRoot());
    return;
  }

  // Otherwise check if the reference counted value we are tracking
  // could be used by the given instruction.
  if (!handlePotentialUser(I, AA))
    return;
  LLVM_DEBUG(llvm::dbgs() << "    Found Potential Use:\n        "
                          << getRCRoot());
}

// Remove "KnownSafe" on the TopDownRefCountState. If we find another unmatched
// release instruction with a different aliasing RCIdentity or the same
// RCIdentity in the child region in the loop case.
void TopDownRefCountState::checkAndResetKnownSafety(
    SILInstruction *I, SILValue VisitedRC,
    std::function<bool(SILInstruction *)> checkIfRefCountInstIsMatched,
    RCIdentityFunctionInfo *RCIA, AliasAnalysis *AA) {
  assert(VisitedRC);
  // If the RefCountState was not marked "KnownSafe", there is nothing to do.
  if (!isKnownSafe())
    return;
  assert(Transition.getKind() == RCStateTransitionKind::StrongIncrement);
  // We only care about release instructions that can potentially pair with a
  // previously visited retain.
  if (!(isa<StrongReleaseInst>(I) || isa<ReleaseValueInst>(I)))
    return;
  SILValue VisitingRC = RCIA->getRCIdentityRoot(I->getOperand(0));
  assert(VisitingRC);
  // If the visiting release instruction was already pair with a retain
  // instruction, return.
  if (checkIfRefCountInstIsMatched(I))
    return;
  // If the VisitingRC and VisitedRC do not alias, they cannot be incorrectly
  // paired.
  if (AA->isNoAlias(VisitingRC, VisitedRC))
    return;
  LLVM_DEBUG(llvm::dbgs() << "Clearing KnownSafe for: ");
  LLVM_DEBUG(VisitedRC->dump());
  clearKnownSafe();
}

// This function is conservative enough that the flow sensitive nature of
// loop summarized instructions does not matter.
void TopDownRefCountState::updateForDifferentLoopInst(SILInstruction *I,
                                                      AliasAnalysis *AA) {
  // If we are not tracking anything, bail.
  if (!isTrackingRefCount())
    return;

  // Any instruction that may need guaranteed use or may decrement the
  // refcount will turn off CodeMotionSafety
  if (valueCanBeGuaranteedUsedGivenLatticeState()) {
    if (mayGuaranteedUseValue(I, getRCRoot(), AA) ||
        mayDecrementRefCount(I, getRCRoot(), AA)) {
      LLVM_DEBUG(llvm::dbgs() << "    Found potential guaranteed use!\n");
      handleGuaranteedUser();
      return;
    }
  }

  if (!handlePotentialUser(I, AA))
    return;
  LLVM_DEBUG(llvm::dbgs() << "    Found Potential Use:\n        "
                          << getRCRoot());
}

//===----------------------------------------------------------------------===//
//                             Printing Utilities
//===----------------------------------------------------------------------===//

void BottomUpRefCountState::dump() {
  llvm::dbgs() << LatState << " "
               << (isKnownSafe() ? "KnownSafe" : "NotKnownSafe") << " "
               << (isCodeMotionSafe() ? "CodeMotionSafe" : "NotCodeMotionSafe")
               << "\n";
  llvm::dbgs() << "Matching Instructions:\n";
  for (auto it : getInstructions()) {
    it->dump();
  }
}
void TopDownRefCountState::dump() {
  llvm::dbgs() << LatState << " "
               << (isKnownSafe() ? "KnownSafe" : "NotKnownSafe") << " "
               << (isCodeMotionSafe() ? "CodeMotionSafe" : "NotCodeMotionSafe")
               << "\n";
  llvm::dbgs() << "Matching Instructions:\n";
  for (auto it : getInstructions()) {
    it->dump();
  }
}

namespace llvm {

raw_ostream &operator<<(raw_ostream &OS,
                        BottomUpRefCountState::LatticeState S) {
  using LatticeState = BottomUpRefCountState::LatticeState;
  switch (S) {
  case LatticeState::None:
    return OS << "None";
  case LatticeState::Decremented:
    return OS << "Decremented";
  case LatticeState::MightBeUsed:
    return OS << "MightBeUsed";
  case LatticeState::MightBeDecremented:
    return OS << "MightBeDecremented";
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              TopDownRefCountState::LatticeState S) {
  using LatticeState = TopDownRefCountState::LatticeState;
  switch (S) {
  case LatticeState::None:
    return OS << "None";
  case LatticeState::Incremented:
    return OS << "Incremented";
  case LatticeState::MightBeUsed:
    return OS << "MightBeUsed";
  case LatticeState::MightBeDecremented:
    return OS << "MightBeDecremented";
  }

  llvm_unreachable("Unhandled LatticeState in switch.");
}
} // end namespace llvm
