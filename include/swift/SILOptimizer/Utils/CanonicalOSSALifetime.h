//===--- CanonicalOSSALifetime.h - Canonicalize OSSA lifetimes --*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// Canonicalize the copies and destroys of a single owned or guaranteed OSSA
/// value.
///
/// This top-level API rewrites the extended OSSA lifetime of a SILValue:
///
///     void canonicalizeValueLifetime(SILValue def, CanonicalOSSALifetime &)
///
/// The extended lifetime transitively includes the uses of `def` itself along
/// with the uses of any copies of `def`. Canonicalization provably minimizes
/// the OSSA lifetime and its copies by rewriting all copies and destroys. Only
/// consusming uses that are not on the liveness boundary require a copy.
///
/// Example #1: Handle consuming and nonconsuming uses.
///
///     bb0(%arg : @owned $T, %addr : @trivial $*T):
///       %copy = copy_value %arg : $T
///       debug_value %copy : $T
///       store %copy to [init] %addr : $*T
///       debug_value %arg : $T
///       debug_value_addr %addr : $*T
///       destroy_value %arg : $T
///
/// Will be transformed to:
///
///     bb0(%arg : @owned $T, %addr : @trivial $*T):
///       // The original copy is deleted.
///       debug_value %arg : $T
///       // A new copy_value is inserted before the consuming store.
///       %copy = copy_value %arg : $T
///       store %copy to [init] %addr : $*T
///       // The non-consuming use now uses the original value.
///       debug_value %arg : $T
///       // A new destroy is inserted after the last use.
///       destroy_value %arg : $T
///       debug_value_addr %addr : $*T
///       // The original destroy is deleted.
///
/// Example #2: Handle control flow.
///
///     bb0(%arg : @owned $T):
///       cond_br %_, bb1, bb2
///     bb1:
///       br bb3
///     bb2:
///       debug_value %arg : $T
///       %copy = copy_value %arg : $T
///       destroy_value %copy : $T
///       br bb3
///     bb3:
///       destroy_value %arg : $T
///
/// Will be transformed to:
///
///     bb0(%arg : @owned $T):
///       cond_br %_, bb1, bb2
///     bb1:
///       destroy_value %arg : $T
///       br bb3
///     bb2:
///       // The original copy is deleted.
///       debug_value %arg : $T
///       destroy_value %arg : $T
///       br bb3
///     bb2:
///       // The original destroy is deleted.
///
/// FIXME: Canonicalization currently bails out if any uses of the def has
/// OperandOwnership::PointerEscape. Once project_box is protected by a borrow
/// scope and mark_dependence is associated with an end_dependence,
/// canonicalization will work everywhere as intended. The intention is to keep
/// the canonicalization algorithm as simple and robust, leaving the remaining
/// performance opportunities contingent on fixing the SIL representation.
///
/// FIXME: Canonicalization currently fails to eliminate copies downstream of a
/// ForwardingBorrow. Aggregates should be fixed to be Reborrow instead of
/// ForwardingBorrow, then computeCanonicalLiveness() can be fixed to extend
/// liveness through ForwardingBorrows.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_CANONICALOSSALIFETIME_H
#define SWIFT_SILOPTIMIZER_UTILS_CANONICALOSSALIFETIME_H

#include "swift/Basic/SmallPtrSetVector.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/Utils/PrunedLiveness.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"

namespace swift {

/// Information about consumes on the extended-lifetime boundary. Consuming uses
/// within the lifetime are not included--they will consume a copy after
/// rewriting. For borrowed def values, the consumes do not include the end of
/// the borrow scope, rather the consumes transitively include consumes of any
/// owned copies of the borrowed value.
///
/// This result remains valid during copy rewriting. The only instructions
/// referenced it contains are consumes that cannot be deleted.
class CanonicalOSSAConsumeInfo {
  /// Map blocks on the lifetime boundary to the last consuming instruction.
  llvm::SmallDenseMap<SILBasicBlock *, SILInstruction *, 4> finalBlockConsumes;

  /// Record any debug_value instructions found after a final consume.
  SmallVector<DebugValueInst *, 8> debugAfterConsume;

  /// For borrowed defs, track per-block copies of the borrowed value that only
  /// have uses outside the borrow scope and will not be removed by
  /// canonicalization. These copies are effectively distinct OSSA lifetimes
  /// that should be canonicalized separately.
  llvm::SmallDenseMap<SILBasicBlock *, CopyValueInst *, 4> persistentCopies;

  /// The set of non-destroy consumes that need to be poisoned. This is
  /// determined in two steps. First findOrInsertDestroyInBlock() checks if the
  /// lifetime shrank within the block. Second rewriteCopies() checks if the
  /// consume is in remnantLiveOutBlock(). Finally injectPoison() inserts new
  /// copies and poison destroys for everything in this set.
  SmallPtrSetVector<SILInstruction *, 4> needsPoisonConsumes;

public:
  bool hasUnclaimedConsumes() const { return !finalBlockConsumes.empty(); }

  void clear() {
    finalBlockConsumes.clear();
    debugAfterConsume.clear();
    persistentCopies.clear();
    needsPoisonConsumes.clear();
  }

  void recordNeedsPoison(SILInstruction *consume) {
    needsPoisonConsumes.insert(consume);
  }

  bool needsPoison(SILInstruction *consume) const {
    return needsPoisonConsumes.count(consume);
  }

  ArrayRef<SILInstruction *> getNeedsPoisonConsumes() const {
    return needsPoisonConsumes.getArrayRef();
  }

  bool hasFinalConsumes() const { return !finalBlockConsumes.empty(); }

  void recordFinalConsume(SILInstruction *inst) {
    assert(!finalBlockConsumes.count(inst->getParent()));
    finalBlockConsumes[inst->getParent()] = inst;
  }

  // Return true if this instruction is marked as a final consume point of the
  // current def's live range. A consuming instruction can only be claimed once
  // because instructions like `tuple` can consume the same value via multiple
  // operands.
  bool claimConsume(SILInstruction *inst) {
    auto destroyPos = finalBlockConsumes.find(inst->getParent());
    if (destroyPos != finalBlockConsumes.end() && destroyPos->second == inst) {
      finalBlockConsumes.erase(destroyPos);
      return true;
    }
    return false;
  }

  /// Record a debug_value that is known to be outside pruned liveness. Assumes
  /// that instructions are only visited once.
  void recordDebugAfterConsume(DebugValueInst *dvi) {
    debugAfterConsume.push_back(dvi);
  }

  void popDebugAfterConsume(DebugValueInst *dvi) {
    if (!debugAfterConsume.empty() && debugAfterConsume.back() == dvi)
      debugAfterConsume.pop_back();
  }

  ArrayRef<DebugValueInst *> getDebugInstsAfterConsume() const {
    return debugAfterConsume;
  }

  bool hasPersistentCopies() const { return !persistentCopies.empty(); }

  bool isPersistentCopy(CopyValueInst *copy) const {
    auto iter = persistentCopies.find(copy->getParent());
    if (iter == persistentCopies.end()) {
      return false;
    }
    return iter->second == copy;
  }

  SWIFT_ASSERT_ONLY_DECL(void dump() const LLVM_ATTRIBUTE_USED);
};

/// Canonicalize OSSA lifetimes.
///
/// Allows the allocation of analysis state to be reused across calls to
/// canonicalizeValueLifetime().
class CanonicalizeOSSALifetime {
public:
  /// Find the original definition of a potentially copied value.
  static SILValue getCanonicalCopiedDef(SILValue v);

private:
  /// If true, then debug_value instructions outside of non-debug
  /// liveness may be pruned during canonicalization.
  bool pruneDebugMode;

  /// If true, then new destroy_value instructions will be poison.
  bool poisonRefsMode;

  NonLocalAccessBlockAnalysis *accessBlockAnalysis;
  // Lazily initialize accessBlocks only when
  // extendLivenessThroughOverlappingAccess is invoked.
  NonLocalAccessBlocks *accessBlocks = nullptr;

  DominanceAnalysis *dominanceAnalysis;

  DeadEndBlocks *deBlocks;

  /// Current copied def for which this state describes the liveness.
  SILValue currentDef;

  /// If an outer copy is created for uses outside the borrow scope
  CopyValueInst *outerCopy = nullptr;

  /// Cumulatively, have any instructions been modified by canonicalization?
  bool changed = false;

  /// Original points in the CFG where the current value's lifetime is consumed
  /// or destroyed. For guaranteed values it remains empty. A backward walk from
  /// these blocks must discover all uses on paths that lead to a return or
  /// throw.
  ///
  /// These blocks are not necessarily in the pruned live blocks since
  /// pruned liveness does not consider destroy_values.
  SmallSetVector<SILBasicBlock *, 8> consumingBlocks;

  /// Record all interesting debug_value instructions here rather then treating
  /// them like a normal use. An interesting debug_value is one that may lie
  /// outisde the pruned liveness at the time it is discovered.
  llvm::SmallPtrSet<DebugValueInst *, 8> debugValues;

  /// Reuse a general worklist for def-use traversal.
  SmallSetVector<SILValue, 8> defUseWorklist;

  /// Reuse a general worklist for CFG traversal.
  SmallSetVector<SILBasicBlock *, 8> blockWorklist;

  /// Pruned liveness for the extended live range including copies. For this
  /// purpose, only consuming instructions are considered "lifetime
  /// ending". end_borrows do not end a liverange that may include owned copies.
  PrunedLiveness liveness;

  /// remnantLiveOutBlocks are part of the original extended lifetime that are
  /// not in canonical pruned liveness. There is a path from a PrunedLiveness
  /// boundary to an original destroy that passes through this block.
  ///
  /// These blocks would be equivalent to PrunedLiveness::LiveOut if
  /// PrunedLiveness were recomputed using all original destroys as interesting
  /// uses, minus blocks already marked PrunedLiveness::LiveOut. (These blocks
  /// may be in PrunedLiveness::LiveWithin).
  SmallSetVector<SILBasicBlock *, 8> remnantLiveOutBlocks;

  /// Information about consuming instructions discovered in this caonical OSSA
  /// lifetime.
  CanonicalOSSAConsumeInfo consumes;

public:
  CanonicalizeOSSALifetime(bool pruneDebugMode, bool poisonRefsMode,
                           NonLocalAccessBlockAnalysis *accessBlockAnalysis,
                           DominanceAnalysis *dominanceAnalysis,
                           DeadEndBlocks *deBlocks)
      : pruneDebugMode(pruneDebugMode), poisonRefsMode(poisonRefsMode),
        accessBlockAnalysis(accessBlockAnalysis),
        dominanceAnalysis(dominanceAnalysis), deBlocks(deBlocks) {}

  SILValue getCurrentDef() const { return currentDef; }

  void initDef(SILValue def) {
    assert(consumingBlocks.empty() && debugValues.empty() && liveness.empty());
    // Clear the cached analysis pointer just in case the client invalidates the
    // analysis, freeing its memory.
    accessBlocks = nullptr;
    consumes.clear();

    currentDef = def;
    outerCopy = nullptr;
    liveness.initializeDefBlock(def->getParentBlock());
  }

  void clearLiveness() {
    consumingBlocks.clear();
    debugValues.clear();
    liveness.clear();
    remnantLiveOutBlocks.clear();
  }

  bool hasChanged() const { return changed; }

  void setChanged() { changed = true; }

  SILValue createdOuterCopy() const { return outerCopy; }

  /// Top-Level API: rewrites copies and destroys within \p def's extended
  /// lifetime. \p lifetime caches transient analysis state across multiple
  /// calls.
  ///
  /// Return false if the OSSA structure cannot be recognized (with a proper
  /// OSSA representation this will always return true).
  ///
  /// Upon returning, isChanged() indicates, cumulatively, whether any SIL
  /// changes were made.
  ///
  /// Upon returning, createdOuterCopy() indicates whether a new copy was
  /// created for uses outside the borrow scope. To canonicalize the new outer
  /// lifetime, call this API again on the value defined by the new copy.
  bool canonicalizeValueLifetime(SILValue def);

protected:
  void recordDebugValue(DebugValueInst *dvi) {
    debugValues.insert(dvi);
  }

  void recordConsumingUse(Operand *use) {
    consumingBlocks.insert(use->getUser()->getParent());
  }

  bool computeBorrowLiveness();

  bool consolidateBorrowScope();

  bool computeCanonicalLiveness();

  bool endsAccessOverlappingPrunedBoundary(SILInstruction *inst);

  void extendLivenessThroughOverlappingAccess();

  void findOrInsertDestroyInBlock(SILBasicBlock *bb);

  void findOrInsertDestroys();

  void insertDestroyOnCFGEdge(SILBasicBlock *predBB, SILBasicBlock *succBB,
                              bool needsPoison);

  void rewriteCopies();

  void injectPoison();
};

} // end namespace swift

#endif
