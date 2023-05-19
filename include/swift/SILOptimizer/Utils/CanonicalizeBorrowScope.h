//===--- CanonicalizeBorrowScope.h - Canonicalize OSSA borrows --*- C++ -*-===//
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
/// This utility canonicalizes borrow scopes by rewriting them to restrict them
/// to only the uses within the scope. To do this, it hoists forwarding
/// operations out of the scope. This exposes many useless scopes that can be
/// deleted, which in turn allows canonicalization of the outer owned values
/// (via CanonicalizeOSSALifetime).
///
/// This does not shrink borrow scopes; it does not rewrite end_borrows.  For
/// that, see ShrinkBorrowScope.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_CANONICALIZEBORROWSCOPES_H
#define SWIFT_SILOPTIMIZER_UTILS_CANONICALIZEBORROWSCOPES_H

#include "swift/Basic/GraphNodeWorklist.h"
#include "swift/Basic/SmallPtrSetVector.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"

namespace swift {

class BasicCalleeAnalysis;

//===----------------------------------------------------------------------===//
//                       MARK: CanonicalizeBorrowScope
//===----------------------------------------------------------------------===//

class CanonicalizeBorrowScope {
public:
  /// Return true if \p inst is an instructions that forwards ownership is its
  /// first operand and can be trivially duplicated, sunk to its uses, hoisted
  /// outside a borrow scope and have it's ownership kind flipped from
  /// guaranteed to owned if needed, as long as OSSA invariants are preserved.
  static bool isRewritableOSSAForward(SILInstruction *inst);

  /// Return the root of a borrowed extended lifetime for \p def or invalid.
  ///
  /// \p def may be any guaranteed value.
  static SILValue getCanonicalBorrowedDef(SILValue def);

private:
  // The borrow that begins this scope.
  BorrowedValue borrowedValue;

  /// The function containing this scope.
  SILFunction *function;

  /// Pruned liveness for the extended live range including copies. For this
  /// purpose, only consuming instructions are considered "lifetime
  /// ending". end_borrows do not end a liverange that may include owned copies.
  BitfieldRef<SSAPrunedLiveness> liveness;

  InstructionDeleter &deleter;

  /// Visited set for general def-use traversal that prevents revisiting values.
  GraphNodeWorklist<SILValue, 8> defUseWorklist;

  /// Visited set general CFG traversal that prevents revisiting blocks.
  GraphNodeWorklist<SILBasicBlock *, 8> blockWorklist;

  /// Record any copies outside the borrow scope that were updated. This
  /// includes the outer copy that us used by outer uses and copies for any
  /// hoisted forwarding instructions.
  SmallVector<CopyValueInst *, 4> updatedCopies;

  /// For borrowed defs, track per-block copies of the borrowed value that only
  /// have uses outside the borrow scope and will not be removed by
  /// canonicalization. These copies are effectively distinct OSSA lifetimes
  /// that should be canonicalized separately.
  llvm::SmallDenseMap<SILBasicBlock *, CopyValueInst *, 4> persistentCopies;

public:
  CanonicalizeBorrowScope(SILFunction *function, InstructionDeleter &deleter)
      : function(function), deleter(deleter) {}

  BorrowedValue getBorrowedValue() const { return borrowedValue; }

  const SSAPrunedLiveness &getLiveness() const { return *liveness; }

  InstructionDeleter &getDeleter() { return deleter; }

  InstModCallbacks &getCallbacks() { return deleter.getCallbacks(); }

  /// Top-level entry point for canonicalizing a SILFunctionArgument. This is a
  /// straightforward canonicalization that can be performed as a stand-alone
  /// utility anywhere.
  bool canonicalizeFunctionArgument(SILFunctionArgument *arg);

  /// Top-level entry point for canonicalizing any borrow scope.
  ///
  /// This creates OSSA compensation code outside the borrow scope. It should
  /// only be called within copy propagation, which knows how to cleanup any new
  /// copies outside the borrow scope, along with hoisted forwarding
  /// instructions, etc.
  bool canonicalizeBorrowScope(BorrowedValue borrow);

  bool hasPersistentCopies() const { return !persistentCopies.empty(); }

  bool isPersistentCopy(CopyValueInst *copy) const {
    auto iter = persistentCopies.find(copy->getParent());
    if (iter == persistentCopies.end()) {
      return false;
    }
    return iter->second == copy;
  }

  // Get all the copies that inserted during canonicalizeBorrowScopes(). These
  // are cleared at the next call to canonicalizeBorrowScopes().
  ArrayRef<CopyValueInst *> getUpdatedCopies() const { return updatedCopies; }

  using OuterUsers = llvm::SmallPtrSet<SILInstruction *, 8>;

  SILValue findDefInBorrowScope(SILValue value);

  template <typename Visitor>
  bool visitBorrowScopeUses(SILValue innerValue, Visitor &visitor);

  void beginVisitBorrowScopeUses() { defUseWorklist.clear(); }

  void recordOuterCopy(CopyValueInst *copy) { updatedCopies.push_back(copy); }

protected:
  void initBorrow(BorrowedValue borrow) {
    assert(borrow && persistentCopies.empty() &&
           (!liveness || liveness->empty()));

    borrowedValue = BorrowedValue();
    defUseWorklist.clear();
    blockWorklist.clear();
    persistentCopies.clear();
    updatedCopies.clear();

    borrowedValue = borrow;
    if (liveness)
      liveness->initializeDef(borrowedValue.value);
  }

  bool computeBorrowLiveness();

  void filterOuterBorrowUseInsts(OuterUsers &outerUseInsts);

  bool consolidateBorrowScope();
};

bool shrinkBorrowScope(
    BeginBorrowInst const &bbi, InstructionDeleter &deleter,
    BasicCalleeAnalysis *calleeAnalysis,
    SmallVectorImpl<CopyValueInst *> &modifiedCopyValueInsts);

MoveValueInst *foldDestroysOfCopiedLexicalBorrow(BeginBorrowInst *bbi,
                                                 DominanceInfo &dominanceTree,
                                                 InstructionDeleter &deleter);

bool hoistDestroysOfOwnedLexicalValue(SILValue const value,
                                      SILFunction &function,
                                      InstructionDeleter &deleter,
                                      BasicCalleeAnalysis *calleeAnalysis);

} // namespace swift

#endif // SWIFT_SILOPTIMIZER_UTILS_CANONICALIZEBORROWSCOPES_H
