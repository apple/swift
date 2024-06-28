//===--- ValueLifetime.cpp - ValueLifetimeAnalysis ------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SILOptimizer/Utils/ValueLifetime.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/STLExtras.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"

using namespace swift;

void ValueLifetimeBoundary::visitInsertionPoints(
    llvm::function_ref<void(SILBasicBlock::iterator insertPt)> visitor,
    DeadEndBlocks *deBlocks) {
  for (SILInstruction *user : lastUsers) {
    if (!isa<TermInst>(user)) {
      visitor(std::next(user->getIterator()));
      continue;
    }
    auto *predBB = user->getParent();
    for (SILBasicBlock *succ : predBB->getSuccessors()) {
      if (deBlocks && deBlocks->isDeadEnd(succ))
        continue;

      assert(succ->getSinglePredecessorBlock() == predBB);
      visitor(succ->begin());
    }
  }
  for (SILBasicBlock *edge : boundaryEdges) {
    if (deBlocks && deBlocks->isDeadEnd(edge))
      continue;

    visitor(edge->begin());
  }
}

void ValueLifetimeAnalysis::propagateLiveness() {
  bool defIsInstruction = defValue.is<SILInstruction *>();
  assert(liveBlocks.empty() && "frontier computed twice");
  assert(
      (!defIsInstruction || !userSet.count(defValue.get<SILInstruction *>())) &&
      "definition cannot be its own use");

  // Compute the def block only if we have a SILInstruction. If we have a
  // SILArgument, this will be nullptr.
  auto *defBB = getDefValueParentBlock();
  int numUsersBeforeDef = 0;

  // Find the initial set of blocks where the value is live, because
  // it is used in those blocks.
  for (SILInstruction *user : userSet) {
    SILBasicBlock *userBlock = user->getParent();
    if (!inLiveBlocks.testAndSet(userBlock))
      liveBlocks.push_back(userBlock);

    // A user in the defBB could potentially be located before the defValue. If
    // we had a SILArgument, defBB will be nullptr, so we should always have
    // numUsersBeforeDef is 0. We assert this at the end of the loop.
    if (defIsInstruction && userBlock == defBB)
      ++numUsersBeforeDef;
  }
  assert((defValue.is<SILInstruction *>() || (numUsersBeforeDef == 0)) &&
         "Non SILInstruction defValue with users before the def?!");

  // Don't count any users in the defBB which are actually located _after_ the
  // defValue.
  if (defIsInstruction) {
    auto instIter = defValue.get<SILInstruction *>()->getIterator();
    while (numUsersBeforeDef > 0 && ++instIter != defBB->end()) {
      if (userSet.count(&*instIter))
        --numUsersBeforeDef;
    }
  }

  // Initialize the hasUsersBeforeDef field.
  hasUsersBeforeDef = numUsersBeforeDef > 0;
  assert(defIsInstruction || !hasUsersBeforeDef);

  // Now propagate liveness backwards until we hit the block that defines the
  // value.
  unsigned workIdx = 0;
  while (workIdx < liveBlocks.size()) {
    auto *bb = liveBlocks[workIdx++];

    // Don't go beyond the definition.
    if (bb == defBB && !hasUsersBeforeDef)
      continue;

    for (auto *predBB : bb->getPredecessorBlocks()) {
      // If it's already in the set, then we've already queued and/or
      // processed the predecessors.
      if (!inLiveBlocks.testAndSet(predBB))
        liveBlocks.push_back(predBB);
    }
  }
}

SILInstruction *ValueLifetimeAnalysis::findLastUserInBlock(SILBasicBlock *bb) {
  // Walk backwards in bb looking for last use of the value.
  for (auto &inst : llvm::reverse(*bb)) {
    assert(defValue.dyn_cast<SILInstruction *>() != &inst &&
           "Found def before finding use!");

    if (userSet.count(&inst))
      return &inst;
  }
  llvm_unreachable("Expected to find use of value in block!");
}

// FIXME: remove the visitBlock callback once DeadEndBlocks is removed.
void ValueLifetimeAnalysis::computeLifetime(
    llvm::function_ref<bool(SILBasicBlock *)> visitBlock,
    llvm::function_ref<void(SILInstruction *)> visitLastUser,
    llvm::function_ref<void(SILBasicBlock *predBB, SILBasicBlock *succBB)>
        visitBoundaryEdge) {
  assert(!isAliveAtBeginOfBlock(getFunction()->getEntryBlock()) &&
         "Can't compute frontier for def which does not dominate all uses");

  /// The lifetime ends if we have a live block and a not-live successor.
  for (SILBasicBlock *bb : liveBlocks) {
    if (!visitBlock(bb))
      continue;

    bool liveInSucc = false;
    bool usedAndRedefinedInSucc = false;
    for (const SILSuccessor &succ : bb->getSuccessors()) {
      if (isAliveAtBeginOfBlock(succ)) {
        liveInSucc = true;
        if (succ == getDefValueParentBlock()) {
          // Here, the basic block bb uses the value but also redefines the
          // value inside bb. The new value could be used by the successors
          // of succ and therefore could be live at the end of succ as well.
          //
          // This should never happen if we have a SILArgument since the
          // SILArgument can not have any uses before it in a block.
          assert(defValue.is<SILInstruction *>() &&
                 "SILArguments dominate all instructions in their defining "
                 "blocks");
          usedAndRedefinedInSucc = true;
        }
      }
    }
    if (usedAndRedefinedInSucc) {
      // Here, the basic block bb uses the value and later redefines the value.
      // Therefore, this value's lifetime ends after its last use preceding the
      // re-definition of the value.
      auto ii = defValue.get<SILInstruction *>()->getReverseIterator();
      for (; ii != bb->rend(); ++ii) {
        if (userSet.count(&*ii)) {
          visitLastUser(&*ii);
          break;
        }
      }
      assert(ii != bb->rend() &&
             "There must be a user in bb before definition");
    }
    if (liveInSucc) {
      for (const SILSuccessor &succ : bb->getSuccessors()) {
        if (!isAliveAtBeginOfBlock(succ))
          visitBoundaryEdge(bb, succ);
      }
    } else {
      // The value is not live in any of the successor blocks. This means the
      // block contains a last use of the value.
      visitLastUser(findLastUserInBlock(bb));
    }
  }
}

// Compute a LifetimeBoundary.
//
// Precondition: no critical edges.
void ValueLifetimeAnalysis::computeLifetimeBoundary(
    ValueLifetimeBoundary &boundary) {
  auto visitBlock = [&](SILBasicBlock *) { return true; };
  auto visitLastUser = [&](SILInstruction *lastUser) {
    boundary.lastUsers.push_back(lastUser);
  };
  auto visitBoundaryEdge = [&](SILBasicBlock *, SILBasicBlock *succBB) {
    boundary.boundaryEdges.push_back(succBB);
  };
  computeLifetime(visitBlock, visitLastUser, visitBoundaryEdge);
}

// FIXME: There is no need for a Mode within the algorithm once critical edges
// are universally prohibited.
//
// FIXME: DeadEndBlocks does not affect value lifetime. It
// should be completely removed and handled by the client.
bool ValueLifetimeAnalysis::computeFrontier(FrontierImpl &frontier, Mode mode,
                                            DeadEndBlocks *deBlocks) {
  bool noCriticalEdges = true;

  // Exit-blocks from the lifetime region. The value is live at the end of
  // a predecessor block but not in the frontier block itself.
  BasicBlockSetVector frontierBlocks(getFunction());

  // Blocks where the value is live at the end of the block and which have
  // a frontier block as successor.
  BasicBlockSetVector liveOutBlocks(getFunction());

  auto visitBlock = [&](SILBasicBlock *bb) {
    return !deBlocks || !deBlocks->isDeadEnd(bb);
  };

  bool foundInvalidLastUser = false;
  auto visitLastUser = [&](SILInstruction *lastUser) {
    if (!isa<TermInst>(lastUser)) {
      // The next instruction after the last use is part of the frontier.
      frontier.push_back(&*std::next(lastUser->getIterator()));
      return;
    }
    // FIXME: DeadObjectElimination and StackPromotion don't currently handle
    // last use terminators, for no good reason. Fix them, then remove the silly
    // UsersMustPostDomDef mode.
    if (mode == UsersMustPostDomDef) {
      foundInvalidLastUser = true;
      return;
    }
    // The last user is a TermInst, and the value is not live into any successor
    // blocks (the usedAndRedefinedInSucc case is never a terminator).  Since
    // there is no further instruction in the block which can be the
    // frontier, add all successor blocks to the frontier.
    auto *termBB = lastUser->getParent();
    for (const SILSuccessor &succ : termBB->getSuccessors()) {
      assert(!isAliveAtBeginOfBlock(succ)
             && "out-of-sync with computeLifetime");

      if (deBlocks && deBlocks->isDeadEnd(succ))
        continue;

      // The successor's first instruction will be added to the frontier. Fake
      // this block as live-out so edge splitting works.
      liveOutBlocks.insert(termBB);
      frontierBlocks.insert(succ);
    }
  };
  auto visitBoundaryEdge = [&](SILBasicBlock *predBB, SILBasicBlock *succBB) {
    if (deBlocks && deBlocks->isDeadEnd(succBB))
      return;
    
    if (mode == UsersMustPostDomDef) {
      foundInvalidLastUser = true;
      return;
    }
    liveOutBlocks.insert(predBB);
    frontierBlocks.insert(succBB);
  };

  // Populate frontierBlocks and call visitLastUser().
  computeLifetime(visitBlock, visitLastUser, visitBoundaryEdge);
  if (foundInvalidLastUser)
    return false;

  // Handle "exit" edges from the lifetime region.
  BasicBlockSet unhandledFrontierBlocks(getFunction());
  bool unhandledFrontierBlocksFound = false;
  for (SILBasicBlock *frontierBB : frontierBlocks) {
    assert(mode != UsersMustPostDomDef);
    bool needSplit = false;
    // If the value is live only in part of the predecessor blocks we have to
    // split those predecessor edges.
    for (SILBasicBlock *Pred : frontierBB->getPredecessorBlocks()) {
      if (!liveOutBlocks.contains(Pred)) {
        needSplit = true;
        break;
      }
    }
    if (needSplit) {
      // We need to split the critical edge to create a frontier instruction.
      unhandledFrontierBlocks.insert(frontierBB);
      unhandledFrontierBlocksFound = true;
    } else {
      // The first instruction of the exit-block is part of the frontier.
      frontier.push_back(&*frontierBB->begin());
    }
  }
  if (!unhandledFrontierBlocksFound) {
    return true;
  }

  // Split critical edges from the lifetime region to not yet handled frontier
  // blocks.
  for (SILBasicBlock *frontierPred : liveOutBlocks) {
    assert(mode != UsersMustPostDomDef);
    auto *term = frontierPred->getTerminator();
    // Cache the successor blocks because splitting critical edges invalidates
    // the successor list iterator of T.
    llvm::SmallVector<SILBasicBlock *, 4> succBlocks;
    for (const SILSuccessor &succ : term->getSuccessors())
      succBlocks.push_back(succ);

    for (unsigned i = 0, e = succBlocks.size(); i != e; ++i) {
      if (unhandledFrontierBlocks.contains(succBlocks[i])) {
        assert((isCriticalEdge(term, i) || userSet.count(term)) &&
               "actually not a critical edge?");
        noCriticalEdges = false;
        if (mode != AllowToModifyCFG) {
          // If the CFG need not be modified, just record the critical edge and
          // continue.
          this->criticalEdges.push_back({term, i});
          continue;
        }
        SILBasicBlock *newBlock = splitEdge(term, i);
        // The single terminator instruction is part of the frontier.
        frontier.push_back(&*newBlock->begin());
      }
    }
  }
  return noCriticalEdges;
}

bool ValueLifetimeAnalysis::isWithinLifetime(SILInstruction *inst) {
  SILBasicBlock *bb = inst->getParent();
  // Check if the value is not live anywhere in inst's block.
  if (!inLiveBlocks.get(bb))
    return false;
  for (const SILSuccessor &succ : bb->getSuccessors()) {
    // If the value is live at the beginning of any successor block it is also
    // live at the end of bb and therefore inst is definitely in the lifetime
    // region (Note that we don't check in upward direction against the value's
    // definition).
    if (isAliveAtBeginOfBlock(succ))
      return true;
  }
  // The value is live in the block but not at the end of the block. Check if
  // inst is located before (or at) the last use.
  for (auto ii = bb->rbegin(); ii != bb->rend(); ++ii) {
    if (userSet.count(&*ii)) {
      return true;
    }
    if (inst == &*ii)
      return false;
  }
  llvm_unreachable("Expected to find use of value in block!");
}

// Searches \p bb backwards from the instruction before \p frontierInst
// to the beginning of the list and returns true if we find a dealloc_ref or an
// dealloc_stack_ref /before/ we find \p defValue (the instruction that
// defines our target value).
static bool
blockContainsDeallocRef(SILBasicBlock *bb,
                        PointerUnion<SILInstruction *, SILArgument *> defValue,
                        SILInstruction *frontierInst) {
  SILBasicBlock::reverse_iterator End = bb->rend();
  SILBasicBlock::reverse_iterator iter = frontierInst->getReverseIterator();
  for (++iter; iter != End; ++iter) {
    SILInstruction *inst = &*iter;
    if (isa<DeallocRefInst>(inst) || isa<DeallocStackRefInst>(inst))
      return true;
    // We know that inst is not a nullptr, so if we have a SILArgument, this
    // will always fail as we want.
    if (inst == defValue.dyn_cast<SILInstruction *>())
      return false;
  }
  return false;
}

bool ValueLifetimeAnalysis::containsDeallocRef(const FrontierImpl &frontier) {
  BasicBlockSet frontierBlocks(getFunction());
  // Search in live blocks where the value is not alive until the end of the
  // block, i.e. the live range is terminated by a frontier instruction.
  for (SILInstruction *frontierInst : frontier) {
    SILBasicBlock *bb = frontierInst->getParent();
    if (blockContainsDeallocRef(bb, defValue, frontierInst))
      return true;
    frontierBlocks.insert(bb);
  }
  // Search in all other live blocks where the value is alive until the end of
  // the block.
  for (SILBasicBlock *bb : liveBlocks) {
    if (frontierBlocks.contains(bb) == 0) {
      if (blockContainsDeallocRef(bb, defValue, bb->getTerminator()))
        return true;
    }
  }
  return false;
}

void ValueLifetimeAnalysis::dump() const {
  llvm::errs() << "lifetime of def: ";
  if (auto *ii = defValue.dyn_cast<SILInstruction *>()) {
    llvm::errs() << *ii;
  } else {
    llvm::errs() << *defValue.get<SILArgument *>();
  }
  for (SILInstruction *use : userSet) {
    llvm::errs() << "  use: " << *use;
  }
  llvm::errs() << "  live blocks:";
  for (SILBasicBlock *bb : liveBlocks) {
    llvm::errs() << ' ' << bb->getDebugID();
  }
  llvm::errs() << '\n';
}

void swift::endLifetimeAtFrontier(
    SILValue valueOrStackLoc,
    const ValueLifetimeAnalysis::FrontierImpl &frontier,
    SILBuilderContext &builderCtxt, InstModCallbacks callbacks) {
  for (SILInstruction *endPoint : frontier) {
    SILBuilderWithScope builder(endPoint, builderCtxt);
    SILLocation loc = RegularLocation(endPoint->getLoc());
    emitDestroyOperation(builder, loc, valueOrStackLoc, callbacks);
    if (isa<AllocStackInst>(valueOrStackLoc)) {
      builder.createDeallocStack(loc, valueOrStackLoc);
    }
  }
}
