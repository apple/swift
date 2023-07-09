//===--- LICM.cpp - Loop invariant code motion ----------------------------===//
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

#define DEBUG_TYPE "sil-licm"

#include "swift/SIL/Dominance.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SILOptimizer/Analysis/AccessStorageAnalysis.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/Analysis/ArraySemantic.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/LoopAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/SILSSAUpdater.h"

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/Debug.h"

#include "llvm/Support/CommandLine.h"

using namespace swift;

namespace {

/// Instructions which can be hoisted:
/// loads, function calls without side effects and (some) exclusivity checks
using InstSet = llvm::SmallPtrSet<SILInstruction *, 8>;

using InstVector = llvm::SmallVector<SILInstruction *, 8>;

/// Returns true if the \p SideEffectInsts set contains any memory writes which
/// may alias with the memory addressed by \a LI.
template <SILInstructionKind K, typename T>
static bool mayWriteTo(AliasAnalysis *AA, InstSet &SideEffectInsts,
                       UnaryInstructionBase<K, T> *Inst) {
  for (auto *I : SideEffectInsts)
    if (AA->mayWriteToMemory(I, Inst->getOperand())) {
      LLVM_DEBUG(llvm::dbgs() << "  mayWriteTo\n" << *I << " to "
                              << *Inst << "\n");
      return true;
    }
  return false;
}

/// Returns a non-null StoreInst if \p I is a store to \p accessPath.
static StoreInst *isStoreToAccess(SILInstruction *I, AccessPath accessPath) {
  auto *SI = dyn_cast<StoreInst>(I);
  if (!SI)
    return nullptr;

  // TODO: handle StoreOwnershipQualifier::Init
  if (SI->getOwnershipQualifier() == StoreOwnershipQualifier::Init)
    return nullptr;

  auto storeAccessPath = AccessPath::compute(SI->getDest());
  if (accessPath != storeAccessPath)
    return nullptr;

  return SI;
}

struct LoadWithAccess {
  LoadInst *li = nullptr;
  AccessPath accessPath;

  operator bool() { return li != nullptr; }
};

static LoadWithAccess doesLoadOverlapAccess(SILInstruction *I,
                                            AccessPath accessPath) {
  auto *LI = dyn_cast_or_null<LoadInst>(I);
  if (!LI)
    return LoadWithAccess();

  // TODO: handle LoadOwnershipQualifier::Take
  if (LI->getOwnershipQualifier() == LoadOwnershipQualifier::Take)
    return LoadWithAccess();

  AccessPath loadAccessPath = AccessPath::compute(LI->getOperand());
  if (!loadAccessPath.isValid())
    return LoadWithAccess();

  // Don't use AccessPath::mayOverlap. We only want definite overlap.
  if (loadAccessPath.contains(accessPath)
      || accessPath.contains(loadAccessPath)) {
    return {LI, loadAccessPath};
  }
  return LoadWithAccess();
}

/// Returns a valid LoadWithAccess if \p I is a load from \p accessPath or a
/// projected address from \p accessPath.
static LoadWithAccess isLoadWithinAccess(SILInstruction *I,
                                         AccessPath accessPath) {
  auto loadWithAccess = doesLoadOverlapAccess(I, accessPath);
  if (!loadWithAccess)
    return loadWithAccess;

  // Make sure that any additional path components beyond the store's access
  // path can be converted to value projections during projectLoadValue (it
  // currently only supports StructElementAddr and TupleElementAddr).
  auto storePathNode = accessPath.getPathNode();
  auto loadPathNode = loadWithAccess.accessPath.getPathNode();
  SILValue loadAddr = loadWithAccess.li->getOperand();
  while (loadPathNode != storePathNode) {
    if (!isa<StructElementAddrInst>(loadAddr)
        && !isa<TupleElementAddrInst>(loadAddr)) {
      return LoadWithAccess();
    }
    loadAddr = cast<SingleValueInstruction>(loadAddr)->getOperand(0);
    loadPathNode = loadPathNode.getParent();
  }
  return loadWithAccess;
}

/// Returns true if all instructions in \p SideEffectInsts which may alias with
/// \p access are either loads or stores from \p access.
///
/// \p storeAddr is only needed for AliasAnalysis until we have an interface
/// that supports AccessPath.
static bool isOnlyLoadedAndStored(AliasAnalysis *AA, InstSet &SideEffectInsts,
                                  ArrayRef<LoadInst *> Loads,
                                  ArrayRef<StoreInst *> Stores,
                                  SILValue storeAddr, AccessPath accessPath) {
  for (auto *I : SideEffectInsts) {
    // Pass the original address value until we can fix AA
    if (AA->mayReadOrWriteMemory(I, storeAddr)
        && !isStoreToAccess(I, accessPath)
        && !isLoadWithinAccess(I, accessPath)) {
      return false;
    }
  }
  for (auto *LI : Loads) {
    if (AA->mayReadFromMemory(LI, storeAddr)
        && !doesLoadOverlapAccess(LI, accessPath))
      return false;
  }
  for (auto *SI : Stores) {
    if (AA->mayWriteToMemory(SI, storeAddr) && !isStoreToAccess(SI, accessPath))
      return false;
  }
  return true;
}

/// Returns true if the \p SideEffectInsts set contains any memory writes which
/// may alias with any memory which is read by \p AI.
/// Note: This function should only be called on a read-only apply!
static bool mayWriteTo(AliasAnalysis *AA, BasicCalleeAnalysis *BCA,
                       InstSet &SideEffectInsts, ApplyInst *AI) {

  if (BCA->getMemoryBehavior(FullApplySite::isa(AI), /*observeRetains*/true) ==
      MemoryBehavior::None) {
    return false;
  }

  // Check if the memory addressed by the argument may alias any writes.
  for (auto *inst : SideEffectInsts) {
    switch (inst->getKind()) {
      case SILInstructionKind::StoreInst: {
        auto *si = cast<StoreInst>(inst);
        if (si->getOwnershipQualifier() == StoreOwnershipQualifier::Assign)
          return true;
        if (AA->mayReadFromMemory(AI, si->getDest()))
          return true;
        break;
      }
      case SILInstructionKind::CopyAddrInst: {
        auto *ca = cast<CopyAddrInst>(inst);
        if (!ca->isInitializationOfDest())
          return true;
        if (AA->mayReadFromMemory(AI, ca->getDest()))
          return true;
        break;
      }
      case SILInstructionKind::ApplyInst:
      case SILInstructionKind::BeginApplyInst:
      case SILInstructionKind::TryApplyInst: {
        if (BCA->getMemoryBehavior(FullApplySite::isa(inst), /*observeRetains*/false) >
            MemoryBehavior::MayRead)
          return true;
        break;
      }
      case SILInstructionKind::CondFailInst:
      case SILInstructionKind::StrongRetainInst:
      case SILInstructionKind::UnmanagedRetainValueInst:
      case SILInstructionKind::RetainValueInst:
      case SILInstructionKind::StrongRetainUnownedInst:
      case SILInstructionKind::FixLifetimeInst:
      case SILInstructionKind::KeyPathInst:
      case SILInstructionKind::DeallocStackInst:
      case SILInstructionKind::DeallocStackRefInst:
      case SILInstructionKind::DeallocRefInst:
        break;
      default:
        if (inst->mayWriteToMemory())
          return true;
        break;
    }
  }
  return false;
}

/// Returns true if \p sideEffectInst cannot be reordered with a call to a
/// global initializer.
static bool mayConflictWithGlobalInit(AliasAnalysis *AA,
                    SILInstruction *sideEffectInst, SILInstruction *globalInitCall) {
  if (auto *SI = dyn_cast<StoreInst>(sideEffectInst)) {
    return AA->mayReadOrWriteMemory(globalInitCall, SI->getDest());
  }
  if (auto *LI = dyn_cast<LoadInst>(sideEffectInst)) {
    return AA->mayWriteToMemory(globalInitCall, LI->getOperand());
  }
  return true;
}

/// Returns true if any of the instructions in \p sideEffectInsts which are
/// post-dominated by a call to a global initializer cannot be reordered with
/// the call.
static bool mayConflictWithGlobalInit(AliasAnalysis *AA,
                       InstSet &sideEffectInsts,
                       SILInstruction *globalInitCall,
                       SILBasicBlock *preHeader, PostDominanceInfo *PD) {
  if (!PD->dominates(globalInitCall->getParent(), preHeader))
    return true;

  SILBasicBlock *globalInitBlock = globalInitCall->getParent();
  for (auto *seInst : sideEffectInsts) {
    // Only check instructions in blocks which are "before" (i.e. post-dominated
    // by) the block which contains the init-call.
    // Instructions which are before the call in the same block have already
    // been checked.
    if (PD->properlyDominates(globalInitBlock, seInst->getParent())) {
      if (mayConflictWithGlobalInit(AA, seInst, globalInitCall))
        return true;
    }
  }
  return false;
}

/// Returns true if any of the instructions in \p sideEffectInsts cannot be
/// reordered with a call to a global initializer (which is in the same basic
/// block).
static bool mayConflictWithGlobalInit(AliasAnalysis *AA,
                       ArrayRef<SILInstruction *> sideEffectInsts,
                       SILInstruction *globalInitCall) {
  for (auto *seInst : sideEffectInsts) {
    assert(seInst->getParent() == globalInitCall->getParent());
    if (mayConflictWithGlobalInit(AA, seInst, globalInitCall))
      return true;
  }
  return false;
}

// When Hoisting / Sinking,
// Don't descend into control-dependent code.
// Only traverse into basic blocks that dominate all exits.
static void getDominatingBlocks(SmallVectorImpl<SILBasicBlock *> &domBlocks,
                                SILLoop *Loop, DominanceInfo *DT) {
  auto HeaderBB = Loop->getHeader();
  auto DTRoot = DT->getNode(HeaderBB);
  SmallVector<SILBasicBlock *, 8> ExitingAndLatchBBs;
  Loop->getExitingAndLatchBlocks(ExitingAndLatchBBs);
  for (llvm::df_iterator<DominanceInfoNode *> It = llvm::df_begin(DTRoot),
                                              E = llvm::df_end(DTRoot);
       It != E;) {
    auto *CurBB = It->getBlock();

    // Don't decent into control-dependent code. Only traverse into basic blocks
    // that dominate all exits.
    if (!std::all_of(ExitingAndLatchBBs.begin(), ExitingAndLatchBBs.end(),
                     [=](SILBasicBlock *ExitBB) {
          return DT->dominates(CurBB, ExitBB);
        })) {
      LLVM_DEBUG(llvm::dbgs() << "  skipping conditional block "
                              << *CurBB << "\n");
      It.skipChildren();
      continue;
    }
    domBlocks.push_back(CurBB);
    // Next block in dominator tree.
    ++It;
  }
}

/// Returns true if \p v is loop invariant in \p L.
static bool isLoopInvariant(SILValue v, SILLoop *L) {
  if (SILBasicBlock *parent = v->getParentBlock())
    return !L->contains(parent);
  return false;
}

static bool hoistInstruction(DominanceInfo *DT, SILInstruction *Inst,
                             SILLoop *Loop, SILBasicBlock *&Preheader) {
  auto Operands = Inst->getAllOperands();
  if (!std::all_of(Operands.begin(), Operands.end(), [=](Operand &Op) {
        return isLoopInvariant(Op.get(), Loop);
      })) {
    LLVM_DEBUG(llvm::dbgs() << "   loop variant operands\n");
    return false;
  }

  auto mvBefore = Preheader->getTerminator();
  ArraySemanticsCall semCall(Inst);
  if (semCall.canHoist(mvBefore, DT)) {
    semCall.hoist(mvBefore, DT);
  } else {
    Inst->moveBefore(mvBefore);
  }
  return true;
}

static bool hoistInstructions(SILLoop *Loop, DominanceInfo *DT,
                              InstSet &HoistUpSet) {
  LLVM_DEBUG(llvm::dbgs() << " Hoisting instructions.\n");
  auto Preheader = Loop->getLoopPreheader();
  assert(Preheader && "Expected a preheader");
  bool Changed = false;
  SmallVector<SILBasicBlock *, 8> domBlocks;
  getDominatingBlocks(domBlocks, Loop, DT);

  for (auto *CurBB : domBlocks) {
    // We know that the block is guaranteed to be executed. Hoist if we can.
    for (auto InstIt = CurBB->begin(), E = CurBB->end(); InstIt != E;) {
      SILInstruction *Inst = &*InstIt;
      ++InstIt;
      LLVM_DEBUG(llvm::dbgs() << "  looking at " << *Inst);
      if (!HoistUpSet.count(Inst)) {
        continue;
      }
      if (!hoistInstruction(DT, Inst, Loop, Preheader)) {
        continue;
      }
      LLVM_DEBUG(llvm::dbgs() << "Hoisted " << *Inst);
      Changed = true;
    }
  }

  return Changed;
}

/// Summary of side effect instructions occurring in the loop tree rooted at \p
/// Loop. This includes all writes of the sub loops and the loop itself.
struct LoopNestSummary {
  SILLoop *Loop;
  InstSet SideEffectInsts;

  LoopNestSummary(SILLoop *Curr) : Loop(Curr) {}


  void copySummary(LoopNestSummary &Other) {
    SideEffectInsts.insert(Other.SideEffectInsts.begin(), Other.SideEffectInsts.end());
  }

  LoopNestSummary(const LoopNestSummary &) = delete;
  LoopNestSummary &operator=(const LoopNestSummary &) = delete;
  LoopNestSummary(LoopNestSummary &&) = delete;
};

static unsigned getEdgeIndex(SILBasicBlock *BB, SILBasicBlock *ExitingBB) {
  auto Succs = ExitingBB->getSuccessors();
  for (unsigned EdgeIdx = 0; EdgeIdx < Succs.size(); ++EdgeIdx) {
    SILBasicBlock *CurrBB = Succs[EdgeIdx];
    if (CurrBB == BB) {
      return EdgeIdx;
    }
  }
  llvm_unreachable("BB is not a Successor");
}

static bool sinkInstruction(DominanceInfo *DT,
                            std::unique_ptr<LoopNestSummary> &LoopSummary,
                            SILInstruction *Inst, SILLoopInfo *LI) {
  auto *Loop = LoopSummary->Loop;
  SmallVector<SILBasicBlock *, 8> ExitBBs;
  Loop->getExitBlocks(ExitBBs);
  SmallVector<SILBasicBlock *, 8> NewExitBBs;
  SmallVector<SILBasicBlock *, 8> ExitingBBs;
  Loop->getExitingBlocks(ExitingBBs);
  auto *ExitBB = Loop->getExitBlock();

  bool Changed = false;
  for (auto *ExitingBB : ExitingBBs) {
    SmallVector<SILBasicBlock *, 8> BBSuccessors;
    auto Succs = ExitingBB->getSuccessors();
    for (unsigned EdgeIdx = 0; EdgeIdx < Succs.size(); ++EdgeIdx) {
      SILBasicBlock *BB = Succs[EdgeIdx];
      BBSuccessors.push_back(BB);
    }
    while (!BBSuccessors.empty()) {
      SILBasicBlock *BB = BBSuccessors.pop_back_val();
      if (std::find(NewExitBBs.begin(), NewExitBBs.end(), BB) !=
          NewExitBBs.end()) {
        // Already got a copy there
        continue;
      }
      auto EdgeIdx = getEdgeIndex(BB, ExitingBB);
      SILBasicBlock *OutsideBB = nullptr;
      if (std::find(ExitBBs.begin(), ExitBBs.end(), BB) != ExitBBs.end()) {
        auto *SplitBB =
            splitCriticalEdge(ExitingBB->getTerminator(), EdgeIdx, DT, LI);
        OutsideBB = SplitBB ? SplitBB : BB;
        NewExitBBs.push_back(OutsideBB);
      }
      if (!OutsideBB) {
        continue;
      }
      // If OutsideBB already contains Inst -> skip
      // This might happen if we have a conditional control flow
      // And a pair
      // We hoisted the first part, we can safely ignore sinking
      auto matchPred = [&](SILInstruction &CurrIns) {
        return Inst->isIdenticalTo(&CurrIns);
      };
      if (std::find_if(OutsideBB->begin(), OutsideBB->end(), matchPred) !=
          OutsideBB->end()) {
        LLVM_DEBUG(llvm::errs() << "  instruction already at exit BB "
                                << *Inst);
        ExitBB = nullptr;
      } else if (ExitBB) {
        // easy case
        LLVM_DEBUG(llvm::errs() << "  moving instruction to exit BB " << *Inst);
        Inst->moveBefore(&*OutsideBB->begin());
      } else {
        LLVM_DEBUG(llvm::errs() << "  cloning instruction to exit BB "
                                << *Inst);
        Inst->clone(&*OutsideBB->begin());
      }
      Changed = true;
    }
  }
  if (Changed && !ExitBB) {
    // Created clones of instruction
    // Remove it from the side-effect set - dangling pointer
    LoopSummary->SideEffectInsts.erase(Inst);
    Inst->getParent()->erase(Inst);
  }
  return Changed;
}

static bool sinkInstructions(std::unique_ptr<LoopNestSummary> &LoopSummary,
                             DominanceInfo *DT, SILLoopInfo *LI,
                             InstVector &SinkDownSet) {
  auto *Loop = LoopSummary->Loop;
  LLVM_DEBUG(llvm::errs() << " Sink instructions attempt\n");
  SmallVector<SILBasicBlock *, 8> domBlocks;
  getDominatingBlocks(domBlocks, Loop, DT);

  bool Changed = false;
  for (auto *Inst : SinkDownSet) {
    // only sink if the block is guaranteed to be executed.
    if (std::find(domBlocks.begin(), domBlocks.end(), Inst->getParent()) ==
        domBlocks.end()) {
      continue;
    }
    Changed |= sinkInstruction(DT, LoopSummary, Inst, LI);
  }

  return Changed;
}

static void getEndAccesses(BeginAccessInst *BI,
                           SmallVectorImpl<EndAccessInst *> &EndAccesses) {
  for (auto Use : BI->getUses()) {
    auto *User = Use->getUser();
    auto *EI = dyn_cast<EndAccessInst>(User);
    if (!EI) {
      continue;
    }
    EndAccesses.push_back(EI);
  }
}

static bool
hoistSpecialInstruction(std::unique_ptr<LoopNestSummary> &LoopSummary,
                        DominanceInfo *DT, SILLoopInfo *LI, InstVector &Special) {
  auto *Loop = LoopSummary->Loop;
  LLVM_DEBUG(llvm::errs() << " Hoist and Sink pairs attempt\n");
  auto Preheader = Loop->getLoopPreheader();
  assert(Preheader && "Expected a preheader");

  bool Changed = false;

  for (auto *Inst : Special) {
    if (isa<BeginAccessInst>(Inst) && LoopSummary->Loop->hasNoExitBlocks()) {
      // If no exit block, don't try to hoist BeginAccess because
      // sinking EndAccess would fail later.
      continue;
    }
    if (!hoistInstruction(DT, Inst, Loop, Preheader)) {
      continue;
    }
    if (auto *BI = dyn_cast<BeginAccessInst>(Inst)) {
      SmallVector<EndAccessInst *, 2> Ends;
      getEndAccesses(BI, Ends);
      LLVM_DEBUG(llvm::dbgs() << "Hoisted BeginAccess " << *BI);
      for (auto *instSink : Ends) {
        if (!sinkInstruction(DT, LoopSummary, instSink, LI)) {
          llvm_unreachable("LICM: Could not perform must-sink instruction");
        }
      }
      LLVM_DEBUG(llvm::errs() << " Successfully hoisted and sank pair\n");
    } else {
      LLVM_DEBUG(llvm::dbgs() << "Hoisted RefElementAddr "
                              << *static_cast<RefElementAddrInst *>(Inst));
    }
    Changed = true;
  }

  return Changed;
}

/// Optimize the loop tree bottom up propagating loop's summaries up the
/// loop tree.
class LoopTreeOptimization {
  llvm::DenseMap<SILLoop *, std::unique_ptr<LoopNestSummary>>
      LoopNestSummaryMap;
  SmallVector<SILLoop *, 8> BotUpWorkList;
  InstSet toDelete;
  SILLoopInfo *LoopInfo;
  AliasAnalysis *AA;
  BasicCalleeAnalysis *BCA;
  DominanceInfo *DomTree;
  PostDominanceAnalysis *PDA;
  PostDominanceInfo *postDomTree = nullptr;
  AccessStorageAnalysis *ASA;
  bool Changed;

  /// True if LICM is done on high-level SIL, i.e. semantic calls are not
  /// inlined yet. In this case some semantic calls can be hoisted.
  bool RunsOnHighLevelSIL;

  /// Instructions that we may be able to hoist up
  InstSet HoistUp;

  /// Instructions that we may be able to sink down
  InstVector SinkDown;

  /// Load and store instructions that we may be able to move out of the loop.
  /// All loads and stores within a block must be in instruction order to
  /// simplify replacement of values after SSA update.
  InstVector LoadsAndStores;

  /// All access paths of the \p LoadsAndStores instructions.
  llvm::SetVector<AccessPath> LoadAndStoreAddrs;

  /// Hoistable Instructions that need special treatment
  /// e.g. begin_access
  InstVector SpecialHoist;

public:
  LoopTreeOptimization(SILLoop *TopLevelLoop, SILLoopInfo *LI,
                       AliasAnalysis *AA, BasicCalleeAnalysis *BCA,
                       DominanceInfo *DT, PostDominanceAnalysis *PDA,
                       AccessStorageAnalysis *ASA,
                       bool RunsOnHighLevelSil)
      : LoopInfo(LI), AA(AA), BCA(BCA), DomTree(DT), PDA(PDA), ASA(ASA),
        Changed(false), RunsOnHighLevelSIL(RunsOnHighLevelSil) {
    // Collect loops for a recursive bottom-up traversal in the loop tree.
    BotUpWorkList.push_back(TopLevelLoop);
    for (unsigned i = 0; i < BotUpWorkList.size(); ++i) {
      auto *L = BotUpWorkList[i];
      for (auto *SubLoop : *L)
        BotUpWorkList.push_back(SubLoop);
    }
  }

  /// Optimize this loop tree.
  bool optimize();

protected:
  /// Propagate the sub-loops' summaries up to the current loop.
  void propagateSummaries(std::unique_ptr<LoopNestSummary> &CurrSummary);

  bool isSafeReadOnlyApply(BasicCalleeAnalysis *BCA, ApplyInst *AI);

  /// Collect a set of instructions that can be hoisted
  void analyzeCurrentLoop(std::unique_ptr<LoopNestSummary> &CurrSummary);

  SingleValueInstruction *splitLoad(SILValue splitAddress,
                                    ArrayRef<AccessPath::Index> remainingPath,
                                    SILBuilder &builder,
                                    SmallVectorImpl<LoadInst *> &Loads,
                                    unsigned ldStIdx);

  /// Given an \p accessPath that is only loaded and stored, split loads that
  /// are wider than \p accessPath.
  bool splitLoads(SmallVectorImpl<LoadInst *> &Loads, AccessPath accessPath,
                  SILValue storeAddr);

  /// Optimize the current loop nest.
  bool optimizeLoop(std::unique_ptr<LoopNestSummary> &CurrSummary);

  /// Move all loads and stores from/to \p accessPath out of the \p loop.
  void hoistLoadsAndStores(AccessPath accessPath, SILLoop *loop);

  /// Move all loads and stores from all addresses in LoadAndStoreAddrs out of
  /// the \p loop.
  ///
  /// This is a combination of load hoisting and store sinking, e.g.
  /// \code
  ///   preheader:
  ///     br header_block
  ///   header_block:
  ///     %x = load %not_aliased_addr
  ///     // use %x and define %y
  ///     store %y to %not_aliased_addr
  ///     ...
  ///   exit_block:
  /// \endcode
  /// is transformed to:
  /// \code
  ///   preheader:
  ///     %x = load %not_aliased_addr
  ///     br header_block
  ///   header_block:
  ///     // use %x and define %y
  ///     ...
  ///   exit_block:
  ///     store %y to %not_aliased_addr
  /// \endcode
  bool hoistAllLoadsAndStores(SILLoop *loop);
};
} // end anonymous namespace

bool LoopTreeOptimization::optimize() {
  // Process loops bottom up in the loop tree.
  while (!BotUpWorkList.empty()) {
    SILLoop *CurrentLoop = BotUpWorkList.pop_back_val();
    LLVM_DEBUG(llvm::dbgs() << "Processing loop " << *CurrentLoop);

    // Collect all summary of all sub loops of the current loop. Since we
    // process the loop tree bottom up they are guaranteed to be available in
    // the map.
    auto CurrLoopSummary = std::make_unique<LoopNestSummary>(CurrentLoop);
    propagateSummaries(CurrLoopSummary);

    // If the current loop changed, then we might reveal more instr to hoist
    // For example, a fix_lifetime's operand, if hoisted outside,
    // Might allow us to sink the instruction out of the loop
    bool currChanged = false;
    do {
      // Analyze the current loop for instructions that can be hoisted.
      analyzeCurrentLoop(CurrLoopSummary);

      currChanged = optimizeLoop(CurrLoopSummary);
      if (currChanged) {
        CurrLoopSummary->SideEffectInsts.clear();
        Changed = true;
      }

      // Reset the data structures for next loop in the list
      HoistUp.clear();
      SinkDown.clear();
      SpecialHoist.clear();
    } while (currChanged);

    // Store the summary for parent loops to use.
    LoopNestSummaryMap[CurrentLoop] = std::move(CurrLoopSummary);
  }
  return Changed;
}

void LoopTreeOptimization::propagateSummaries(
    std::unique_ptr<LoopNestSummary> &CurrSummary) {
  for (auto *SubLoop : *CurrSummary->Loop) {
    assert(LoopNestSummaryMap.count(SubLoop) && "Must have data for sub loops");
    CurrSummary->copySummary(*LoopNestSummaryMap[SubLoop]);
    LoopNestSummaryMap.erase(SubLoop);
  }
}

bool LoopTreeOptimization::isSafeReadOnlyApply(BasicCalleeAnalysis *BCA, ApplyInst *AI) {
  if (auto ri = AI->getSingleResult()) {
    // We don't balance CSE'd apply results which return an owned value.
    if (ri.value().getConvention() != ResultConvention::Unowned)
      return false;
  }

  if (RunsOnHighLevelSIL) {
    // The array-property-opt needs this semantic call inside the loop.
    // After high-level SIL we can hoist it (if it's not inlined already).
    if (ArraySemanticsCall(AI, "array.props.isNativeTypeChecked"))
      return false;
  }

  return BCA->getMemoryBehavior(AI, /*observeRetains*/false) <=
         MemoryBehavior::MayRead;
}

static void checkSideEffects(swift::SILInstruction &Inst,
                      InstSet &SideEffectInsts,
                      SmallVectorImpl<SILInstruction *> &sideEffectsInBlock) {
  if (Inst.mayHaveSideEffects()) {
    SideEffectInsts.insert(&Inst);
    sideEffectsInBlock.push_back(&Inst);
  }
}

/// Returns true if the \p Inst follows the default hoisting heuristic
static bool canHoistUpDefault(SILInstruction *inst, SILLoop *Loop,
                              DominanceInfo *DT, bool RunsOnHighLevelSil) {
  auto Preheader = Loop->getLoopPreheader();
  if (!Preheader) {
    return false;
  }

  if (isa<TermInst>(inst) || isa<AllocationInst>(inst) ||
      isa<DeallocationInst>(inst)) {
    return false;
  }

  // We can’t hoist everything that is hoist-able
  // The canHoist method does not do all the required analysis
  // Some of the work is done at COW Array Opt
  // TODO: Refactor COW Array Opt + canHoist - radar 41601468
  ArraySemanticsCall semCall(inst);
  switch (semCall.getKind()) {
    case ArrayCallKind::kGetCount:
    case ArrayCallKind::kGetCapacity:
      if (RunsOnHighLevelSil && semCall.canHoist(Preheader->getTerminator(), DT))
        return true;
      break;
    case ArrayCallKind::kArrayPropsIsNativeTypeChecked:
      // The array-property-opt needs this semantic call inside the loop.
      // After high-level SIL we can hoist it (if it's not inlined already).
      if (RunsOnHighLevelSil)
        return false;
      break;
    default:
      break;
  }

  if (inst->getMemoryBehavior() == MemoryBehavior::None) {
    return true;
  }
  return false;
}

// Check If all the end accesses of the given begin do not prevent hoisting
// There are only two legal placements for the end access instructions:
// 1) Inside the same loop (sink to loop exists)
// Potential TODO: At loop exit block
static bool handledEndAccesses(BeginAccessInst *BI, SILLoop *Loop) {
  SmallVector<EndAccessInst *, 2> AllEnds;
  getEndAccesses(BI, AllEnds);
  if (AllEnds.empty()) {
    return false;
  }
  for (auto *User : AllEnds) {
    auto *BB = User->getParent();
    if (Loop->getBlocksSet().count(BB) != 0) {
      continue;
    }
    return false;
  }
  return true;
}

static bool isCoveredByScope(BeginAccessInst *BI, DominanceInfo *DT,
                             SILInstruction *applyInstr) {
  if (!DT->dominates(BI, applyInstr))
    return false;
  for (auto *EI : BI->getEndAccesses()) {
    if (!DT->dominates(applyInstr, EI))
      return false;
  }
  return true;
}

static bool analyzeBeginAccess(BeginAccessInst *BI,
                               SmallVector<BeginAccessInst *, 8> &BeginAccesses,
                               SmallVector<FullApplySite, 8> &fullApplies,
                               InstSet &SideEffectInsts,
                               AccessStorageAnalysis *ASA,
                               DominanceInfo *DT) {
  auto storage = AccessStorage::compute(BI->getSource());
  if (!storage) {
    return false;
  }

  auto BIAccessStorageNonNested = AccessStorage::compute(BI);
  auto safeBeginPred = [&](BeginAccessInst *OtherBI) {
    if (BI == OtherBI) {
      return true;
    }
    return BIAccessStorageNonNested.isDistinctFrom(
        AccessStorage::compute(OtherBI));
  };

  if (!std::all_of(BeginAccesses.begin(), BeginAccesses.end(), safeBeginPred))
    return false;

  for (auto fullApply : fullApplies) {
    FunctionAccessStorage callSiteAccesses;
    ASA->getCallSiteEffects(callSiteAccesses, fullApply);
    SILAccessKind accessKind = BI->getAccessKind();
    if (!callSiteAccesses.mayConflictWith(accessKind, storage))
      continue;
    // Check if we can ignore this conflict:
    // If the apply is “sandwiched” between the begin and end access,
    // there’s no reason we can’t hoist out of the loop.
    auto *applyInstr = fullApply.getInstruction();
    if (!isCoveredByScope(BI, DT, applyInstr))
      return false;
  }

  // Check may releases
  // Only class and global access that may alias would conflict
  const AccessStorage::Kind kind = storage.getKind();
  if (kind != AccessStorage::Class && kind != AccessStorage::Global) {
    return true;
  }
  // TODO Introduce "Pure Swift" deinitializers
  // We can then make use of alias information for instr's operands
  // If they don't alias - we might get away with not recording a conflict
  for (SILInstruction *I : SideEffectInsts) {
    // we actually compute all SideEffectInsts in analyzeCurrentLoop
    if (!I->mayRelease()) {
      continue;
    }
    if (!isCoveredByScope(BI, DT, I))
      return false;
  }

  return true;
}

// Analyzes current loop for hosting/sinking potential:
// Computes set of instructions we may be able to move out of the loop
// Important Note:
// We can't bail out of this method! we have to run it on all loops.
// We *need* to discover all SideEffectInsts -
// even if the loop is otherwise skipped!
// This is because outer loops will depend on the inner loop's writes.
//
// This may split some loads into smaller loads.
void LoopTreeOptimization::analyzeCurrentLoop(
    std::unique_ptr<LoopNestSummary> &CurrSummary) {
  InstSet &sideEffects = CurrSummary->SideEffectInsts;
  SILLoop *Loop = CurrSummary->Loop;
  LLVM_DEBUG(llvm::dbgs() << " Analyzing accesses.\n");

  auto *Preheader = Loop->getLoopPreheader();
  if (!Preheader) {
    // Can't hoist/sink instructions
    return;
  }

  // Interesting instructions in the loop:
  SmallVector<ApplyInst *, 8> ReadOnlyApplies;

  // Contains either:
  // * an apply to the addressor of the global
  // * a builtin "once" of the global initializer
  SmallVector<SILInstruction *, 8> globalInitCalls;

  SmallVector<LoadInst *, 8> Loads;
  SmallVector<StoreInst *, 8> Stores;
  SmallVector<FixLifetimeInst *, 8> FixLifetimes;
  SmallVector<BeginAccessInst *, 8> BeginAccesses;
  SmallVector<FullApplySite, 8> fullApplies;

  for (auto *BB : Loop->getBlocks()) {
    SmallVector<SILInstruction *, 8> sideEffectsInBlock;
    for (auto &Inst : *BB) {
      switch (Inst.getKind()) {
      case SILInstructionKind::FixLifetimeInst: {
        auto *FL = cast<FixLifetimeInst>(&Inst);
        if (DomTree->dominates(FL->getOperand()->getParentBlock(), Preheader))
          FixLifetimes.push_back(FL);
        // We can ignore the side effects of FixLifetimes
        break;
      }
      case SILInstructionKind::LoadInst:
        Loads.push_back(cast<LoadInst>(&Inst));
        LoadsAndStores.push_back(&Inst);
        break;
      case SILInstructionKind::StoreInst: {
        Stores.push_back(cast<StoreInst>(&Inst));
        LoadsAndStores.push_back(&Inst);
        checkSideEffects(Inst, sideEffects, sideEffectsInBlock);
        break;
      }
      case SILInstructionKind::BeginAccessInst:
        BeginAccesses.push_back(cast<BeginAccessInst>(&Inst));
        checkSideEffects(Inst, sideEffects, sideEffectsInBlock);
        break;
      case SILInstructionKind::RefElementAddrInst:
        SpecialHoist.push_back(cast<RefElementAddrInst>(&Inst));
        break;
      case swift::SILInstructionKind::CondFailInst:
        // We can (and must) hoist cond_fail instructions if the operand is
        // invariant. We must hoist them so that we preserve memory safety. A
        // cond_fail that would have protected (executed before) a memory access
        // must - after hoisting - also be executed before said access.
        HoistUp.insert(&Inst);
        checkSideEffects(Inst, sideEffects, sideEffectsInBlock);
        break;
      case SILInstructionKind::ApplyInst: {
        auto *AI = cast<ApplyInst>(&Inst);
        if (isSafeReadOnlyApply(BCA, AI)) {
          ReadOnlyApplies.push_back(AI);
        } else if (SILFunction *callee = AI->getReferencedFunctionOrNull()) {
          // Calls to global inits are different because we don't care about
          // side effects which are "after" the call in the loop.
          if (callee->isGlobalInit() &&
              // Check against side-effects within the same block.
              // Side-effects in other blocks are checked later (after we
              // scanned all blocks of the loop).
              !mayConflictWithGlobalInit(AA, sideEffectsInBlock, &Inst))
            globalInitCalls.push_back(&Inst);
        }
        // check for array semantics and side effects - same as default
        LLVM_FALLTHROUGH;
      }
      default:
        if (auto fullApply = FullApplySite::isa(&Inst)) {
          fullApplies.push_back(fullApply);
        } else if (auto *bi = dyn_cast<BuiltinInst>(&Inst)) {
          switch (bi->getBuiltinInfo().ID) {
            case BuiltinValueKind::Once:
            case BuiltinValueKind::OnceWithContext:
              if (!mayConflictWithGlobalInit(AA, sideEffectsInBlock, &Inst))
                globalInitCalls.push_back(&Inst);
              break;
            default:
              break;
          }
        }

        checkSideEffects(Inst, sideEffects, sideEffectsInBlock);
        if (canHoistUpDefault(&Inst, Loop, DomTree, RunsOnHighLevelSIL)) {
          HoistUp.insert(&Inst);
        }
        break;
      }
    }
  }

  // Avoid quadratic complexity in corner cases. Usually, this limit will not be exceeded.
  if (ReadOnlyApplies.size() * sideEffects.size() < 8000) {
    for (auto *AI : ReadOnlyApplies) {
      if (!mayWriteTo(AA, BCA, sideEffects, AI)) {
        HoistUp.insert(AI);
      }
    }
  }
  // Avoid quadratic complexity in corner cases. Usually, this limit will not be exceeded.
  if (Loads.size() * sideEffects.size() < 8000) {
    for (auto *LI : Loads) {
      if (!mayWriteTo(AA, sideEffects, LI)) {
        HoistUp.insert(LI);
      }
    }
  }

  if (!globalInitCalls.empty()) {
    if (!postDomTree) {
      postDomTree = PDA->get(Preheader->getParent());
    }
    if (postDomTree->getRootNode()) {
      for (SILInstruction *ginitCall : globalInitCalls) {
        // Check against side effects which are "before" (i.e. post-dominated
        // by) the global initializer call.
        if (!mayConflictWithGlobalInit(AA, sideEffects, ginitCall, Preheader,
             postDomTree)) {
          HoistUp.insert(ginitCall);
        }
      }
    }
  }

  // Collect memory locations for which we can move all loads and stores out
  // of the loop.
  //
  // Note: The Loads set and LoadsAndStores set may mutate during this loop.
  for (StoreInst *SI : Stores) {
    // Use AccessPathWithBase to recover a base address that can be used for
    // newly inserted memory operations. If we instead teach hoistLoadsAndStores
    // how to rematerialize global_addr, then we don't need this base.
    auto access = AccessPathWithBase::compute(SI->getDest());
    auto accessPath = access.accessPath;
    if (accessPath.isValid() &&
        (access.base && isLoopInvariant(access.base, Loop))) {
      if (isOnlyLoadedAndStored(AA, sideEffects, Loads, Stores, SI->getDest(),
                                accessPath)) {
        if (!LoadAndStoreAddrs.count(accessPath)) {
          if (splitLoads(Loads, accessPath, SI->getDest())) {
            LoadAndStoreAddrs.insert(accessPath);
          }
        }
      }
    }
  }
  if (!FixLifetimes.empty()) {
    bool sideEffectsMayRelease =
        std::any_of(sideEffects.begin(), sideEffects.end(),
                    [&](SILInstruction *W) { return W->mayRelease(); });
    for (auto *FL : FixLifetimes) {
      if (!sideEffectsMayRelease || !mayWriteTo(AA, sideEffects, FL)) {
        SinkDown.push_back(FL);
      }
    }
  }
  for (auto *BI : BeginAccesses) {
    if (!handledEndAccesses(BI, Loop)) {
      LLVM_DEBUG(llvm::dbgs() << "Skipping: " << *BI);
      LLVM_DEBUG(llvm::dbgs() << "Some end accesses can't be handled\n");
      continue;
    }
    if (analyzeBeginAccess(BI, BeginAccesses, fullApplies, sideEffects, ASA,
                           DomTree)) {
      SpecialHoist.push_back(BI);
    }
  }
}

// Recursively determine whether the innerAddress is a direct tuple or struct
// projection chain from outerPath. Populate \p reversePathIndices with the path
// difference.
static bool
computeInnerAccessPath(AccessPath::PathNode outerPath,
                       AccessPath::PathNode innerPath, SILValue innerAddress,
                       SmallVectorImpl<AccessPath::Index> &reversePathIndices) {
  if (outerPath == innerPath)
    return true;

  if (!isa<StructElementAddrInst>(innerAddress)
      && !isa<TupleElementAddrInst>(innerAddress)) {
    return false;
  }
  assert(ProjectionIndex(innerAddress).Index
         == innerPath.getIndex().getSubObjectIndex());

  reversePathIndices.push_back(innerPath.getIndex());
  SILValue srcAddr = cast<SingleValueInstruction>(innerAddress)->getOperand(0);
  if (!computeInnerAccessPath(outerPath, innerPath.getParent(), srcAddr,
                              reversePathIndices)) {
    return false;
  }
  return true;
}

/// Split a load from \p outerAddress recursively following remainingPath.
///
/// Creates a load with identical \p accessPath and a set of
/// non-overlapping loads. Add the new non-overlapping loads to HoistUp.
///
/// \p ldstIdx is the index into LoadsAndStores of the original outer load.
///
/// Return the aggregate produced by merging the loads.
SingleValueInstruction *LoopTreeOptimization::splitLoad(
    SILValue splitAddress, ArrayRef<AccessPath::Index> remainingPath,
    SILBuilder &builder, SmallVectorImpl<LoadInst *> &Loads, unsigned ldstIdx) {
  auto loc = LoadsAndStores[ldstIdx]->getLoc();
  // Recurse until we have a load that matches accessPath.
  if (remainingPath.empty()) {
    // Create a load that matches the stored access path.
    LoadInst *load = builder.createLoad(loc, splitAddress,
                                        LoadOwnershipQualifier::Unqualified);
    Loads.push_back(load);
    // Replace the outer load in the list of loads and stores to hoist and
    // sink. LoadsAndStores must remain in instruction order.
    LoadsAndStores[ldstIdx] = load;
    LLVM_DEBUG(llvm::dbgs() << "Created load from stored path: " << *load);
    return load;
  }
  auto recordDisjointLoad = [&](LoadInst *newLoad) {
    Loads.push_back(newLoad);
    LoadsAndStores.insert(LoadsAndStores.begin() + ldstIdx + 1, newLoad);
  };
  auto subIndex = remainingPath.back().getSubObjectIndex();
  SILType loadTy = splitAddress->getType();
  if (CanTupleType tupleTy = loadTy.getAs<TupleType>()) {
    SmallVector<SILValue, 4> elements;
    for (int tupleIdx : range(tupleTy->getNumElements())) {
      auto *projection = builder.createTupleElementAddr(
          loc, splitAddress, tupleIdx, loadTy.getTupleElementType(tupleIdx));
      SILValue elementVal;
      if (tupleIdx == subIndex) {
        elementVal = splitLoad(projection, remainingPath.drop_back(), builder,
                               Loads, ldstIdx);
      } else {
        elementVal = builder.createLoad(loc, projection,
                                        LoadOwnershipQualifier::Unqualified);
        recordDisjointLoad(cast<LoadInst>(elementVal));
      }
      elements.push_back(elementVal);
    }
    return builder.createTuple(loc, elements);
  }
  auto structTy = loadTy.getStructOrBoundGenericStruct();
  assert(structTy && "tuple and struct elements are checked earlier");
  auto &module = builder.getModule();
  auto expansionContext = builder.getFunction().getTypeExpansionContext();

  SmallVector<SILValue, 4> elements;
  int fieldIdx = 0;
  for (auto *field : structTy->getStoredProperties()) {
    SILType fieldTy = loadTy.getFieldType(field, module, expansionContext);
    auto *projection =
        builder.createStructElementAddr(loc, splitAddress, field, fieldTy);
    SILValue fieldVal;
    if (fieldIdx++ == subIndex)
      fieldVal = splitLoad(projection, remainingPath.drop_back(), builder,
                           Loads, ldstIdx);
    else {
      fieldVal = builder.createLoad(loc, projection,
                                    LoadOwnershipQualifier::Unqualified);
      recordDisjointLoad(cast<LoadInst>(fieldVal));
    }
    elements.push_back(fieldVal);
  }
  return builder.createStruct(loc, loadTy.getObjectType(), elements);
}

/// Find all loads that contain \p accessPath. Split them into a load with
/// identical accessPath and a set of non-overlapping loads. Add the new
/// non-overlapping loads to LoadsAndStores and HoistUp.
///
/// TODO: The \p storeAddr parameter is only needed until we have an
/// AliasAnalysis interface that handles AccessPath.
bool LoopTreeOptimization::splitLoads(SmallVectorImpl<LoadInst *> &Loads,
                                      AccessPath accessPath,
                                      SILValue storeAddr) {
  // The Loads set may mutate during this loop, but we only want to visit the
  // original set.
  for (unsigned loadsIdx = 0, endIdx = Loads.size(); loadsIdx != endIdx;
       ++loadsIdx) {
    auto *load = Loads[loadsIdx];
    if (toDelete.count(load))
      continue;

    if (!AA->mayReadFromMemory(load, storeAddr))
      continue;

    AccessPath loadAccessPath = AccessPath::compute(load->getOperand());
    if (accessPath.contains(loadAccessPath))
      continue;

    assert(loadAccessPath.contains(accessPath));
    LLVM_DEBUG(llvm::dbgs() << "Overlaps with loop stores: " << *load);
    SmallVector<AccessPath::Index, 4> reversePathIndices;
    if (!computeInnerAccessPath(loadAccessPath.getPathNode(),
                                accessPath.getPathNode(), storeAddr,
                                reversePathIndices)) {
      return false;
    }
    // Found a load wider than the store to accessPath.
    //
    // SplitLoads is called for each unique access path in the loop that is
    // only loaded from and stored to and this loop takes time proportional to:
    //   num-wide-loads x num-fields x num-loop-memops
    //
    // For each load wider than the store, it creates a new load for each field
    // in that type. Each new load is inserted in the LoadsAndStores vector.  To
    // avoid super-linear behavior for large types (e.g. giant tuples), limit
    // growth of new loads to an arbitrary constant factor per access path.
    if (Loads.size() >= endIdx + 6) {
      LLVM_DEBUG(llvm::dbgs() << "...Refusing to split more loads\n");
      return false;
    }
    LLVM_DEBUG(llvm::dbgs() << "...Splitting load\n");

    unsigned ldstIdx = [this, load]() {
      auto ldstIter = llvm::find(LoadsAndStores, load);
      assert(ldstIter != LoadsAndStores.end() && "outerLoad missing");
      return std::distance(LoadsAndStores.begin(), ldstIter);
    }();

    SILBuilderWithScope builder(load);

    SILValue aggregateVal = splitLoad(load->getOperand(), reversePathIndices,
                                      builder, Loads, ldstIdx);

    load->replaceAllUsesWith(aggregateVal);
    auto iterAndInserted = toDelete.insert(load);
    (void)iterAndInserted;
    assert(iterAndInserted.second && "the same load should only be split once");
  }
  return true;
}

bool LoopTreeOptimization::optimizeLoop(
    std::unique_ptr<LoopNestSummary> &CurrSummary) {
  auto *CurrentLoop = CurrSummary->Loop;
  // We only support Loops with a preheader
  if (!CurrentLoop->getLoopPreheader())
    return false;
  bool currChanged = false;
  if (hoistAllLoadsAndStores(CurrentLoop))
    return true;

  currChanged |= hoistInstructions(CurrentLoop, DomTree, HoistUp);
  currChanged |= sinkInstructions(CurrSummary, DomTree, LoopInfo, SinkDown);
  currChanged |=
      hoistSpecialInstruction(CurrSummary, DomTree, LoopInfo, SpecialHoist);

  assert(toDelete.empty() && "only hostAllLoadsAndStores deletes");
  return currChanged;
}

/// Creates a value projection from \p rootVal based on the address projection
/// from \a rootVal to \a addr.
static SILValue projectLoadValue(SILValue addr, AccessPath accessPath,
                                 SILValue rootVal, AccessPath rootAccessPath,
                                 SILInstruction *beforeInst) {
  if (accessPath == rootAccessPath)
    return rootVal;

  auto pathNode = accessPath.getPathNode();
  int elementIdx = pathNode.getIndex().getSubObjectIndex();
  if (auto *SEI = dyn_cast<StructElementAddrInst>(addr)) {
    assert(ProjectionIndex(SEI).Index == elementIdx);
    SILValue val = projectLoadValue(
        SEI->getOperand(),
        AccessPath(accessPath.getStorage(), pathNode.getParent(),
                   accessPath.getOffset()),
        rootVal, rootAccessPath, beforeInst);
    SILBuilder B(beforeInst);
    return B.createStructExtract(beforeInst->getLoc(), val, SEI->getField(),
                                 SEI->getType().getObjectType());
  }
  if (auto *TEI = dyn_cast<TupleElementAddrInst>(addr)) {
    assert(ProjectionIndex(TEI).Index == elementIdx);
    SILValue val = projectLoadValue(
        TEI->getOperand(),
        AccessPath(accessPath.getStorage(), pathNode.getParent(),
                   accessPath.getOffset()),
        rootVal, rootAccessPath, beforeInst);
    SILBuilder B(beforeInst);
    return B.createTupleExtract(beforeInst->getLoc(), val, TEI->getFieldIndex(),
                                TEI->getType().getObjectType());
  }
  llvm_unreachable("unknown projection");
}

/// Returns true if all stores to \p addr commonly dominate the loop exits.
static bool
storesCommonlyDominateLoopExits(AccessPath accessPath,
                                SILLoop *loop,
                                ArrayRef<SILBasicBlock *> exitingBlocks) {
  BasicBlockSet stores(loop->getHeader()->getParent());
  SmallVector<Operand *, 8> uses;
  // Collect as many recognizable stores as possible. It's ok if not all stores
  // are collected.
  accessPath.collectUses(uses, AccessUseType::Exact, loop->getFunction());
  for (Operand *use : uses) {
    SILInstruction *user = use->getUser();
    if (isa<StoreInst>(user))
      stores.insert(user->getParent());
  }
  SILBasicBlock *header = loop->getHeader();
  // If a store is in the loop header, we already know that it's dominating all
  // loop exits.
  if (stores.contains(header))
    return true;

  // Also a store in the pre-header dominates all exists. Although the situation
  // is a bit different here: the store in the pre-header remains - it's not
  // (re)moved by the LICM transformation.
  // But even if the loop-stores are not dominating the loop exits, it
  // makes sense to move them out of the loop if this case. When this is done,
  // dead-store-elimination can then most likely eliminate the store in the
  // pre-header.
  //
  //   pre_header:
  //     store %v1 to %addr
  //   header:
  //     cond_br %cond, then, tail
  //   then:
  //     store %v2 to %addr    // a conditional store in the loop
  //     br tail
  //   tail:
  //     cond_br %loop_cond, header, exit
  //   exit:
  //
  //  will be transformed to
  //
  //   pre_header:
  //     store %v1 to %addr    // <- can be removed by DSE afterwards
  //   header:
  //     cond_br %cond, then, tail
  //   then:
  //     br tail
  //   tail(%phi):
  //     cond_br %loop_cond, header, exit
  //   exit:
  //     store %phi to %addr
  //
  if (stores.contains(loop->getLoopPreheader()))
    return true;

  // Propagate the store-is-not-alive flag through the control flow in the loop,
  // starting at the header.
  BasicBlockSet storesNotAlive(loop->getHeader()->getParent());
  storesNotAlive.insert(header);
  bool changed = false;
  do {
    changed = false;
    for (SILBasicBlock *block : loop->blocks()) {
      bool storeAlive = !storesNotAlive.contains(block);
      if (storeAlive && !stores.contains(block) &&
          std::any_of(block->pred_begin(), block->pred_end(),
            [&](SILBasicBlock *b) { return storesNotAlive.contains(b); })) {
        storesNotAlive.insert(block);
        changed = true;
      }
    }
  } while (changed);

  auto isUnreachableBlock = [](SILBasicBlock *succ) {
    return isa<UnreachableInst>(succ->getTerminator());
  };

  // Check if the store-is-not-alive flag reaches any of the exits.
  for (SILBasicBlock *eb : exitingBlocks) {
    // Ignore loop exits to blocks which end in an unreachable.
    if (!std::any_of(eb->succ_begin(), eb->succ_end(), isUnreachableBlock) &&
        storesNotAlive.contains(eb)) {
      return false;
    }
  }
  return true;
}

void LoopTreeOptimization::
hoistLoadsAndStores(AccessPath accessPath, SILLoop *loop) {
  SmallVector<SILBasicBlock *, 4> exitingAndLatchBlocks;
  loop->getExitingAndLatchBlocks(exitingAndLatchBlocks);

  // This is not a requirement for functional correctness, but we don't want to
  // _speculatively_ load and store the value (outside of the loop).
  if (!storesCommonlyDominateLoopExits(accessPath, loop,
                                       exitingAndLatchBlocks))
    return;

  // Inserting the stores requires the exit edges to be not critical.
  for (SILBasicBlock *exitingOrLatchBlock : exitingAndLatchBlocks) {
    for (unsigned idx = 0, e = exitingOrLatchBlock->getSuccessors().size();
         idx != e; ++idx) {
      // exitingBlock->getSuccessors() must not be moved out of this loop,
      // because the successor list is invalidated by splitCriticalEdge.
      if (!loop->contains(exitingOrLatchBlock->getSuccessors()[idx])) {
        splitCriticalEdge(exitingOrLatchBlock->getTerminator(), idx, DomTree,
                          LoopInfo);
      }
    }
  }

  SILBasicBlock *preheader = loop->getLoopPreheader();
  assert(preheader && "Expected a preheader");

  // Initially load the value in the loop pre header.
  SILBuilder B(preheader->getTerminator());
  SILValue storeAddr;
  SILSSAUpdater ssaUpdater;

  // Set all stored values as available values in the ssaUpdater.
  // If there are multiple stores in a block, only the last one counts.
  llvm::Optional<SILLocation> loc;
  for (SILInstruction *I : LoadsAndStores) {
    if (auto *SI = isStoreToAccess(I, accessPath)) {
      loc = SI->getLoc();

      // If a store just stores the loaded value, bail. The operand (= the load)
      // will be removed later, so it cannot be used as available value.
      // This corner case is surprisingly hard to handle, so we just give up.
      if (isLoadWithinAccess(dyn_cast<LoadInst>(SI->getSrc()), accessPath))
        return;

      if (!storeAddr) {
        storeAddr = SI->getDest();
        ssaUpdater.initialize(storeAddr->getType().getObjectType(),
                              storeAddr->getOwnershipKind());
      } else if (SI->getDest()->getType() != storeAddr->getType()) {
        // This transformation assumes that the values of all stores in the loop
        // must be interchangeable. It won't work if stores different types
        // because of casting or payload extraction even though they have the
        // same access path.
        return;
      }
      ssaUpdater.addAvailableValue(SI->getParent(), SI->getSrc());
    }
  }
  assert(storeAddr && "hoistLoadsAndStores requires a store in the loop");
  auto checkBase = [&](SILValue srcAddr) {
    // Clone projections until the address dominates preheader.
    if (DomTree->dominates(srcAddr->getParentBlock(), preheader))
      return srcAddr;

    // return an invalid SILValue to continue cloning.
    return SILValue();
  };
  SILValue initialAddr =
      cloneUseDefChain(storeAddr, preheader->getTerminator(), checkBase);
  // cloneUseDefChain may currently fail if a begin_borrow or mark_dependence is
  // in the chain.
  if (!initialAddr)
    return;

  LoadInst *initialLoad =
      B.createLoad(preheader->getTerminator()->getLoc(), initialAddr,
                   LoadOwnershipQualifier::Unqualified);
  LLVM_DEBUG(llvm::dbgs() << "Creating preload " << *initialLoad);
  ssaUpdater.addAvailableValue(preheader, initialLoad);

  // Remove all stores and replace the loads with the current value.
  SILBasicBlock *currentBlock = nullptr;
  SILValue currentVal;
  for (SILInstruction *I : LoadsAndStores) {
    SILBasicBlock *block = I->getParent();
    if (block != currentBlock) {
      currentBlock = block;
      currentVal = SILValue();
    }
    if (auto *SI = isStoreToAccess(I, accessPath)) {
      LLVM_DEBUG(llvm::dbgs() << "Deleting reloaded store " << *SI);
      currentVal = SI->getSrc();
      toDelete.insert(SI);
      continue;
    }
    auto loadWithAccess = isLoadWithinAccess(I, accessPath);
    if (!loadWithAccess) {
      continue;
    }
    // If we didn't see a store in this block yet, get the current value from
    // the ssaUpdater.
    if (!currentVal)
      currentVal = ssaUpdater.getValueInMiddleOfBlock(block);

    LoadInst *load = loadWithAccess.li;
    auto loadAddress = load->getOperand();
    SILValue projectedValue = projectLoadValue(
        loadAddress, loadWithAccess.accessPath, currentVal, accessPath, load);
    LLVM_DEBUG(llvm::dbgs() << "Replacing stored load " << *load << " with "
                            << projectedValue);
    load->replaceAllUsesWith(projectedValue);
    toDelete.insert(load);
  }

  // Store back the value at all loop exits.
  for (SILBasicBlock *exitingOrLatchBlock : exitingAndLatchBlocks) {
    for (SILBasicBlock *succ : exitingOrLatchBlock->getSuccessors()) {
      if (loop->contains(succ))
        continue;

      assert(succ->getSinglePredecessorBlock()
             && "should have split critical edges");
      SILBuilder B(succ->begin());
      auto *SI = B.createStore(
          loc.value(), ssaUpdater.getValueInMiddleOfBlock(succ), initialAddr,
          StoreOwnershipQualifier::Unqualified);
      (void)SI;
      LLVM_DEBUG(llvm::dbgs() << "Creating loop-exit store " << *SI);
    }
  }

  // In case the value is only stored but never loaded in the loop.
  eliminateDeadInstruction(initialLoad);
}

bool LoopTreeOptimization::hoistAllLoadsAndStores(SILLoop *loop) {
  for (AccessPath accessPath : LoadAndStoreAddrs) {
    hoistLoadsAndStores(accessPath, loop);
  }
  LoadsAndStores.clear();
  LoadAndStoreAddrs.clear();

  if (toDelete.empty())
    return false;

  for (SILInstruction *I : toDelete) {
    recursivelyDeleteTriviallyDeadInstructions(I, /*force*/ true);
  }
  toDelete.clear();
  return true;
}

namespace {
/// Hoist loop invariant code out of innermost loops.
///
/// Transforms are identified by type, not instance. Split this
/// Into two types: "High-level Loop Invariant Code Motion"
/// and "Loop Invariant Code Motion".
class LICM : public SILFunctionTransform {

public:
  LICM(bool RunsOnHighLevelSil) : RunsOnHighLevelSil(RunsOnHighLevelSil) {}

  /// True if LICM is done on high-level SIL, i.e. semantic calls are not
  /// inlined yet. In this case some semantic calls can be hoisted.
  /// We only hoist semantic calls on high-level SIL because we can be sure that
  /// e.g. an Array as SILValue is really immutable (including its content).
  bool RunsOnHighLevelSil;

  void run() override {
    SILFunction *F = getFunction();

    // If our function has ownership, skip it.
    if (F->hasOwnership())
      return;

    SILLoopAnalysis *LA = PM->getAnalysis<SILLoopAnalysis>();
    SILLoopInfo *LoopInfo = LA->get(F);

    if (LoopInfo->empty()) {
      LLVM_DEBUG(llvm::dbgs() << "No loops in " << F->getName() << "\n");
      return;
    }

    DominanceAnalysis *DA = PM->getAnalysis<DominanceAnalysis>();
    PostDominanceAnalysis *PDA = PM->getAnalysis<PostDominanceAnalysis>();
    AliasAnalysis *AA = PM->getAnalysis<AliasAnalysis>(F);
    BasicCalleeAnalysis *BCA = PM->getAnalysis<BasicCalleeAnalysis>();
    AccessStorageAnalysis *ASA = getAnalysis<AccessStorageAnalysis>();
    DominanceInfo *DomTree = nullptr;

    LLVM_DEBUG(llvm::dbgs() << "Processing loops in " << F->getName() << "\n");
    bool Changed = false;

    for (auto *TopLevelLoop : *LoopInfo) {
      if (!DomTree) DomTree = DA->get(F);
      LoopTreeOptimization Opt(TopLevelLoop, LoopInfo, AA, BCA, DomTree, PDA,
                               ASA, RunsOnHighLevelSil);
      Changed |= Opt.optimize();
    }

    if (Changed) {
      LA->lockInvalidation();
      DA->lockInvalidation();
      PM->invalidateAnalysis(F, SILAnalysis::InvalidationKind::FunctionBody);
      LA->unlockInvalidation();
      DA->unlockInvalidation();
    }
  }
};
} // end anonymous namespace

SILTransform *swift::createLICM() {
  return new LICM(false);
}

SILTransform *swift::createHighLevelLICM() {
  return new LICM(true);
}
