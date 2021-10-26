//===--- CanonicalOSSALifetime.h - Canonicalize OSSA lifetimes --*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// Canonicalize the copies and destroys of a single owned OSSA value.
///
/// This top-level API rewrites the extended OSSA lifetime of a SILValue:
///
///     void canonicalizeValueLifetime(SILValue def, CanonicalizeOSSALifetime &)
///
/// The "extended lifetime" of the references defined by 'def' transitively
/// includes the uses of 'def' itself along with the uses of any copies of
/// 'def'. Canonicalization provably minimizes the OSSA lifetime and its copies
/// by rewriting all copies and destroys. Only consusming uses that are not on
/// the liveness boundary require a copy.
///
/// Example #1: The last consuming use ends the reference lifetime.
///
///     bb0(%arg : @owned $T, %addr : @trivial $*T):
///       %copy = copy_value %arg : $T
///       store %copy to [init] %addr : $*T
///       debug_value %addr : $*T, expr op_deref
///       destroy_value %arg : $T
///
/// Will be transformed to:
///
///     bb0(%arg : @owned $T, %addr : @trivial $*T):
///       store %copy to [init] %addr : $*T
///       debug_value %addr : $*T, expr op_deref
///
/// Example #2: Destroys are hoisted to the last use. Copies are inserted only
/// at consumes within the lifetime (to directly satisfy ownership conventions):
///
///     bb0(%arg : @owned $T, %addr : @trivial $*T):
///       %copy1 = copy_value %arg : $T
///       store %arg to [init] %addr : $*T
///       %_ = apply %_(%copy1) : $@convention(thin) (@guaranteed T) -> ()
///       debug_value %addr : $*T, expr op_deref
///       destroy_value %copy1 : $T
///
/// Will be transformed to:
///
///     bb0(%arg : @owned $T, %addr : @trivial $*T):
///       %copy1 = copy_value %arg : $T
///       store %copy1 to [init] %addr : $*T
///       %_ = apply %_(%arg) : $@convention(thin) (@guaranteed T) -> ()
///       destroy_value %arg : $T
///       debug_value %addr : $*T, expr op_deref
///
/// Example #3: Handle control flow.
///
///     bb0(%arg : @owned $T):
///       cond_br %_, bb1, bb2
///     bb1:
///       br bb3
///     bb2:
///       %copy = copy_value %arg : $T
///       %_ = apply %_(%copy) : $@convention(thin) (@guaranteed T) -> ()
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
///       %_ = apply %_(%arg) : $@convention(thin) (@guaranteed T) -> ()
///       destroy_value %arg : $T
///       br bb3
///     bb2:
///       // The original destroy is deleted.
///
/// Pass Requirements:
///
///   This utility invalidates instructions but both uses and preserves
///   NonLocalAccessBlockAnalysis.
///
///   The use-def walks in this utility, e.g. getCanonicalCopiedDef, assume no
///   cycles in the data flow graph without a phi.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_CANONICALOSSALIFETIME_H
#define SWIFT_SILOPTIMIZER_UTILS_CANONICALOSSALIFETIME_H

#include "swift/Basic/GraphNodeWorklist.h"
#include "swift/Basic/SmallPtrSetVector.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"

namespace swift {

extern llvm::Statistic NumCopiesEliminated;
extern llvm::Statistic NumCopiesGenerated;

/// Insert a copy on this operand. Trace and update stats.
void copyLiveUse(Operand *use, InstModCallbacks &instModCallbacks);

/// Diagnose that the given value is a move only type for which \p use causes a
/// need to copy the move only value.
///
/// copy on this operand. Trace and update stats.
void diagnoseRequiredCopyOfMoveOnly(Operand *use,
                                    InstModCallbacks &instModCallbacks);

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

  /// The set of non-destroy consumes that need to be poisoned. This is
  /// determined in two steps. First findOrInsertDestroyInBlock() checks if the
  /// lifetime shrank within the block. Second rewriteCopies() checks if the
  /// consume is in remnantLiveOutBlock(). Finally injectPoison() inserts new
  /// copies and poison destroys for everything in this set.
  SmallPtrSetVector<SILInstruction *, 4> needsPoisonConsumes;

public:
  void clear() {
    finalBlockConsumes.clear();
    debugAfterConsume.clear();
    needsPoisonConsumes.clear();
  }

  bool empty() {
    return finalBlockConsumes.empty() && debugAfterConsume.empty()
           && needsPoisonConsumes.empty();
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

  bool hasUnclaimedConsumes() const { return !finalBlockConsumes.empty(); }

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

  SWIFT_ASSERT_ONLY_DECL(void dump() const LLVM_ATTRIBUTE_USED);
};

/// Canonicalize OSSA lifetimes.
///
/// Allows the allocation of analysis state to be reused across calls to
/// canonicalizeValueLifetime().
///
/// TODO: Move all the private per-definition members into an implementation
/// class in the .cpp file.
class CanonicalizeOSSALifetime {
public:
  /// Find the original definition of a potentially copied value. \p copiedValue
  /// must be an owned value. It is usually a copy but may also be a destroy.
  ///
  /// This single helper specifies the root of an owned extended lifetime. Owned
  /// extended lifetimes may not overlap. This ensures that canonicalization can
  /// run as a utility without invalidating instruction worklists in the client,
  /// as long as the client works on each canonical def independently.
  ///
  /// If the source of a copy is guaranteed, then the copy itself is the root of
  /// an owned extended lifetime. Note that it will also be part of a borrowed
  /// extended lifetime, which will be canonicalized separately by
  /// CanonicalizeBorrowScope.
  ///
  /// This use-def walk must be consistent with the def-use walks performed
  /// within the canonicalizeValueLifetime() and canonicalizeBorrowScopes()
  /// implementations.
  static SILValue getCanonicalCopiedDef(SILValue v) {
    while (auto *copy = dyn_cast<CopyValueInst>(v)) {
      auto def = copy->getOperand();
      if (def.getOwnershipKind() != OwnershipKind::Owned) {
        // This guaranteed value cannot be handled, treat the copy as an owned
        // live range def instead.
        return copy;
      }
      v = def;
    }
    return v;
  }

private:
  /// If true, then debug_value instructions outside of non-debug
  /// liveness may be pruned during canonicalization.
  bool pruneDebugMode;

  /// If true, then new destroy_value instructions will be poison.
  bool poisonRefsMode;

  /// If true and we are processing a value of move_only type, emit a diagnostic
  /// when-ever we need to insert a copy_value.
  std::function<void(Operand *)> moveOnlyCopyValueNotification;

  /// If true and we are processing a value of move_only type, pass back to the
  /// caller any consuming uses that are going to be used as part of the final
  /// lifetime boundary in case we need to emit diagnostics.
  std::function<void(Operand *)> moveOnlyFinalConsumingUse;

  NonLocalAccessBlockAnalysis *accessBlockAnalysis;
  // Lazily initialize accessBlocks only when
  // extendLivenessThroughOverlappingAccess is invoked.
  NonLocalAccessBlocks *accessBlocks = nullptr;

  DominanceInfo *domTree;

  InstructionDeleter &deleter;

  /// Current copied def for which this state describes the liveness.
  SILValue currentDef;

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

  /// Visited set for general def-use traversal that prevents revisiting values.
  GraphNodeWorklist<SILValue, 8> defUseWorklist;

  /// Visited set general CFG traversal that prevents revisiting blocks.
  GraphNodeWorklist<SILBasicBlock *, 8> blockWorklist;

  /// Pruned liveness for the extended live range including copies. For this
  /// purpose, only consuming instructions are considered "lifetime
  /// ending". end_borrows do not end a liverange that may include owned copies.
  PrunedLiveness liveness;

  /// remnantLiveOutBlocks are part of the original extended lifetime that are
  /// not in canonical pruned liveness. There is a path from a PrunedLiveness
  /// boundary to an original destroy that passes through a remnant block.
  ///
  /// These blocks would be equivalent to PrunedLiveness::LiveOut if
  /// PrunedLiveness were recomputed using all original destroys as interesting
  /// uses, minus blocks already marked PrunedLiveness::LiveOut. (Remnant blocks
  /// may be in PrunedLiveness::LiveWithin).
  SmallSetVector<SILBasicBlock *, 8> remnantLiveOutBlocks;

  /// Information about consuming instructions discovered in this canonical OSSA
  /// lifetime.
  CanonicalOSSAConsumeInfo consumes;

public:
  void maybeNotifyMoveOnlyCopy(Operand *use) {
    if (!moveOnlyCopyValueNotification)
      return;
    moveOnlyCopyValueNotification(use);
  }

  void maybeNotifyFinalConsumingUse(Operand *use) {
    if (!moveOnlyFinalConsumingUse)
      return;
    moveOnlyFinalConsumingUse(use);
  }

  CanonicalizeOSSALifetime(
      bool pruneDebugMode, bool poisonRefsMode,
      NonLocalAccessBlockAnalysis *accessBlockAnalysis, DominanceInfo *domTree,
      InstructionDeleter &deleter,
      std::function<void(Operand *)> moveOnlyCopyValueNotification = nullptr,
      std::function<void(Operand *)> moveOnlyFinalConsumingUse = nullptr)
      : pruneDebugMode(pruneDebugMode), poisonRefsMode(poisonRefsMode),
        moveOnlyCopyValueNotification(moveOnlyCopyValueNotification),
        moveOnlyFinalConsumingUse(moveOnlyFinalConsumingUse),
        accessBlockAnalysis(accessBlockAnalysis), domTree(domTree),
        deleter(deleter) {}

  SILValue getCurrentDef() const { return currentDef; }

  void initDef(SILValue def) {
    assert(consumingBlocks.empty() && debugValues.empty() && liveness.empty());
    // Clear the cached analysis pointer just in case the client invalidates the
    // analysis, freeing its memory.
    accessBlocks = nullptr;
    consumes.clear();

    currentDef = def;
    liveness.initializeDefBlock(def->getParentBlock());
  }

  void clearLiveness() {
    consumingBlocks.clear();
    debugValues.clear();
    liveness.clear();
    remnantLiveOutBlocks.clear();
  }

  /// Top-Level API: rewrites copies and destroys within \p def's extended
  /// lifetime. \p lifetime caches transient analysis state across multiple
  /// calls.
  ///
  /// Return true if any change was made to \p def's extended lifetime. \p def
  /// itself will not be deleted and no instructions outside of \p def's
  /// extended lifetime will be affected (only copies and destroys are
  /// rewritten).
  ///
  /// This only deletes instructions within \p def's extended lifetime. Use
  /// InstructionDeleter::cleanUpDeadInstructions() to recursively delete dead
  /// operands.
  bool canonicalizeValueLifetime(SILValue def);

  InstModCallbacks &getCallbacks() { return deleter.getCallbacks(); }

protected:
  void recordDebugValue(DebugValueInst *dvi) {
    debugValues.insert(dvi);
  }

  void recordConsumingUse(Operand *use) {
    consumingBlocks.insert(use->getUser()->getParent());
  }
  bool computeCanonicalLiveness();

  bool endsAccessOverlappingPrunedBoundary(SILInstruction *inst);

  void extendLivenessThroughOverlappingAccess();

  void findOrInsertDestroyInBlock(SILBasicBlock *bb);

  void findOrInsertDestroys();

  void findOrInsertDestroyOnCFGEdge(SILBasicBlock *predBB,
                                    SILBasicBlock *succBB, bool needsPoison);

  void rewriteCopies();

  void injectPoison();
};

} // end namespace swift

#endif
