//===--- BasicBlockUtils.h - Utilities for SILBasicBlock  -------*- C++ -*-===//
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

#ifndef SWIFT_SIL_BASICBLOCKUTILS_H
#define SWIFT_SIL_BASICBLOCKUTILS_H

#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/SILValue.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

class SILFunction;
class SILBasicBlock;
class TermInst;
class DominanceInfo;
class SILLoopInfo;

/// Replace a branch target.
///
/// \param T The terminating instruction to modify.
/// \param edgeIdx The successor edges index that will be replaced.
/// \param newDest The new target block.
/// \param preserveArgs If set, preserve arguments on the replaced edge.
void changeBranchTarget(TermInst *T, unsigned edgeIdx, SILBasicBlock *newDest,
                        bool preserveArgs);

/// Returns the arguments values on the specified CFG edge. If necessary, may
/// add create new SILPHIArguments, using `NewEdgeBB` as the placeholder.
void getEdgeArgs(TermInst *T, unsigned edgeIdx, SILBasicBlock *newEdgeBB,
                 llvm::SmallVectorImpl<SILValue> &args);

/// Splits the edge from terminator.
///
/// Also updates dominance and loop information if not null.
///
/// Returns the newly created basic block.
SILBasicBlock *splitEdge(TermInst *T, unsigned edgeIdx,
                         DominanceInfo *DT = nullptr,
                         SILLoopInfo *LI = nullptr);

/// Merge a basic block ending in a branch with its successor
/// if possible.
void mergeBasicBlockWithSingleSuccessor(SILBasicBlock *BB,
                                        SILBasicBlock *succBB);

/// A utility for finding dead-end blocks.
///
/// Dead-end blocks are blocks from which there is no path to the function exit
/// (either return or throw). These are blocks which end with an unreachable
/// instruction and blocks from which all paths end in "unreachable" blocks.
/// This utility is needed to determine if the a value definition can have a
/// lack of users ignored along a specific path.
class DeadEndBlocks {
  llvm::SetVector<const SILBasicBlock *> reachableBlocks;
  const SILFunction *f;
  bool didComputeValue = false;

  void compute();

public:
  DeadEndBlocks(const SILFunction *f) : f(f) {}

  /// Returns true if \p BB is a dead-end block.
  bool isDeadEnd(const SILBasicBlock *block) {
    if (!didComputeValue) {
      // Lazily compute the dataflow.
      compute();
      didComputeValue = true;
    }
    return reachableBlocks.count(block) == 0;
  }

  /// Return true if this dead end blocks has computed its internal cache yet.
  ///
  /// Used to determine if we need to verify a DeadEndBlocks.
  bool isComputed() const { return didComputeValue; }

  /// Add any (new) blocks that are backward-reachable from \p reachableBB to
  /// the set of reachable blocks.
  void updateForReachableBlock(SILBasicBlock *reachableBB);

  /// Add new blocks to the set of reachable blocks.
  void updateForNewBlock(SILBasicBlock *newBB);

  const SILFunction *getFunction() const { return f; }

  /// Performs a simple check if \p block (or its single successor) ends in an
  /// "unreachable".
  ///
  /// This handles the common case of failure-handling blocks, which e.g.
  /// contain a call to fatalError().
  static bool triviallyEndsInUnreachable(SILBasicBlock *block);

protected:
  void propagateNewlyReachableBlocks(unsigned startIdx);
};

/// Compute joint-postdominating set for \p dominatingBlock and \p
/// dominatedBlockSet found by walking up the CFG from the latter to the
/// former.
///
/// We pass back the following information via callbacks so our callers can
/// use whatever container they need to:
///
/// * inputBlocksFoundDuringWalk: Any blocks from the "dominated
///   block set" that was found as a predecessor block during our traversal is
///   passed to this callback. These can occur for two reasons:
///
///   1. We actually had a block in \p dominatedBlockSet that was reachable
///      from another block in said set. This is a valid usage of the API
///      since it could be that the user does not care about such uses and
///      leave this callback empty.
///
///   2. We had a block in \p dominatedBlockSet that is in a sub-loop in the
///      loop-nest relative to \p dominatingBlock causing us to go around a
///      backedge and hit the block during our traversal. In this case, we
///      have already during the traversal passed the exiting blocks of the
///      sub-loop as joint postdominance completion set blocks. This is useful
///      if one is using this API for lifetime extension purposes of lifetime
///      ending uses and one needs to insert compensating copy_value at these
///      locations due to the lack of strong control-equivalence in between
///      the block and \p dominatingBlock.
///
///
/// * foundJointPostDomSetCompletionBlocks: The set of blocks not in \p
///   dominatedBlockSet that together with \p dominatedBlockSet
///   jointly-postdominate \p dominatedBlock. This is "completing" the joint
///   post-dominance set.
///
/// * inputBlocksInJointPostDomSet: Any of our input blocks that were never
///   found as a predecessor is passed to this callback. This block is in the
///   final minimal joint-postdominance set and is passed to this
///   callback. This is optional and we will avoid doing work if it is not
///   set.
void findJointPostDominatingSet(
    SILBasicBlock *dominatingBlock,
    ArrayRef<SILBasicBlock *> dominatedBlockSet,
    function_ref<void(SILBasicBlock *)> inputBlocksFoundDuringWalk,
    function_ref<void(SILBasicBlock *)> foundJointPostDomSetCompletionBlocks,
    function_ref<void(SILBasicBlock *)> inputBlocksInJointPostDomSet = {});

#ifndef NDEBUG
bool checkDominates(SILBasicBlock *sourceBlock, SILBasicBlock *destBlock);
#endif

/// Walk depth-first the region backwards reachable from the provided roots
/// constrained by \p region's \p isInRegion member function.
///
/// interface Region {
///   /// Whether the indicated basic block is within the region of the graph
///   /// that should be traversed.
///   bool isInRegion(SILBasicBlock *)
/// }
template <typename Region>
struct SILCFGBackwardDFS {
  Region &region;
  ArrayRef<SILBasicBlock *> roots;
  llvm::Optional<SmallVector<SILBasicBlock *, 16>> cachedPostOrder;
  llvm::Optional<BasicBlockSet> cachedVisited;

  SILCFGBackwardDFS(Region &region, ArrayRef<SILBasicBlock *> roots)
      : region(region), roots(roots) {}

  /// Visit the blocks of the region in post-order.
  ///
  /// interface Visitor {
  ///     /// Visit each block in topological order.
  ///     void visit(SILBasicBlock *)
  /// }
  template <typename Visitor>
  void visitPostOrder(Visitor &visitor) {
    if (roots.empty())
      return;
    auto *function = roots.front()->getParent();
    cachedVisited.emplace(function);
    for (auto *root : roots) {
      SmallVector<std::pair<SILBasicBlock *, SILBasicBlock::pred_iterator>, 32>
          stack;
      if (!region.isInRegion(root))
        continue;
      stack.push_back({root, root->pred_begin()});
      while (!stack.empty()) {
        while (stack.back().second != stack.back().first->pred_end()) {
          auto predecessor = *stack.back().second;
          stack.back().second++;
          if (!region.isInRegion(predecessor))
            continue;
          if (cachedVisited->insert(predecessor))
            stack.push_back({predecessor, predecessor->pred_begin()});
        }
        visitor.visit(stack.back().first);
        stack.pop_back();
      }
    }
  }

  /// Visit the region in post-order and cache the visited blocks.
  void cachePostOrder() {
    if (cachedPostOrder)
      return;
    struct Visitor {
      SILCFGBackwardDFS<Region> &dfs;
      void visit(SILBasicBlock *block) {
        dfs.cachedPostOrder->push_back(block);
      }
    };
    cachedPostOrder.emplace();
    Visitor visitor{*this};
    visitPostOrder(visitor);
  }

  /// The region in post-order.
  ArrayRef<SILBasicBlock *> postOrder() {
    cachePostOrder();
    return *cachedPostOrder;
  };

  /// The region in reverse post-order.
  auto reversePostOrder() { return llvm::reverse(postOrder()); }
};
} // namespace swift

#endif
