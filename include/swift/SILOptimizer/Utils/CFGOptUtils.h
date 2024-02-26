//===--- CFGOptUtils.h - SIL CFG edge utilities -----------------*- C++ -*-===//
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
/// APIs used by the SILOptimizer for low-level branch and CFG edge analysis
/// and operations. These may merge blocks, split blocks, or create empty
/// blocks, but don't duplicate whole blocks.
///
/// Essential CFG utilities are in SIL/CFG.h.
///
/// Whole block-level transformations are in BasicBlockOptUtils.h.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_CFGOPTUTILS_H
#define SWIFT_SILOPTIMIZER_UTILS_CFGOPTUTILS_H

#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Utils/InstModCallbacks.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"

namespace llvm {
template <typename T> class TinyPtrVector;
}

namespace swift {

class DominanceInfo;
class SILLoop;
class SILLoopInfo;

/// Adds a new argument to an edge between a branch and a destination
/// block. Allows for user injected callbacks via \p callbacks.
///
/// \param branch The terminator to add the argument to.
/// \param dest The destination block of the edge.
/// \param val The value to the arguments of the branch.
/// \return The created branch. The old branch is deleted.
/// The argument is appended at the end of the argument tuple.
TermInst *addNewEdgeValueToBranch(TermInst *branch, SILBasicBlock *dest,
                                  SILValue val, InstructionDeleter &deleter);

/// Changes the edge value between a branch and destination basic block
/// at the specified index. Changes all edges from \p Branch to \p Dest to carry
/// the value.
///
/// \param branch The branch to modify.
/// \param dest The destination of the edge.
/// \param idx The index of the argument to modify.
/// \param val The new value to use.
/// \return The new branch. Deletes the old one.
TermInst *changeEdgeValue(TermInst *branch, SILBasicBlock *dest, size_t idx,
                          SILValue val);

/// Deletes the edge value between a branch and a destination basic block at the
/// specified index. Asserts internally that the argument along the edge does
/// not have uses.
TermInst *deleteEdgeValue(TermInst *branch, SILBasicBlock *destBlock,
                          size_t argIndex, bool cleanupDeadPhiOp = true,
                          InstModCallbacks callbacks = InstModCallbacks());

/// Erase the \p argIndex phi argument from \p block. Asserts that the argument
/// is a /real/ phi argument. Removes all incoming values for the argument from
/// predecessor terminators. Asserts internally that it only ever is given
/// "true" phi argument.
void erasePhiArgument(SILBasicBlock *block, unsigned argIndex,
                      bool cleanupDeadPhiOp = true,
                      InstModCallbacks callbacks = InstModCallbacks());

/// Check if the edge from the terminator is critical.
bool isCriticalEdge(TermInst *t, unsigned edgeIdx);

inline bool isNonCriticalEdge(SILBasicBlock *predBB, SILBasicBlock *succBB) {
  return predBB->getSingleSuccessorBlock() == succBB
    || succBB->getSinglePredecessorBlock() == predBB;
}

/// Splits the edge from terminator if it is critical.
///
/// Updates dominance information and loop information if not null.
/// Returns the newly created basic block on success or nullptr otherwise (if
/// the edge was not critical).
SILBasicBlock *splitCriticalEdge(TermInst *, unsigned edgeIdx,
                                 DominanceInfo *domInfo = nullptr,
                                 SILLoopInfo *loopInfo = nullptr);

/// Splits the critical edges between from and to. This code assumes there is
/// exactly one edge between the two basic blocks. It will return the wrong
/// result if there are multiple edges and will assert if there are no edges in
/// between the two blocks.
///
/// Updates dominance information and loop information if not null.
SILBasicBlock *splitIfCriticalEdge(SILBasicBlock *from, SILBasicBlock *to,
                                   DominanceInfo *domInfo = nullptr,
                                   SILLoopInfo *loopInfo = nullptr);

/// Splits all critical edges originating from `fromBB`.
bool splitCriticalEdgesFrom(SILBasicBlock *fromBB,
                            DominanceInfo *domInfo = nullptr,
                            SILLoopInfo *loopInfo = nullptr);

/// Splits all critical edges that have `toBB` as a destination.
bool splitCriticalEdgesTo(SILBasicBlock *toBB, DominanceInfo *domInfo = nullptr,
                          SILLoopInfo *loopInfo = nullptr);

/// Splits the edges between two basic blocks.
///
/// Updates dominance information and loop information if not null.
void splitEdgesFromTo(SILBasicBlock *from, SILBasicBlock *to,
                      DominanceInfo *domInfo = nullptr,
                      SILLoopInfo *loopInfo = nullptr);

/// Create a basic block to serve as the target of a conditional branch, or
/// other terminator with multiple successors. This avoids introducing critical
/// edges when inserting conditional branches.
///
/// This is a lightweight helper that assumes no block arguments and does not
/// update dominators or loops. For more general functionality, just create the
/// conditional branch then call splitCriticalEdge to fix it up.
SILBasicBlock *createSplitBranchTarget(SILBasicBlock *targetBlock,
                                       SILBuilder &builder,
                                       SILLocation loc);

/// Splits the basic block before the instruction with an unconditional branch
/// and updates the dominator tree and loop info. Returns the new, branched to
/// block that contains the end of \p SplitBeforeInst's block.
SILBasicBlock *splitBasicBlockAndBranch(SILBuilder &builder,
                                        SILInstruction *splitBeforeInst,
                                        DominanceInfo *domInfo,
                                        SILLoopInfo *loopInfo);

/// A version of splitBasicBlockAndBranch that takes a SILBuilderContext instead
/// of a SILBuilder. We generally are trying to eliminate APIs that take in
/// SILBuilder directly since that can cause weird downstream mistakes around
/// debug info scopes. So this provides a better choice for engineers.
///
/// TODO: Migrate all callers of splitBasicBlockAndBranch to use this entry
/// point.
inline SILBasicBlock *splitBasicBlockAndBranch(SILBuilderContext &builderCtx,
                                               SILInstruction *splitBeforeInst,
                                               DominanceInfo *domInfo,
                                               SILLoopInfo *loopInfo) {
  // Make sure we have the right debug scope from split before inst.
  SILBuilderWithScope builder(splitBeforeInst, builderCtx);
  return splitBasicBlockAndBranch(builder, splitBeforeInst, domInfo, loopInfo);
}

/// Return true if the function has a critical edge, false otherwise.
bool hasCriticalEdges(SILFunction &f, bool onlyNonCondBr);

/// Split all critical edges in the given function, updating the
/// dominator tree and loop information if they are provided.
///
/// FIXME: This should never be called! Fix passes that create critical edges.
bool splitAllCriticalEdges(SILFunction &F, DominanceInfo *domInfo,
                           SILLoopInfo *loopInfo);

/// Split all cond_br critical edges with non-trivial arguments in the
/// function updating the dominator tree and loop information (if they are not
/// set to null).
///
/// A current invariant of Ownership SIL is that cond_br can only have critical
/// edges with non-trivial arguments. This simplifies computation.
bool splitAllCondBrCriticalEdgesWithNonTrivialArgs(SILFunction &fn,
                                                   DominanceInfo *domInfo,
                                                   SILLoopInfo *loopInfo);

/// Merge a basic block ending in a branch with its successor
/// if possible. If dominance information or loop info is non null update it.
/// Return true if block was merged.
bool mergeBasicBlockWithSuccessor(SILBasicBlock *bb, DominanceInfo *domInfo,
                                  SILLoopInfo *loopInfo);

/// Merge basic blocks in the given function by eliminating all unconditional
/// branches to single-predecessor branch targets.
///
/// During optimization, SimplifyCFG also handles this, but this is a basic
/// canonicalization after any pass that splits blocks, such as inlining. This
/// is not done on-the-fly after splitting blocks because merging is linear in
/// the number of instructions, so interleaved merging and splitting is
/// quadratic.
bool mergeBasicBlocks(SILFunction *f);

bool isTrapNoReturnFunction(SILFunction *f);

/// Return true if we conservatively find all bb's that are non-failure exit
/// basic blocks and place them in \p bbs. If we find something we don't
/// understand, bail.
///
/// A non-failure exit BB is defined as a BB that:
///
/// 1. Has a return terminator.
/// 2. unreachable + noreturn terminator sequence.
/// 3. has a throw terminator.
///
/// If we just have an unreachable without a noreturn call before it, we must
/// have a failure BB.
///
/// We use a TinyPtrVector since in most cases this will only return one
/// SILBasicBlock since non-failure noreturn functions should not occur often
/// implying in most cases this will be one element.
///
/// TODO:
bool findAllNonFailureExitBBs(SILFunction *f,
                              llvm::TinyPtrVector<SILBasicBlock *> &bbs);

} // end namespace swift

#endif
