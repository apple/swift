//===--- BasicBlockOptUtils.h - SIL basic block utilities -------*- C++ -*-===//
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
///
/// Utilities used by the SILOptimizer for analyzing and operating on whole
/// basic blocks, including as removal, cloning, and SSA update.
///
/// CFGOptUtils.h provides lower-level CFG branch and edge utilities.
///
/// SIL/BasicBlockUtils.h provides essential SILBasicBlock utilities.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_BASICBLOCKOPTUTILS_H
#define SWIFT_SILOPTIMIZER_UTILS_BASICBLOCKOPTUTILS_H

#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"

namespace swift {

class BasicBlockCloner;
class SILLoop;
class SILLoopInfo;

/// Compute the set of reachable blocks.
class ReachableBlocks {
  BasicBlockSet visited;

public:
  ReachableBlocks(SILFunction *function) : visited(function) {}

  /// Invoke \p visitor for each reachable block in \p f in worklist order (at
  /// least one predecessor has been visited--defs are always visited before
  /// uses except for phi-type block args). The \p visitor takes a block
  /// argument, which is already marked visited, and must return true to
  /// continue visiting blocks.
  ///
  /// Returns true if all reachable blocks were visited.
  bool visit(function_ref<bool(SILBasicBlock *)> visitor);

  /// Return true if \p bb has been visited.
  bool isVisited(SILBasicBlock *bb) const { return visited.contains(bb); }
};

/// Computes the set of blocks from which a path to the return-block exists.
/// This does not include paths to a throw-block.
class ReachingReturnBlocks {
    BasicBlockWorklist worklist;

public:
  ReachingReturnBlocks(SILFunction *function);

  /// Returns true if there exists a path from \p block to the return-block.
  bool reachesReturn(SILBasicBlock *block) const {
    return worklist.isVisited(block);
  }
};

/// Computes the set of blocks which are not used for error handling, i.e. not
/// (exclusively) reachable from the error-block of a try_apply.
class NonErrorHandlingBlocks {
    BasicBlockWorklist worklist;

public:
  NonErrorHandlingBlocks(SILFunction *function);

  /// Returns true if there exists a path from \p block to the return-block.
  bool isNonErrorHandling(SILBasicBlock *block) const {
    return worklist.isVisited(block);
  }
};

/// Remove all instructions in the body of \p bb in safe manner by using
/// undef.
void clearBlockBody(SILBasicBlock *bb);

/// Handle the mechanical aspects of removing an unreachable block.
void removeDeadBlock(SILBasicBlock *bb);

/// Remove all unreachable blocks in a function.
bool removeUnreachableBlocks(SILFunction &f);

/// Return true if there are any users of v outside the specified block.
inline bool isUsedOutsideOfBlock(SILValue v) {
  auto *bb = v->getParentBlock();
  for (auto *use : v->getUses())
    if (use->getUser()->getParent() != bb)
      return true;
  return false;
}

/// Rotate a loop's header as long as it is exiting and not equal to the
/// passed basic block.
/// If \p RotateSingleBlockLoops is true a single basic block loop will be
/// rotated once. ShouldVerify specifies whether to perform verification after
/// the transformation.
/// Returns true if the loop could be rotated.
bool rotateLoop(SILLoop *loop, DominanceInfo *domInfo, SILLoopInfo *loopInfo,
                bool rotateSingleBlockLoops, SILBasicBlock *upToBB,
                bool shouldVerify);

//===----------------------------------------------------------------------===//
//                             BasicBlock Cloning
//===----------------------------------------------------------------------===//

/// Return true if the \p termInst can be cloned. If termInst produces a
/// guaranteed result, then it must be possible to create a nested borrow scope
/// for that result when the cloner generates a guaranteed phi.
///
/// Note: if all the terminators uses will also be cloned, then a guaranteed phi
/// won't be necessary. This is one of the reasons that cloning a region is much
/// better than cloning a single block at a time.
bool canCloneTerminator(TermInst *termInst);

/// Sink address projections to their out-of-block uses. This is
/// required after cloning a block and before calling
/// updateSSAAfterCloning to avoid address-type phis.
///
/// This clones address projections at their use points, but does not
/// mutate the block containing the projections.
///
/// BasicBlockCloner handles this internally.
class SinkAddressProjections {
  // Projections ordered from last to first in the chain.
  SmallVector<SingleValueInstruction *, 4> projections;
  SmallSetVector<SILValue, 4> inBlockDefs;

  // Transient per-projection data for use during cloning.
  SmallVector<Operand *, 4> usesToReplace;
  llvm::SmallDenseMap<SILBasicBlock *, Operand *, 4> firstBlockUse;

public:
  /// Check for an address projection chain ending at \p inst. Return true if
  /// the given instruction is successfully analyzed.
  ///
  /// If \p inst does not produce an address, then return
  /// true. getInBlockDefs() will contain \p inst if any of its
  /// (non-address) values are used outside its block.
  ///
  /// If \p inst does produce an address, return true only of the
  /// chain of address projections within this block is clonable at
  /// their use sites. getInBlockDefs will return all non-address
  /// operands in the chain that are also defined in this block. These
  /// may require phis after cloning the projections.
  bool analyzeAddressProjections(SILInstruction *inst);

  /// After analyzing projections, returns the list of (non-address) values
  /// defined in the same block as the projections which will have uses outside
  /// the block after cloning.
  ArrayRef<SILValue> getInBlockDefs() const {
    return inBlockDefs.getArrayRef();
  }
  /// Clone the chain of projections at their use sites.
  ///
  /// Return true if anything was done.
  ///
  /// getInBlockProjectionOperandValues() can be called before or after cloning.
  bool cloneProjections();
};

/// Clone a single basic block and any required successor edges within the same
/// function.
///
/// Before cloning, call either canCloneBlock or call canCloneInstruction for
/// every instruction in the original block.
///
/// To clone just the block, call cloneBlock. To also update the original
/// block's branch to jump to the newly cloned block, call cloneBranchTarget
/// instead.
///
/// After cloning, call updateSSAAfterCloning. This is decoupled from cloning
/// because some clients perform CFG edges updates after cloning but before
/// splitting CFG edges.
class BasicBlockCloner : public SILCloner<BasicBlockCloner> {
  using SuperTy = SILCloner<BasicBlockCloner>;
  friend class SILCloner<BasicBlockCloner>;

protected:
  /// The original block to be cloned.
  SILBasicBlock *origBB;

  /// Will cloning require an SSA update?
  bool needsSSAUpdate = false;

  /// Transient object for analyzing a single address projection chain. It's
  /// state is reset each time analyzeAddressProjections is called.
  SinkAddressProjections sinkProj;

  // If available, the current DeadEndBlocks for incremental update.
  DeadEndBlocks *deBlocks;

public:
  /// An ordered list of old to new available value pairs.
  ///
  /// updateSSAAfterCloning() expects this public field to hold values that may
  /// be remapped in the cloned block and live out.
  SmallVector<std::pair<SILValue, SILValue>, 16> availVals;

  // Clone blocks starting at `origBB`, within the same function.
  BasicBlockCloner(SILBasicBlock *origBB, DeadEndBlocks *deBlocks = nullptr)
      : SILCloner(*origBB->getParent()), origBB(origBB), deBlocks(deBlocks) {}

  bool canCloneBlock() {
    for (auto &inst : *origBB) {
      if (!canCloneInstruction(&inst))
        return false;
    }
    canCloneTerminator(origBB->getTerminator());
    return true;
  }

  /// Returns true if \p inst can be cloned.
  ///
  /// If canCloneBlock is not called, then this must be called for every
  /// instruction in origBB, both to ensure clonability and to handle internal
  /// book-keeping (needsSSAUpdate).
  bool canCloneInstruction(SILInstruction *inst) {
    assert(inst->getParent() == origBB);

    if (!inst->isTriviallyDuplicatable())
      return false;

    if (!sinkProj.analyzeAddressProjections(inst))
      return false;

    // Check if any of the non-address defs in the cloned block (including the
    // current instruction) will still have uses outside the block after sinking
    // address projections.
    needsSSAUpdate |= !sinkProj.getInBlockDefs().empty();
    return true;
  }

  void cloneBlock(SILBasicBlock *insertAfterBB = nullptr) {
    sinkAddressProjections();

    SmallVector<SILBasicBlock *, 4> successorBBs;
    successorBBs.reserve(origBB->getSuccessors().size());
    llvm::copy(origBB->getSuccessors(), std::back_inserter(successorBBs));
    cloneReachableBlocks(origBB, successorBBs, insertAfterBB);

    if (deBlocks) {
      for (auto *succBB : successorBBs) {
        deBlocks->updateForReachableBlock(succBB);
      }
    }
  }

  /// Clone the given branch instruction's destination block, splitting
  /// its successors, and rewrite the branch instruction.
  ///
  /// Return false if the branch's destination block cannot be cloned. When
  /// false is returned, no changes have been made.
  void cloneBranchTarget(BranchInst *bi) {
    assert(origBB == bi->getDestBB());

    cloneBlock(/*insertAfter*/ bi->getParent());

    SILBuilderWithScope(bi).createBranch(bi->getLoc(), getNewBB(),
                                         bi->getArgs());
    bi->eraseFromParent();
  }

  /// Helper function to perform SSA updates
  void updateSSAAfterCloning();

  /// Get the newly cloned block corresponding to `origBB`.
  SILBasicBlock *getNewBB() {
    return remapBasicBlock(origBB);
  }

  bool wasCloned() { return isBlockCloned(origBB); }

protected:
  // MARK: CRTP overrides.

  /// Override getMappedValue to allow values defined outside the block to be
  /// cloned to be reused in the newly cloned block.
  SILValue getMappedValue(SILValue value) {
    if (auto si = value->getDefiningInstruction()) {
      if (!isBlockCloned(si->getParent()))
        return value;
    } else if (auto bbArg = dyn_cast<SILArgument>(value)) {
      if (!isBlockCloned(bbArg->getParent()))
        return value;
    } else {
      assert(isa<SILUndef>(value) && "Unexpected Value kind");
      return value;
    }
    // `value` is not defined outside the cloned block, so consult the cloner's
    // map of cloned values.
    return SuperTy::getMappedValue(value);
  }

  void mapValue(SILValue origValue, SILValue mappedValue) {
    SuperTy::mapValue(origValue, mappedValue);
    availVals.emplace_back(origValue, mappedValue);
  }

  void sinkAddressProjections();
};

// Helper class that provides a callback that can be used in
// inliners/cloners for collecting new call sites.
class CloneCollector {
public:
  typedef std::pair<SILInstruction *, SILInstruction *> value_type;
  typedef std::function<void(SILInstruction *, SILInstruction *)> CallbackType;
  typedef std::function<bool (SILInstruction *)> FilterType;

private:
  FilterType filter;

  // Pairs of collected instructions; (new, old)
  llvm::SmallVector<value_type, 4> instructionpairs;

  void collect(SILInstruction *oldI, SILInstruction *newI) {
    if (filter(newI))
      instructionpairs.push_back(std::make_pair(newI, oldI));
  }

public:
  CloneCollector(FilterType filter) : filter(filter) {}

  CallbackType getCallback() {
    return std::bind(&CloneCollector::collect, this, std::placeholders::_1,
                     std::placeholders::_2);
  }

  llvm::SmallVectorImpl<value_type> &getInstructionPairs() {
    return instructionpairs;
  }
};

/// Utility class for cloning init values into the static initializer of a
/// SILGlobalVariable.
class StaticInitCloner : public SILCloner<StaticInitCloner> {
  friend class SILInstructionVisitor<StaticInitCloner>;
  friend class SILCloner<StaticInitCloner>;

  /// The number of not yet cloned operands for each instruction.
  llvm::DenseMap<SILInstruction *, int> numOpsToClone;

  /// List of instructions for which all operands are already cloned (or which
  /// don't have any operands).
  llvm::SmallVector<SILInstruction *, 8> readyToClone;

  SILDebugLocation insertLoc;

  SILInstruction *firstClonedInst = nullptr;

public:
  StaticInitCloner(SILGlobalVariable *gVar)
      : SILCloner<StaticInitCloner>(gVar),
        insertLoc(ArtificialUnreachableLocation(), nullptr) {}

  StaticInitCloner(SILInstruction *insertionPoint)
      : SILCloner<StaticInitCloner>(*insertionPoint->getFunction()),
        insertLoc(insertionPoint->getDebugLocation()) {
    Builder.setInsertionPoint(insertionPoint);
  }

  StaticInitCloner(const SILBuilder &builder)
    : SILCloner<StaticInitCloner>(builder.getFunction()),
      insertLoc(ArtificialUnreachableLocation(), nullptr) {
    Builder.setInsertionPoint(builder.getInsertionBB(), builder.getInsertionPoint());
    if (builder.getInsertionPoint() != builder.getInsertionBB()->end())
      insertLoc = builder.getInsertionPoint()->getDebugLocation();
  }

  /// Add \p InitVal and all its operands (transitively) for cloning.
  ///
  /// Note: all init values must are added, before calling clone().
  /// Returns false if cloning is not possible, e.g. if we would end up cloning
  /// a reference to a private function into a function which is serialized.
  bool add(SILValue initVal);

  /// Clone \p InitVal and all its operands into the initializer of the
  /// SILGlobalVariable.
  ///
  /// \return Returns the cloned instruction in the SILGlobalVariable.
  SILValue clone(SILValue initVal);

  SILInstruction *getFirstClonedInst() const { return firstClonedInst; }

  /// Convenience function to clone a single \p InitVal.
  static void appendToInitializer(SILGlobalVariable *gVar,
                                  SingleValueInstruction *initVal) {
    StaticInitCloner cloner(gVar);
    bool success = cloner.add(initVal);
    (void)success;
    assert(success && "adding initVal cannot fail for a global variable");
    cloner.clone(initVal);
  }

protected:

  void postProcess(SILInstruction *Orig, SILInstruction *Cloned) {
    if (!firstClonedInst)
      firstClonedInst = Cloned;
    SILCloner<StaticInitCloner>::postProcess(Orig, Cloned);
  }

  SILLocation remapLocation(SILLocation loc) {
    return insertLoc.getLocation();
  }

  const SILDebugScope *remapScope(const SILDebugScope *DS) {
    return insertLoc.getScope();
  }
};

} // namespace swift

#endif
