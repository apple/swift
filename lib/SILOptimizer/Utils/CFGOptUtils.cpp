//===--- CFGOptUtils.cpp - SIL CFG edge utilities -------------------------===//
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

#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/LoopInfo.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/TinyPtrVector.h"

using namespace swift;

TermInst *swift::addNewEdgeValueToBranch(TermInst *branch, SILBasicBlock *dest,
                                         SILValue val,
                                         InstructionDeleter &deleter) {
  SILBuilderWithScope builder(branch);
  TermInst *newBr = nullptr;

  if (auto *cbi = dyn_cast<CondBranchInst>(branch)) {
    SmallVector<SILValue, 8> trueArgs;
    SmallVector<SILValue, 8> falseArgs;

    for (auto arg : cbi->getTrueArgs())
      trueArgs.push_back(arg);

    for (auto arg : cbi->getFalseArgs())
      falseArgs.push_back(arg);

    if (dest == cbi->getTrueBB()) {
      trueArgs.push_back(val);
      assert(trueArgs.size() == dest->getNumArguments());
    }
    if (dest == cbi->getFalseBB()) {
      falseArgs.push_back(val);
      assert(falseArgs.size() == dest->getNumArguments());
    }

    newBr = builder.createCondBranch(
        cbi->getLoc(), cbi->getCondition(), cbi->getTrueBB(), trueArgs,
        cbi->getFalseBB(), falseArgs, cbi->getTrueBBCount(),
        cbi->getFalseBBCount());
    deleter.getCallbacks().createdNewInst(newBr);
  } else if (auto *bi = dyn_cast<BranchInst>(branch)) {
    SmallVector<SILValue, 8> args;

    for (auto arg : bi->getArgs())
      args.push_back(arg);

    args.push_back(val);
    assert(args.size() == dest->getNumArguments());
    newBr = builder.createBranch(bi->getLoc(), bi->getDestBB(), args);
    deleter.getCallbacks().createdNewInst(newBr);
  } else {
    // At the moment we can only add arguments to br and cond_br.
    llvm_unreachable("Can't add argument to terminator");
  }

  deleter.forceDelete(branch);

  return newBr;
}

static void deleteTriviallyDeadOperandsOfDeadArgument(
    MutableArrayRef<Operand> termOperands, unsigned deadArgIndex,
    InstModCallbacks callbacks = InstModCallbacks()) {
  Operand &op = termOperands[deadArgIndex];
  auto *i = op.get()->getDefiningInstruction();
  if (!i)
    return;
  op.set(SILUndef::get(op.get()));
  eliminateDeadInstruction(i, callbacks);
}

// Our implementation assumes that our caller is attempting to remove a dead
// SILPhiArgument from a SILBasicBlock and has already RAUWed the argument.
TermInst *swift::deleteEdgeValue(TermInst *branch, SILBasicBlock *destBlock,
                                 size_t argIndex, bool cleanupDeadPhiOps,
                                 InstModCallbacks callbacks) {
  if (auto *cbi = dyn_cast<CondBranchInst>(branch)) {
    SmallVector<SILValue, 8> trueArgs;
    SmallVector<SILValue, 8> falseArgs;

    llvm::copy(cbi->getTrueArgs(), std::back_inserter(trueArgs));
    llvm::copy(cbi->getFalseArgs(), std::back_inserter(falseArgs));

    if (destBlock == cbi->getTrueBB()) {
      if (cleanupDeadPhiOps) {
        deleteTriviallyDeadOperandsOfDeadArgument(cbi->getTrueOperands(),
                                                  argIndex, callbacks);
      }
      trueArgs.erase(trueArgs.begin() + argIndex);
    }

    if (destBlock == cbi->getFalseBB()) {
      if (cleanupDeadPhiOps) {
        deleteTriviallyDeadOperandsOfDeadArgument(cbi->getFalseOperands(),
                                                  argIndex, callbacks);
      }
      falseArgs.erase(falseArgs.begin() + argIndex);
    }

    SILBuilderWithScope builder(cbi);
    auto *result = builder.createCondBranch(
        cbi->getLoc(), cbi->getCondition(), cbi->getTrueBB(), trueArgs,
        cbi->getFalseBB(), falseArgs, cbi->getTrueBBCount(),
        cbi->getFalseBBCount());
    branch->eraseFromParent();
    return result;
  }

  if (auto *bi = dyn_cast<BranchInst>(branch)) {
    SmallVector<SILValue, 8> args;
    llvm::copy(bi->getArgs(), std::back_inserter(args));
    if (cleanupDeadPhiOps) {
      deleteTriviallyDeadOperandsOfDeadArgument(bi->getAllOperands(), argIndex,
                                                callbacks);
    }
    args.erase(args.begin() + argIndex);
    auto *result = SILBuilderWithScope(bi).createBranch(bi->getLoc(),
                                                        bi->getDestBB(), args);
    branch->eraseFromParent();
    return result;
  }

  llvm_unreachable("unsupported terminator");
}

void swift::erasePhiArgument(SILBasicBlock *block, unsigned argIndex,
                             bool cleanupDeadPhiOps,
                             InstModCallbacks callbacks) {
  SILArgument *arg = block->getArgument(argIndex);
  assert(arg->isPhi() && "Only should be used on phi arguments");
  if (auto *bfi = getBorrowedFromUser(arg)) {
    bfi->replaceAllUsesWith(arg);
    bfi->eraseFromParent();
  }
  block->eraseArgument(argIndex);

  // Determine the set of predecessors in case any predecessor has
  // two edges to this block (e.g. a conditional branch where both
  // sides reach this block).
  //
  // NOTE: This needs to be a SmallSetVector since we need both uniqueness /and/
  // insertion order. Otherwise non-determinism can result.
  BasicBlockSetVector predBlocks(block->getParent());

  for (auto *pred : block->getPredecessorBlocks())
    predBlocks.insert(pred);

  for (auto *pred : predBlocks)
    deleteEdgeValue(pred->getTerminator(), block, argIndex, cleanupDeadPhiOps,
                    callbacks);
}

/// Changes the edge value between a branch and destination basic block
/// at the specified index. Changes all edges from \p branch to \p dest to carry
/// the value.
///
/// \param branch The branch to modify.
/// \param dest The destination of the edge.
/// \param idx The index of the argument to modify.
/// \param Val The new value to use.
/// \return The new branch. Deletes the old one.
/// Changes the edge value between a branch and destination basic block at the
/// specified index.
TermInst *swift::changeEdgeValue(TermInst *branch, SILBasicBlock *dest,
                                 size_t idx, SILValue Val) {
  SILBuilderWithScope builder(branch);

  if (auto *cbi = dyn_cast<CondBranchInst>(branch)) {
    SmallVector<SILValue, 8> trueArgs;
    SmallVector<SILValue, 8> falseArgs;

    OperandValueArrayRef oldTrueArgs = cbi->getTrueArgs();
    bool branchOnTrue = cbi->getTrueBB() == dest;
    assert((!branchOnTrue || idx < oldTrueArgs.size()) && "Not enough edges");

    // Copy the edge values overwriting the edge at idx.
    for (unsigned i = 0, e = oldTrueArgs.size(); i != e; ++i) {
      if (branchOnTrue && idx == i)
        trueArgs.push_back(Val);
      else
        trueArgs.push_back(oldTrueArgs[i]);
    }
    assert(trueArgs.size() == cbi->getTrueBB()->getNumArguments()
           && "Destination block's number of arguments must match");

    OperandValueArrayRef oldFalseArgs = cbi->getFalseArgs();
    bool branchOnFalse = cbi->getFalseBB() == dest;
    assert((!branchOnFalse || idx < oldFalseArgs.size()) && "Not enough edges");

    // Copy the edge values overwriting the edge at idx.
    for (unsigned i = 0, e = oldFalseArgs.size(); i != e; ++i) {
      if (branchOnFalse && idx == i)
        falseArgs.push_back(Val);
      else
        falseArgs.push_back(oldFalseArgs[i]);
    }
    assert(falseArgs.size() == cbi->getFalseBB()->getNumArguments()
           && "Destination block's number of arguments must match");

    cbi = builder.createCondBranch(
        cbi->getLoc(), cbi->getCondition(), cbi->getTrueBB(), trueArgs,
        cbi->getFalseBB(), falseArgs, cbi->getTrueBBCount(),
        cbi->getFalseBBCount());
    branch->dropAllReferences();
    branch->eraseFromParent();
    return cbi;
  }

  if (auto *bi = dyn_cast<BranchInst>(branch)) {
    SmallVector<SILValue, 8> args;

    assert(idx < bi->getNumArgs() && "Not enough edges");
    OperandValueArrayRef oldArgs = bi->getArgs();

    // Copy the edge values overwriting the edge at idx.
    for (unsigned i = 0, e = oldArgs.size(); i != e; ++i) {
      if (idx == i)
        args.push_back(Val);
      else
        args.push_back(oldArgs[i]);
    }
    assert(args.size() == dest->getNumArguments());

    bi = builder.createBranch(bi->getLoc(), bi->getDestBB(), args);
    branch->dropAllReferences();
    branch->eraseFromParent();
    return bi;
  }

  llvm_unreachable("Unhandled terminator leading to merge block");
}

/// Check if the edge from the terminator is critical.
bool swift::isCriticalEdge(TermInst *t, unsigned edgeIdx) {
  assert(t->getSuccessors().size() > edgeIdx && "Not enough successors");

  auto srcSuccs = t->getSuccessors();

  if (srcSuccs.size() <= 1 &&
      // Also consider non-branch instructions with a single successor for
      // critical edges, for example: a switch_enum of a single-case enum.
      (isa<BranchInst>(t) || isa<CondBranchInst>(t)))
    return false;

  SILBasicBlock *destBB = srcSuccs[edgeIdx];
  assert(!destBB->pred_empty() && "There should be a predecessor");
  if (destBB->getSinglePredecessorBlock())
    return false;

  return true;
}

SILBasicBlock *swift::createSplitBranchTarget(SILBasicBlock *targetBlock,
                                              SILBuilder &builder,
                                              SILLocation loc) {
  auto *function = targetBlock->getParent();
  auto *edgeBB = function->createBasicBlockBefore(targetBlock);
  SILBuilderWithScope(edgeBB, builder.getBuilderContext(),
                      builder.getCurrentDebugScope())
      .createBranch(loc, targetBlock);
  return edgeBB;
}

/// Splits the basic block at the iterator with an unconditional branch and
/// updates the dominator tree and loop info.
SILBasicBlock *swift::splitBasicBlockAndBranch(SILBuilder &builder,
                                               SILInstruction *splitBeforeInst,
                                               DominanceInfo *domInfo,
                                               SILLoopInfo *loopInfo) {
  auto *origBB = splitBeforeInst->getParent();
  auto *newBB = origBB->split(splitBeforeInst->getIterator());
  builder.setInsertionPoint(origBB);
  builder.createBranch(splitBeforeInst->getLoc(), newBB);

  // Update the dominator tree.
  if (domInfo) {
    auto origBBDTNode = domInfo->getNode(origBB);
    if (origBBDTNode) {
      // Change the immediate dominators of the children of the block we
      // splitted to the splitted block.
      SmallVector<DominanceInfoNode *, 16> Adoptees(origBBDTNode->begin(),
                                                    origBBDTNode->end());

      auto newBBDTNode = domInfo->addNewBlock(newBB, origBB);
      for (auto *adoptee : Adoptees)
        domInfo->changeImmediateDominator(adoptee, newBBDTNode);
    }
  }

  // Update loop info.
  if (loopInfo)
    if (auto *origBBLoop = loopInfo->getLoopFor(origBB)) {
      origBBLoop->addBasicBlockToLoop(newBB, loopInfo->getBase());
    }

  return newBB;
}

/// Split every edge between two basic blocks.
void swift::splitEdgesFromTo(SILBasicBlock *From, SILBasicBlock *To,
                             DominanceInfo *domInfo, SILLoopInfo *loopInfo) {
  for (unsigned edgeIndex = 0, E = From->getSuccessors().size(); edgeIndex != E;
       ++edgeIndex) {
    SILBasicBlock *succBB = From->getSuccessors()[edgeIndex];
    if (succBB != To)
      continue;
    splitEdge(From->getTerminator(), edgeIndex, domInfo, loopInfo);
  }
}

/// Splits the n-th critical edge from the terminator and updates dominance and
/// loop info if set.
/// Returns the newly created basic block on success or nullptr otherwise (if
/// the edge was not critical.
SILBasicBlock *swift::splitCriticalEdge(TermInst *t, unsigned edgeIdx,
                                        DominanceInfo *domInfo,
                                        SILLoopInfo *loopInfo) {
  if (!isCriticalEdge(t, edgeIdx))
    return nullptr;

  return splitEdge(t, edgeIdx, domInfo, loopInfo);
}

bool swift::splitCriticalEdgesFrom(SILBasicBlock *fromBB,
                                   DominanceInfo *domInfo,
                                   SILLoopInfo *loopInfo) {
  bool changed = false;
  for (unsigned idx = 0, e = fromBB->getSuccessors().size(); idx != e; ++idx) {
    auto *newBB =
        splitCriticalEdge(fromBB->getTerminator(), idx, domInfo, loopInfo);
    changed |= (newBB != nullptr);
  }
  return changed;
}

bool swift::splitCriticalEdgesTo(SILBasicBlock *toBB, DominanceInfo *domInfo,
                                 SILLoopInfo *loopInfo) {
  bool changed = false;
  unsigned numPreds = std::distance(toBB->pred_begin(), toBB->pred_end());

  for (unsigned idx = 0; idx != numPreds; ++idx) {
    SILBasicBlock *fromBB = *std::next(toBB->pred_begin(), idx);
    auto *newBB = splitIfCriticalEdge(fromBB, toBB);
    changed |= (newBB != nullptr);
  }

  return changed;
}

bool swift::hasCriticalEdges(SILFunction &f, bool onlyNonCondBr) {
  for (SILBasicBlock &bb : f) {
    // Only consider critical edges for terminators that don't support block
    // arguments.
    if (onlyNonCondBr && isa<CondBranchInst>(bb.getTerminator()))
      continue;

    if (isa<BranchInst>(bb.getTerminator()))
      continue;

    for (SILBasicBlock *succBB : bb.getSuccessorBlocks()) {
      if (!isNonCriticalEdge(&bb, succBB))
        return true;
    }
  }
  return false;
}

/// Split all critical edges in the function updating the dominator tree and
/// loop information (if they are not set to null).
bool swift::splitAllCriticalEdges(SILFunction &f, DominanceInfo *domInfo,
                                  SILLoopInfo *loopInfo) {
  bool changed = false;

  for (SILBasicBlock &bb : f) {
    if (isa<BranchInst>(bb.getTerminator()))
      continue;

    for (unsigned idx = 0, e = bb.getSuccessors().size(); idx != e; ++idx) {
      auto *newBB =
          splitCriticalEdge(bb.getTerminator(), idx, domInfo, loopInfo);
      assert(!newBB
             || isa<CondBranchInst>(bb.getTerminator())
                    && "Only cond_br may have a critical edge.");
      changed |= (newBB != nullptr);
    }
  }
  return changed;
}

/// Merge the basic block with its successor if possible. If dominance
/// information or loop info is non null update it. Return true if block was
/// merged.
bool swift::mergeBasicBlockWithSuccessor(SILBasicBlock *bb,
                                         DominanceInfo *domInfo,
                                         SILLoopInfo *loopInfo) {
  auto *branch = dyn_cast<BranchInst>(bb->getTerminator());
  if (!branch)
    return false;

  auto *succBB = branch->getDestBB();
  if (bb == succBB || !succBB->getSinglePredecessorBlock())
    return false;

  if (domInfo)
    if (auto *succBBNode = domInfo->getNode(succBB)) {
      // Change the immediate dominator for children of the successor to be the
      // current block.
      auto *bbNode = domInfo->getNode(bb);
      SmallVector<DominanceInfoNode *, 8> Children(succBBNode->begin(),
                                                   succBBNode->end());
      for (auto *ChildNode : Children)
        domInfo->changeImmediateDominator(ChildNode, bbNode);

      domInfo->eraseNode(succBB);
    }

  if (loopInfo)
    loopInfo->removeBlock(succBB);

  mergeBasicBlockWithSingleSuccessor(bb, succBB);

  return true;
}

bool swift::mergeBasicBlocks(SILFunction *f) {
  bool merged = false;
  for (auto bbIter = f->begin(); bbIter != f->end();) {
    if (mergeBasicBlockWithSuccessor(&*bbIter, /*domInfo*/ nullptr,
                                     /*loopInfo*/ nullptr)) {
      merged = true;
      // Continue to merge the current block without advancing.
      continue;
    }
    ++bbIter;
  }
  return merged;
}

/// Splits the critical edges between from and to. This code assumes there is
/// only one edge between the two basic blocks.
SILBasicBlock *swift::splitIfCriticalEdge(SILBasicBlock *from,
                                          SILBasicBlock *to,
                                          DominanceInfo *domInfo,
                                          SILLoopInfo *loopInfo) {
  auto *t = from->getTerminator();
  for (unsigned i = 0, e = t->getSuccessors().size(); i != e; ++i) {
    if (t->getSuccessors()[i] == to)
      return splitCriticalEdge(t, i, domInfo, loopInfo);
  }
  llvm_unreachable("Destination block not found");
}

bool swift::splitAllCondBrCriticalEdgesWithNonTrivialArgs(
    SILFunction &fn, DominanceInfo *domInfo, SILLoopInfo *loopInfo) {
  // Find our targets.
  llvm::SmallVector<std::pair<SILBasicBlock *, unsigned>, 8> targets;
  for (auto &block : fn) {
    auto *cbi = dyn_cast<CondBranchInst>(block.getTerminator());
    if (!cbi)
      continue;

    // See if our true index is a critical edge. If so, add block to the list
    // and continue. If the false edge is also critical, we will handle it at
    // the same time.
    if (isCriticalEdge(cbi, CondBranchInst::TrueIdx)) {
      targets.emplace_back(&block, CondBranchInst::TrueIdx);
    }

    if (!isCriticalEdge(cbi, CondBranchInst::FalseIdx)) {
      continue;
    }

    targets.emplace_back(&block, CondBranchInst::FalseIdx);
  }

  if (targets.empty())
    return false;

  for (auto p : targets) {
    SILBasicBlock *block = p.first;
    unsigned index = p.second;
    auto *result =
        splitCriticalEdge(block->getTerminator(), index, domInfo, loopInfo);
    (void)result;
    assert(result);
  }

  return true;
}

bool isSafeNonExitTerminator(TermInst *ti) {
  switch (ti->getTermKind()) {
  case TermKind::BranchInst:
  case TermKind::CondBranchInst:
  case TermKind::SwitchValueInst:
  case TermKind::SwitchEnumInst:
  case TermKind::SwitchEnumAddrInst:
  case TermKind::DynamicMethodBranchInst:
  case TermKind::CheckedCastBranchInst:
  case TermKind::CheckedCastAddrBranchInst:
    return true;
  case TermKind::UnreachableInst:
  case TermKind::ReturnInst:
  case TermKind::ThrowInst:
  case TermKind::ThrowAddrInst:
  case TermKind::UnwindInst:
    return false;
  // yield is special because it can do arbitrary,
  // potentially-process-terminating things.
  case TermKind::YieldInst:
  case TermKind::AwaitAsyncContinuationInst:
    return false;
  case TermKind::TryApplyInst:
    return true;
  }

  llvm_unreachable("Unhandled TermKind in switch.");
}

bool swift::isTrapNoReturnFunction(SILFunction *f) {
  const char *fatalName = MANGLE_AS_STRING(
      MANGLE_SYM(s18_fatalErrorMessageyys12StaticStringV_AcCSutF));

  // We use ends_with here since if we specialize fatal error we will always
  // prepend the specialization records to fatalName.
  if (!f || !f->getName().ends_with(fatalName))
    return false;

  return true;
}

bool swift::findAllNonFailureExitBBs(
    SILFunction *f, llvm::TinyPtrVector<SILBasicBlock *> &bbs) {
  for (SILBasicBlock &bb : *f) {
    TermInst *ti = bb.getTerminator();

    // If we know that this terminator is not an exit terminator, continue.
    if (isSafeNonExitTerminator(ti))
      continue;

    // A return inst is always a non-failure exit bb.
    if (ti->isFunctionExiting()) {
      bbs.push_back(&bb);
      continue;
    }

    // If we don't have an unreachable inst at this point, this is a terminator
    // we don't understand. Be conservative and return false.
    if (!isa<UnreachableInst>(ti))
      return false;

    // Ok, at this point we know we have a terminator. If it is the only
    // instruction in our bb, it is a failure bb. continue...
    if (ti == &*bb.begin())
      continue;

    // If the unreachable is preceded by a no-return apply inst, then it is a
    // non-failure exit bb. Add it to our list and continue.
    auto prevIter = std::prev(SILBasicBlock::iterator(ti));
    if (auto *ai = dyn_cast<ApplyInst>(&*prevIter)) {
      if (ai->isCalleeNoReturn() &&
          !isTrapNoReturnFunction(ai->getReferencedFunctionOrNull())) {
        bbs.push_back(&bb);
        continue;
      }
    }

    // Otherwise, it must be a failure bb where we leak, continue.
    continue;
  }

  // We understood all terminators, return true.
  return true;
}
