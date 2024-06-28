//===--------- CopyPropagation.cpp - Remove redundant SSA copies. ---------===//
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
/// SSA Copy propagation pass to remove unnecessary copy_value and destroy_value
/// instructions.
///
/// Because this algorithm rewrites copies and destroys without attempting to
/// balance the retain count, it is only sound when SIL is in ownership-SSA
/// form.
///
/// This pass operates independently on each extended lifetime--the lifetime of
/// an OSSA reference after propagating that reference through all copies. For
/// owned references, this is a simple process of canonicalization that can be
/// invoked separately via the CanonicalizeOSSALifetime utility. The
/// CanonicalizeBorrowScope utility handles borrowed references, but this is
/// much more involved. It requires coordination to cleanup owned lifetimes
/// outside the borrow scope after canonicalizing the scope itself.
///
/// This pass also coordinates other transformations that affect lifetime
/// canonicalization:
///
/// - converting extract to destructure
///
/// - sinking owned forwarding operations
///
/// TODO: Cleanup the resulting SIL by deleting instructions that produce dead
/// values (after removing its copies).
///
/// PASS DEPENDENCIES:
/// - ComputeSideEffects
///
/// ANALYSES USED:
/// - BasicCalleeAnalysis
/// - DeadEndBlocksAnalysis
/// - DominanceAnalysis
/// - NonLocalAccessBlockAnalysis
/// - PostOrderAnalysis
///
/// ===----------------------------------------------------------------------===

#define DEBUG_TYPE "copy-propagation"

#include "swift/Basic/Assertions.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalizeBorrowScope.h"
#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/SetVector.h"

using namespace swift;

// Canonicalize borrow scopes.
// This only applies to -O copy-propagation.
llvm::cl::opt<bool>
    EnableRewriteBorrows("canonical-ossa-rewrite-borrows",
                         llvm::cl::init(false),
                         llvm::cl::desc("Enable rewriting borrow scopes"));

namespace {

/// Worklist of defs to be canonicalized. Defs may be revisited after changing
/// their uses.
struct CanonicalDefWorklist {
  bool canonicalizeBorrows;

  llvm::SmallSetVector<SILValue, 16> ownedValues;
  llvm::SmallSetVector<SILValue, 16> borrowedValues;
  // Ideally, ownedForwards is in def-use order.
  llvm::SmallSetVector<SILInstruction *, 16> ownedForwards;

  CanonicalDefWorklist(bool canonicalizeBorrows)
      : canonicalizeBorrows(canonicalizeBorrows) {}

  // Update the worklist for the def corresponding to \p bbi, a BeginBorrow.
  void updateForBorrow(BeginBorrowInst *bbi) { borrowedValues.insert(bbi); }

  // Update the worklist for the def corresponding to \p copy, which is usually
  // a CopyValue, but may be any owned value such as the operand of a
  // DestroyValue (to force lifetime shortening).
  void updateForCopy(SILValue copy) {
    SILValue def = copy;
    // sometimes a destroy_value's operand is not owned.
    if (def->getOwnershipKind() != OwnershipKind::Owned)
      return;

    while (true) {
      def = CanonicalizeOSSALifetime::getCanonicalCopiedDef(def);

      // If the copy's source is guaranteed, find the root of a borrowed
      // extended lifetime.
      if (auto *copy = dyn_cast<CopyValueInst>(def)) {
        if (SILValue borrowDef =
                CanonicalizeBorrowScope::getCanonicalBorrowedDef(
                    copy->getOperand())) {
          if (canonicalizeBorrows || isa<SILFunctionArgument>(borrowDef)) {
            borrowedValues.insert(borrowDef);
            return;
          }
        }
      }
      if (!canonicalizeBorrows) {
        recordOwnedValue(def);
        return;
      }
      // Look through hoistable owned forwarding instructions on the
      // use-def chain.
      if (SILInstruction *defInst = def->getDefiningInstruction()) {
        if (CanonicalizeBorrowScope::isRewritableOSSAForward(defInst)) {
          SILValue forwardedDef = defInst->getOperand(0);
          if (forwardedDef->getOwnershipKind() == OwnershipKind::Owned) {
            def = forwardedDef;
            continue;
          }
        }
      }
      assert(def->getOwnershipKind() == OwnershipKind::Owned
             && "getCanonicalCopiedDef returns owned values");
      // Add any forwarding uses of this owned def. This may include uses that
      // we looked through above, but may also include other uses.
      addForwardingUses(def);
      recordOwnedValue(def);
      return;
    }
  }

  // Add forwarding uses of \p def in def-use order. They will be popped from
  // the list for sinking in use-def order.
  void addForwardingUses(SILValue def) {
    SmallVector<Operand *, 4> useWorklist(def->getUses());
    while (!useWorklist.empty()) {
      auto *user = useWorklist.pop_back_val()->getUser();
      if (auto *copy = dyn_cast<CopyValueInst>(user)) {
        useWorklist.append(copy->getUses().begin(), copy->getUses().end());
        continue;
      }
      if (!CanonicalizeBorrowScope::isRewritableOSSAForward(user))
        continue;

      if (!ownedForwards.insert(user))
        continue;

      for (auto result : user->getResults()) {
        useWorklist.append(result->getUses().begin(), result->getUses().end());
      }
    }
  }

  void erase(SILInstruction *i) {
    for (auto result : i->getResults()) {
      ownedValues.remove(result);
      borrowedValues.remove(result);
    }
    ownedForwards.remove(i);
  }

private:
  void recordOwnedValue(SILValue def) {
    ownedValues.insert(def);
    // Direct copies of owned lexical values are not themselves lexical and
    // consequently need to be canonicalized separately because the
    // canonicalization of the canonical def will respect deinit barriers
    // but canonicalization of the copies should not.
    //
    // Add these copies to the worklist _after_ the canonical def because the
    // worklist is drained backwards and canonicalizing the copies first
    // enables the canonical lexical defs to be further canonicalized.
    if (def->isLexical()) {
      for (auto *cvi : def->getUsersOfType<CopyValueInst>()) {
        ownedValues.insert(cvi);
      }
    }
  }
};

} // namespace

//===----------------------------------------------------------------------===//
//                MARK: Convert struct_extract to destructure
//===----------------------------------------------------------------------===//

/// Convert a struct_extract into a copy + destructure. Return the destructured
/// result or invalid SILValue. The caller must delete the extract and its
/// now-dead copy use.
///
/// Converts:
///   %extract = struct_extract %src : $TypeWithSingleOwnershipValue
///   %copy = copy_value %extract : $OwnershipValue
/// To:
///   %copy = copy_value %src : $TypeWithSingleOwnershipValue
///   (%extracted,...) = destructure %copy : $OwnershipValue
///
/// This allows the ownership of '%src' to be forwarded to its member.
///
/// This utility runs during copy propagation as a prerequisite to
/// CanonicalizeBorrowScopes.
///
/// TODO: generalize this to handle multiple nondebug uses of the
/// struct_extract.
///
/// TODO: generalize this to handle multiple reference member. At that point, it
/// may need to have its own analysis.
static SILValue convertExtractToDestructure(StructExtractInst *extract) {
  if (!hasOneNonDebugUse(extract))
    return nullptr;

  if (!extract->isFieldOnlyNonTrivialField())
    return nullptr;

  auto *extractCopy =
      dyn_cast<CopyValueInst>(getNonDebugUses(extract).begin()->getUser());
  if (!extractCopy)
    return nullptr;

  SILBuilderWithScope builder(extract);
  auto loc = extract->getLoc();
  auto *copy = builder.createCopyValue(loc, extract->getOperand());
  auto *destructure = builder.createDestructureStruct(loc, copy);

  SILValue nonTrivialResult = destructure->getResult(extract->getFieldIndex());
  assert(!nonTrivialResult->getType().isTrivial(*destructure->getFunction())
         && "field idx mismatch");

  extractCopy->replaceAllUsesWith(nonTrivialResult);
  return nonTrivialResult;
}

/// Push copy_value instructions above their struct_extract operands by
/// inserting destructures.
///
/// For types with a single reference member, converts
///   src -> struct_extract -> copy
/// into
///   src -> copy -> destructure
///
/// Returns true if any changes were made. Uses \p deleter for deletion but does
/// not use callbacks for other modifications.
static bool convertExtractsToDestructures(CanonicalDefWorklist &copiedDefs,
                                          InstructionDeleter &deleter) {
  SmallVector<StructExtractInst *, 4> extracts;
  auto pushExtract = [&extracts](CopyValueInst *copy) {
    if (auto *extract = dyn_cast<StructExtractInst>(copy->getOperand())) {
      if (SILValue(extract)->getOwnershipKind() == OwnershipKind::Guaranteed) {
        extracts.push_back(extract);
      }
    }
  };
  for (SILValue v : copiedDefs.ownedValues) {
    auto *copy = dyn_cast<CopyValueInst>(v);
    if (!copy)
      continue;

    pushExtract(copy);
  }
  bool changed = false;
  // extracts may grow as copies are added
  for (unsigned idx = 0; idx < extracts.size(); ++idx) {
    auto *extract = extracts[idx];
    SILValue destructuredResult = convertExtractToDestructure(extract);
    if (!destructuredResult)
      continue;

    changed = true;

    auto *destructure = cast<DestructureStructInst>(
        destructuredResult.getDefiningInstruction());
    auto *newCopy = cast<CopyValueInst>(destructure->getOperand());
    copiedDefs.updateForCopy(newCopy);
    pushExtract(newCopy);

    LLVM_DEBUG(llvm::dbgs() << "Destructure Conversion:\n"
                            << *extract << "  to " << *destructure);

    // Delete both the copy and the extract.
    deleter.recursivelyDeleteUsersIfDead(extract);
  }
  return changed;
}

//===----------------------------------------------------------------------===//
//                MARK: Eliminate redundant moves
//===----------------------------------------------------------------------===//

/// If the specified move_value is redundant (there's no benefit to separating
/// the lifetime at it), replace its uses with uses of the moved-from value and
/// delete it.
static bool eliminateRedundantMove(MoveValueInst *mvi,
                                   InstructionDeleter &deleter,
                                   CanonicalDefWorklist &defWorklist) {
  if (!isRedundantMoveValue(mvi))
    return false;
  auto original = mvi->getOperand();
  mvi->replaceAllUsesWith(original);
  // Call InstructionDeleter::forceDeleteWithUsers to avoid "fixing up"
  // ownership of the moved-from value, i.e. inserting a destroy_value.
  deleter.forceDeleteWithUsers(mvi);
  defWorklist.updateForCopy(original);
  return true;
}

//===----------------------------------------------------------------------===//
//                   MARK: Sink owned forwarding operations
//===----------------------------------------------------------------------===//

/// Find loop preheaders on the reverse path from useBlock to defBlock. Here, a
/// preheader is a block on this path with a CFG successor that is the target of
/// a DFS back edge.
///
/// Uses PostOrderAnalysis to identify back edges.
///
/// Precondition: defBlock and useBlock are weakly control equivalent
/// - defBlock dominates useBlock
/// - useBlock postdominates defBlock
///
/// defBlock maybe within an inner loop relative to useBlock.
static void findPreheadersOnControlEquivalentPath(
    SILBasicBlock *defBlock, SILBasicBlock *useBlock,
    PostOrderFunctionInfo *postorder,
    SmallVectorImpl<SILBasicBlock *> &preheaders) {

  assert(useBlock != defBlock);
  BasicBlockWorklist worklist(useBlock);
  while (auto *bb = worklist.pop()) {
    unsigned rpo = *postorder->getRPONumber(bb);
    bool hasBackedge =
        llvm::any_of(bb->getPredecessorBlocks(), [&](SILBasicBlock *pred) {
          return postorder->getRPONumber(pred) > rpo;
        });
    for (auto *pred : bb->getPredecessorBlocks()) {
      if (hasBackedge && postorder->getRPONumber(pred) < rpo) {
        preheaders.push_back(pred);
      }

      // Follow predecessor even if it's a preheader in case of irreducibility.
      if (pred != defBlock) {
        worklist.pushIfNotVisited(pred);
      }
    }
  }
}

/// Sink \p ownedForward to its uses.
///
/// Owned forwarding instructions are identified by
/// CanonicalizeOSSALifetime::isRewritableOSSAForward().
///
/// Assumes that the uses of ownedForward jointly postdominate it (valid OSSA).
///
/// TODO: consider cloning into each use block (or loop preheader for blocks
/// inside loops).
static bool sinkOwnedForward(SILInstruction *ownedForward,
                             PostOrderAnalysis *postOrderAnalysis,
                             DominanceInfo *domTree) {
  // First find the LCA block to sink this forward without cloning it.
  SILBasicBlock *lca = nullptr;
  for (auto result : ownedForward->getResults()) {
    for (auto *use : result->getUses()) {
      auto *bb = use->getParentBlock();
      lca = lca ? domTree->findNearestCommonDominator(lca, bb) : bb;
    }
  }
  // Find any preheader on the path from ownedForward to lca. Consider them uses
  // and recompute lca.
  auto *defBB = ownedForward->getParent();
  if (lca != defBB) {
    auto *f = defBB->getParent();
    SmallVector<SILBasicBlock *, 4> preheaders;
    findPreheadersOnControlEquivalentPath(defBB, lca, postOrderAnalysis->get(f),
                                          preheaders);
    for (SILBasicBlock *preheader : preheaders) {
      lca = domTree->findNearestCommonDominator(lca, preheader);
    }
  }
  // Mark all uses in this LCA block.
  SmallPtrSet<SILInstruction *, 4> lcaUses;
  for (auto result : ownedForward->getResults()) {
    for (auto *use : result->getUses()) {
      if (use->getParentBlock() != lca)
        continue;

      lcaUses.insert(use->getUser());
    }
  }
  // Find the position in the LCA before the first use.
  SILBasicBlock::iterator forwardPos;
  if (lcaUses.empty()) {
    forwardPos = lca->getTerminator()->getIterator();
  } else {
    // Start at the def or beginning of the block and search forward.
    if (ownedForward->getParent() == lca)
      forwardPos = std::next(ownedForward->getIterator());
    else
      forwardPos = lca->begin();
    while (!lcaUses.count(&*forwardPos)) {
      ++forwardPos;
    }
  }
  if (forwardPos == std::next(ownedForward->getIterator()))
    return false;

  ownedForward->moveBefore(&*forwardPos);
  return true;
}

//===----------------------------------------------------------------------===//
// CopyPropagation: Top-Level Function Transform.
//===----------------------------------------------------------------------===//

namespace {

class CopyPropagation : public SILFunctionTransform {
  /// If true, debug_value instructions should be pruned.
  PruneDebugInsts_t pruneDebug;
  /// If true, all values will be canonicalized.
  bool canonicalizeAll;
  /// If true, then borrow scopes will be canonicalized, allowing copies of
  /// guaranteed values to be optimized. Does *not* shrink the borrow scope.
  bool canonicalizeBorrows;

public:
  CopyPropagation(PruneDebugInsts_t pruneDebug, bool canonicalizeAll,
                  bool canonicalizeBorrows)
      : pruneDebug(pruneDebug), canonicalizeAll(canonicalizeAll),
        canonicalizeBorrows(canonicalizeBorrows) {}

  /// The entry point to this function transformation.
  void run() override;
};

} // end anonymous namespace

/// Top-level pass driver.
void CopyPropagation::run() {
  auto *f = getFunction();
  auto *postOrderAnalysis = getAnalysis<PostOrderAnalysis>();
  auto *accessBlockAnalysis = getAnalysis<NonLocalAccessBlockAnalysis>();
  auto *dominanceAnalysis = getAnalysis<DominanceAnalysis>();
  auto *calleeAnalysis = getAnalysis<BasicCalleeAnalysis>();
  DominanceInfo *domTree = dominanceAnalysis->get(f);

  // Label for unit testing with debug output.
  LLVM_DEBUG(llvm::dbgs() << "*** CopyPropagation: " << f->getName() << "\n");

  // This algorithm fundamentally assumes ownership.
  if (!f->hasOwnership())
    return;

  CanonicalDefWorklist defWorklist(canonicalizeBorrows);
  auto callbacks =
      InstModCallbacks().onDelete([&](SILInstruction *instToDelete) {
        defWorklist.erase(instToDelete);
        instToDelete->eraseFromParent();
      });

  InstructionDeleter deleter(std::move(callbacks));
  bool changed = false;

  StackList<BeginBorrowInst *> beginBorrowsToShrink(f);
  StackList<MoveValueInst *> moveValues(f);

  // Driver: Find all copied or borrowed defs.
  for (auto &bb : *f) {
    for (auto &i : bb) {
      if (auto *copy = dyn_cast<CopyValueInst>(&i)) {
        defWorklist.updateForCopy(copy);
      } else if (auto *borrow = dyn_cast<BeginBorrowInst>(&i)) {
        beginBorrowsToShrink.push_back(borrow);
      } else if (auto *move = dyn_cast<MoveValueInst>(&i)) {
        moveValues.push_back(move);
      } else if (canonicalizeAll) {
        if (auto *destroy = dyn_cast<DestroyValueInst>(&i)) {
          defWorklist.updateForCopy(destroy->getOperand());
        }
      }
    }
  }

  // canonicalizer performs all modifications through deleter's callbacks, so we
  // don't need to explicitly check for changes.
  CanonicalizeOSSALifetime canonicalizer(
      pruneDebug, MaximizeLifetime_t(!getFunction()->shouldOptimize()),
      getFunction(), accessBlockAnalysis, domTree, calleeAnalysis, deleter);
  // NOTE: We assume that the function is in reverse post order so visiting the
  //       blocks and pushing begin_borrows as we see them and then popping them
  //       off the end will result in shrinking inner borrow scopes first.
  for (auto *bbi : beginBorrowsToShrink) {
    bool firstRun = true;
    // Run the sequence of utilities:
    // - ShrinkBorrowScope
    // - CanonicalizeOSSALifetime
    // - LexicalDestroyFolding
    // at least once and then until each stops making changes.
    while (true) {
      SmallVector<CopyValueInst *, 4> modifiedCopyValueInsts;
      auto shrunk = shrinkBorrowScope(*bbi, deleter, calleeAnalysis,
                                      modifiedCopyValueInsts);
      for (auto *cvi : modifiedCopyValueInsts)
        defWorklist.updateForCopy(cvi);
      changed |= shrunk;
      if (!shrunk && !firstRun)
        break;

      // If borrowed value is not owned, neither CanonicalizeOSSALifetime nor
      // LexicalDestroyFolding will do anything with it.  Just bail out now.
      auto borrowee = bbi->getOperand();
      if (borrowee->getOwnershipKind() != OwnershipKind::Owned)
        break;

      auto canonicalized = canonicalizer.canonicalizeValueLifetime(borrowee);
      if (!canonicalized && !firstRun)
        break;

      auto folded = foldDestroysOfCopiedLexicalBorrow(bbi, *domTree, deleter);
      if (!folded)
        break;
      auto hoisted =
          hoistDestroysOfOwnedLexicalValue(folded, *f, deleter, calleeAnalysis);
      // Keep running even if the new move's destroys can't be hoisted.
      (void)hoisted;
      eliminateRedundantMove(folded, deleter, defWorklist);
      firstRun = false;
    }
  }
  for (auto *mvi : moveValues) {
    eliminateRedundantMove(mvi, deleter, defWorklist);
  }
  for (auto *argument : f->getArguments()) {
    if (argument->getOwnershipKind() == OwnershipKind::Owned) {
      hoistDestroysOfOwnedLexicalValue(argument, *f, deleter, calleeAnalysis);
    }
  }
  deleter.cleanupDeadInstructions();

  // For now, only modify forwarding instructions
  // At -Onone, we do nothing but rewrite copies of owned values.
  if (canonicalizeBorrows) {
    // Canonicalize extracts to destructures. struct_extracts are initially part
    // of the copiedDefs. If the are converted, they are removed from copiedDefs
    // and the source of the new destructure is added.
    changed |= convertExtractsToDestructures(defWorklist, deleter);
  }
  // borrowCanonicalizer performs all modifications through deleter's
  // callbacks, so we don't need to explicitly check for changes.
  CanonicalizeBorrowScope borrowCanonicalizer(f, deleter);
  // The utilities in this loop cannot delete borrows before they are popped
  // from the worklist.
  while (true) {
    while (!defWorklist.ownedForwards.empty()) {
      assert(canonicalizeBorrows);

      SILInstruction *ownedForward = defWorklist.ownedForwards.pop_back_val();
      // Delete a dead forwarded value before sinking to avoid this pattern:
      //   %outerVal = destructure_struct %def
      //   destroy %outerVal           <= delete this destroy now
      //   destroy %def                <= so we don't delete this one later
      if (deleter.deleteIfDead(ownedForward)) {
        LLVM_DEBUG(llvm::dbgs() << "  Deleted " << *ownedForward);
        continue;
      }
      // Canonicalize a forwarded owned value before sinking the forwarding
      // instruction, and sink the instruction before canonicalizing the owned
      // value being forwarded. Process 'ownedForwards' in reverse since
      // they may be chained, and CanonicalizeBorrowScopes pushes them
      // top-down.
      for (auto result : ownedForward->getResults()) {
        canonicalizer.canonicalizeValueLifetime(result);
      }
      if (sinkOwnedForward(ownedForward, postOrderAnalysis, domTree)) {
        changed = true;
        // Sinking 'ownedForward' may create an opportunity to sink its
        // operand. This handles chained forwarding instructions that were
        // pushed onto the list out-of-order.
        if (SILInstruction *forwardDef =
                CanonicalizeOSSALifetime::getCanonicalCopiedDef(
                    ownedForward->getOperand(0))
                    ->getDefiningInstruction()) {
          if (CanonicalizeBorrowScope::isRewritableOSSAForward(forwardDef)) {
            defWorklist.ownedForwards.insert(forwardDef);
          }
        }
      }
    }
    if (defWorklist.borrowedValues.empty())
      break;

    BorrowedValue borrow(defWorklist.borrowedValues.pop_back_val());
    assert(canonicalizeBorrows || !borrow.isLocalScope());

    borrowCanonicalizer.canonicalizeBorrowScope(borrow);
    for (CopyValueInst *copy : borrowCanonicalizer.getUpdatedCopies()) {
      defWorklist.updateForCopy(copy);
    }
    // Dead borrow scopes must be removed as uses before canonicalizing the
    // outer copy.
    if (auto *beginBorrow = dyn_cast<BeginBorrowInst>(borrow.value)) {
      if (hasOnlyEndOfScopeOrEndOfLifetimeUses(beginBorrow)) {
        deleter.recursivelyDeleteUsersIfDead(beginBorrow);
      }
    }
    deleter.cleanupDeadInstructions();
  }
  // Canonicalize all owned defs.
  while (!defWorklist.ownedValues.empty()) {
    SILValue def = defWorklist.ownedValues.pop_back_val();
    auto canonicalized = canonicalizer.canonicalizeValueLifetime(def);
    if (!canonicalized)
      continue;
    // Copies of borrowed values may be dead.
    if (auto *inst = def->getDefiningInstruction())
      deleter.trackIfDead(inst);
  }
  // Recursively cleanup dead defs after removing uses.
  deleter.cleanupDeadInstructions();

  // Invalidate analyses.
  if (changed || deleter.hadCallbackInvocation()) {
    // Preserves NonLocalAccessBlockAnalysis.
    accessBlockAnalysis->lockInvalidation();
    invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    accessBlockAnalysis->unlockInvalidation();
    if (f->getModule().getOptions().VerifySILOwnership) {
      auto *deBlocksAnalysis = getAnalysis<DeadEndBlocksAnalysis>();
      f->verifyOwnership(f->getModule().getOptions().OSSAVerifyComplete
                             ? nullptr
                             : deBlocksAnalysis->get(f));
    }
  }
}

// MandatoryCopyPropagation is not currently enabled in the -Onone pipeline
// because it may negatively affect the debugging experience.
SILTransform *swift::createMandatoryCopyPropagation() {
  return new CopyPropagation(PruneDebugInsts, /*canonicalizeAll*/ true,
                             /*canonicalizeBorrows*/ false);
}

SILTransform *swift::createCopyPropagation() {
  return new CopyPropagation(PruneDebugInsts, /*canonicalizeAll*/ true,
                             /*canonicalizeBorrows*/ EnableRewriteBorrows);
}
