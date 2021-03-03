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

public:
  /// Instructions that might end the unowned remnant's lifetime.
  ///
  /// After findOrInsertDestroys, this contains any consume within a LiveWithin
  /// block but outside pruned liveness (occurring after the final consume in
  /// the same block).
  ///
  /// After rewriteCopies, this contains only those consumes in a
  /// block containing a final unowned consume. rewriteUnownedRemnantDestroys
  /// will pick out the final unowned consume in each block.
  SmallSetVector<SILInstruction *, 8> possibleUnownedRemnantConsumes;

  void clear() {
    finalBlockConsumes.clear();
    debugAfterConsume.clear();
    persistentCopies.clear();
    possibleUnownedRemnantConsumes.clear();
  }

  bool hasFinalConsumes() const { return !finalBlockConsumes.empty(); }

  void recordFinalConsume(SILInstruction *inst) {
    assert(!finalBlockConsumes.count(inst->getParent()));
    finalBlockConsumes[inst->getParent()] = inst;
    // If the final consume turned out to be an existing destroy, that destroy
    // might have already been recorded as a possible unowned remnant
    // destroy. Remove it from that set--it can't be both the real destroy and
    // unowned remnant destroy.
    if (auto *destroy = dyn_cast<DestroyValueInst>(inst))
      possibleUnownedRemnantConsumes.remove(destroy);
  }

  // Return true if this instruction is marked as a final consume point of the
  // current def's live range. A consuming instruction can only be claimed once
  // because instructions like `tuple` can consume the same value via multiple
  // operands.
  bool claimConsume(SILInstruction *inst) {
    assert((!isa<DestroyValueInst>(inst)
            || !possibleUnownedRemnantConsumes.count(inst))
           && "don't claim possible unowned remnant destroys");
    auto consumePos = finalBlockConsumes.find(inst->getParent());
    if (consumePos != finalBlockConsumes.end() && consumePos->second == inst) {
      finalBlockConsumes.erase(consumePos);
      return true;
    }
    return false;
  }

  bool isDestoyFinalConsume(DestroyValueInst *destroy) {
    auto consumePos = finalBlockConsumes.find(destroy->getParent());
    return consumePos != finalBlockConsumes.end()
           && consumePos->second == destroy;
  }

  /// Record a debug_value that is known to be outside pruned liveness. Assumes
  /// that instructions are only visited once.
  void recordDebugAfterConsume(DebugValueInst *dvi) {
    debugAfterConsume.push_back(dvi);
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

  void recordPossibleUnownedRemnantConsume(SILInstruction *consume) {
    auto inserted = possibleUnownedRemnantConsumes.insert(consume);
    assert(inserted && "destroy can only be seen once");
    (void)inserted;
  }

  SWIFT_ASSERT_ONLY_DECL(void checkClaimedConsumes());

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

  /// If true, then the lifetime of the currentDef itself will shrink, but a new
  /// lifetime will be created with ReferenceOwnership::Unowned (@sil_unowned)
  /// for any references within that lifetime. This "unowned remnant" is exactly
  /// the same size as the currentDef's original extended lifetime.
  ///
  /// An unknowned remnant does *not* have Unowned ownership. Each reference
  /// copy is an Owned value with ReferenceOwnership::Unowned.
  ///
  /// Note: subsequently running CanonicalizeOSSALifetime with
  /// unownedRemnantMode=false will erase the unowned remnants created by
  /// previous canonicalization.
  bool unownedRemnantMode;

  NonLocalAccessBlockAnalysis *accessBlockAnalysis;
  // Lazily initialize accessBlocks only when
  // extendLivenessThroughOverlappingAccess is invoked.
  NonLocalAccessBlocks *accessBlocks = nullptr;

  DominanceAnalysis *dominanceAnalysis;

  DeadEndBlocks *deBlocks;

  /// Current copied def for which this state describes the liveness.
  SILValue currentDef;

  /// Should an unowned remnant lifetime be created for this def?
  bool shouldCreateUnownedRemnant = false;

  /// If an outer copy is created for uses outside the borrow scope
  CopyValueInst *outerCopy = nullptr;

  /// Cumulatively, have any instructions been modified by canonicalization?
  bool changed = false;

  /// Cummulatively, have any alloc_stack's been created since the last call to
  /// fixStackNesting().
  bool invalidStackNesting = false;

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

  /// unownedRemnantLiveOutBlocks are part of the unowned remnant's lifetime
  /// that are not already in canonical pruned liveness. They are the blocks in
  /// which canonical pruned liveness has ended but the unowned remnant will be
  /// used on at least one successor path. In other words, there is a path
  /// from a PrunedLiveness boundary to an original destroy that passes through
  /// this block.
  ///
  /// These blocks would be equivalent to PrunedLiveness::LiveOut if
  /// PrunedLiveness were recomputed using all original destroys as interesting
  /// uses, minus blocks already marked PrunedLiveness::LiveOut
  SmallSetVector<SILBasicBlock *, 8> unownedRemnantLiveOutBlocks;

  /// Information about consuming instructions discovered in this caonical OSSA
  /// lifetime.
  CanonicalOSSAConsumeInfo consumes;

public:
  CanonicalizeOSSALifetime(bool pruneDebugMode, bool unownedRemnantMode,
                           NonLocalAccessBlockAnalysis *accessBlockAnalysis,
                           DominanceAnalysis *dominanceAnalysis,
                           DeadEndBlocks *deBlocks)
      : pruneDebugMode(pruneDebugMode), unownedRemnantMode(unownedRemnantMode),
        accessBlockAnalysis(accessBlockAnalysis),
        dominanceAnalysis(dominanceAnalysis), deBlocks(deBlocks) {}

  ~CanonicalizeOSSALifetime() {
    assert(!invalidStackNesting && "must call fixStackNesting()");
  }

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

    shouldCreateUnownedRemnant = checkShouldCreateUnownedRemnant();
  }

  void clearLiveness() {
    consumingBlocks.clear();
    debugValues.clear();
    liveness.clear();
    unownedRemnantLiveOutBlocks.clear();
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

  /// Call this at least once after canonicalizing lifetimes before this
  /// instance goes out of scope.
  ///
  /// Returns true if any changes were made.
  bool fixStackNesting(SILFunction *function);

protected:
  bool checkShouldCreateUnownedRemnant();

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

  bool checkPossibleUnownedRemnantConsume(SILInstruction *consume);

  void rewriteCopies();

  void rewriteUnownedRemnantDestroys(
      SmallVectorImpl<SILInstruction *> &instsToDelete);
};

} // end namespace swift

#endif
