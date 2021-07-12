//===--- SILMem2Reg.cpp - Promotes AllocStacks to registers ---------------===//
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
//
// This pass promotes AllocStack instructions into virtual register
// references. It only handles load, store and deallocation
// instructions. The algorithm is based on:
//
//  Sreedhar and Gao. A linear time algorithm for placing phi-nodes. POPL '95.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-mem2reg"

#include "swift/AST/DiagnosticsSIL.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/OwnershipOptUtils.h"
#include "swift/SILOptimizer/Utils/ScopeOptUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
#include <algorithm>
#include <queue>

using namespace swift;
using namespace swift::siloptimizer;

STATISTIC(NumAllocStackFound,    "Number of AllocStack found");
STATISTIC(NumAllocStackCaptured, "Number of AllocStack captured");
STATISTIC(NumInstRemoved,        "Number of Instructions removed");

namespace {

using DomTreeNode = llvm::DomTreeNodeBase<SILBasicBlock>;
using DomTreeLevelMap = llvm::DenseMap<DomTreeNode *, unsigned>;

} // anonymous namespace

//===----------------------------------------------------------------------===//
//                                 Utilities
//===----------------------------------------------------------------------===//

static void replaceDestroy(DestroyAddrInst *dai, SILValue newValue,
                           SILBuilderContext &ctx) {
  SILFunction *f = dai->getFunction();
  auto ty = dai->getOperand()->getType();

  assert(ty.isLoadable(*f) && "Unexpected promotion of address-only type!");

  assert(newValue ||
         (ty.is<TupleType>() && ty.getAs<TupleType>()->getNumElements() == 0));

  SILBuilderWithScope builder(dai, ctx);

  auto &typeLowering = f->getTypeLowering(ty);

  bool expand = shouldExpand(dai->getModule(),
                             dai->getOperand()->getType().getObjectType());
  using TypeExpansionKind = Lowering::TypeLowering::TypeExpansionKind;
  auto expansionKind = expand ? TypeExpansionKind::MostDerivedDescendents
                              : TypeExpansionKind::None;
  typeLowering.emitLoweredDestroyValue(builder, dai->getLoc(), newValue,
                                       expansionKind);
  dai->eraseFromParent();
}

/// Promote a DebugValueAddr to a DebugValue of the given value.
static void promoteDebugValueAddr(DebugValueAddrInst *dvai, SILValue value,
                                  SILBuilderContext &ctx) {
  assert(dvai->getOperand()->getType().isLoadable(*dvai->getFunction()) &&
         "Unexpected promotion of address-only type!");
  assert(value && "Expected valid value");
  // Avoid inserting the same debug_value twice.
  for (auto *use : value->getUses()) {
    if (auto *dvi = dyn_cast<DebugValueInst>(use->getUser())) {
      if (*dvi->getVarInfo() == *dvai->getVarInfo()) {
        dvai->eraseFromParent();
        return;
      }
    }
  }

  SILBuilderWithScope b(dvai, ctx);
  b.createDebugValue(dvai->getLoc(), value, *dvai->getVarInfo());
  dvai->eraseFromParent();
}

/// Returns true if \p I is a load which loads from \p ASI.
static bool isLoadFromStack(SILInstruction *i, AllocStackInst *asi) {
  if (!isa<LoadInst>(i))
    return false;

  // Skip struct and tuple address projections.
  ValueBase *op = i->getOperand(0);
  while (op != asi) {
    if (!isa<UncheckedAddrCastInst>(op) && !isa<StructElementAddrInst>(op) &&
        !isa<TupleElementAddrInst>(op))
      return false;
    op = cast<SingleValueInstruction>(op)->getOperand(0);
  }
  return true;
}

/// Collects all load instructions which (transitively) use \p I as address.
static void collectLoads(SILInstruction *i,
                         SmallVectorImpl<LoadInst *> &foundLoads) {
  if (auto *load = dyn_cast<LoadInst>(i)) {
    foundLoads.push_back(load);
    return;
  }
  if (!isa<UncheckedAddrCastInst>(i) && !isa<StructElementAddrInst>(i) &&
      !isa<TupleElementAddrInst>(i))
    return;

  // Recursively search for other loads in the instruction's uses.
  for (auto *use : cast<SingleValueInstruction>(i)->getUses()) {
    collectLoads(use->getUser(), foundLoads);
  }
}

static void replaceLoad(LoadInst *li, SILValue newValue, AllocStackInst *asi,
                        SILBuilderContext &ctx) {
  ProjectionPath projections(newValue->getType());
  SILValue op = li->getOperand();
  SILBuilderWithScope builder(li, ctx);
  SILOptScope scope;

  while (op != asi) {
    assert(isa<UncheckedAddrCastInst>(op) || isa<StructElementAddrInst>(op) ||
           isa<TupleElementAddrInst>(op));
    auto *inst = cast<SingleValueInstruction>(op);
    projections.push_back(Projection(inst));
    op = inst->getOperand(0);
  }

  for (const auto &proj : llvm::reverse(projections)) {
    assert(proj.getKind() == ProjectionKind::BitwiseCast ||
           proj.getKind() == ProjectionKind::Struct ||
           proj.getKind() == ProjectionKind::Tuple);

    // struct_extract and tuple_extract expect guaranteed operand ownership
    // non-trivial RunningVal is owned. Insert borrow operation to convert them
    // to guaranteed!
    if (proj.getKind() == ProjectionKind::Struct ||
        proj.getKind() == ProjectionKind::Tuple) {
      if (auto opVal = scope.borrowValue(li, newValue)) {
        assert(*opVal != newValue &&
               "Valid value should be different from input value");
        newValue = *opVal;
      }
    }
    newValue =
        proj.createObjectProjection(builder, li->getLoc(), newValue).get();
  }

  op = li->getOperand();

  // Replace users of the loaded value with `val`
  // If we have a load [copy], replace the users with copy_value of `val`
  if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Copy) {
    li->replaceAllUsesWith(builder.createCopyValue(li->getLoc(), newValue));
  } else {
    assert(!asi->getFunction()->hasOwnership() ||
           newValue.getOwnershipKind() != OwnershipKind::Guaranteed);
    li->replaceAllUsesWith(newValue);
  }

  // Pop the scope so that we emit cleanups.
  std::move(scope).popAtEndOfScope(&*builder.getInsertionPoint());

  // Delete the load
  li->eraseFromParent();

  while (op != asi && op->use_empty()) {
    assert(isa<UncheckedAddrCastInst>(op) || isa<StructElementAddrInst>(op) ||
           isa<TupleElementAddrInst>(op));
    auto *inst = cast<SingleValueInstruction>(op);
    SILValue next = inst->getOperand(0);
    inst->eraseFromParent();
    op = next;
  }
}

/// Create a tuple value for an empty tuple or a tuple of empty tuples.
static SILValue createValueForEmptyTuple(SILType ty,
                                         SILInstruction *insertionPoint,
                                         SILBuilderContext &ctx) {
  auto tupleTy = ty.castTo<TupleType>();
  SmallVector<SILValue, 4> elements;
  for (unsigned idx : range(tupleTy->getNumElements())) {
    SILType elementTy = ty.getTupleElementType(idx);
    elements.push_back(
        createValueForEmptyTuple(elementTy, insertionPoint, ctx));
  }
  SILBuilderWithScope builder(insertionPoint, ctx);
  return builder.createTuple(insertionPoint->getLoc(), ty, elements);
}

//===----------------------------------------------------------------------===//
//                     Single Stack Allocation Promotion
//===----------------------------------------------------------------------===//

namespace {

/// Promotes a single AllocStackInst into registers..
class StackAllocationPromoter {
  using BlockSetVector = BasicBlockSetVector;
  using BlockToInstMap = llvm::DenseMap<SILBasicBlock *, SILInstruction *>;

  // Use a priority queue keyed on dominator tree level so that inserted nodes
  // are handled from the bottom of the dom tree upwards.
  using DomTreeNodePair = std::pair<DomTreeNode *, unsigned>;
  using NodePriorityQueue =
      std::priority_queue<DomTreeNodePair, SmallVector<DomTreeNodePair, 32>,
                          llvm::less_second>;

  /// The AllocStackInst that we are handling.
  AllocStackInst *asi;

  /// The deallocation Instruction. This value could be NULL if there are
  /// multiple deallocations.
  DeallocStackInst *dsi;

  /// Dominator info.
  DominanceInfo *domInfo;

  /// Map from dominator tree node to tree level.
  DomTreeLevelMap &domTreeLevels;

  /// The SIL builder used when creating new instructions during register
  /// promotion.
  SILBuilderContext &ctx;

  /// Records the last store instruction in each block for a specific
  /// AllocStackInst.
  BlockToInstMap lastStoreInBlock;

public:
  /// C'tor.
  StackAllocationPromoter(AllocStackInst *inputASI, DominanceInfo *inputDomInfo,
                          DomTreeLevelMap &inputDomTreeLevels,
                          SILBuilderContext &inputCtx)
      : asi(inputASI), dsi(nullptr), domInfo(inputDomInfo),
        domTreeLevels(inputDomTreeLevels), ctx(inputCtx) {
    // Scan the users in search of a deallocation instruction.
    for (auto *use : asi->getUses()) {
      if (auto *foundDealloc = dyn_cast<DeallocStackInst>(use->getUser())) {
        // Don't record multiple dealloc instructions.
        if (dsi) {
          dsi = nullptr;
          break;
        }
        // Record the deallocation instruction.
        dsi = foundDealloc;
      }
    }
  }

  /// Promote the Allocation.
  void run();

private:
  /// Promote AllocStacks into SSA.
  void promoteAllocationToPhi();

  /// Replace the dummy nodes with new block arguments.
  void addBlockArguments(BlockSetVector &phiBlocks);

  /// Check if \p phi is a proactively added phi by SILMem2Reg
  bool isProactivePhi(SILPhiArgument *phi, const BlockSetVector &phiBlocks);

  /// Check if \p proactivePhi is live.
  bool isNecessaryProactivePhi(SILPhiArgument *proactivePhi,
                               const BlockSetVector &phiBlocks);

  /// Given a \p proactivePhi that is live, backward propagate liveness to
  /// other proactivePhis.
  void propagateLiveness(SILPhiArgument *proactivePhi,
                         const BlockSetVector &phiBlocks,
                         SmallPtrSetImpl<SILPhiArgument *> &livePhis);

  /// Fix all of the branch instructions and the uses to use
  /// the AllocStack definitions (which include stores and Phis).
  void fixBranchesAndUses(BlockSetVector &blocks);

  /// update the branch instructions with the new Phi argument.
  /// The blocks in \p PhiBlocks are blocks that define a value, \p Dest is
  /// the branch destination, and \p Pred is the predecessors who's branch we
  /// modify.
  void fixPhiPredBlock(BlockSetVector &phiBlocks, SILBasicBlock *dest,
                       SILBasicBlock *pred);

  /// Get the value for this AllocStack variable that is
  /// flowing out of StartBB.
  SILValue getLiveOutValue(BlockSetVector &phiBlocks,
                           SILBasicBlock *startBlock);

  /// Get the value for this AllocStack variable that is
  /// flowing into BB.
  SILValue getLiveInValue(BlockSetVector &phiBlocks, SILBasicBlock *block);

  /// Prune AllocStacks usage in the function. Scan the function
  /// and remove in-block usage of the AllocStack. Leave only the first
  /// load and the last store.
  void pruneAllocStackUsage();

  /// Promote all of the AllocStacks in a single basic block in one
  /// linear scan. This function deletes all of the loads and stores except
  /// for the first load and the last store.
  /// \returns the last StoreInst found or zero if none found.
  StoreInst *promoteAllocationInBlock(SILBasicBlock *block);
};

} // end of namespace

StoreInst *StackAllocationPromoter::promoteAllocationInBlock(
    SILBasicBlock *blockPromotingWithin) {
  LLVM_DEBUG(llvm::dbgs() << "*** Promoting ASI in block: " << *asi);

  // RunningVal is the current value in the stack location.
  // We don't know the value of the alloca until we find the first store.
  SILValue runningVal = SILValue();
  // Keep track of the last StoreInst that we found.
  StoreInst *lastStore = nullptr;

  // For all instructions in the block.
  for (auto bbi = blockPromotingWithin->begin(),
            bbe = blockPromotingWithin->end();
       bbi != bbe;) {
    SILInstruction *inst = &*bbi;
    ++bbi;

    if (isLoadFromStack(inst, asi)) {
      auto *li = cast<LoadInst>(inst);
      if (runningVal) {
        // If we are loading from the AllocStackInst and we already know the
        // content of the Alloca then use it.
        LLVM_DEBUG(llvm::dbgs() << "*** Promoting load: " << *li);
        replaceLoad(li, runningVal, asi, ctx);
        ++NumInstRemoved;
      } else if (li->getOperand() == asi &&
                 li->getOwnershipQualifier() != LoadOwnershipQualifier::Copy) {
        // If we don't know the content of the AllocStack then the loaded
        // value *is* the new value;
        // Don't use result of load [copy] as a RunningVal, it necessitates
        // additional logic for cleanup of consuming instructions of the result.
        // StackAllocationPromoter::fixBranchesAndUses will later handle it.
        LLVM_DEBUG(llvm::dbgs() << "*** First load: " << *li);
        runningVal = li;
      }
      continue;
    }

    // Remove stores and record the value that we are saving as the running
    // value.
    if (auto *si = dyn_cast<StoreInst>(inst)) {
      if (si->getDest() != asi)
        continue;

      // If we see a store [assign], always convert it to a store [init]. This
      // simplifies further processing.
      if (si->getOwnershipQualifier() == StoreOwnershipQualifier::Assign) {
        if (runningVal) {
          SILBuilderWithScope(si, ctx).createDestroyValue(si->getLoc(),
                                                          runningVal);
        } else {
          SILBuilderWithScope localBuilder(si, ctx);
          auto *newLoad = localBuilder.createLoad(si->getLoc(), asi,
                                                  LoadOwnershipQualifier::Take);
          localBuilder.createDestroyValue(si->getLoc(), newLoad);
        }
        si->setOwnershipQualifier(StoreOwnershipQualifier::Init);
      }

      // If we met a store before this one, delete it.
      if (lastStore) {
        assert(lastStore->getOwnershipQualifier() !=
                   StoreOwnershipQualifier::Assign &&
               "store [assign] to the stack location should have been "
               "transformed to a store [init]");
        LLVM_DEBUG(llvm::dbgs()
                   << "*** Removing redundant store: " << *lastStore);
        ++NumInstRemoved;
        lastStore->eraseFromParent();
      }

      // The stored value is the new running value.
      runningVal = si->getSrc();
      // The current store is now the LastStore
      lastStore = si;
      continue;
    }

    // Replace debug_value_addr with debug_value of the promoted value
    // if we have a valid value to use at this point. Otherwise we'll
    // promote this when we deal with hooking up phis.
    if (auto *dvai = dyn_cast<DebugValueAddrInst>(inst)) {
      if (dvai->getOperand() == asi && runningVal)
        promoteDebugValueAddr(dvai, runningVal, ctx);
      continue;
    }

    // Replace destroys with a release of the value.
    if (auto *dai = dyn_cast<DestroyAddrInst>(inst)) {
      if (dai->getOperand() == asi && runningVal) {
        replaceDestroy(dai, runningVal, ctx);
      }
      continue;
    }

    if (auto *dvi = dyn_cast<DestroyValueInst>(inst)) {
      if (dvi->getOperand() == runningVal) {
        // Reset LastStore.
        // So that we don't end up passing dead values as phi args in
        // StackAllocationPromoter::fixBranchesAndUses
        lastStore = nullptr;
      }
    }

    // Stop on deallocation.
    if (auto *dsi = dyn_cast<DeallocStackInst>(inst)) {
      if (dsi->getOperand() == asi)
        break;
    }
  }

  if (lastStore) {
    assert(lastStore->getOwnershipQualifier() !=
               StoreOwnershipQualifier::Assign &&
           "store [assign] to the stack location should have been "
           "transformed to a store [init]");
    LLVM_DEBUG(llvm::dbgs()
               << "*** Finished promotion. Last store: " << *lastStore);
  } else {
    LLVM_DEBUG(llvm::dbgs() << "*** Finished promotion with no stores.\n");
  }
  return lastStore;
}

void StackAllocationPromoter::addBlockArguments(BlockSetVector &phiBlocks) {
  LLVM_DEBUG(llvm::dbgs() << "*** Adding new block arguments.\n");

  for (auto *block : phiBlocks)
    block->createPhiArgument(asi->getElementType(), OwnershipKind::Owned);
}

SILValue StackAllocationPromoter::getLiveOutValue(BlockSetVector &phiBlocks,
                                                  SILBasicBlock *startBlock) {
  LLVM_DEBUG(llvm::dbgs() << "*** Searching for a value definition.\n");
  // Walk the Dom tree in search of a defining value:
  for (DomTreeNode *domNode = domInfo->getNode(startBlock); domNode;
       domNode = domNode->getIDom()) {
    SILBasicBlock *domBlock = domNode->getBlock();

    // If there is a store (that must come after the phi), use its value.
    BlockToInstMap::iterator it = lastStoreInBlock.find(domBlock);
    if (it != lastStoreInBlock.end())
      if (auto *si = dyn_cast_or_null<StoreInst>(it->second)) {
        LLVM_DEBUG(llvm::dbgs() << "*** Found Store def " << *si->getSrc());
        return si->getSrc();
      }

    // If there is a Phi definition in this block:
    if (phiBlocks.contains(domBlock)) {
      // Return the dummy instruction that represents the new value that we will
      // add to the basic block.
      SILValue phi = domBlock->getArgument(domBlock->getNumArguments() - 1);
      LLVM_DEBUG(llvm::dbgs() << "*** Found a dummy Phi def " << *phi);
      return phi;
    }

    // Move to the next dominating block.
    LLVM_DEBUG(llvm::dbgs() << "*** Walking up the iDOM.\n");
  }
  LLVM_DEBUG(llvm::dbgs() << "*** Could not find a Def. Using Undef.\n");
  return SILUndef::get(asi->getElementType(), *asi->getFunction());
}

SILValue StackAllocationPromoter::getLiveInValue(BlockSetVector &phiBlocks,
                                                 SILBasicBlock *block) {
  // First, check if there is a Phi value in the current block. We know that
  // our loads happen before stores, so we need to first check for Phi nodes
  // in the first block, but stores first in all other stores in the idom
  // chain.
  if (phiBlocks.contains(block)) {
    LLVM_DEBUG(llvm::dbgs() << "*** Found a local Phi definition.\n");
    return block->getArgument(block->getNumArguments() - 1);
  }

  if (block->pred_empty() || !domInfo->getNode(block))
    return SILUndef::get(asi->getElementType(), *asi->getFunction());

  // No phi for this value in this block means that the value flowing
  // out of the immediate dominator reaches here.
  DomTreeNode *iDom = domInfo->getNode(block)->getIDom();
  assert(iDom &&
         "Attempt to get live-in value for alloc_stack in entry block!");

  return getLiveOutValue(phiBlocks, iDom->getBlock());
}

void StackAllocationPromoter::fixPhiPredBlock(BlockSetVector &phiBlocks,
                                              SILBasicBlock *destBlock,
                                              SILBasicBlock *predBlock) {
  TermInst *ti = predBlock->getTerminator();
  LLVM_DEBUG(llvm::dbgs() << "*** Fixing the terminator " << ti << ".\n");

  SILValue def = getLiveOutValue(phiBlocks, predBlock);

  LLVM_DEBUG(llvm::dbgs() << "*** Found the definition: " << *def);

  addArgumentToBranch(def, destBlock, ti);
  ti->eraseFromParent();
}

bool StackAllocationPromoter::isProactivePhi(SILPhiArgument *phi,
                                             const BlockSetVector &phiBlocks) {
  auto *phiBlock = phi->getParentBlock();
  return phiBlocks.contains(phiBlock) &&
         phi == phiBlock->getArgument(phiBlock->getNumArguments() - 1);
}

bool StackAllocationPromoter::isNecessaryProactivePhi(
    SILPhiArgument *proactivePhi, const BlockSetVector &phiBlocks) {
  assert(isProactivePhi(proactivePhi, phiBlocks));
  for (auto *use : proactivePhi->getUses()) {
    auto *branch = dyn_cast<BranchInst>(use->getUser());
    // A non-branch use is a necessary use
    if (!branch)
      return true;
    auto *destBB = branch->getDestBB();
    auto opNum = use->getOperandNumber();
    // A phi has a necessary use if it is used as a branch op for a
    // non-proactive phi
    if (!phiBlocks.contains(destBB) || (opNum != branch->getNumArgs() - 1))
      return true;
  }
  return false;
}

void StackAllocationPromoter::propagateLiveness(
    SILPhiArgument *proactivePhi, const BlockSetVector &phiBlocks,
    SmallPtrSetImpl<SILPhiArgument *> &livePhis) {
  assert(isProactivePhi(proactivePhi, phiBlocks));
  if (livePhis.contains(proactivePhi))
    return;
  // If liveness has not been propagated, go over the incoming operands and mark
  // any operand values that are proactivePhis as live
  livePhis.insert(proactivePhi);
  SmallVector<SILValue> incomingPhiVals;
  proactivePhi->getIncomingPhiValues(incomingPhiVals);
  for (auto &inVal : incomingPhiVals) {
    auto *inPhi = dyn_cast<SILPhiArgument>(inVal);
    if (!inPhi)
      continue;
    if (!isProactivePhi(inPhi, phiBlocks))
      continue;
    propagateLiveness(inPhi, phiBlocks, livePhis);
  }
}

void StackAllocationPromoter::fixBranchesAndUses(BlockSetVector &phiBlocks) {
  // First update uses of the value.
  SmallVector<LoadInst *, 4> collectedLoads;

  for (auto ui = asi->use_begin(), ue = asi->use_end(); ui != ue;) {
    auto *user = ui->getUser();
    ++ui;
    bool removedUser = false;

    collectedLoads.clear();
    collectLoads(user, collectedLoads);
    for (auto *li : collectedLoads) {
      SILValue def;
      // If this block has no predecessors then nothing dominates it and
      // the instruction is unreachable. If the instruction we're
      // examining is a value, replace it with undef. Either way, delete
      // the instruction and move on.
      SILBasicBlock *loadBlock = li->getParent();
      def = getLiveInValue(phiBlocks, loadBlock);

      LLVM_DEBUG(llvm::dbgs()
                 << "*** Replacing " << *li << " with Def " << *def);

      // Replace the load with the definition that we found.
      replaceLoad(li, def, asi, ctx);
      removedUser = true;
      ++NumInstRemoved;
    }

    if (removedUser)
      continue;

    // If this block has no predecessors then nothing dominates it and
    // the instruction is unreachable. Delete the instruction and move
    // on.
    SILBasicBlock *userBlock = user->getParent();

    if (auto *dvai = dyn_cast<DebugValueAddrInst>(user)) {
      // Replace DebugValueAddr with DebugValue.
      SILValue def = getLiveInValue(phiBlocks, userBlock);
      promoteDebugValueAddr(dvai, def, ctx);
      ++NumInstRemoved;
      continue;
    }

    // Replace destroys with a release of the value.
    if (auto *dai = dyn_cast<DestroyAddrInst>(user)) {
      SILValue def = getLiveInValue(phiBlocks, userBlock);
      replaceDestroy(dai, def, ctx);
      continue;
    }
  }

  // Now that all of the uses are fixed we can fix the branches that point
  // to the blocks with the added arguments.
  // For each Block with a new Phi argument:
  for (auto *block : phiBlocks) {
    // Fix all predecessors.
    for (auto pbbi = block->getPredecessorBlocks().begin(),
              pbbe = block->getPredecessorBlocks().end();
         pbbi != pbbe;) {
      auto *predBlock = *pbbi;
      ++pbbi;
      assert(predBlock && "Invalid block!");
      fixPhiPredBlock(phiBlocks, block, predBlock);
    }
  }

  // Fix ownership of proactively created non-trivial phis
  if (asi->getFunction()->hasOwnership() &&
      !asi->getType().isTrivial(*asi->getFunction())) {
    SmallPtrSet<SILPhiArgument *, 4> livePhis;

    for (auto *block : phiBlocks) {
      auto *proactivePhi = cast<SILPhiArgument>(
          block->getArgument(block->getNumArguments() - 1));
      // First, check if the proactively added phi is necessary by looking at
      // it's immediate uses.
      if (isNecessaryProactivePhi(proactivePhi, phiBlocks)) {
        // Backward propagate liveness to other dependent proactively added phis
        propagateLiveness(proactivePhi, phiBlocks, livePhis);
      }
    }
    // Go over all proactively added phis, and delete those that were not marked
    // live above.
    for (auto *block : phiBlocks) {
      auto *proactivePhi = cast<SILPhiArgument>(
          block->getArgument(block->getNumArguments() - 1));
      if (!livePhis.contains(proactivePhi)) {
        proactivePhi->replaceAllUsesWithUndef();
        erasePhiArgument(block, block->getNumArguments() - 1);
      }
    }
  }
}

void StackAllocationPromoter::pruneAllocStackUsage() {
  LLVM_DEBUG(llvm::dbgs() << "*** Pruning : " << *asi);
  BlockSetVector functionBlocks(asi->getFunction());

  // Insert all of the blocks that asi is live in.
  for (auto *use : asi->getUses())
    functionBlocks.insert(use->getUser()->getParent());

  // Clear AllocStack state.
  lastStoreInBlock.clear();

  for (auto block : functionBlocks) {
    StoreInst *si = promoteAllocationInBlock(block);
    lastStoreInBlock[block] = si;
  }

  LLVM_DEBUG(llvm::dbgs() << "*** Finished pruning : " << *asi);
}

void StackAllocationPromoter::promoteAllocationToPhi() {
  LLVM_DEBUG(llvm::dbgs() << "*** Placing Phis for : " << *asi);

  // A list of blocks that will require new Phi values.
  BlockSetVector phiBlocks(asi->getFunction());

  // The "piggy-bank" data-structure that we use for processing the dom-tree
  // bottom-up.
  NodePriorityQueue priorityQueue;

  // Collect all of the stores into the AllocStack. We know that at this point
  // we have at most one store per block.
  for (auto *use : asi->getUses()) {
    SILInstruction *user = use->getUser();
    // We need to place Phis for this block.
    if (isa<StoreInst>(user)) {
      // If the block is in the dom tree (dominated by the entry block).
      if (auto *node = domInfo->getNode(user->getParent()))
        priorityQueue.push(std::make_pair(node, domTreeLevels[node]));
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "*** Found: " << priorityQueue.size()
                          << " Defs\n");

  // A list of nodes for which we already calculated the dominator frontier.
  llvm::SmallPtrSet<DomTreeNode *, 32> visited;

  SmallVector<DomTreeNode *, 32> worklist;

  // Scan all of the definitions in the function bottom-up using the priority
  // queue.
  while (!priorityQueue.empty()) {
    DomTreeNodePair rootPair = priorityQueue.top();
    priorityQueue.pop();
    DomTreeNode *root = rootPair.first;
    unsigned rootLevel = rootPair.second;

    // Walk all dom tree children of Root, inspecting their successors. Only
    // J-edges, whose target level is at most Root's level are added to the
    // dominance frontier.
    worklist.clear();
    worklist.push_back(root);

    while (!worklist.empty()) {
      DomTreeNode *node = worklist.pop_back_val();
      SILBasicBlock *nodeBlock = node->getBlock();

      // For all successors of the node:
      for (auto &nodeBlockSuccs : nodeBlock->getSuccessors()) {
        auto *successorNode = domInfo->getNode(nodeBlockSuccs);

        // Skip D-edges (edges that are dom-tree edges).
        if (successorNode->getIDom() == node)
          continue;

        // Ignore J-edges that point to nodes that are not smaller or equal
        // to the root level.
        unsigned succLevel = domTreeLevels[successorNode];
        if (succLevel > rootLevel)
          continue;

        // Ignore visited nodes.
        if (!visited.insert(successorNode).second)
          continue;

        // If the new PHInode is not dominated by the allocation then it's dead.
        if (!domInfo->dominates(asi->getParent(), successorNode->getBlock()))
          continue;

        // If the new PHInode is properly dominated by the deallocation then it
        // is obviously a dead PHInode, so we don't need to insert it.
        if (dsi && domInfo->properlyDominates(dsi->getParent(),
                                              successorNode->getBlock()))
          continue;

        // The successor node is a new PHINode. If this is a new PHI node
        // then it may require additional definitions, so add it to the PQ.
        if (phiBlocks.insert(nodeBlockSuccs))
          priorityQueue.push(std::make_pair(successorNode, succLevel));
      }

      // Add the children in the dom-tree to the worklist.
      for (auto *child : node->children())
        if (!visited.count(child))
          worklist.push_back(child);
    }
  }

  // At this point we calculated the locations of all of the new Phi values.
  // Next, add the Phi values and promote all of the loads and stores into the
  // new locations.

  // Replace the dummy values with new block arguments.
  addBlockArguments(phiBlocks);

  // Hook up the Phi nodes, loads, and debug_value_addr with incoming values.
  fixBranchesAndUses(phiBlocks);

  LLVM_DEBUG(llvm::dbgs() << "*** Finished placing Phis ***\n");
}

void StackAllocationPromoter::run() {
  // Reduce the number of load/stores in the function to minimum.
  // After this phase we are left with up to one load and store
  // per block and the last store is recorded.
  pruneAllocStackUsage();

  // Replace AllocStacks with Phi-nodes.
  promoteAllocationToPhi();
}

//===----------------------------------------------------------------------===//
//                      General Memory To Registers Impl
//===----------------------------------------------------------------------===//

namespace {

/// Promote memory to registers
class MemoryToRegisters {
  /// The function that we are optimizing.
  SILFunction &f;

  /// Dominators.
  DominanceInfo *domInfo;

  /// The builder context used when creating new instructions during register
  /// promotion.
  SILBuilderContext ctx;

  /// Check if the AllocStackInst \p ASI is only written into.
  bool isWriteOnlyAllocation(AllocStackInst *asi);

  /// Promote all of the AllocStacks in a single basic block in one
  /// linear scan. Note: This function deletes all of the users of the
  /// AllocStackInst, including the DeallocStackInst but it does not remove the
  /// AllocStackInst itself!
  void removeSingleBlockAllocation(AllocStackInst *asi);

  /// Attempt to promote the specified stack allocation, returning true if so
  /// or false if not.  On success, all uses of the AllocStackInst have been
  /// removed, but the ASI itself is still in the program.
  bool promoteSingleAllocation(AllocStackInst *asi,
                               DomTreeLevelMap &domTreeLevels);

public:
  /// C'tor
  MemoryToRegisters(SILFunction &inputFunc, DominanceInfo *inputDomInfo)
      : f(inputFunc), domInfo(inputDomInfo), ctx(inputFunc.getModule()) {}

  /// Promote memory to registers. Return True on change.
  bool run();
};

} // end anonymous namespace

/// Returns true if \p I is an address of a LoadInst, skipping struct and
/// tuple address projections. Sets \p singleBlock to null if the load (or
/// it's address is not in \p singleBlock.
/// This function looks for these patterns:
/// 1. (load %ASI)
/// 2. (load (struct_element_addr/tuple_element_addr/unchecked_addr_cast %ASI))
static bool isAddressForLoad(SILInstruction *load, SILBasicBlock *&singleBlock,
                             bool &hasGuaranteedOwnership) {

  if (isa<LoadInst>(load)) {
    // SILMem2Reg is disabled when we find:
    // (load [take] (struct_element_addr/tuple_element_addr %ASI))
    // struct_element_addr and tuple_element_addr are lowered into
    // struct_extract and tuple_extract and these SIL instructions have a
    // guaranteed ownership. For replacing load's users, we need an owned value.
    // We will need a new copy and destroy of the running val placed after the
    // last use. This is not implemented currently.
    if (hasGuaranteedOwnership &&
        cast<LoadInst>(load)->getOwnershipQualifier() ==
            LoadOwnershipQualifier::Take) {
      return false;
    }
    return true;
  }

  if (!isa<UncheckedAddrCastInst>(load) && !isa<StructElementAddrInst>(load) &&
      !isa<TupleElementAddrInst>(load))
    return false;

  if (isa<StructElementAddrInst>(load) || isa<TupleElementAddrInst>(load)) {
    hasGuaranteedOwnership = true;
  }

  // Recursively search for other (non-)loads in the instruction's uses.
  for (auto *use : cast<SingleValueInstruction>(load)->getUses()) {
    SILInstruction *user = use->getUser();
    if (user->getParent() != singleBlock)
      singleBlock = nullptr;

    if (!isAddressForLoad(user, singleBlock, hasGuaranteedOwnership))
      return false;
  }
  return true;
}

/// Returns true if \p I is a dead struct_element_addr or tuple_element_addr.
static bool isDeadAddrProjection(SILInstruction *inst) {
  if (!isa<UncheckedAddrCastInst>(inst) && !isa<StructElementAddrInst>(inst) &&
      !isa<TupleElementAddrInst>(inst))
    return false;

  // Recursively search for uses which are dead themselves.
  for (auto UI : cast<SingleValueInstruction>(inst)->getUses()) {
    SILInstruction *II = UI->getUser();
    if (!isDeadAddrProjection(II))
      return false;
  }
  return true;
}

/// Returns true if this AllocStacks is captured.
/// Sets \p inSingleBlock to true if all uses of \p ASI are in a single block.
static bool isCaptured(AllocStackInst *asi, bool &inSingleBlock) {
  SILBasicBlock *singleBlock = asi->getParent();

  // For all users of the AllocStack instruction.
  for (auto *use : asi->getUses()) {
    SILInstruction *user = use->getUser();

    if (user->getParent() != singleBlock)
      singleBlock = nullptr;

    // Loads are okay.
    bool hasGuaranteedOwnership = false;
    if (isAddressForLoad(user, singleBlock, hasGuaranteedOwnership))
      continue;

    // We can store into an AllocStack (but not the pointer).
    if (auto *si = dyn_cast<StoreInst>(user))
      if (si->getDest() == asi)
        continue;

    // Deallocation is also okay, as are DebugValueAddr. We will turn
    // the latter into DebugValue.
    if (isa<DeallocStackInst>(user) || isa<DebugValueAddrInst>(user))
      continue;

    // Destroys of loadable types can be rewritten as releases, so
    // they are fine.
    if (auto *dai = dyn_cast<DestroyAddrInst>(user))
      if (dai->getOperand()->getType().isLoadable(*dai->getFunction()))
        continue;

    // Other instructions are assumed to capture the AllocStack.
    LLVM_DEBUG(llvm::dbgs() << "*** AllocStack is captured by: " << *user);
    return true;
  }

  // None of the users capture the AllocStack.
  inSingleBlock = (singleBlock != nullptr);
  return false;
}

/// Returns true if the AllocStack is only stored into.
bool MemoryToRegisters::isWriteOnlyAllocation(AllocStackInst *asi) {
  // For all users of the AllocStack:
  for (auto *use : asi->getUses()) {
    SILInstruction *user = use->getUser();

    // It is okay to store into this AllocStack.
    if (auto *si = dyn_cast<StoreInst>(user))
      if (!isa<AllocStackInst>(si->getSrc()))
        continue;

    // Deallocation is also okay.
    if (isa<DeallocStackInst>(user))
      continue;

    // If we haven't already promoted the AllocStack, we may see
    // DebugValueAddr uses.
    if (isa<DebugValueAddrInst>(user))
      continue;

    if (isDeadAddrProjection(user))
      continue;

    // Can't do anything else with it.
    LLVM_DEBUG(llvm::dbgs() << "*** AllocStack has non-write use: " << *user);
    return false;
  }

  return true;
}

void MemoryToRegisters::removeSingleBlockAllocation(AllocStackInst *asi) {
  LLVM_DEBUG(llvm::dbgs() << "*** Promoting in-block: " << *asi);

  SILBasicBlock *parentBlock = asi->getParent();

  // The default value of the AllocStack is NULL because we don't have
  // uninitialized variables in Swift.
  SILValue runningVal = SILValue();

  // For all instructions in the block.
  for (auto bbi = parentBlock->begin(), bbe = parentBlock->end(); bbi != bbe;) {
    SILInstruction *inst = &*bbi;
    ++bbi;

    // Remove instructions that we are loading from. Replace the loaded value
    // with our running value.
    if (isLoadFromStack(inst, asi)) {
      if (!runningVal) {
        // Loading without a previous store is only acceptable if the type is
        // Void (= empty tuple) or a tuple of Voids.
        runningVal = createValueForEmptyTuple(asi->getElementType(), inst, ctx);
      }
      replaceLoad(cast<LoadInst>(inst), runningVal, asi, ctx);
      ++NumInstRemoved;
      continue;
    }

    // Remove stores and record the value that we are saving as the running
    // value.
    if (auto *si = dyn_cast<StoreInst>(inst)) {
      if (si->getDest() == asi) {
        if (si->getOwnershipQualifier() == StoreOwnershipQualifier::Assign) {
          assert(runningVal);
          SILBuilderWithScope(si, ctx).createDestroyValue(si->getLoc(),
                                                          runningVal);
        }
        runningVal = si->getSrc();
        inst->eraseFromParent();
        ++NumInstRemoved;
        continue;
      }
    }

    // Replace debug_value_addr with debug_value of the promoted value.
    if (auto *dvai = dyn_cast<DebugValueAddrInst>(inst)) {
      if (dvai->getOperand() == asi) {
        if (runningVal) {
          promoteDebugValueAddr(dvai, runningVal, ctx);
        } else {
          // Drop debug_value_addr of uninitialized void values.
          assert(asi->getElementType().isVoid() &&
                 "Expected initialization of non-void type!");
          dvai->eraseFromParent();
        }
      }
      continue;
    }

    // Replace destroys with a release of the value.
    if (auto *dai = dyn_cast<DestroyAddrInst>(inst)) {
      if (dai->getOperand() == asi) {
        replaceDestroy(dai, runningVal, ctx);
      }
      continue;
    }

    // Remove deallocation.
    if (auto *dsi = dyn_cast<DeallocStackInst>(inst)) {
      if (dsi->getOperand() == asi) {
        inst->eraseFromParent();
        NumInstRemoved++;
        // No need to continue scanning after deallocation.
        break;
      }
    }

    // Remove dead address instructions that may be uses of the allocation.
    auto *addrInst = dyn_cast<SingleValueInstruction>(inst);
    while (addrInst && addrInst->use_empty() &&
           (isa<StructElementAddrInst>(addrInst) ||
            isa<TupleElementAddrInst>(addrInst) ||
            isa<UncheckedAddrCastInst>(addrInst))) {
      SILValue op = addrInst->getOperand(0);
      addrInst->eraseFromParent();
      ++NumInstRemoved;
      addrInst = dyn_cast<SingleValueInstruction>(op);
    }
  }
}

/// Compute the dominator tree levels for domInfo.
static void computeDomTreeLevels(DominanceInfo *domInfo,
                                 DomTreeLevelMap &domTreeLevels) {
  // TODO: This should happen once per function.
  SmallVector<DomTreeNode *, 32> worklist;
  DomTreeNode *rootNode = domInfo->getRootNode();
  domTreeLevels[rootNode] = 0;
  worklist.push_back(rootNode);
  while (!worklist.empty()) {
    DomTreeNode *domNode = worklist.pop_back_val();
    unsigned childLevel = domTreeLevels[domNode] + 1;
    for (auto *childNode : domNode->children()) {
      domTreeLevels[childNode] = childLevel;
      worklist.push_back(childNode);
    }
  }
}

/// Attempt to promote the specified stack allocation, returning true if so
/// or false if not.  On success, this returns true and usually drops all of the
/// uses of the AllocStackInst, but never deletes the ASI itself.  Callers
/// should check to see if the ASI is dead after this and remove it if so.
bool MemoryToRegisters::promoteSingleAllocation(
    AllocStackInst *alloc, DomTreeLevelMap &domTreeLevels) {
  LLVM_DEBUG(llvm::dbgs() << "*** Memory to register looking at: " << *alloc);
  ++NumAllocStackFound;

  // In OSSA, don't do Mem2Reg on non-trivial alloc_stack with dynamic_lifetime.
  if (alloc->hasDynamicLifetime() && f.hasOwnership() &&
      !alloc->getType().isTrivial(f)) {
    return false;
  }

  // Don't handle captured AllocStacks.
  bool inSingleBlock = false;
  if (isCaptured(alloc, inSingleBlock)) {
    ++NumAllocStackCaptured;
    return false;
  }

  // Remove write-only AllocStacks.
  if (isWriteOnlyAllocation(alloc)) {
    eraseUsesOfInstruction(alloc);

    LLVM_DEBUG(llvm::dbgs() << "*** Deleting store-only AllocStack: "<< *alloc);
    return true;
  }

  // For AllocStacks that are only used within a single basic blocks, use
  // the linear sweep to remove the AllocStack.
  if (inSingleBlock) {
    removeSingleBlockAllocation(alloc);

    LLVM_DEBUG(llvm::dbgs() << "*** Deleting single block AllocStackInst: "
                            << *alloc);
    if (!alloc->use_empty()) {
      // Handle a corner case where the ASI still has uses:
      // This can come up if the source contains a withUnsafePointer where
      // the pointer escapes. It's illegal code but we should not crash.
      // Re-insert a dealloc_stack so that the verifier is happy.
      auto *next = alloc->getNextInstruction();
      SILBuilderWithScope b(next, ctx);
      b.createDeallocStack(next->getLoc(), alloc);
    }
    return true;
  }

  LLVM_DEBUG(llvm::dbgs() << "*** Need to insert BB arguments for " << *alloc);

  // Promote this allocation.
  StackAllocationPromoter(alloc, domInfo, domTreeLevels, ctx).run();

  // Make sure that all of the allocations were promoted into registers.
  assert(isWriteOnlyAllocation(alloc) && "Non-write uses left behind");
  // ... and erase the allocation.
  eraseUsesOfInstruction(alloc);
  return true;
}

bool MemoryToRegisters::run() {
  bool madeChange = false;

  if (f.getModule().getOptions().VerifyAll)
    f.verifyCriticalEdges();

  // Compute dominator tree node levels for the function.
  DomTreeLevelMap domTreeLevels;
  computeDomTreeLevels(domInfo, domTreeLevels);

  for (auto &block : f) {
    auto ii = block.begin(), ie = block.end();
    while (ii != ie) {
      SILInstruction *inst = &*ii;
      auto *asi = dyn_cast<AllocStackInst>(inst);
      if (!asi) {
        ++ii;
        continue;
      }

      bool promoted = promoteSingleAllocation(asi, domTreeLevels);
      ++ii;
      if (promoted) {
        if (asi->use_empty())
          asi->eraseFromParent();
        ++NumInstRemoved;
        madeChange = true;
      }
    }
  }
  return madeChange;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class SILMem2Reg : public SILFunctionTransform {
  void run() override {
    SILFunction *f = getFunction();

    LLVM_DEBUG(llvm::dbgs()
               << "** Mem2Reg on function: " << f->getName() << " **\n");

    auto *da = PM->getAnalysis<DominanceAnalysis>();

    bool madeChange = MemoryToRegisters(*f, da->get(f)).run();
    if (madeChange)
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
};

} // end anonymous namespace

SILTransform *swift::createMem2Reg() {
  return new SILMem2Reg();
}
