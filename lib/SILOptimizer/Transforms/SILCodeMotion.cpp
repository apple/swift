//===--- SILCodeMotion.cpp - Code Motion Optimizations --------------------===//
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

#define DEBUG_TYPE "sil-codemotion"
#include "swift/AST/Module.h"
#include "swift/Basic/BlotMapVector.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

STATISTIC(NumSunk, "Number of instructions sunk");
STATISTIC(NumRefCountOpsSimplified, "Number of enum ref count ops simplified");
STATISTIC(NumHoisted, "Number of instructions hoisted");
STATISTIC(NumSILArgumentReleaseHoisted, "Number of silargument release instructions hoisted");

llvm::cl::opt<bool> DisableSILRRCodeMotion("disable-sil-cm-rr-cm", llvm::cl::init(true));

using namespace swift;

namespace {
  
//===----------------------------------------------------------------------===//
//                                  Utility
//===----------------------------------------------------------------------===//

static void createRefCountOpForPayload(SILBuilder &Builder, SILInstruction *I,
                                       EnumElementDecl *EnumDecl,
                                       SILValue DefOfEnum = SILValue()) {
  assert(EnumDecl->hasAssociatedValues() &&
         "We assume enumdecl has an argument type");

  SILModule &Mod = I->getModule();

  // The enum value is either passed as an extra argument if we are moving an
  // retain that does not refer to the enum typed value - otherwise it is the
  // argument to the refcount instruction.
  SILValue EnumVal = DefOfEnum ? DefOfEnum : I->getOperand(0);

  SILType ArgType = EnumVal->getType().getEnumElementType(
      EnumDecl, Mod, TypeExpansionContext(Builder.getFunction()));

  auto *UEDI =
    Builder.createUncheckedEnumData(I->getLoc(), EnumVal, EnumDecl, ArgType);

  SILType UEDITy = UEDI->getType();

  // If our payload is trivial, we do not need to insert any retain or release
  // operations.
  if (UEDITy.isTrivial(*I->getFunction()))
    return;

  ++NumRefCountOpsSimplified;

  // If we have a retain value...
  if (auto RCI = dyn_cast<RetainValueInst>(I)) {
    // And our payload is refcounted, insert a strong_retain onto the
    // payload.
    if (UEDITy.isReferenceCounted(Mod)) {
      Builder.createStrongRetain(I->getLoc(), UEDI, RCI->getAtomicity());
      return;
    }

    // Otherwise, insert a retain_value on the payload.
    Builder.createRetainValue(I->getLoc(), UEDI, RCI->getAtomicity());
    return;
  }

  // At this point we know that we must have a release_value and a non-trivial
  // payload.
  assert(isa<ReleaseValueInst>(I) && "If I is not a retain value here, it must "
         "be a release value since enums do not have reference semantics.");
  auto *RCI = cast<ReleaseValueInst>(I);

  // If our payload has reference semantics, insert the strong release.
  if (UEDITy.isReferenceCounted(Mod)) {
    Builder.createStrongRelease(I->getLoc(), UEDI, RCI->getAtomicity());
    return;
  }

  // Otherwise if our payload is non-trivial but lacking reference semantics,
  // insert the release_value.
  Builder.createReleaseValue(I->getLoc(), UEDI, RCI->getAtomicity());
}

//===----------------------------------------------------------------------===//
//                             Enum Tag Dataflow
//===----------------------------------------------------------------------===//

namespace {

class EnumCaseDataflowContext;

using EnumBBCaseList =
    llvm::SmallVector<std::pair<SILBasicBlock *, EnumElementDecl *>, 2>;

/// Class that performs enum tag state dataflow on the given BB.
class BBEnumTagDataflowState
    : public SILInstructionVisitor<BBEnumTagDataflowState, bool> {
  EnumCaseDataflowContext *Context;
  NullablePtr<SILBasicBlock> BB;

  SmallBlotMapVector<unsigned, EnumElementDecl *, 4> ValueToCaseMap;
  SmallBlotMapVector<unsigned, EnumBBCaseList, 4> EnumToEnumBBCaseListMap;

public:
  BBEnumTagDataflowState() = default;
  BBEnumTagDataflowState(const BBEnumTagDataflowState &Other) = default;
  ~BBEnumTagDataflowState() = default;

  SWIFT_DEBUG_DUMP;

  bool init(EnumCaseDataflowContext &Context, SILBasicBlock *NewBB);

  SILBasicBlock *getBB() { return BB.get(); }

  using iterator = decltype(ValueToCaseMap)::iterator;
  iterator begin() { return ValueToCaseMap.getItems().begin(); }
  iterator end() { return ValueToCaseMap.getItems().end(); }

  void clear() { ValueToCaseMap.clear(); }

  bool visitSILInstruction(SILInstruction *I) { return false; }

  bool visitEnumInst(EnumInst *EI);
  bool visitUncheckedEnumDataInst(UncheckedEnumDataInst *UEDI);
  bool visitRetainValueInst(RetainValueInst *RVI);
  bool visitReleaseValueInst(ReleaseValueInst *RVI);
  bool process();
  bool hoistDecrementsIntoSwitchRegions(AliasAnalysis *AA);
  bool sinkIncrementsOutOfSwitchRegions(AliasAnalysis *AA,
                                        RCIdentityFunctionInfo *RCIA);
  void handlePredSwitchEnum(SwitchEnumInst *S);
  void handlePredCondSelectEnum(CondBranchInst *CondBr);

  /// Helper method which initializes this state map with the data from the
  /// first predecessor BB.
  ///
  /// We will be performing an intersection in a later step of the merging.
  bool initWithFirstPred(SILBasicBlock *FirstPredBB);

  /// Top level merging function for predecessors.
  void mergePredecessorStates();

  /// If we have a single predecessor, see if the predecessor's terminator was a
  /// switch_enum or a (cond_br + select_enum). If so, track in this block the
  /// enum state of the operand.
  void mergeSinglePredTermInfoIntoState(SILBasicBlock *Pred);

private:
  EnumCaseDataflowContext &getContext() const;
  unsigned getIDForValue(SILValue V) const;
  SILValue getValueForID(unsigned ID) const;
};

/// Map all blocks to BBEnumTagDataflowState in RPO order.
class EnumCaseDataflowContext {
  PostOrderFunctionInfo *PO;
  std::vector<BBEnumTagDataflowState> BBToStateVec;
  std::vector<SILValue> IDToEnumValueMap;
  llvm::DenseMap<SILValue, unsigned> EnumValueToIDMap;

public:
  EnumCaseDataflowContext(PostOrderFunctionInfo *PO) : PO(PO), BBToStateVec() {
    BBToStateVec.resize(PO->size());
    unsigned RPOIdx = 0;
    for (SILBasicBlock *BB : PO->getReversePostOrder()) {
      BBToStateVec[RPOIdx].init(*this, BB);
      ++RPOIdx;
    }
  }

  /// Return true if we have a valid value for the given ID;
  bool hasValueForID(unsigned ID) const {
    return ID < IDToEnumValueMap.size() && IDToEnumValueMap[ID];
  }

  SILValue getValueForID(unsigned ID) const { return IDToEnumValueMap[ID]; }

  unsigned getIDForValue(SILValue V) const {
    // We are being a little tricky here. Here is what is happening:
    //
    // 1. When we start we do not know if we are already tracking V, so we
    // prepare /as if/ V was new.
    //
    // 2. When we perform the insertion, if V already has a value associated
    // with it, we return the iterator to that value. The iterator contains
    // inside of it the actual index.
    //
    // 3. Otherwise, we initialize V in EnumValueToIDMap with NewID. Rather than
    // re-accessing the iterator, we just return the value we have already
    // computed.
    unsigned NewID = IDToEnumValueMap.size();
    auto &This = const_cast<EnumCaseDataflowContext &>(*this);
    auto Iter = This.EnumValueToIDMap.insert({V, NewID});
    if (!Iter.second)
      return Iter.first->second;
    This.IDToEnumValueMap.emplace_back(V);
    return NewID;
  }

  void blotValue(SILValue V) {
    unsigned ID = getIDForValue(V);
    IDToEnumValueMap[ID] = SILValue();
  }

  unsigned size() const { return BBToStateVec.size(); }
  BBEnumTagDataflowState &getRPOState(unsigned RPOIdx) {
    return BBToStateVec[RPOIdx];
  }
  /// \return BBEnumTagDataflowState or NULL for unreachable blocks.
  BBEnumTagDataflowState *getBBState(SILBasicBlock *BB) {
    if (auto ID = PO->getRPONumber(BB)) {
      return &getRPOState(*ID);
    }
    return nullptr;
  }
};

} // end anonymous namespace

EnumCaseDataflowContext &BBEnumTagDataflowState::getContext() const {
  // Context and BB are initialized together, so we only need to check one.
  assert(BB.isNonNull() && "Uninitialized state?!");
  return *Context;
}

unsigned BBEnumTagDataflowState::getIDForValue(SILValue V) const {
  return getContext().getIDForValue(V);
}

SILValue BBEnumTagDataflowState::getValueForID(unsigned ID) const {
  return getContext().getValueForID(ID);
}

void BBEnumTagDataflowState::handlePredSwitchEnum(SwitchEnumInst *S) {

  // Find the tag associated with our BB and set the state of the
  // enum we switch on to that value. This is important so we can determine
  // covering switches for enums that have cases without payload.

  // Next check if we are the target of a default switch_enum case. If we are,
  // no interesting information can be extracted, so bail...
  if (S->hasDefault() && S->getDefaultBB() == getBB())
    return;

  // Otherwise, attempt to find the tag associated with this BB in the switch
  // enum...
  for (unsigned i = 0, e = S->getNumCases(); i != e; ++i) {
    auto P = S->getCase(i);

    // If this case of the switch is not matched up with this BB, skip the
    // case...
    if (P.second != getBB())
      continue;

    // Ok, we found the case for our BB. If we don't have an enum tag (which can
    // happen if we have a default statement), return. There is nothing more we
    // can do.
    if (!P.first)
      return;

    // Ok, we have a matching BB and a matching enum tag. Set the state and
    // return.
    ValueToCaseMap[getIDForValue(S->getOperand())] = P.first;
    return;
  }
  llvm_unreachable("A successor of a switch_enum terminated BB should be in "
                   "the switch_enum.");
}

void BBEnumTagDataflowState::handlePredCondSelectEnum(CondBranchInst *CondBr) {

  auto *EITI = dyn_cast<SelectEnumInst>(CondBr->getCondition());
  if (!EITI)
    return;

  NullablePtr<EnumElementDecl> TrueElement = EITI->getSingleTrueElement();
  if (TrueElement.isNull())
    return;

  // Find the tag associated with our BB and set the state of the
  // enum we switch on to that value. This is important so we can determine
  // covering switches for enums that have cases without payload.

  // Check if we are the true case, ie, we know that we are the given tag.
  const auto &Operand = EITI->getEnumOperand();
  if (CondBr->getTrueBB() == getBB()) {
    ValueToCaseMap[getIDForValue(Operand)] = TrueElement.get();
    return;
  }

  // If the enum only has 2 values and its tag isn't the true branch, then we
  // know the true branch must be the other tag.
  if (EnumDecl *E = Operand->getType().getEnumOrBoundGenericEnum()) {
    // We can't do this optimization on non-exhaustive enums.
    const SILFunction *Fn = CondBr->getFunction();
    bool IsExhaustive =
        E->isEffectivelyExhaustive(Fn->getModule().getSwiftModule(),
                                   Fn->getResilienceExpansion());
    if (!IsExhaustive)
      return;
    // Look for a single other element on this enum.
    EnumElementDecl *OtherElt = nullptr;
    for (EnumElementDecl *Elt : E->getAllElements()) {
      // Skip the case where we find the select_enum element
      if (Elt == TrueElement.get())
        continue;
      // If we find another element, then we must have more than 2, so bail.
      if (OtherElt)
        return;
      OtherElt = Elt;
    }
    // Only a single enum element?  How would this even get here?  We should
    // handle it in SILCombine.
    if (!OtherElt)
      return;
    // FIXME: Can we ever not be the false BB here?
    if (CondBr->getTrueBB() != getBB()) {
      ValueToCaseMap[getIDForValue(Operand)] = OtherElt;
      return;
    }
  }
}

bool BBEnumTagDataflowState::initWithFirstPred(SILBasicBlock *FirstPredBB) {
  // Try to look up the state for the first pred BB.
  BBEnumTagDataflowState *FirstPredState = getContext().getBBState(FirstPredBB);

  // If we fail, we found an unreachable block, bail.
  if (FirstPredState == nullptr) {
    LLVM_DEBUG(llvm::dbgs() << "        Found an unreachable block!\n");
    return false;
  }

  // Ok, our state is in the map, copy in the predecessors value to case map.
  ValueToCaseMap = FirstPredState->ValueToCaseMap;

  // If we are predecessors only successor, we can potentially hoist releases
  // into it, so associate the first pred BB and the case for each value that we
  // are tracking with it.
  //
  // TODO: I am writing this too fast. Clean this up later.
  if (FirstPredBB->getSingleSuccessorBlock()) {
    for (auto P : ValueToCaseMap.getItems()) {
      if (!P.has_value())
        continue;
      EnumToEnumBBCaseListMap[P->first].push_back({FirstPredBB, P->second});
    }
  }

  return true;
}

void BBEnumTagDataflowState::mergeSinglePredTermInfoIntoState(
    SILBasicBlock *Pred) {
  // Grab the terminator of our one predecessor and if it is a switch enum, mix
  // it into this state.
  TermInst *PredTerm = Pred->getTerminator();
  if (auto *S = dyn_cast<SwitchEnumInst>(PredTerm)) {
    handlePredSwitchEnum(S);
    return;
  }

  auto *CondBr = dyn_cast<CondBranchInst>(PredTerm);
  if (!CondBr)
    return;

  handlePredCondSelectEnum(CondBr);
}

void BBEnumTagDataflowState::mergePredecessorStates() {

  // If we have no predecessors, there is nothing to do so return early...
  if (getBB()->pred_empty()) {
    LLVM_DEBUG(llvm::dbgs() << "            No Preds.\n");
    return;
  }

  auto PI = getBB()->pred_begin(), PE = getBB()->pred_end();
  if (*PI == getBB()) {
    LLVM_DEBUG(llvm::dbgs() << "            Found a self loop. Bailing!\n");
    return;
  }

  // Grab the first predecessor BB.
  SILBasicBlock *FirstPred = *PI;
  ++PI;

  // Attempt to initialize our state with our first predecessor's state by just
  // copying. We will be doing an intersection with all of the other BB.
  if (!initWithFirstPred(FirstPred))
    return;

  // If we only have one predecessor see if we can gain any information and or
  // knowledge from the terminator of our one predecessor. There is nothing more
  // that we can do, return.
  //
  // This enables us to get enum information from switch_enum and cond_br about
  // the value that an enum can take in our block. This is a common case that
  // comes up.
  if (PI == PE) {
    mergeSinglePredTermInfoIntoState(FirstPred);
    return;
  }

  LLVM_DEBUG(llvm::dbgs() <<"            Merging in rest of predecessors...\n");

  // Enum values that while merging we found conflicting values for. We blot
  // them after the loop in order to ensure that we can still find the ends of
  // switch regions.
  llvm::SmallVector<unsigned, 4> CurBBValuesToBlot;

  // If we do not find state for a specific value in any of our predecessor BBs,
  // we cannot be the end of a switch region since we cannot cover our
  // predecessor BBs with enum decls. Blot after the loop.
  llvm::SmallVector<unsigned, 4> PredBBValuesToBlot;

  // And for each remaining predecessor...
  do {
    // If we loop on ourselves, bail...
    if (*PI == getBB()) {
      LLVM_DEBUG(llvm::dbgs() << "            Found a self loop. Bailing!\n");
      return;
    }

    // Grab the predecessors state...
    SILBasicBlock *PredBB = *PI;

    BBEnumTagDataflowState *PredBBState = getContext().getBBState(PredBB);
    if (PredBBState == nullptr) {
      LLVM_DEBUG(llvm::dbgs() << "            Found an unreachable block!\n");
      return;
    }

    ++PI;

    // Then for each (SILValue, Enum Tag) that we are tracking...
    for (auto P : ValueToCaseMap.getItems()) {
      // If this SILValue was blotted, there is nothing left to do, we found
      // some sort of conflicting definition and are being conservative.
      if (!P.has_value())
        continue;

      // Then attempt to look up the enum state associated in our SILValue in
      // the predecessor we are processing.
      auto PredIter = PredBBState->ValueToCaseMap.find(P->first);

      // If we cannot find the state associated with this SILValue in this
      // predecessor or the ID in the corresponding predecessor was blotted, we
      // cannot find a covering switch for this BB or forward any enum tag
      // information for this enum value.
      if (PredIter == PredBBState->ValueToCaseMap.end() ||
          !(*PredIter).has_value()) {
        // Otherwise, we are conservative and do not forward the EnumTag that we
        // are tracking. Blot it!
        LLVM_DEBUG(llvm::dbgs() << "                Blotting: " << P->first);
        CurBBValuesToBlot.push_back(P->first);
        PredBBValuesToBlot.push_back(P->first);
        continue;
      }

      // Then try to lookup the actual value associated with the ID. If we do
      // not find one, then the enum was destroyed by another part of the pass.
      SILValue PredValue = getValueForID((*PredIter)->first);
      if (!PredValue)
        continue;

      // Check if out predecessor has any other successors. If that is true we
      // clear all the state since we cannot hoist safely.
      if (!PredBB->getSingleSuccessorBlock()) {
        EnumToEnumBBCaseListMap.clear();
        LLVM_DEBUG(llvm::dbgs() << "                Predecessor has other "
                                   "successors. Clearing BB cast list map.\n");
      } else {
        // Otherwise, add this case to our predecessor case list. We will unique
        // this after we have finished processing all predecessors.
        auto Case = std::make_pair(PredBB, (*PredIter)->second);
        EnumToEnumBBCaseListMap[(*PredIter)->first].push_back(Case);
      }

      // And the states match, the enum state propagates to this BB.
      if ((*PredIter)->second == P->second)
        continue;

      // Otherwise, we are conservative and do not forward the EnumTag that we
      // are tracking. Blot it!
      LLVM_DEBUG(llvm::dbgs() << "                Blotting: " << P->first);
      CurBBValuesToBlot.push_back(P->first);
    }
  } while (PI != PE);

  for (unsigned ID : CurBBValuesToBlot) {
    ValueToCaseMap.erase(ID);
  }
  for (unsigned ID : PredBBValuesToBlot) {
    EnumToEnumBBCaseListMap.erase(ID);
  }
}

bool BBEnumTagDataflowState::visitEnumInst(EnumInst *EI) {
  unsigned ID = getIDForValue(SILValue(EI));
  LLVM_DEBUG(llvm::dbgs() << "    Storing enum into map. ID: " << ID
                          << ". Value: " << *EI);
  ValueToCaseMap[ID] = EI->getElement();
  return false;
}

bool BBEnumTagDataflowState::visitUncheckedEnumDataInst(
    UncheckedEnumDataInst *UEDI) {
  unsigned ID = getIDForValue(UEDI->getOperand());
  LLVM_DEBUG(llvm::dbgs() << "    Storing unchecked enum data into map. ID: "
                          << ID << ". Value: " << *UEDI);
  ValueToCaseMap[ID] = UEDI->getElement();
  return false;
}

bool BBEnumTagDataflowState::visitRetainValueInst(RetainValueInst *RVI) {
  auto FindResult = ValueToCaseMap.find(getIDForValue(RVI->getOperand()));
  if (FindResult == ValueToCaseMap.end())
    return false;

  // If we do not have any argument, kill the retain_value.
  if (!(*FindResult)->second->hasAssociatedValues()) {
    RVI->eraseFromParent();
    return true;
  }

  LLVM_DEBUG(llvm::dbgs() << "    Found RetainValue: " << *RVI);
  LLVM_DEBUG(llvm::dbgs() << "        Paired to Enum Oracle: "
                          << (*FindResult)->first);

  SILBuilderWithScope Builder(RVI);
  createRefCountOpForPayload(Builder, RVI, (*FindResult)->second);
  RVI->eraseFromParent();
  return true;
}

bool BBEnumTagDataflowState::visitReleaseValueInst(ReleaseValueInst *RVI) {
  auto FindResult = ValueToCaseMap.find(getIDForValue(RVI->getOperand()));
  if (FindResult == ValueToCaseMap.end())
    return false;

  // If the enum has a deinit, preserve the original release.
  if (hasValueDeinit(RVI->getOperand()))
    return false;

  // If we do not have any argument, just delete the release value.
  if (!(*FindResult)->second->hasAssociatedValues()) {
    RVI->eraseFromParent();
    return true;
  }

  LLVM_DEBUG(llvm::dbgs() << "    Found ReleaseValue: " << *RVI);
  LLVM_DEBUG(llvm::dbgs() << "        Paired to Enum Oracle: "
                          << (*FindResult)->first);

  SILBuilderWithScope Builder(RVI);
  createRefCountOpForPayload(Builder, RVI, (*FindResult)->second);
  RVI->eraseFromParent();
  return true;
}

bool BBEnumTagDataflowState::process() {
  bool Changed = false;

  auto SI = getBB()->begin();
  while (SI != getBB()->end()) {
    SILInstruction *I = &*SI;
    ++SI;
    Changed |= visit(I);
  }

  return Changed;
}

bool BBEnumTagDataflowState::hoistDecrementsIntoSwitchRegions(
    AliasAnalysis *AA) {
  bool Changed = false;
  unsigned NumPreds = std::distance(getBB()->pred_begin(), getBB()->pred_end());

  for (auto II = getBB()->begin(), IE = getBB()->end(); II != IE;) {
    auto *RVI = dyn_cast<ReleaseValueInst>(&*II);
    ++II;

    // If this instruction is not a release, skip it...
    if (!RVI)
      continue;

    LLVM_DEBUG(llvm::dbgs() << "        Visiting release: " << *RVI);

    // Grab the operand of the release value inst.
    SILValue Op = RVI->getOperand();

    // Lookup the [(BB, EnumTag)] list for this operand.
    unsigned ID = getIDForValue(Op);
    auto R = EnumToEnumBBCaseListMap.find(ID);
    // If we don't have one, skip this release value inst.
    if (R == EnumToEnumBBCaseListMap.end()) {
      LLVM_DEBUG(llvm::dbgs() << "            Could not find [(BB, EnumTag)] "
                                "list for release_value's operand. Bailing!\n");
      continue;
    }

    // If the enum has a deinit, preserve the original release.
    if (hasValueDeinit(Op))
      return false;

    auto &EnumBBCaseList = (*R)->second;
    // If we don't have an enum tag for each predecessor of this BB, bail since
    // we do not know how to handle that BB.
    if (EnumBBCaseList.size() != NumPreds) {
      LLVM_DEBUG(llvm::dbgs() << "            Found [(BB, EnumTag)] list for "
                                 "release_value's operand, but we do not have "
                                "an enum tag for each predecessor. Bailing!\n");
      LLVM_DEBUG(llvm::dbgs() << "            List:\n");
      LLVM_DEBUG(for (auto P : EnumBBCaseList) {
        llvm::dbgs() << "                ";
        P.second->dump(llvm::dbgs());
      });
      continue;
    }

    // Finally ensure that we have no users of this operand preceding the
    // release_value in this BB. If we have users like that we cannot hoist the
    // release past them unless we know that there is an additional set of
    // releases that together post-dominate this release. If we cannot do this,
    // skip this release.
    //
    // TODO: We need information from the ARC optimizer to prove that property
    // if we are going to use it.
    if (valueHasARCUsesInInstructionRange(Op, getBB()->begin(),
                                          SILBasicBlock::iterator(RVI), AA)) {
      LLVM_DEBUG(llvm::dbgs() << "            Release value has use that stops "
                                 "hoisting! Bailing!\n");
      continue;
    }

    LLVM_DEBUG(llvm::dbgs() << "            Its safe to perform the "
                               "transformation!\n");

    // Otherwise perform the transformation.
    for (auto P : EnumBBCaseList) {
      // If we don't have an argument for this case, there is nothing to
      // do... continue...
      if (!P.second->hasAssociatedValues())
        continue;

      // Otherwise create the release_value before the terminator of the
      // predecessor.
      assert(P.first->getSingleSuccessorBlock() &&
             "Cannot hoist release into BB that has multiple successors");
      SILBuilderWithScope Builder(P.first->getTerminator(), RVI);
      createRefCountOpForPayload(Builder, RVI, P.second);
    }

    RVI->eraseFromParent();
    ++NumHoisted;
    Changed = true;
  }

  return Changed;
}

static SILInstruction *findLastSinkableMatchingEnumValueRCIncrementInPred(
    AliasAnalysis *AA, RCIdentityFunctionInfo *RCIA, SILValue EnumValue,
    SILBasicBlock *BB) {
  // Otherwise, see if we can find a retain_value or strong_retain associated
  // with that enum in the relevant predecessor.
  auto FirstInc = std::find_if(
      BB->rbegin(), BB->rend(),
      [&RCIA, &EnumValue](const SILInstruction &I) -> bool {
        // If I is not an increment, ignore it.
        if (!isa<StrongRetainInst>(I) && !isa<RetainValueInst>(I))
          return false;

        // Otherwise, if the increments operand stripped of RC identity
        // preserving
        // ops matches EnumValue, it is the first increment we are interested
        // in.
        return EnumValue == RCIA->getRCIdentityRoot(I.getOperand(0));
      });

  // If we do not find a ref count increment in the relevant BB, skip this
  // enum since there is nothing we can do.
  if (FirstInc == BB->rend())
    return nullptr;

  // Otherwise, see if there are any instructions in between FirstPredInc and
  // the end of the given basic block that could decrement first pred. If such
  // an instruction exists, we cannot perform this optimization so continue.
  if (valueHasARCDecrementOrCheckInInstructionRange(
          EnumValue, (*FirstInc).getIterator(),
          BB->getTerminator()->getIterator(), AA))
    return nullptr;

  return &*FirstInc;
}

static bool findRetainsSinkableFromSwitchRegionForEnum(
    AliasAnalysis *AA, RCIdentityFunctionInfo *RCIA, SILValue EnumValue,
    EnumBBCaseList &Map, SmallVectorImpl<SILInstruction *> &DeleteList) {

  // For each predecessor with argument type...
  for (auto &P : Map) {
    SILBasicBlock *PredBB = P.first;
    EnumElementDecl *Decl = P.second;

    // If the case does not have an argument type, skip the predecessor since
    // there will not be a retain to sink.
    if (!Decl->hasAssociatedValues())
      continue;

    // Ok, we found a payloaded predecessor. Look backwards through the
    // predecessor for the first ref count increment on EnumValue. If there
    // are no ref count decrements in between the increment and the terminator
    // of the BB, then we can sink the retain out of the switch enum.
    auto *Inc = findLastSinkableMatchingEnumValueRCIncrementInPred(
        AA, RCIA, EnumValue, PredBB);
    // If we do not find such an increment, there is nothing we can do, bail.
    if (!Inc)
      return false;

    // Otherwise add the increment to the delete list.
    DeleteList.push_back(Inc);
  }

  // If we were able to process each predecessor successfully, return true.
  return true;
}

bool BBEnumTagDataflowState::sinkIncrementsOutOfSwitchRegions(
    AliasAnalysis *AA, RCIdentityFunctionInfo *RCIA) {
  bool Changed = false;
  unsigned NumPreds = std::distance(getBB()->pred_begin(), getBB()->pred_end());
  llvm::SmallVector<SILInstruction *, 4> DeleteList;

  // For each (EnumValue, [(BB, EnumTag)]) that we are tracking...
  for (auto &P : EnumToEnumBBCaseListMap) {
    // Clear our delete list.
    DeleteList.clear();

    // If EnumValue is null, we deleted this entry. There is nothing to do for
    // this value... Skip it.
    if (!P.has_value())
      continue;

    // Look up the actual enum value using our index to make sure that other
    // parts of the pass have not destroyed the value. In such a case, just
    // continue.
    SILValue EnumValue = getContext().getValueForID(P->first);
    if (!EnumValue)
      continue;
    EnumValue = RCIA->getRCIdentityRoot(EnumValue);
    EnumBBCaseList &Map = P->second;

    // If we do not have a tag associated with this enum value for each
    // predecessor, we are not a switch region exit for this enum value. Skip
    // this value.
    if (Map.size() != NumPreds)
      continue;

    // Look through our predecessors for a set of ref count increments on our
    // enum value for every payloaded case that *could* be sunk. If we miss an
    // increment from any of the payloaded case there is nothing we can do here,
    // so skip this enum value.
    if (!findRetainsSinkableFromSwitchRegionForEnum(AA, RCIA, EnumValue, Map,
                                                    DeleteList))
      continue;

    // If we do not have any payload arguments, then we should have an empty
    // delete list and there is nothing to do here.
    if (DeleteList.empty())
      continue;

    // Ok, we can perform this transformation! Insert the new retain_value and
    // delete all of the ref count increments from the predecessor BBs.
    //
    // TODO: Which debug loc should we use here? Using one of the locs from the
    // delete list seems reasonable for now...
    SILBuilder Builder(getBB()->begin());
    Builder.createRetainValue(
        DeleteList[0]->getLoc(), EnumValue,
        cast<RefCountingInst>(DeleteList[0])->getAtomicity());
    for (auto *I : DeleteList)
      I->eraseFromParent();
    ++NumSunk;
    Changed = true;
  }

  return Changed;
}

void BBEnumTagDataflowState::dump() const {
#ifndef NDEBUG
  llvm::dbgs() << "Dumping state for BB" << BB.get()->getDebugID() << "\n";
  llvm::dbgs() << "Block States:\n";
  for (auto &P : ValueToCaseMap) {
    if (!P.hasValue()) {
      llvm::dbgs() << "  Skipping blotted value.\n";
      continue;
    }
    unsigned ID = P->first;
    SILValue V = getContext().getValueForID(ID);
    if (!V) {
      llvm::dbgs() << "  ID: " << ID << ". Value: BLOTTED.\n";
      continue;
    }
    llvm::dbgs() << "  ID: " << ID << ". Value: " << V;
  }

  llvm::dbgs() << "Predecessor States:\n";
  // For each (EnumValue, [(BB, EnumTag)]) that we are tracking...
  for (auto &P : EnumToEnumBBCaseListMap) {
    if (!P.hasValue()) {
      llvm::dbgs() << "  Skipping blotted value.\n";
      continue;
    }
    unsigned ID = P->first;
    SILValue V = getContext().getValueForID(ID);
    if (!V) {
      llvm::dbgs() << "  ID: " << ID << ". Value: BLOTTED.\n";
      continue;
    }
    llvm::dbgs() << "  ID: " << ID << ". Value: " << V;
    llvm::dbgs() << "  Case List:\n";
    for (auto &P2 : P->second) {
      llvm::dbgs() << "    BB" << P2.first->getDebugID() << ": ";
      P2.second->dump(llvm::dbgs());
      llvm::dbgs() << "\n";
    }
    llvm::dbgs() << "  End Case List.\n";
  }
#endif
}

bool BBEnumTagDataflowState::init(EnumCaseDataflowContext &NewContext,
                                  SILBasicBlock *NewBB) {
  assert(NewBB && "NewBB should not be null");
  Context = &NewContext;
  BB = NewBB;
  return true;
}

//===----------------------------------------------------------------------===//
//                            Generic Sinking Code
//===----------------------------------------------------------------------===//

/// Hoist release on a SILArgument to its predecessors.
static bool hoistSILArgumentReleaseInst(SILBasicBlock *BB) {
  // There is no block to hoist releases to.
  if (BB->pred_empty())
    return false;

  // Only try to hoist the first instruction. RRCM should have hoisted the
  // release
  // to the beginning of the block if it can.
  auto Head = &*BB->begin();
  // Make sure it is a release instruction.
  if (!isReleaseInstruction(&*Head))
    return false;

  // Make sure it is a release on a SILArgument of the current basic block..
  auto *SA = dyn_cast<SILArgument>(Head->getOperand(0));
  if (!SA || SA->getParent() != BB)
    return false;

  // Make sure the release will not be blocked by the terminator instructions
  // Make sure the terminator does not block, nor is a branch with multiple
  // targets.
  for (auto P : BB->getPredecessorBlocks()) {
    if (!isa<BranchInst>(P->getTerminator()))
      return false;
  }

  // Make sure we can get all the incoming values.
  llvm::SmallVector<SILValue, 4> PredValues;
  if (!SA->getIncomingPhiValues(PredValues))
    return false;

  // Ok, we can get all the incoming values and create releases for them.
  unsigned indices = 0;
  for (auto P : BB->getPredecessorBlocks()) {
    createDecrementBefore(PredValues[indices++], P->getTerminator());
  }
  // Erase the old instruction.
  Head->eraseFromParent();
  ++NumSILArgumentReleaseHoisted;
  return true;
}

static const int SinkSearchWindow = 6;

/// Returns True if we can sink this instruction to another basic block.
static bool canSinkInstruction(SILInstruction *Inst) {
  return !Inst->hasUsesOfAnyResult() && !isa<TermInst>(Inst);
}

/// Returns true if this instruction is a skip barrier, which means that
/// we can't sink other instructions past it.
static bool isSinkBarrier(SILInstruction *Inst) {
  if (isa<TermInst>(Inst))
    return false;

  if (Inst->mayHaveSideEffects())
    return true;

  return false;
}

using ValueInBlock = std::pair<SILValue, SILBasicBlock *>;
using ValueToBBArgIdxMap = llvm::DenseMap<ValueInBlock, int>;

enum OperandRelation {
  /// Uninitialized state.
  NotDeterminedYet,

  /// The original operand values are equal.
  AlwaysEqual,

  /// The operand values are equal after replacing with the successor block
  /// arguments.
  EqualAfterMove
};

/// Find a root value for operand \p In. This function inspects a sil
/// value and strips trivial conversions such as values that are passed
/// as arguments to basic blocks with a single predecessor or type casts.
/// This is a shallow one-step search and not a deep recursive search.
///
/// For example, in the SIL code below, the root of %10 is %3, because it is
/// the only possible incoming value.
///
/// bb1:
///  %3 = unchecked_enum_data %0 : $Optional<X>, #Optional.Some!enumelt
///  checked_cast_br [exact] %3 : $X to $X, bb4, bb5 // id: %4
///
/// bb4(%10 : $X):                                    // Preds: bb1
///  strong_release %10 : $X
///  br bb2
///
static SILValue findValueShallowRoot(const SILValue &In) {
  // If this is a basic block argument with a single caller
  // then we know exactly which value is passed to the argument.
  if (auto *Arg = dyn_cast<SILArgument>(In)) {
    SILBasicBlock *Parent = Arg->getParent();
    SILBasicBlock *Pred = Parent->getSinglePredecessorBlock();
    if (!Pred)
      return In;

    // If the terminator is a cast instruction then use the pre-cast value.
    if (auto CCBI = dyn_cast<CheckedCastBranchInst>(Pred->getTerminator())) {
      assert(CCBI->getSuccessBB() == Parent && "Inspecting the wrong block");

      // In swift it is legal to cast non reference-counted references into
      // object references. For example: func f(x : C.Type) -> Any {return x}
      // Here we check that the uncasted reference is reference counted.
      SILValue V = CCBI->getOperand();
      if (V->getType().isReferenceCounted(Pred->getParent()->getModule())) {
        return V;
      }
    }

    // If the single predecessor terminator is a branch then the root is
    // the argument to the terminator.
    if (auto BI = dyn_cast<BranchInst>(Pred->getTerminator())) {
      assert(BI->getDestBB() == Parent && "Invalid terminator");
      unsigned Idx = Arg->getIndex();
      return BI->getArg(Idx);
    }

    if (auto CBI = dyn_cast<CondBranchInst>(Pred->getTerminator())) {
      return CBI->getArgForDestBB(Parent, Arg);
    }
  }
  return In;
}

/// Search for an instruction that is identical to \p Iden by scanning
/// \p BB starting at the end of the block, stopping on sink barriers.
/// The \p opRelation must be consistent for all operand comparisons.
SILInstruction *findIdenticalInBlock(SILBasicBlock *BB, SILInstruction *Iden,
                                     const ValueToBBArgIdxMap &valueToArgIdxMap,
                                     OperandRelation &opRelation) {
  int SkipBudget = SinkSearchWindow;

  SILBasicBlock::iterator InstToSink = BB->getTerminator()->getIterator();
  SILBasicBlock *IdenBlock = Iden->getParent();

  // The compare function for instruction operands.
  auto operandCompare = [&](const SILValue &Op1, const SILValue &Op2) -> bool {

    if (opRelation != EqualAfterMove && Op1 == Op2) {
      // The trivial case.
      opRelation = AlwaysEqual;
      return true;
    }

    // Check if both operand values are passed to the same block argument in the
    // successor block. This means that the operands are equal after we move the
    // instruction into the successor block.
    if (opRelation != AlwaysEqual) {
      auto Iter1 = valueToArgIdxMap.find({Op1, IdenBlock});
      if (Iter1 != valueToArgIdxMap.end()) {
        auto Iter2 = valueToArgIdxMap.find({Op2, BB});
        if (Iter2 != valueToArgIdxMap.end() && Iter1->second == Iter2->second) {
          opRelation = EqualAfterMove;
          return true;
        }
      }
    }
    return false;
  };

  while (SkipBudget) {
    // If we found a sinkable instruction that is identical to our goal
    // then return it.
    if (canSinkInstruction(&*InstToSink) &&
        Iden->isIdenticalTo(&*InstToSink, operandCompare)) {
      LLVM_DEBUG(llvm::dbgs() << "Found an identical instruction.");
      return &*InstToSink;
    }

    // If this instruction is a skip-barrier end the scan.
    if (isSinkBarrier(&*InstToSink))
      return nullptr;

    // If this is the first instruction in the block then we are done.
    if (InstToSink == BB->begin())
      return nullptr;

    --SkipBudget;
    InstToSink = std::prev(InstToSink);
    LLVM_DEBUG(llvm::dbgs() << "Continuing scan. Next inst: " << *InstToSink);
  }

  return nullptr;
}

/// The 2 instructions given are not identical, but are passed as arguments
/// to a common successor.  It may be cheaper to pass one of their operands
/// to the successor instead of the whole instruction.
/// Return None if no such operand could be found, otherwise return the index
/// of a suitable operand.
static llvm::Optional<unsigned>
cheaperToPassOperandsAsArguments(SILInstruction *First,
                                 SILInstruction *Second) {
  // This will further enable to sink strong_retain_unowned instructions,
  // which provides more opportunities for the unowned-optimization in
  // LLVMARCOpts.
#define LOADABLE_REF_STORAGE(Name, ...) \
  if (isa<Name##ToRefInst>(First) && isa<Name##ToRefInst>(Second)) { \
    return 0; \
  }
#include "swift/AST/ReferenceStorage.def"

  // TODO: Add more cases than Struct
  auto *FirstStruct = dyn_cast<StructInst>(First);
  auto *SecondStruct = dyn_cast<StructInst>(Second);

  if (!FirstStruct || !SecondStruct)
    return llvm::None;

  assert(FirstStruct->getNumOperands() == SecondStruct->getNumOperands() &&
         FirstStruct->getType() == SecondStruct->getType() &&
         "Types should be identical");

  llvm::Optional<unsigned> DifferentOperandIndex;

  // Check operands.
  for (unsigned i = 0, e = First->getNumOperands(); i != e; ++i) {
    if (FirstStruct->getOperand(i) != SecondStruct->getOperand(i)) {
      // Only track one different operand for now
      if (DifferentOperandIndex)
        return llvm::None;
      DifferentOperandIndex = i;
    }
  }

  if (!DifferentOperandIndex)
    return llvm::None;

  // Found a different operand, now check to see if its type is something
  // cheap enough to sink.
  // TODO: Sink more than just integers.
  SILType ArgTy = FirstStruct->getOperand(*DifferentOperandIndex)->getType();
  if (!ArgTy.is<BuiltinIntegerType>())
    return llvm::None;

  return *DifferentOperandIndex;
}

/// Return the value that's passed from block \p From to block \p To
/// (if there is a branch between From and To) as the Nth argument.
SILValue getArgForBlock(SILBasicBlock *From, SILBasicBlock *To,
                        unsigned ArgNum) {
  TermInst *Term = From->getTerminator();
  if (auto *CondBr = dyn_cast<CondBranchInst>(Term)) {
    if (CondBr->getFalseBB() == To)
      return CondBr->getFalseArgs()[ArgNum];

    if (CondBr->getTrueBB() == To)
      return CondBr->getTrueArgs()[ArgNum];
  }

  if (auto *Br = dyn_cast<BranchInst>(Term))
    return Br->getArg(ArgNum);

  return SILValue();
}

// Try to sink values from the Nth argument \p ArgNum.
static bool sinkLiteralArguments(SILBasicBlock *BB, unsigned ArgNum) {
  assert(ArgNum < BB->getNumArguments() && "Invalid argument");

  // Check if the argument passed to the first predecessor is a literal inst.
  SILBasicBlock *FirstPred = *BB->pred_begin();
  SILValue FirstArg = getArgForBlock(FirstPred, BB, ArgNum);
  LiteralInst *FirstLiteral = dyn_cast_or_null<LiteralInst>(FirstArg);
  if (!FirstLiteral)
    return false;

  // Check if the Nth argument in all predecessors is identical.
  for (auto P : BB->getPredecessorBlocks()) {
    if (P == FirstPred)
      continue;

    // Check that the incoming value is identical to the first literal.
    SILValue PredArg = getArgForBlock(P, BB, ArgNum);
    LiteralInst *PredLiteral = dyn_cast_or_null<LiteralInst>(PredArg);
    if (!PredLiteral || !PredLiteral->isIdenticalTo(FirstLiteral))
      return false;
  }

  // Replace the use of the argument with the cloned literal.
  auto Cloned = FirstLiteral->clone(&*BB->begin());
  BB->getArgument(ArgNum)->replaceAllUsesWith(Cloned);

  return true;
}

// Try to sink values from the Nth argument \p ArgNum.
static bool sinkArgument(EnumCaseDataflowContext &Context, SILBasicBlock *BB, unsigned ArgNum) {
  assert(ArgNum < BB->getNumArguments() && "Invalid argument");

  // Find the first predecessor, the first terminator and the Nth argument.
  SILBasicBlock *FirstPred = *BB->pred_begin();
  TermInst *FirstTerm = FirstPred->getTerminator();
  auto FirstPredArg = FirstTerm->getOperand(ArgNum);
  auto *FSI = dyn_cast<SingleValueInstruction>(FirstPredArg);
  // TODO: MultiValueInstruction?

  // We only move instructions with a single use.
  if (!FSI || !hasOneNonDebugUse(FSI))
    return false;

  // The list of identical instructions.
  SmallVector<SingleValueInstruction *, 8> Clones;
  Clones.push_back(FSI);

  // Don't move instructions that are sensitive to their location.
  //
  // If this instruction can read memory, we try to be conservatively not to
  // move it, as there may be instructions that can clobber the read memory
  // from current place to the place where it is moved to.
  if (FSI->mayReadFromMemory() ||
      (FSI->mayHaveSideEffects() && !isa<AllocationInst>(FSI)))
    return false;

  // If the instructions are different, but only in terms of a cheap operand
  // then we can still sink it, and create new arguments for this operand.
  llvm::Optional<unsigned> DifferentOperandIndex;

  // Check if the Nth argument in all predecessors is identical.
  for (auto P : BB->getPredecessorBlocks()) {
    if (P == FirstPred)
      continue;

    // Only handle branch or conditional branch instructions.
    TermInst *TI = P->getTerminator();
    if (!isa<BranchInst>(TI) && !isa<CondBranchInst>(TI))
      return false;

    // Find the Nth argument passed to BB.
    SILValue Arg = TI->getOperand(ArgNum);

    // If it's not the same basic kind of node, neither isIdenticalTo nor
    // cheaperToPassOperandsAsArguments will return true.
    if (Arg->getKind() != FSI->getValueKind())
      return false;

    // Since it's the same kind, Arg must also be a single-value instruction.
    auto *SI = cast<SingleValueInstruction>(Arg);

    if (!hasOneNonDebugUse(SI))
      return false;

    if (SI->isIdenticalTo(FSI)) {
      Clones.push_back(SI);
      continue;
    }

    // If the instructions are close enough, then we should sink them anyway.
    // For example, we should sink 'struct S(%0)' if %0 is small, eg, an integer
    auto MaybeDifferentOp = cheaperToPassOperandsAsArguments(FSI, SI);
    // Couldn't find a suitable operand, so bail.
    if (!MaybeDifferentOp)
      return false;
    unsigned DifferentOp = *MaybeDifferentOp;
    // Make sure we found the same operand as prior iterations.
    if (DifferentOperandIndex && DifferentOp != *DifferentOperandIndex)
      return false;

    DifferentOperandIndex = DifferentOp;
    Clones.push_back(SI);
  }

  auto *Undef = SILUndef::get(FSI->getType(), *BB->getParent());

  // Delete the debug info of the instruction that we are about to sink.
  deleteAllDebugUses(FSI);

  if (DifferentOperandIndex) {
    // Sink one of the instructions to BB
    FSI->moveBefore(&*BB->begin());

    // The instruction we are lowering has an argument which is different
    // for each predecessor.  We need to sink the instruction, then add
    // arguments for each predecessor.
    BB->getArgument(ArgNum)->replaceAllUsesWith(FSI);

    const auto &ArgType = FSI->getOperand(*DifferentOperandIndex)->getType();
    BB->replacePhiArgument(ArgNum, ArgType, OwnershipKind::Owned);

    // Update all branch instructions in the predecessors to pass the new
    // argument to this BB.
    auto CloneIt = Clones.begin();
    for (auto P : BB->getPredecessorBlocks()) {
      // Only handle branch or conditional branch instructions.
      TermInst *TI = P->getTerminator();
      assert((isa<BranchInst>(TI) || isa<CondBranchInst>(TI)) &&
             "Branch instruction required");

      // TODO: MultiValueInstruction
      auto *CloneInst = *CloneIt;
      TI->setOperand(ArgNum, CloneInst->getOperand(*DifferentOperandIndex));
      // Now delete the clone as we only needed it operand.
      if (CloneInst != FSI)
        eliminateDeadInstruction(CloneInst);
      ++CloneIt;
    }
    assert(CloneIt == Clones.end() && "Clone/pred mismatch");

    // The sunk instruction should now read from the argument of the BB it
    // was moved to.
    FSI->setOperand(*DifferentOperandIndex, BB->getArgument(ArgNum));
    return true;
  }

  // Sink one of the copies of the instruction.
  FSI->replaceAllUsesWithUndef();
  FSI->moveBefore(&*BB->begin());
  BB->getArgument(ArgNum)->replaceAllUsesWith(FSI);

  // The argument is no longer in use. Replace all incoming inputs with undef
  // and try to delete the instruction.
  for (auto S : Clones) {
    if (S != FSI) {
      deleteAllDebugUses(S);
      S->replaceAllUsesWith(Undef);
      auto DeadArgInst = cast<SILInstruction>(S);
      for (SILValue Result : DeadArgInst->getResults()) {
        Context.blotValue(Result);
      }
      DeadArgInst->eraseFromParent();
    }
  }

  return true;
}

/// Try to sink literals that are passed to arguments that are coming from
/// multiple predecessors.
/// Notice that unlike other sinking methods in this file we do allow sinking
/// of literals from blocks with multiple successors.
static bool sinkLiteralsFromPredecessors(SILBasicBlock *BB) {
  if (BB->pred_empty() || BB->getSinglePredecessorBlock())
    return false;

  // Try to sink values from each of the arguments to the basic block.
  bool Changed = false;
  for (int i = 0, e = BB->getNumArguments(); i < e; ++i)
    Changed |= sinkLiteralArguments(BB, i);

  return Changed;
}

/// Try to sink identical arguments coming from multiple predecessors.
static bool sinkArgumentsFromPredecessors(EnumCaseDataflowContext &Context,
                                          SILBasicBlock *BB) {
  if (BB->pred_empty() || BB->getSinglePredecessorBlock())
    return false;

  // This block must be the only successor of all the predecessors.
  for (auto P : BB->getPredecessorBlocks())
    if (P->getSingleSuccessorBlock() != BB)
      return false;

  // Try to sink values from each of the arguments to the basic block.
  bool Changed = false;
  for (int i = 0, e = BB->getNumArguments(); i < e; ++i)
    Changed |= sinkArgument(Context, BB, i);

  return Changed;
}

/// canonicalize retain/release instructions and make them amenable to
/// sinking by selecting canonical pointers. We reduce the number of possible
/// inputs by replacing values that are unlikely to be a canonical values.
/// Reducing the search space increases the chances of matching ref count
/// instructions to one another and the chance of sinking them. We replace
/// values that come from basic block arguments with the caller values and
/// strip casts.
static bool canonicalizeRefCountInstrs(SILBasicBlock *BB) {
  bool Changed = false;
  for (auto I = BB->begin(), E = BB->end(); I != E; ++I) {
    if (!isa<StrongReleaseInst>(I) && !isa<StrongRetainInst>(I))
      continue;

    SILValue Ref = I->getOperand(0);
    SILValue Root = findValueShallowRoot(Ref);
    if (Ref != Root) {
      I->setOperand(0, Root);
      Changed = true;
    }
  }

  return Changed;
}

static bool sinkCodeFromPredecessors(EnumCaseDataflowContext &Context,
                                     SILBasicBlock *BB) {
  bool Changed = false;
  if (BB->pred_empty())
    return Changed;

  // This block must be the only successor of all the predecessors.
  for (auto P : BB->getPredecessorBlocks())
    if (P->getSingleSuccessorBlock() != BB)
      return Changed;

  SILBasicBlock *FirstPred = *BB->pred_begin();
  // The first Pred must have at least one non-terminator.
  if (FirstPred->getTerminator() == &*FirstPred->begin())
    return Changed;

  LLVM_DEBUG(llvm::dbgs() << " Sinking values from predecessors.\n");

  // Map values in predecessor blocks to argument indices of the successor
  // block. For example:
  //
  // bb1:
  //   br bb3(%a, %b)    // %a -> 0, %b -> 1
  // bb2:
  //   br bb3(%c, %d)    // %c -> 0, %d -> 1
  // bb3(%x, %y):
  //   ...
  ValueToBBArgIdxMap valueToArgIdxMap;
  for (auto P : BB->getPredecessorBlocks()) {
    if (auto *BI = dyn_cast<BranchInst>(P->getTerminator())) {
      auto Args = BI->getArgs();
      for (size_t idx = 0, size = Args.size(); idx < size; ++idx) {
        valueToArgIdxMap[{Args[idx], P}] = idx;
      }
    }
  }

  unsigned SkipBudget = SinkSearchWindow;

  // Start scanning backwards from the terminator.
  auto InstToSink = FirstPred->getTerminator()->getIterator();

  while (SkipBudget) {
    LLVM_DEBUG(llvm::dbgs() << "Processing: " << *InstToSink);

    // Save the duplicated instructions in case we need to remove them.
    SmallVector<SILInstruction *, 4> Dups;

    if (canSinkInstruction(&*InstToSink)) {

      OperandRelation opRelation = NotDeterminedYet;

      // For all preds:
      for (auto P : BB->getPredecessorBlocks()) {
        if (P == FirstPred)
          continue;

        // Search the duplicated instruction in the predecessor.
        if (SILInstruction *DupInst = findIdenticalInBlock(
                P, &*InstToSink, valueToArgIdxMap, opRelation)) {
          Dups.push_back(DupInst);
        } else {
          LLVM_DEBUG(llvm::dbgs() << "Instruction mismatch.\n");
          Dups.clear();
          break;
        }
      }

      // If we found duplicated instructions, sink one of the copies and delete
      // the rest.
      if (Dups.size()) {
        LLVM_DEBUG(llvm::dbgs() << "Moving: " << *InstToSink);
        InstToSink->moveBefore(&*BB->begin());

        if (opRelation == EqualAfterMove) {
          // Replace operand values (which are passed to the successor block)
          // with corresponding block arguments.
          for (size_t idx = 0, numOps = InstToSink->getNumOperands();
               idx < numOps; ++idx) {
            ValueInBlock OpInFirstPred(InstToSink->getOperand(idx), FirstPred);
            assert(valueToArgIdxMap.count(OpInFirstPred) != 0);
            int argIdx = valueToArgIdxMap[OpInFirstPred];
            InstToSink->setOperand(idx, BB->getArgument(argIdx));
          }
        }
        Changed = true;
        for (auto I : Dups) {
          I->replaceAllUsesPairwiseWith(&*InstToSink);
          for (SILValue Result : I->getResults()) {
            Context.blotValue(Result);
          }
          I->eraseFromParent();
          ++NumSunk;
        }

        // Restart the scan.
        InstToSink = FirstPred->getTerminator()->getIterator();
        LLVM_DEBUG(llvm::dbgs() << "Restarting scan. Next inst: "
                                << *InstToSink);
        continue;
      }
    }

    // If this instruction was a barrier then we can't sink anything else.
    if (isSinkBarrier(&*InstToSink)) {
      LLVM_DEBUG(llvm::dbgs() << "Aborting on barrier: " << *InstToSink);
      return Changed;
    }

    // This is the first instruction, we are done.
    if (InstToSink == FirstPred->begin()) {
      LLVM_DEBUG(llvm::dbgs() << "Reached the first instruction.");
      return Changed;
    }

    --SkipBudget;
    InstToSink = std::prev(InstToSink);
    LLVM_DEBUG(llvm::dbgs() << "Continuing scan. Next inst: " << *InstToSink);
  }

  return Changed;
}

/// Sink retain_value, release_value before switch_enum to be retain_value,
/// release_value on the payload of the switch_enum in the destination BBs. We
/// only do this if the destination BBs have only the switch enum as its
/// predecessor.
static bool tryToSinkRefCountAcrossSwitch(SwitchEnumInst *Switch,
                                          SILBasicBlock::iterator RV,
                                          AliasAnalysis *AA,
                                          RCIdentityFunctionInfo *RCIA) {
  // If this instruction is not a retain_value, there is nothing left for us to
  // do... bail...
  if (!isa<RetainValueInst>(RV))
    return false;

  SILValue Ptr = RV->getOperand(0);

  // Next go over all instructions after I in the basic block. If none of them
  // can decrement our ptr value, we can move the retain over the ref count
  // inst. If any of them do potentially decrement the ref count of Ptr, we can
  // not move it.
  auto SwitchIter = Switch->getIterator();
  if (auto B = valueHasARCDecrementOrCheckInInstructionRange(Ptr, RV,
                                                             SwitchIter, AA)) {
    RV->moveBefore(&**B);
    return true;
  }

  // If the retain value's argument is not the switch's argument, we can't do
  // anything with our simplistic analysis... bail...
  if (RCIA->getRCIdentityRoot(Ptr) !=
      RCIA->getRCIdentityRoot(Switch->getOperand()))
    return false;

  // If the enum has a deinit, preserve the original release.
  assert(!hasValueDeinit(Ptr) &&
         "enum with deinit is not RC-identical to its payload");

  // If S has a default case bail since the default case could represent
  // multiple cases.
  //
  // TODO: I am currently just disabling this behavior so we can get this out
  // for Seed 5. After Seed 5, we should be able to recognize if a switch_enum
  // handles all cases except for 1 and has a default case. We might be able to
  // stick code into SILBuilder that has this behavior.
  if (Switch->hasDefault())
    return false;

  // Ok, we have a ref count instruction, sink it!
  SILBuilderWithScope Builder(Switch, &*RV);
  for (unsigned i = 0, e = Switch->getNumCases(); i != e; ++i) {
    auto Case = Switch->getCase(i);
    EnumElementDecl *Enum = Case.first;
    SILBasicBlock *Succ = Case.second;
    Builder.setInsertionPoint(&*Succ->begin());
    if (Enum->hasAssociatedValues())
      createRefCountOpForPayload(Builder, &*RV, Enum, Switch->getOperand());
  }

  RV->eraseFromParent();
  ++NumSunk;
  return true;
}

/// Sink retain_value, release_value before select_enum to be retain_value,
/// release_value on the payload of the switch_enum in the destination BBs. We
/// only do this if the destination BBs have only the switch enum as its
/// predecessor.
static bool tryToSinkRefCountAcrossSelectEnum(CondBranchInst *CondBr,
                                              SILBasicBlock::iterator I,
                                              AliasAnalysis *AA,
                                              RCIdentityFunctionInfo *RCIA) {
  // If this instruction is not a retain_value, there is nothing left for us to
  // do... bail...
  if (!isa<RetainValueInst>(I))
    return false;

  // Make sure the condition comes from a select_enum
  auto *SEI = dyn_cast<SelectEnumInst>(CondBr->getCondition());
  if (!SEI)
    return false;

  // Try to find a single literal "true" case.
  // TODO: More general conditions in which we can relate the BB to a single
  // case, such as when there's a single literal "false" case.
  NullablePtr<EnumElementDecl> TrueElement = SEI->getSingleTrueElement();
  if (TrueElement.isNull())
    return false;

  // Next go over all instructions after I in the basic block. If none of them
  // can decrement our ptr value, we can move the retain over the ref count
  // inst. If any of them do potentially decrement the ref count of Ptr, we can
  // not move it.

  SILValue Ptr = I->getOperand(0);
  auto CondBrIter = CondBr->getIterator();
  if (auto B = valueHasARCDecrementOrCheckInInstructionRange(Ptr, std::next(I),
                                                             CondBrIter, AA)) {
    I->moveBefore(&**B);
    return false;
  }

  // If the retain value's argument is not the cond_br's argument, we can't do
  // anything with our simplistic analysis... bail...
  if (RCIA->getRCIdentityRoot(Ptr) !=
      RCIA->getRCIdentityRoot(SEI->getEnumOperand()))
    return false;

  // If the enum has a deinit, preserve the original release.
  assert(!hasValueDeinit(Ptr) &&
         "enum with deinit is not RC-identical to its payload");

  // Work out which enum element is the true branch, and which is false.
  // If the enum only has 2 values and its tag isn't the true branch, then we
  // know the true branch must be the other tag.
  EnumElementDecl *Elts[2] = {TrueElement.get(), nullptr};
  EnumDecl *E = SEI->getEnumOperand()->getType().getEnumOrBoundGenericEnum();
  if (!E)
    return false;

  // Look for a single other element on this enum.
  EnumElementDecl *OtherElt = nullptr;
  for (EnumElementDecl *Elt : E->getAllElements()) {
    // Skip the case where we find the select_enum element
    if (Elt == TrueElement.get())
      continue;
    // If we find another element, then we must have more than 2, so bail.
    if (OtherElt)
      return false;
    OtherElt = Elt;
  }
  // Only a single enum element?  How would this even get here?  We should
  // handle it in SILCombine.
  if (!OtherElt)
    return false;

  Elts[1] = OtherElt;

  SILBuilderWithScope Builder(SEI, &*I);

  // Ok, we have a ref count instruction, sink it!
  for (unsigned i = 0; i != 2; ++i) {
    EnumElementDecl *Enum = Elts[i];
    SILBasicBlock *Succ = i == 0 ? CondBr->getTrueBB() : CondBr->getFalseBB();
    Builder.setInsertionPoint(&*Succ->begin());
    if (Enum->hasAssociatedValues())
      createRefCountOpForPayload(Builder, &*I, Enum, SEI->getEnumOperand());
  }

  I->eraseFromParent();
  ++NumSunk;
  return true;
}

static bool tryTosinkIncrementsIntoSwitchRegions(SILBasicBlock::iterator T,
                                                 SILBasicBlock::iterator I,
                                                 bool CanSinkToSuccessors,
                                                 AliasAnalysis *AA,
                                                 RCIdentityFunctionInfo *RCIA) {
  // The following methods should only be attempted if we can sink to our
  // successor.
  if (CanSinkToSuccessors) {
    // If we have a switch, try to sink ref counts across it and then return
    // that result. We do not keep processing since the code below cannot
    // properly sink ref counts over switch_enums so we might as well exit
    // early.
    if (auto *S = dyn_cast<SwitchEnumInst>(T))
      return tryToSinkRefCountAcrossSwitch(S, I, AA, RCIA);

    // In contrast, even if we do not sink ref counts across a cond_br from a
    // select_enum, we may be able to sink anyways. So we do not return on a
    // failure case.
    if (auto *CondBr = dyn_cast<CondBranchInst>(T))
      if (tryToSinkRefCountAcrossSelectEnum(CondBr, I, AA, RCIA))
        return true;
  }

  // At this point, this is a retain on a regular SSA value, leave it to retain
  // release code motion to sink.
  return false;
}

/// Try sink a retain as far as possible.  This is either to successor BBs,
/// or as far down the current BB as possible
static bool sinkIncrementsIntoSwitchRegions(SILBasicBlock *BB,
                                            AliasAnalysis *AA,
                                            RCIdentityFunctionInfo *RCIA) {
  // Make sure that each one of our successors only has one predecessor,
  // us.
  // If that condition is not true, we can still sink to the end of this BB,
  // but not to successors.
  bool CanSinkToSuccessor = std::none_of(
      BB->succ_begin(), BB->succ_end(), [](const SILSuccessor &S) -> bool {
        SILBasicBlock *SuccBB = S.getBB();
        return !SuccBB || !SuccBB->getSinglePredecessorBlock();
      });

  SILInstruction *S = BB->getTerminator();
  auto SI = S->getIterator(), SE = BB->begin();
  if (SI == SE)
    return false;

  bool Changed = false;

  // Walk from the terminator up the BB.  Try move retains either to the next
  // BB, or the end of this BB.  Note that ordering is maintained of retains
  // within this BB.
  SI = std::prev(SI);
  while (SI != SE) {
    SILInstruction *Inst = &*SI;
    SI = std::prev(SI);

    // Try to:
    //
    //   1. If there are no decrements between our ref count inst and
    //      terminator, sink the ref count inst into either our successors.
    //   2. If there are such decrements, move the retain right before that
    //      decrement.
    Changed |= tryTosinkIncrementsIntoSwitchRegions(
        S->getIterator(), Inst->getIterator(), CanSinkToSuccessor, AA, RCIA);
  }

  // Handle the first instruction in the BB.
  Changed |= tryTosinkIncrementsIntoSwitchRegions(S->getIterator(), SI,
                                                  CanSinkToSuccessor, AA, RCIA);
  return Changed;
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

static bool processFunction(SILFunction *F, AliasAnalysis *AA,
                            PostOrderFunctionInfo *PO,
                            RCIdentityFunctionInfo *RCIA,
                            bool HoistReleases) {

  bool Changed = false;

  EnumCaseDataflowContext BBToStateMap(PO);
  for (unsigned RPOIdx = 0, RPOEnd = BBToStateMap.size(); RPOIdx < RPOEnd;
       ++RPOIdx) {

    LLVM_DEBUG(llvm::dbgs() << "Visiting BB RPO#" << RPOIdx << "\n");

    BBEnumTagDataflowState &State = BBToStateMap.getRPOState(RPOIdx);

    LLVM_DEBUG(llvm::dbgs() <<"    Predecessors (empty if no predecessors):\n");
    LLVM_DEBUG(for (SILBasicBlock *Pred
               : State.getBB()->getPredecessorBlocks()) {
      llvm::dbgs() << "        BB#" << RPOIdx << "; Ptr: " << Pred << "\n";
    });
    LLVM_DEBUG(llvm::dbgs() << "    State Addr: " << &State << "\n");

    // Merge in our predecessor states. We relook up our the states for our
    // predecessors to avoid memory invalidation issues due to copying in the
    // dense map.
    LLVM_DEBUG(llvm::dbgs() << "    Merging predecessors!\n");
    State.mergePredecessorStates();

    // If our predecessors cover any of our enum values, attempt to hoist
    // releases up the CFG onto enum payloads or sink retains out of switch
    // regions.
    LLVM_DEBUG(llvm::dbgs() << "    Attempting to move releases into "
                               "predecessors!\n");

    // Perform a relatively local forms of retain sinking and release hoisting
    // regarding switch regions and SILargument. This are not handled by retain
    // release code motion.
    if (HoistReleases) {
      Changed |= State.hoistDecrementsIntoSwitchRegions(AA);
    }

    // Sink switch related retains.
    Changed |= sinkIncrementsIntoSwitchRegions(State.getBB(), AA, RCIA);
    Changed |= State.sinkIncrementsOutOfSwitchRegions(AA, RCIA);

    // Then attempt to sink code from predecessors. This can include retains
    // which is why we always attempt to move releases up the CFG before sinking
    // code from predecessors. We will never sink the hoisted releases from
    // predecessors since the hoisted releases will be on the enum payload
    // instead of the enum itself.
    Changed |= canonicalizeRefCountInstrs(State.getBB());
    Changed |= sinkCodeFromPredecessors(BBToStateMap, State.getBB());
    Changed |= sinkArgumentsFromPredecessors(BBToStateMap, State.getBB());
    Changed |= sinkLiteralsFromPredecessors(State.getBB());
    // Try to hoist release of a SILArgument to predecessors.
    Changed |= hoistSILArgumentReleaseInst(State.getBB());

    // Then perform the dataflow.
    LLVM_DEBUG(llvm::dbgs() << "    Performing the dataflow!\n");
    Changed |= State.process();
  }

  return Changed;
}

class SILCodeMotion : public SILFunctionTransform {

  bool HoistReleases;

public:

  SILCodeMotion(bool TryReleaseHoisting) : HoistReleases(TryReleaseHoisting) {}

  /// The entry point to the transformation.
  void run() override {
    auto *F = getFunction();
    // Skip functions with ownership for now.
    if (F->hasOwnership())
      return;

    auto *AA = getAnalysis<AliasAnalysis>(F);
    auto *PO = getAnalysis<PostOrderAnalysis>()->get(F);
    auto *RCIA = getAnalysis<RCIdentityAnalysis>()->get(getFunction());

    LLVM_DEBUG(llvm::dbgs() << "***** CodeMotion on function: " << F->getName()
                            << " *****\n");

    if (processFunction(F, AA, PO, RCIA, HoistReleases))
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }

};

} // end anonymous namespace

/// Code motion that does not releases into diamonds.
SILTransform *swift::createEarlyCodeMotion() {
  return new SILCodeMotion(false);
}

/// Code motion that hoists releases into diamonds.
SILTransform *swift::createLateCodeMotion() {
  return new SILCodeMotion(true);
}
