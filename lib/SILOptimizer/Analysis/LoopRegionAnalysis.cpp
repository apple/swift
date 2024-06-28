//===--- LoopRegionAnalysis.cpp -------------------------------------------===//
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

#define DEBUG_TYPE "sil-loop-region-analysis"
#include "swift/SILOptimizer/Analysis/LoopRegionAnalysis.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Range.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/GraphWriter.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                                 LoopRegion
//===----------------------------------------------------------------------===//

LoopRegion::~LoopRegion() {
  // Blocks do not have subregion data, so everything should just clean up via
  // RAII.
  if (isBlock())
    return;

  // Otherwise, we need to cleanup subregion data.
  getSubregionData().~SubregionData();
}

LoopRegion::BlockTy *LoopRegion::getBlock() const {
  return Ptr.get<BlockTy *>();
}

LoopRegion::LoopTy *LoopRegion::getLoop() const {
  return Ptr.get<LoopTy *>();
}

LoopRegion::FunctionTy *LoopRegion::getFunction() const {
  return Ptr.get<FunctionTy *>();
}

void LoopRegion::dump(bool isVerbose) const {
  print(llvm::outs(), false, isVerbose);
  llvm::outs() << "\n";
}

void LoopRegion::dumpName() const {
  printName(llvm::outs());
  llvm::outs() << "\n";
}

void LoopRegion::printName(llvm::raw_ostream &os) const {
  if (isBlock()) {
    os << "BB" << getID();
    return;
  }
  if (isLoop()) {
    os << "Loop" << getID();
    return;
  }
  assert(isFunction());
  os << "Function" << getID();
  return;
}

void LoopRegion::print(llvm::raw_ostream &os, bool isShort,
                       bool isVerbose) const {
  os << "(region id:" << ID;
  if (isShort) {
    os << ")";
    return;
  }
  os << " kind:";
  if (isBlock()) {
    os << "bb  ";
  } else if (isLoop()) {
    os << "loop";
  } else if (isFunction()) {
    os << "func";
  } else {
    llvm_unreachable("Unknown region type");
  }

  os << " ucfh:" << (IsUnknownControlFlowEdgeHead? "true " : "false")
     << " ucft:" << (IsUnknownControlFlowEdgeTail? "true " : "false");

  if (!isVerbose) {
    return;
  }
  os << "\n";
  if (isBlock()) {
    getBlock()->dump();
  } else if (isLoop()) {
    getLoop()->dump();
  } else if (isFunction()) {
    getFunction()->dump();
  } else {
    llvm_unreachable("Unknown region type");
  }
}

llvm::raw_ostream &llvm::operator<<(llvm::raw_ostream &os, LoopRegion &LR) {
  LR.print(os);
  return os;
}

LoopRegion::SuccRange LoopRegion::getSuccs() const {
  auto Range = InnerSuccRange(Succs.begin(), Succs.end());
  return SuccRange(Range, SuccessorID::ToLiveSucc());
}

LoopRegion::LocalSuccRange LoopRegion::getLocalSuccs() const {
  auto Range = InnerSuccRange(Succs.begin(), Succs.end());
  return LocalSuccRange(Range, SuccessorID::ToLiveLocalSucc());
}

LoopRegion::NonLocalSuccRange LoopRegion::getNonLocalSuccs() const {
  auto Range = InnerSuccRange(Succs.begin(), Succs.end());
  return NonLocalSuccRange(Range, SuccessorID::ToLiveNonLocalSucc());
}

/// Replace OldSuccID by NewSuccID, just deleting OldSuccID if what NewSuccID
/// is already in the list.
void LoopRegion::replaceSucc(SuccessorID OldSucc, SuccessorID NewSucc) {
  LLVM_DEBUG(llvm::dbgs() << "                Replacing " << OldSucc << " with "
                          << NewSucc << "\n");
  Succs.replace(OldSucc, NewSucc);
}

llvm::raw_ostream &llvm::operator<<(llvm::raw_ostream &os,
                                    LoopRegion::SuccessorID &S) {
  return os << "(succ id:" << S.ID
            << " nonlocal:" << (S.IsNonLocal ? "true" : "false") << ")";
}

//===----------------------------------------------------------------------===//
//                           LoopRegionFunctionInfo
//===----------------------------------------------------------------------===//

LoopRegionFunctionInfo::LoopRegionFunctionInfo(FunctionTy *F,
                                               PostOrderFunctionInfo *PI,
                                               LoopInfoTy *LI)
    : F(F), Allocator(), BBToIDMap(), LoopToIDMap(),
#ifndef NDEBUG
      IDToRegionMap(PI->size()), AllBBRegionsCreated(false) {
#else
      IDToRegionMap(PI->size()) {
#endif
  if (F->isExternalDeclaration())
    return;
  LLVM_DEBUG(llvm::dbgs() << "**** LOOP REGION FUNCTION INFO ****\n");
  LLVM_DEBUG(llvm::dbgs() << "Analyzing function: " << F->getName() << "\n");
  initializeBlockRegions(PI, LI);
  initializeLoopRegions(LI);
  initializeFunctionRegion(LI->getTopLevelLoops());
#ifndef NDEBUG
  verify();
#endif
}

LoopRegionFunctionInfo::~LoopRegionFunctionInfo() {
  for (auto *R : IDToRegionMap) {
    R->~RegionTy();
  }
  IDToRegionMap.clear();
}

void LoopRegionFunctionInfo::verify() {
#ifndef NDEBUG
  llvm::SmallVector<unsigned, 8> UniquePredList;
  for (auto *R : IDToRegionMap) {
    // Make sure that our region has a pred list without duplicates. We do not
    // care if the predecessor list is sorted, just that it is unique.
    {
      UniquePredList.clear();
      std::copy(R->Preds.begin(), R->Preds.end(), std::back_inserter(UniquePredList));
      std::sort(UniquePredList.begin(), UniquePredList.end());
      auto End = UniquePredList.end();
      assert(End == std::unique(UniquePredList.begin(), UniquePredList.end()) &&
             "Expected UniquePredList to not have any duplicate elements");
    }

    // If this node does not have a parent, it should have no non-local
    // successors.
    if (!R->ParentID.has_value()) {
      auto NLSuccs = R->getNonLocalSuccs();
      assert(NLSuccs.begin() == NLSuccs.end() &&
             "Cannot have non local "
             "successors without a parent node");
      continue;
    }

    // If this node /does/ have a parent, make sure that all non-local
    // successors point to a successor in the parent and match whether or not it
    // is dead.
    auto *ParentRegion = getRegion(*R->ParentID);
    unsigned NumParentSuccs = ParentRegion->succ_size();
    for (LoopRegion::SuccessorID ID : R->getSuccs()) {
      // Skip local successors. We are not verifying anything here.
      if (!ID.IsNonLocal)
        continue;
      assert(ID.ID < NumParentSuccs && "Non local successor pointing off the "
                                       "parent node successor list?!");
      // Since we are not dead, make sure our parent is not dead.
      assert(ParentRegion->Succs[ID.ID].has_value() &&
             "non-local successor edge sources should have the same liveness "
             "properties as non-local successor edge targets");
      // Make sure that we can look up the local region corresponding to this
      // region's successor.
      auto *OtherR = getRegionForNonLocalSuccessor(R, ID.ID);
      (void)OtherR;

      // If R and OtherR are blocks, then OtherR should be a successor of the
      // real block.
      if (R->isBlock() && OtherR->isBlock()) {
        auto succs = R->getBlock()->getSuccessors();
        assert(std::find(succs.begin(), succs.end(), OtherR->getBlock()) != succs.end() &&
               "Expected either R was not a block or OtherR was a CFG level "
               "successor of R.");
      }
    }
  }
#endif
}

//===---
// Region Creation Functions
//

LoopRegionFunctionInfo::RegionTy *
LoopRegionFunctionInfo::getRegion(BlockTy *BB) const {
  assert(AllBBRegionsCreated && "All BB Regions have not been created yet?!");
  // Check if we have allocated a region for this BB. If so, just return it.
  auto Iter = BBToIDMap.find(BB);
  if (Iter != BBToIDMap.end()) {
    return IDToRegionMap[Iter->second];
  }
  llvm_unreachable("Unknown BB?!");
}

LoopRegionFunctionInfo::RegionTy *
LoopRegionFunctionInfo::getRegion(FunctionTy *F) const {
  if (FunctionRegionID.has_value()) {
    return IDToRegionMap[FunctionRegionID.value()];
  }

  auto &Self = const_cast<LoopRegionFunctionInfo &>(*this);
  unsigned Idx = IDToRegionMap.size();
  unsigned NumBytes = sizeof(RegionTy) + sizeof(LoopRegion::SubregionData);
  void *Memory = Self.Allocator.Allocate(NumBytes, alignof(RegionTy));
  auto *R = new (Memory) RegionTy(F, Idx);
  new ((void *)&R[1]) LoopRegion::SubregionData();
  Self.IDToRegionMap.push_back(R);
  Self.FunctionRegionID = Idx;
  return R;
}

LoopRegionFunctionInfo::RegionTy *
LoopRegionFunctionInfo::getRegion(LoopTy *Loop) const {
  auto Iter = LoopToIDMap.find(Loop);
  if (Iter != LoopToIDMap.end()) {
    return IDToRegionMap[Iter->second];
  }

  auto &Self = const_cast<LoopRegionFunctionInfo &>(*this);
  unsigned Idx = IDToRegionMap.size();
  unsigned NumBytes = sizeof(RegionTy) + sizeof(LoopRegion::SubregionData);
  void *Memory = Self.Allocator.Allocate(NumBytes, alignof(RegionTy));
  auto *R = new (Memory) RegionTy(Loop, Idx);
  new ((void *)&R[1]) LoopRegion::SubregionData();
  Self.IDToRegionMap.push_back(R);
  Self.LoopToIDMap[Loop] = Idx;
  return R;
}

LoopRegionFunctionInfo::RegionTy *
LoopRegionFunctionInfo::createRegion(BlockTy *BB, unsigned RPONum) {
  assert(!AllBBRegionsCreated && "All BB Regions have been created?!");
  // Check if we have allocated a region for this BB. If so, just return it.
  auto Iter = BBToIDMap.find(BB);
  if (Iter != BBToIDMap.end()) {
    return IDToRegionMap[Iter->second];
  }

  unsigned Idx = RPONum;
  auto *R = new (Allocator) RegionTy(BB, RPONum);
  IDToRegionMap[RPONum] = R;
  BBToIDMap[BB] = Idx;
  return R;
}

//===---
// Block Region Initialization
//

void LoopRegionFunctionInfo::initializeBlockRegionSuccessors(
    BlockTy *BB, RegionTy *BBRegion, PostOrderFunctionInfo *PI) {
  for (auto *SuccBB : BB->getSuccessorBlocks()) {
    unsigned SuccRPOIndex = *PI->getRPONumber(SuccBB);
    auto *SuccRegion = createRegion(SuccBB, SuccRPOIndex);
    BBRegion->addSucc(SuccRegion);
    SuccRegion->addPred(BBRegion);
    LLVM_DEBUG(llvm::dbgs() << "    Succ: ";
               SuccBB->printAsOperand(llvm::dbgs());
               llvm::dbgs() << " RPONum: " << SuccRPOIndex << "\n");
  }
}

void LoopRegionFunctionInfo::markIrreducibleLoopPredecessorsOfNonLoopHeader(
    BlockTy *NonHeaderBB, RegionTy *NonHeaderBBRegion,
    PostOrderFunctionInfo *PI) {
  for (BlockTy *Pred : NonHeaderBB->getPredecessorBlocks()) {
    // If we do not have an RPO number for a predecessor, it is because the
    // predecessor is unreachable and a pass did not clean up after
    // itself. Just ignore it, it will be cleaned up by simplify-cfg.
    auto PredRPONumber = PI->getRPONumber(Pred);
    if (!PredRPONumber || *PredRPONumber < NonHeaderBBRegion->getRPONumber())
      continue;

    auto *PredRegion = createRegion(Pred, *PredRPONumber);
    LLVM_DEBUG(llvm::dbgs() << "    Backedge: ";
               Pred->printAsOperand(llvm::dbgs());
               llvm::dbgs() << " " << PredRegion->getID() << "\n");

    // We mark the head/tail as unknown control flow regions since in CFGs like
    // the following:
    //
    //         /------\
    //         v      |
    // BB0 -> BB1 -> BB2 -> BB4
    //  |             |^
    //  |             v|
    //  \----------> BB3
    //
    // We need to ensure that the edge from BB0 -> BB3 does not propagate any
    // dataflow state into the irreducible loop even if the back edge we see is
    // from BB3 -> BB2.
    NonHeaderBBRegion->IsUnknownControlFlowEdgeHead = true;
    NonHeaderBBRegion->IsUnknownControlFlowEdgeTail = true;
    PredRegion->IsUnknownControlFlowEdgeHead = true;
    PredRegion->IsUnknownControlFlowEdgeTail = true;
  }
  LLVM_DEBUG(llvm::dbgs() << "\n");
}

void
LoopRegionFunctionInfo::
markMultipleLoopLatchLoopBackEdges(RegionTy *LoopHeaderRegion, LoopTy *Loop,
                                   PostOrderFunctionInfo *PI) {
  llvm::SmallVector<BlockTy *, 4> Latches;
  Loop->getLoopLatches(Latches);
  assert(Latches.size() > 1 &&
         "Assumed that this loop had multiple loop latches");

  LoopHeaderRegion->IsUnknownControlFlowEdgeHead = true;
  for (auto *LatchBB : Latches) {
    auto *LatchBBRegion = createRegion(LatchBB, *PI->getRPONumber(LatchBB));
    LatchBBRegion->IsUnknownControlFlowEdgeTail = true;
  }
}

void LoopRegionFunctionInfo::initializeBlockRegions(PostOrderFunctionInfo *PI,
                                                    LoopInfoTy *LI) {
  LLVM_DEBUG(llvm::dbgs() << "Visiting BB Regions:\n");

  // Initialize regions for each BB and associate RPO numbers with each BB.
  //
  // We use the RPO number of a BB as its Index in our data structures.
  for (auto P : llvm::enumerate(PI->getReversePostOrder())) {
    BlockTy *BB = P.value();
    unsigned RPOIndex = P.index();
    auto *BBRegion = createRegion(BB, RPOIndex);
    assert(BBRegion && "Create region fail to create a BB?");
    assert(*PI->getRPONumber(BB) == RPOIndex &&
           "Enumerated Reverse Post Order out of sync with RPO number");

    LLVM_DEBUG(llvm::dbgs() << "Visiting BB: ";
               BB->printAsOperand(llvm::dbgs());
               llvm::dbgs() << " RPO: " << RPOIndex << "\n");

    // Wire up this BB as an "initial predecessor" of all of its successors
    // and make each of its successors a successor for the region.
    initializeBlockRegionSuccessors(BB, BBRegion, PI);

    // Then try to lookup the innermost loop that BB belongs to. If we get back
    // a nullptr, then we know that the BB belongs to the function region and is
    // also not a loop header.
    auto *Loop = LI->getLoopFor(BB);
    auto *ParentRegion = Loop? getRegion(Loop) : getRegion(F);

    // Add BBRegion to the ParentRegion.
    ParentRegion->addBlockSubregion(BBRegion);

    // Determine if this basic block is part of an irreducible loop by looking
    // at all blocks that are:
    //
    // 1. Not in any loop identified by loop info.
    // 2. Or are part of an identified loop but are not a loop header.
    //
    // In such a case, check if any predecessors have a smaller PO number. If so
    // then we know that both this BB and the predecessor are boundaries of a
    // loop that is not understood by SILLoopInfo. Mark them as unknown control
    // flow boundaries. Then add the BB as a subregion to its parent region.
    LLVM_DEBUG(llvm::dbgs() << "Checking Preds for Back Edges\n");
    if (!Loop || !LI->isLoopHeader(BB)) {
      markIrreducibleLoopPredecessorsOfNonLoopHeader(BB, BBRegion, PI);
      continue;
    }
    assert(Loop && "Should have a non-null Loop at this point");
    assert(ParentRegion && "Expected a non-null LoopRegion");

    // Now we know that the block is in a loop and is a loop header. Check if
    // this loop has multiple latches. If so, mark the latch head/tail blocks as
    // head/tail unknown cfg boundaries.
    //
    // We rely on Loop canonicalization to separate such loops into separate
    // nested loops.
    ParentRegion->getSubregionData().RPONumOfHeaderBlock = RPOIndex;

    // If we have one loop latch, continue.
    if (Loop->getLoopLatch()) {
      LLVM_DEBUG(llvm::dbgs() << "\n");
      continue;
    }

    // Otherwise, mark each of the loop latches as irreducible control flow edge
    // tails so we are conservative around them.
    markMultipleLoopLatchLoopBackEdges(BBRegion, Loop, PI);
    LLVM_DEBUG(llvm::dbgs() << "\n");
  }

#ifndef NDEBUG
  AllBBRegionsCreated = true;
#endif
}

//===---
// Loop and Function Region Initialization
//

void LoopRegionFunctionInfo::initializeLoopRegions(LoopInfoTy *LI) {
  // Now visit the loop nest in a DFS.
  llvm::SmallVector<LoopTy *, 16> LoopWorklist(LI->begin(), LI->end());
  llvm::SmallPtrSet<LoopTy *, 16> VisitedLoops;

  while (LoopWorklist.size()) {
    LoopTy *L = LoopWorklist.back();
    auto *LRegion = getRegion(L);

    // If we have not visited this loop yet and it has subloops, add its
    // subloops to the worklist and continue.
    auto SubLoops = L->getSubLoopRange();
    if (VisitedLoops.insert(L).second && SubLoops.begin() != SubLoops.end()) {
      for (auto *SubLoop : SubLoops) {
        LoopWorklist.push_back(SubLoop);
      }
      continue;
    }
    LoopWorklist.pop_back();

    // Otherwise, process this loop. We have already visited all of its
    // potential children loops.
    initializeLoopFunctionRegion(LRegion, SubLoops);
  }
}

/// For each predecessor of the header of the loop:
///
/// 1. If the predecessor is not in the loop, make the predecessor a predecessor
/// of the loop instead of the header.
///
/// 2. If the predecessor *is* in the loop it must be the tail of a backedge. We
/// remove these back edges and instead represent them as unknown control flow
/// edges. This is because we rely on loop canonicalization to canonicalize
/// multiple backedge loops into separate loops.
LoopRegionFunctionInfo::RegionTy *
LoopRegionFunctionInfo::
rewriteLoopHeaderPredecessors(LoopTy *SubLoop, RegionTy *SubLoopRegion) {
  auto *SubLoopHeaderRegion = getRegion(SubLoop->getHeader());
  assert(SubLoopHeaderRegion->isBlock() && "A header must always be a block");

  LLVM_DEBUG(llvm::dbgs()
             << "        Header: " << SubLoopHeaderRegion->getID() << "\n"
             << "        Rewiring Header Predecessors to be Loop Preds.\n");

  if (SubLoopHeaderRegion->IsUnknownControlFlowEdgeHead)
    SubLoopRegion->IsUnknownControlFlowEdgeHead = true;

  for (unsigned PredID : SubLoopHeaderRegion->Preds) {
    auto *PredRegion = getRegion(PredID);

    LLVM_DEBUG(llvm::dbgs() << "            " << PredRegion->getID() << "\n");
    if (!SubLoopRegion->containsSubregion(PredRegion)) {
      LLVM_DEBUG(llvm::dbgs() << "            Not in loop... Replacing...\n");
      // This is always a local edge since non-local edges can only have loops
      // as heads. Since the head of our edge is SubLoopHeaderRegion, this must
      // be local.
      PredRegion->replaceSucc(LoopRegion::SuccessorID(SubLoopHeaderRegion->ID,
                                                      /*IsNonLocal*/ false),
                              LoopRegion::SuccessorID(SubLoopRegion->ID,
                                                      /*IsNonLocal*/ false));
      propagateLivenessDownNonLocalSuccessorEdges(PredRegion);
      SubLoopRegion->addPred(PredRegion);
      continue;
    }

    LLVM_DEBUG(llvm::dbgs() << "            Is in loop... Erasing...\n");
    // Ok, we have a predecessor inside the loop. This must be a backedge.
    //
    // We are abusing the fact that a block can only be a local successor.
    PredRegion->removeLocalSucc(SubLoopHeaderRegion->getID());
    propagateLivenessDownNonLocalSuccessorEdges(PredRegion);
    SubLoopRegion->getSubregionData().addBackedgeSubregion(PredRegion->getID());
  }
  SubLoopHeaderRegion->Preds.clear();

  return SubLoopHeaderRegion;
}

static void getExitingRegions(LoopRegionFunctionInfo *LRFI, SILLoop *Loop,
                              LoopRegion *LRegion,
                              llvm::SmallVectorImpl<unsigned> &ExitingRegions) {
  llvm::SmallVector<SILBasicBlock *, 8> ExitingBlocks;
  Loop->getExitingBlocks(ExitingBlocks);

  // Determine the outer most region that contains the exiting block that is not
  // this subloop's region. That is the *true* exiting region.
  for (auto *BB : ExitingBlocks) {
    auto *Region = LRFI->getRegion(BB);
    unsigned RegionParentID = *Region->getParentID();
    while (RegionParentID != LRegion->getID()) {
      Region = LRFI->getRegion(RegionParentID);
      RegionParentID = *Region->getParentID();
    }
    ExitingRegions.push_back(Region->getID());
  }

  // We can have a loop subregion that has multiple exiting edges from the
  // current loop. We do not want to visit that loop subregion multiple
  // times. So we unique the exiting region list.
  //
  // In order to make sure we have a deterministic ordering when we visiting
  // exiting subregions, we need to sort our exiting regions by ID, not pointer
  // value.
  sortUnique(ExitingRegions);
}

/// For each exiting block:
///
/// 1. Make any successors outside of the loop successors of the loop instead
/// of the exiting block.
///
/// 2. Any successors that are inside the loop but are not back edges are
/// left alone.
///
/// 3. Any successors that are inside the loop but are the head of a backedge
/// have the edge removed. This is computed by the successor having a greater
/// post order number than the exit.
///
/// *NOTE* We have to be careful here since exiting blocks may include BBs
/// that have been subsumed into a subloop already. Also to determine back
/// edges, we need to compare post order numbers potentially of loop headers,
/// not the loops themselves.
void
LoopRegionFunctionInfo::
rewriteLoopExitingBlockSuccessors(LoopTy *Loop, RegionTy *LRegion) {
  // Begin by using loop info and loop region info to find all of the exiting
  // regions.
  //
  // We do this by looking up the exiting blocks and finding the outermost
  // region which the block is a subregion of. Since we initialize our data
  // structure by processing the loop nest bottom up, this should always give us
  // the correct region for the level of the loop we are processing.
  auto &ExitingSubregions = LRegion->getSubregionData().ExitingSubregions;
  getExitingRegions(this, Loop, LRegion, ExitingSubregions);

  // Then for each exiting region ER of the Loop L...
  LLVM_DEBUG(llvm::dbgs() << "    Visiting Exit Blocks...\n");
  for (unsigned ExitingSubregionID : ExitingSubregions) {
    auto *ExitingSubregion = getRegion(ExitingSubregionID);
    LLVM_DEBUG(llvm::dbgs() << "        Exiting Region: "
                            << ExitingSubregion->getID() << "\n");
    bool HasBackedge = false;

    // For each successor region S of ER...
    for (auto SuccID : ExitingSubregion->getSuccs()) {
      LLVM_DEBUG(llvm::dbgs() << "            Succ: " << SuccID.ID
                              << ". IsNonLocal: "
                              << (SuccID.IsNonLocal ? "true" : "false") <<"\n");

      // If S is not contained in L, then:
      //
      // 1. The successor/predecessor edge in between S and ER with a new
      // successor/predecessor edge in between S and L.
      // 2. ER is given a non-local successor edge that points at the successor
      // index in L that points at S. This will enable us to recover the
      // original edge if we need to.
      //
      // Then we continue.
      auto *SuccRegion = getRegion(SuccID.ID);
      if (!LRegion->containsSubregion(SuccRegion)) {
        LLVM_DEBUG(llvm::dbgs() << "            Is not a subregion, "
                   "replacing.\n");
        SuccRegion->replacePred(ExitingSubregion->ID, LRegion->ID);
        if (ExitingSubregion->IsUnknownControlFlowEdgeTail)
          LRegion->IsUnknownControlFlowEdgeTail = true;
        // If the successor region is already in this LRegion this returns that
        // regions index. Otherwise it returns a new index.
        unsigned Index = LRegion->addSucc(SuccRegion);
        ExitingSubregion->replaceSucc(SuccID,
                                      LoopRegion::SuccessorID(Index, true));
        propagateLivenessDownNonLocalSuccessorEdges(ExitingSubregion);
        continue;
      }

      // Otherwise, we know S is in L. If the RPO number of S is less than the
      // RPO number of ER, then we know that the edge in between them is not a
      // backedge and thus we do not want to clip the edge.
      if (SuccRegion->getRPONumber() > ExitingSubregion->getRPONumber()) {
        LLVM_DEBUG(llvm::dbgs() << "            Is a subregion, but not a "
                   "backedge, not removing.\n");
        continue;
      }

      // If the edge from ER to S is a back edge, we want to clip it and add
      // exiting subregion to
      LLVM_DEBUG(llvm::dbgs() << "            Is a subregion and a backedge, "
                 "removing.\n");
      HasBackedge = true;
      auto Iter =
          std::remove(SuccRegion->Preds.begin(), SuccRegion->Preds.end(),
                      ExitingSubregion->getID());
      SuccRegion->Preds.erase(Iter);
    }

    // If we found a backedge, add ER's ID to LRegion's Backedge list.
    if (!HasBackedge) {
      continue;
    }

    LRegion->getSubregionData().addBackedgeSubregion(ExitingSubregion->getID());
  }
}

/// The high level algorithm is that via the loop info we already know for
/// each subloop:
///
/// 1. predecessors.
/// 2. header.
/// 3. exiting blocks.
/// 4. successor blocks.
///
/// This means that we can simply fix up those BB regions.
///
/// *NOTE* We do not touch the successors/predecessors of the current loop. We
/// leave those connections in place for our parent loop to fix up. In the case
/// we are the function level loop, these will of course be empty anyways.
void
LoopRegionFunctionInfo::
initializeLoopFunctionRegion(RegionTy *ParentRegion,
                             iterator_range<LoopInfoTy::iterator> SubLoops) {

  LLVM_DEBUG(llvm::dbgs() << "Initializing Loop Region "
                          << ParentRegion->getID() << "\n");
  // For each subloop...
  for (auto *SubLoop : SubLoops) {
    // Grab the region associated with the subloop...
    auto *SubLoopRegion = getRegion(SubLoop);

    LLVM_DEBUG(llvm::dbgs() << "    Visiting Subloop: "
                            << SubLoopRegion->getID() << "\n");

    // First rewrite predecessors of the loop header to point at the loop.
    auto *SubLoopHeaderRegion = rewriteLoopHeaderPredecessors(SubLoop,
                                                              SubLoopRegion);

    // Then rewrite successors of all exits.
    rewriteLoopExitingBlockSuccessors(SubLoop, SubLoopRegion);

    // Add the subloop region to the subregion data.
    ParentRegion->addLoopSubregion(SubLoopRegion, SubLoopHeaderRegion);
  }

  // Now that we have finished processing this loop, sort its subregions so that
  // they are now in RPO order. This works because each BB's ID is its RPO
  // number and we represent loops by the RPO number of their preheader (with a
  // flag in the first bit to say to look in the subloop array for the *real* ID
  // of the loop).
  //
  // TODO: Is this necessary? We visit BBs in RPO order. This means that we
  // should always add BBs in RPO order to subregion lists, no? For now I am
  // going to sort just to be careful while bringing this up.
  ParentRegion->getSubregionData().sortSubregions();
}

void LoopRegionFunctionInfo::
initializeFunctionRegion(iterator_range<LoopInfoTy::iterator> SubLoops) {
  // We have already processed all of our subloops, so everything there has
  // already been properly setup.
  initializeLoopFunctionRegion(getRegion(F), SubLoops);
}

/// Recursively visit all the descendants of Parent. If there is a non-local
/// successor edge path that points to a dead edge in Parent, mark the
/// descendant non-local successor edge as dead.
void LoopRegionFunctionInfo::
propagateLivenessDownNonLocalSuccessorEdges(LoopRegion *Parent) {
  llvm::SmallVector<LoopRegion *, 4> Worklist;
  Worklist.push_back(Parent);

  while (Worklist.size()) {
    LoopRegion *R = Worklist.pop_back_val();

    for (unsigned SubregionID : R->getSubregions()) {
      LoopRegion *Subregion = getRegion(SubregionID);
      bool ShouldVisit = false;

      // Make sure we can identify when the subregion has at least one dead
      // non-local edge and no remaining live edges. In such a case, we need to
      // remove the subregion from the exiting subregion array of R after the
      // loop.
      bool HasDeadNonLocalEdge = false;
      bool HasNoLiveLocalEdges = true;
      for (auto &SuccID : Subregion->Succs) {
        // If the successor is already dead, skip it. We should have visited all
        // its children when we marked it as dead.
        if (!SuccID)
          continue;

        // We do not care about local IDs, we only process non-local IDs.
        if (!SuccID->IsNonLocal)
          continue;

        // If the non-local successor edge points to a parent successor that is
        // not dead continue.
        if (R->Succs[SuccID->ID].has_value()) {
          HasNoLiveLocalEdges = false;
          continue;
        }

        // Ok, we found a target! Mark it as dead and make sure that we visit
        // the subregion's children if it is not a block.
        HasDeadNonLocalEdge = true;
        ShouldVisit = true;

        // This is safe to do since when erasing in a BlotSetVector, we do not
        // invalidate the iterators.
        Subregion->Succs.erase(*SuccID);
      }

      // Remove Subregion from R's exiting subregion array if Subregion no
      // longer has /any/ non-local successors.
      if (HasDeadNonLocalEdge && HasNoLiveLocalEdges) {
        auto &ExitingSubregions = R->getSubregionData().ExitingSubregions;
        auto Iter =
          std::remove(ExitingSubregions.begin(), ExitingSubregions.end(),
                      Subregion->getID());
        ExitingSubregions.erase(Iter);
      }

      if (ShouldVisit)
        Worklist.push_back(Subregion);
    }
  }
}

LoopRegionFunctionInfo::RegionTy *
LoopRegionFunctionInfo::
getRegionForNonLocalSuccessor(const LoopRegion *Child, unsigned SuccID) const {
  const LoopRegion *Iter = Child;
  LoopRegion::SuccessorID Succ = {0, 0};

  do {
    Iter = getRegion(*Iter->getParentID());
    Succ = Iter->Succs[SuccID].value();
    SuccID = Succ.ID;
  } while (Succ.IsNonLocal);

  return getRegion(SuccID);
}

void LoopRegionFunctionInfo::dump() const {
  print(llvm::outs());
  llvm::outs() << "\n";
}

void LoopRegionFunctionInfo::print(raw_ostream &os) const {
  // Print out the information for each loop.
  std::vector<std::pair<LoopRegion *, LoopRegion *>> RegionWorklist;

  // Initialize the worklist with the function level region.
  RegionWorklist.push_back({nullptr, getRegion(F)});

  // Vectors that we use to sort our local successor/predecessor indices to make
  // our output deterministic.
  llvm::SmallVector<unsigned, 4> SortedPreds;
  llvm::SmallVector<unsigned, 4> SortedSuccs;

  // Then go to town...
  while (RegionWorklist.size()) {
    LoopRegion *Parent, *R;
    std::tie(Parent, R) = RegionWorklist.back();
    RegionWorklist.pop_back();

    os << *R;

    // If we have a parent, print out its id. This is not strictly necessary,
    // but it makes it easier to read the output from a dump.
    if (Parent)
      os << " parent:" << Parent->getID();
    os << "\n";

    // Print predecessors.
    SortedPreds.clear();
    for (unsigned Pred : R->Preds)
      SortedPreds.push_back(Pred);
    std::sort(SortedPreds.begin(), SortedPreds.end());
    os << "    (preds";
    for (unsigned SID : SortedPreds) {
      os << "\n        ";
      LoopRegion *PredRegion = getRegion(SID);
      PredRegion->print(os, true);
    }
    os << ")\n";

    os << "    (succs";

    // To make our output deterministic, we sort local successor indices.
    SortedSuccs.clear();
    auto LSuccRange = R->getLocalSuccs();
    std::copy(LSuccRange.begin(), LSuccRange.end(),
              std::back_inserter(SortedSuccs));
    std::sort(SortedSuccs.begin(), SortedSuccs.end());
    for (unsigned SID : SortedSuccs) {
      os << "\n        ";
      LoopRegion *SuccRegion = getRegion(SID);
      SuccRegion->print(os, true);
    }
    os << ")\n";

    os << "    (subregs";
    if (!R->isBlock()) {
      // Go through the subregions.
      for (unsigned SID : R->getSubregions()) {
        os << "\n        ";
        LoopRegion *Subregion = getRegion(SID);
        Subregion->print(os, true);
        RegionWorklist.push_back({R, Subregion});
      }
    }
    os << ")\n";

    SortedSuccs.clear();
    auto NonLSuccs = R->getNonLocalSuccs();
    std::copy(NonLSuccs.begin(), NonLSuccs.end(),
              std::back_inserter(SortedSuccs));
    std::sort(SortedSuccs.begin(), SortedSuccs.end());
    os << "    (non-local-succs";
    for (unsigned I : SortedSuccs) {
      os << "\n        (parentindex:" << I << ")";
    }
    os << ")\n";

    os << "    (exiting-subregs";
    if (!R->isBlock()) {
      llvm::SmallVector<unsigned, 4> ExitingSubregions;
      auto ExitingSubRegs = R->getExitingSubregions();
      std::copy(ExitingSubRegs.begin(), ExitingSubRegs.end(),
                std::back_inserter(ExitingSubregions));
      std::sort(ExitingSubregions.begin(), ExitingSubregions.end());
      for (unsigned SubregionID : ExitingSubregions) {
        os << "\n        ";
        LoopRegion *Subregion = getRegion(SubregionID);
        Subregion->print(os, true);
      }
    }
    os << ")\n";

    os << "    (backedge-regs";
    if (!R->isBlock()) {
      auto BackedgeIDs = R->getBackedgeRegions();
      if (!BackedgeIDs.empty()) {
        for (unsigned BackedgeID : BackedgeIDs) {
          os << "\n        ";
          LoopRegion *BackedgeRegion = getRegion(BackedgeID);
          BackedgeRegion->print(os, true);
        }
      }
    }
    os << "))\n";
  }
}

//===----------------------------------------------------------------------===//
//                              Graphing Support
//===----------------------------------------------------------------------===//

#ifndef NDEBUG

/// A lot of the code below is unfortunate and due to the inflexibility of
/// GraphUtils. But you gotta do what you gotta do.
namespace {

struct LoopRegionWrapper;

struct LoopRegionFunctionInfoGrapherWrapper {
  LoopRegionFunctionInfo *FuncInfo;
  std::vector<LoopRegionWrapper> Data;
};

struct alledge_iterator;

struct LoopRegionWrapper {
  LoopRegionFunctionInfoGrapherWrapper &FuncInfo;
  LoopRegion *Region;

  LoopRegionWrapper *getParent() const {
    unsigned ParentIndex = *Region->getParentID();
    return &FuncInfo.Data[ParentIndex];
  }

  alledge_iterator begin();
  alledge_iterator end();
};

/// An iterator on Regions that first iterates over subregions and then over
/// successors.
struct alledge_iterator {
  using iterator_category = std::forward_iterator_tag;
  using value_type = LoopRegionWrapper;
  using difference_type = std::ptrdiff_t;
  using pointer = value_type*;
  using reference = value_type&;    

  LoopRegionWrapper *Wrapper;
  LoopRegion::subregion_iterator SubregionIter;
  LoopRegion::backedge_iterator BackedgeIter;

  using SuccIterTy =
    OptionalTransformIterator<LoopRegion::const_succ_iterator,
                              LoopRegion::SuccessorID::ToLiveSucc>;
  SuccIterTy SuccIter;

  alledge_iterator(LoopRegionWrapper *w,
                   swift::LoopRegion::subregion_iterator subregioniter,
                   LoopRegion::const_succ_iterator succiter,
                   LoopRegion::backedge_iterator backedgeiter)
      : Wrapper(w), SubregionIter(subregioniter),
        BackedgeIter(backedgeiter),
        SuccIter(succiter, w->Region->succ_end(),
                 LoopRegion::SuccessorID::ToLiveSucc()) {}

  // This is not efficient, but this is graphing code...
  SuccIterTy getSuccEnd() const {
    return SuccIterTy(Wrapper->Region->succ_end(), Wrapper->Region->succ_end(),
                      LoopRegion::SuccessorID::ToLiveSucc());
  }

  bool isSubregion() const {
    return SubregionIter != Wrapper->Region->subregion_end();
  }

  bool isNonLocalEdge() const {
    // If we are not a subregion...
    if (isSubregion())
      return false;

    // Then if succ iterator is not succ end, we are a non local edge if that
    // value is Non Local.
    return getSuccEnd() != SuccIter && (*SuccIter).IsNonLocal;
  }

  bool isBackEdge() const {
    if (isSubregion())
      return false;
    // If our successor iterator has reached the end of the successor list.
    return getSuccEnd() == SuccIter;
  }

  LoopRegionWrapper *operator*() const {
    // If we have a subregion... return the wrapper for it.
    if (isSubregion()) {
      return &Wrapper->FuncInfo.Data[*SubregionIter];
    }

    // If we have a backedge... return the wrapper for that.
    if (isBackEdge()) {
      return &Wrapper->FuncInfo.Data[*BackedgeIter];
    }

    // If we have a non-local id, just return the parent region's data.
    if ((*SuccIter).IsNonLocal)
      return &Wrapper->FuncInfo.Data[*(Wrapper->Region->getParentID())];
    // Otherwise return the data associated with this successor.
    return &Wrapper->FuncInfo.Data[(*SuccIter).ID];
  }

  alledge_iterator &operator++() {
    // If we are still a subregion, increment the SubregionIter and return.
    if (isSubregion()) {
      ++SubregionIter;
      return *this;
    }

    // Make sure that we skip past any dead successors. If after skipping dead
    // successors, if our SuccIter is not end, then return. We have a true
    // successor.
    if (SuccIter != getSuccEnd()) {
      ++SuccIter;
      return *this;
    }

    // Ok, now we know that we have a back edge region. Increment the backedge
    // region.
    ++BackedgeIter;

    return *this;
  }

  alledge_iterator operator++(int) {
    alledge_iterator copy = *this;
    ++copy;
    return copy;
  }

  bool operator==(alledge_iterator rhs) const {
    if (Wrapper->Region != rhs.Wrapper->Region)
      return false;
    if (SubregionIter != rhs.SubregionIter)
      return false;
    if (SuccIter != rhs.SuccIter)
      return false;
    if (BackedgeIter.hasValue() != rhs.BackedgeIter.hasValue())
      return false;
    return BackedgeIter == rhs.BackedgeIter;
  }

  bool operator!=(alledge_iterator rhs) const { return !(*this == rhs); }
};

} // end anonymous namespace

alledge_iterator LoopRegionWrapper::begin() {
  return alledge_iterator(this, Region->subregion_begin(), Region->succ_begin(),
                          Region->backedge_begin());
}

alledge_iterator LoopRegionWrapper::end() {
  return alledge_iterator(this, Region->subregion_end(), Region->succ_end(),
                          Region->backedge_end());
}

namespace llvm {
template <> struct GraphTraits<LoopRegionWrapper> {
  using ChildIteratorType = alledge_iterator;
  typedef LoopRegionWrapper *NodeRef;

  static NodeRef getEntryNode(NodeRef BB) { return BB; }

  static ChildIteratorType child_begin(NodeRef N) { return N->begin(); }
  static ChildIteratorType child_end(NodeRef N) { return N->end(); }
};

template <>
struct GraphTraits<LoopRegionFunctionInfoGrapherWrapper *>
    : public GraphTraits<LoopRegionWrapper> {
  using GraphType = LoopRegionFunctionInfoGrapherWrapper;
  typedef LoopRegionWrapper *NodeRef;

  static NodeRef getEntryNode(GraphType *F) { return &F->Data[0]; }

  using nodes_iterator =
          pointer_iterator<std::vector<LoopRegionWrapper>::iterator>;

  static nodes_iterator nodes_begin(GraphType *F) {
    return nodes_iterator(F->Data.begin());
  }
  static nodes_iterator nodes_end(GraphType *F) {
    return nodes_iterator(F->Data.end());
  }
  static unsigned size(GraphType *F) { return F->Data.size(); }
};

} // end namespace llvm

static llvm::cl::opt<unsigned>
    MaxColumns("view-loop-regions-max-columns", llvm::cl::init(80),
               llvm::cl::desc("Maximum width of a printed node"));

namespace {
enum class LongLineBehavior { None, Truncate, Wrap };
} // end anonymous namespace
static llvm::cl::opt<LongLineBehavior> LLBehavior(
    "view-loop-regions-long-line-behavior",
    llvm::cl::init(LongLineBehavior::Truncate),
    llvm::cl::desc("Behavior when line width is greater than the "
                   "value provided my -view-loop-regions-max-columns "
                   "option"),
    llvm::cl::values(
        clEnumValN(LongLineBehavior::None, "none", "Print everything"),
        clEnumValN(LongLineBehavior::Truncate, "truncate",
                   "Truncate long lines"),
        clEnumValN(LongLineBehavior::Wrap, "wrap", "Wrap long lines")));

static llvm::cl::opt<bool> RemoveUseListComments(
    "view-loop-regions-remove-use-list-comments", llvm::cl::init(false),
    llvm::cl::desc("Should use list comments be removed"));

namespace llvm {
template <>
struct DOTGraphTraits<LoopRegionFunctionInfoGrapherWrapper *>
    : public DefaultDOTGraphTraits {

  DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(isSimple) {}

  static std::string
  getGraphName(const LoopRegionFunctionInfoGrapherWrapper *F) {
    return "Loop Regions for '" + F->FuncInfo->getFunction()->getName().str() +
           "' function";
  }

  static std::string
  getSimpleNodeLabel(LoopRegionWrapper *Node,
                     const LoopRegionFunctionInfoGrapherWrapper *F) {
    std::string OutStr;
    raw_string_ostream OSS(OutStr);
    Node->Region->printName(OSS);
    return OSS.str();
  }

  static std::string
  getCompleteNodeLabel(LoopRegionWrapper *Node,
                       const LoopRegionFunctionInfoGrapherWrapper *F) {
    std::string Tmp0;
    raw_string_ostream OSS(Tmp0);
    Node->Region->printName(OSS);
    if (Node->Region->isUnknownControlFlowEdgeHead())
      OSS << " Unknown CF Head";
    if (Node->Region->isUnknownControlFlowEdgeTail())
      OSS << " Unknown CF Tail";
    if (!Node->Region->isBlock())
      return OSS.str();
    OSS << "\n";
    std::string Tmp1;
    raw_string_ostream OSS2(Tmp1);
    OSS2 << *Node->Region->getBlock();
    std::string OutStr = OSS2.str();
    if (OutStr[0] == '\n')
      OutStr.erase(OutStr.begin());

    // Process string output to make it nicer...
    unsigned ColNum = 0;
    unsigned LastSpace = 0;
    for (unsigned i = 0; i != OutStr.length(); ++i) {
      if (OutStr[i] == '\n') { // Left justify
        OutStr[i] = '\\';
        OutStr.insert(OutStr.begin() + i + 1, 'l');
        ColNum = 0;
        LastSpace = 0;
      } else if (RemoveUseListComments && OutStr[i] == '/' &&
                 i != (OutStr.size() - 1) && OutStr[i + 1] == '/') {
        unsigned Idx = OutStr.find('\n', i + 1); // Find end of line
        OutStr.erase(OutStr.begin() + i, OutStr.begin() + Idx);
        --i;

      } else if (ColNum == MaxColumns) { // Handle long lines.

        if (LLBehavior == LongLineBehavior::Wrap) {
          if (!LastSpace)
            LastSpace = i;
          OutStr.insert(LastSpace, "\\l...");
          ColNum = i - LastSpace;
          LastSpace = 0;
          i += 3; // The loop will advance 'i' again.
        } else if (LLBehavior == LongLineBehavior::Truncate) {
          unsigned Idx = OutStr.find('\n', i + 1); // Find end of line
          OutStr.erase(OutStr.begin() + i, OutStr.begin() + Idx);
          --i;
        }

        // Else keep trying to find a space.
      } else
        ++ColNum;
      if (OutStr[i] == ' ')
        LastSpace = i;
    }

    return OSS.str() + OutStr;
  }

  std::string getNodeLabel(LoopRegionWrapper *Node,
                           const LoopRegionFunctionInfoGrapherWrapper *Graph) {
    if (isSimple())
      return getSimpleNodeLabel(Node, Graph);
    else
      return getCompleteNodeLabel(Node, Graph);
  }

  static bool hasEdgeDestLabels() { return true; }

  static unsigned numEdgeDestLabels(LoopRegionWrapper *Node) {
    return std::distance(Node->begin(), Node->end());
  }

  static std::string getEdgeDestLabel(LoopRegionWrapper *Node, unsigned i) {
    alledge_iterator Iter = Node->begin();
    std::advance(Iter, i);
    return getEdgeSourceLabel(Node, Iter);
  }

  static std::string getEdgeSourceLabel(LoopRegionWrapper *Node,
                                        alledge_iterator I) {
    if (I.isNonLocalEdge())
      return "NLSuc";
    if (I.isSubregion())
      return "Sub";
    if (I.isBackEdge())
      return "B";
    return "LSuc";
  }

  /// If you want to override the dot attributes printed for a particular
  /// edge, override this method.
  static std::string
  getEdgeAttributes(const LoopRegionWrapper *Node, alledge_iterator EI,
                    const LoopRegionFunctionInfoGrapherWrapper *Graph) {
    if (EI.isSubregion())
      return "color=red";
    if (EI.isNonLocalEdge())
      return "color=blue";
    if (EI.isBackEdge())
      return "color=darkgreen";
    return "";
  }

  static bool edgeTargetsEdgeSource(const LoopRegionWrapper *Node,
                                    alledge_iterator I) {
    return I.isNonLocalEdge();
  }

  /// If edgeTargetsEdgeSource returns true, this method is called to determine
  /// which outgoing edge of Node is the target of this edge.
  ///
  /// Currently this can only happen given a non-local edge in which case we
  /// want the non-local edge to point at its parent's successor edge.
  static alledge_iterator getEdgeTarget(LoopRegionWrapper *Node,
                                        alledge_iterator I) {
    assert(I.isNonLocalEdge() && "This should only be called given a non "
                                 "local edge");
    auto *ParentRegion = Node->getParent();
    auto SuccIter = ParentRegion->Region->succ_begin();
    std::advance(SuccIter, (*I.SuccIter).ID);
    return alledge_iterator(ParentRegion, ParentRegion->Region->subregion_end(),
                            SuccIter, ParentRegion->Region->backedge_begin());
  }
};
} // namespace llvm

static llvm::cl::opt<std::string> TargetFunction(
    "view-loop-regions-only-for-function", llvm::cl::init(""),
    llvm::cl::desc("Only print out the loop regions for this function"));
#endif

void LoopRegionFunctionInfo::viewLoopRegions() const {
// This is a no-op when asserts are disabled.
#ifndef NDEBUG
  // If we have a target function, only print that function out.
  if (!TargetFunction.empty() && !(F->getName().str() == TargetFunction))
    return;
  LoopRegionFunctionInfoGrapherWrapper Wrapper;
  Wrapper.FuncInfo = const_cast<LoopRegionFunctionInfo *>(this);
  for (auto *R : IDToRegionMap) {
    Wrapper.Data.push_back({Wrapper, R});
  }
  llvm::ViewGraph(&Wrapper, "loop region function info" + F->getName().str());
#endif
}

//===----------------------------------------------------------------------===//
//                             LoopRegionAnalysis
//===----------------------------------------------------------------------===//

void LoopRegionAnalysis::initialize(SILPassManager *PM) {
  SLA = PM->getAnalysis<SILLoopAnalysis>();
  POA = PM->getAnalysis<PostOrderAnalysis>();
}

//===----------------------------------------------------------------------===//
//                              Main Entry Point
//===----------------------------------------------------------------------===//

SILAnalysis *swift::createLoopRegionAnalysis(SILModule *M) {
  return new LoopRegionAnalysis(M);
}
