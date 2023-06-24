//===-- CanonicalizeOSSALifetime.cpp - Canonicalize OSSA value lifetimes --===//
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
/// This top-level API rewrites the extended lifetime of a SILValue:
///
///     bool CanonicalizeOSSALifetime::canonicalizeValueLifetime(SILValue def)
///
/// Each time it's called on a single OSSA value, `def`, it performs four
/// steps:
///
/// 1. Compute "pruned" liveness of def and its copies, ignoring original
///    destroys. Initializes `liveness`.
///
/// 2. Find the "original" boundary of liveness using
///    PrunedLiveness::computeBoundary.
///
/// 3. (Optional) At Onone, extend liveness up to original extent when possible
///    without incurring extra copies.
///
/// 4. Find the "extended" boundary of liveness by walking out from the boundary
///    computed by PrunedLiveness out to destroys which aren't separated from
///    the original destory by "interesting" instructions.
///
/// 5. Initializes `consumes` and inserts new destroy_value instructions.
///
/// 6. Rewrite `def`s original copies and destroys, inserting new copies where
///    needed. Deletes original copies and destroys and inserts new copies.
///
/// See CanonicalizeOSSALifetime.h for examples.
///
/// TODO: Canonicalization currently bails out if any uses of the def has
/// OperandOwnership::PointerEscape. Once project_box is protected by a borrow
/// scope and mark_dependence is associated with an end_dependence, those will
/// no longer be represented as PointerEscapes, and canonicalization will
/// naturally work everywhere as intended. The intention is to keep the
/// canonicalization algorithm as simple and robust, leaving the remaining
/// performance opportunities contingent on fixing the SIL representation.
///
/// TODO: Replace BasicBlock SmallDenseMaps with inlined bits;
/// see BasicBlockDataStructures.h.
///
/// TODO: This algorithm would be extraordinarily simple and cheap except for
/// the following issues:
///
/// 1. Liveness is extended by any overlapping begin/end_access scopes. This
/// avoids calling a destructor within an exclusive access. A simpler
/// alternative would be to model all end_access instructions as deinit
/// barriers, but that may significantly limit optimization.
///
/// 2. Liveness is extended out to original destroys to avoid spurious changes.
///
/// 3. In the Onone mode, liveness is preserved to its previous extent whenever
/// doing so doesn't incur extra copies compared to what is done in the O mode.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "copy-propagation"

#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/NodeDatastructures.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/Analysis/Reachability.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/DebugOptUtils.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"
#include "llvm/ADT/Statistic.h"

using namespace swift;
using llvm::SmallSetVector;

llvm::Statistic swift::NumCopiesAndMovesEliminated = {
    DEBUG_TYPE, "NumCopiesAndMovesEliminated",
    "number of copy_value and move_value instructions removed"};

llvm::Statistic swift::NumCopiesGenerated = {
    DEBUG_TYPE, "NumCopiesGenerated",
    "number of copy_value instructions created"};

STATISTIC(NumDestroysEliminated,
          "number of destroy_value instructions removed");
STATISTIC(NumDestroysGenerated, "number of destroy_value instructions created");

//===----------------------------------------------------------------------===//
//                           MARK: General utilities
//===----------------------------------------------------------------------===//

template <typename... T, typename... U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
                     U &&...args) {
  Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

/// Is \p instruction a destroy_value whose operand is \p def, or its
/// transitive copy.
static bool isDestroyOfCopyOf(SILInstruction *instruction, SILValue def) {
  auto *destroy = dyn_cast<DestroyValueInst>(instruction);
  if (!destroy)
    return false;
  auto destroyed = destroy->getOperand();
  while (true) {
    if (destroyed == def)
      return true;
    auto *copy = dyn_cast<CopyValueInst>(destroyed);
    if (!copy)
      break;
    destroyed = copy->getOperand();
  }
  return false;
}

//===----------------------------------------------------------------------===//
// MARK: Step 1. Compute pruned liveness
//===----------------------------------------------------------------------===//

bool CanonicalizeOSSALifetime::computeCanonicalLiveness() {
  defUseWorklist.initialize(getCurrentDef());
  // Only the first level of reborrows need to be consider. All nested inner
  // adjacent reborrows and phis are encapsulated within their lifetimes.
  SILPhiArgument *arg;
  if ((arg = dyn_cast<SILPhiArgument>(getCurrentDef())) && arg->isPhi()) {
    visitInnerAdjacentPhis(arg, [&](SILArgument *reborrow) {
      defUseWorklist.insert(reborrow);
      return true;
    });
  }
  while (SILValue value = defUseWorklist.pop()) {
    for (Operand *use : value->getUses()) {
      auto *user = use->getUser();
      // Recurse through copies.
      if (auto *copy = dyn_cast<CopyValueInst>(user)) {
        defUseWorklist.insert(copy);
        continue;
      }
      // Handle debug_value instructions separately.
      if (pruneDebugMode) {
        if (auto *dvi = dyn_cast<DebugValueInst>(user)) {
          // Only instructions potentially outside current pruned liveness are
          // interesting.
          if (liveness->getBlockLiveness(dvi->getParent())
              != PrunedLiveBlocks::LiveOut) {
            recordDebugValue(dvi);
          }
          continue;
        }
      }
      switch (use->getOperandOwnership()) {
      case OperandOwnership::NonUse:
        break;
      case OperandOwnership::TrivialUse:
        llvm_unreachable("this operand cannot handle ownership");

      // Conservatively treat a conversion to an unowned value as a pointer
      // escape. Is it legal to canonicalize ForwardingUnowned?
      case OperandOwnership::ForwardingUnowned:
      case OperandOwnership::PointerEscape:
        return false;
      case OperandOwnership::InstantaneousUse:
      case OperandOwnership::UnownedInstantaneousUse:
      case OperandOwnership::BitwiseEscape:
        liveness->updateForUse(user, /*lifetimeEnding*/ false);
        break;
      case OperandOwnership::ForwardingConsume:
        recordConsumingUse(use);
        liveness->updateForUse(user, /*lifetimeEnding*/ true);
        break;
      case OperandOwnership::DestroyingConsume:
        if (isDestroyOfCopyOf(user, getCurrentDef())) {
          destroys.insert(user);
        } else {
          // destroy_value of a transitive copy of the currentDef does not
          // force pruned liveness (but store etc. does).

          // Even though this instruction is a DestroyingConsume of its operand,
          // if it's a destroy_value whose operand is not a transitive copy of
          // currentDef, then it's just ending an implicit borrow of currentDef,
          // not consuming it.
          auto lifetimeEnding = !isa<DestroyValueInst>(user);
          liveness->updateForUse(user, lifetimeEnding);
        }
        recordConsumingUse(use);
        break;
      case OperandOwnership::Borrow:
        if (liveness->updateForBorrowingOperand(use)
            != InnerBorrowKind::Contained) {
          return false;
        }
        break;
      case OperandOwnership::InteriorPointer:
      case OperandOwnership::GuaranteedForwarding:
      case OperandOwnership::EndBorrow:
        // Guaranteed values are exposed by inner adjacent reborrows. If user is
        // a guaranteed phi (GuaranteedForwarding), then the owned lifetime
        // either dominates it or its lifetime ends at an outer adjacent
        // reborrow. Only instructions that end the reborrow lifetime should
        // actually affect liveness of the outer owned value.
        liveness->updateForUse(user, /*lifetimeEnding*/ false);
        break;
      case OperandOwnership::Reborrow:
        BranchInst *branch = cast<BranchInst>(user);
        // This is a cheap variation on visitEnclosingDef. We already know that
        // getCurrentDef() is the enclosing def for this use. If the reborrow's
        // has a enclosing def is an outer adjacent phi then this branch must
        // consume getCurrentDef() as the outer phi operand.
        if (is_contained(branch->getOperandValues(), getCurrentDef())) {
          // An adjacent phi consumes the value being reborrowed. Although this
          // use doesn't end the lifetime, this branch does end the lifetime by
          // consuming the owned value.
          liveness->updateForUse(branch, /*lifetimeEnding*/ true);
          break;
        }
        // No adjacent phi consumes the value.  This use is not lifetime ending.
        liveness->updateForUse(branch, /*lifetimeEnding*/ false);
        // This branch reborrows a guaranteed phi whose lifetime is dependent on
        // currentDef.  Uses of the reborrowing phi extend liveness.
        auto *reborrow = PhiOperand(use).getValue();
        defUseWorklist.insert(reborrow);
        break;
      }
    }
  }
  return true;
}

void CanonicalizeOSSALifetime::findDestroysOutsideBoundary(
    SmallVectorImpl<SILInstruction *> &outsideDestroys) {
  for (auto destroy : destroys) {
    if (liveness->isWithinBoundary(destroy))
      continue;
    outsideDestroys.push_back(destroy);
  }
}

void CanonicalizeOSSALifetime::extendLivenessToDeinitBarriers() {
  SmallVector<SILInstruction *, 4> outsideDestroys;
  findDestroysOutsideBoundary(outsideDestroys);

  auto *def = getCurrentDef()->getDefiningInstruction();
  using InitialBlocks = ArrayRef<SILBasicBlock *>;
  auto *defBlock = getCurrentDef()->getParentBlock();
  auto initialBlocks = defBlock ? InitialBlocks(defBlock) : InitialBlocks();
  ReachableBarriers barriers;
  findBarriersBackward(outsideDestroys, initialBlocks,
                       *getCurrentDef()->getFunction(), barriers,
                       [&](auto *inst) {
                         if (inst == def)
                           return true;
                         if (!isDeinitBarrier(inst, calleeAnalysis))
                           return false;
                         // For the most part, instructions that are deinit
                         // barriers in the abstract are also deinit barriers
                         // for the purposes of canonicalizing def's lifetime.
                         //
                         // There is an important exception: transferring an
                         // owned lexical lifetime into a callee.  If the
                         // instruction is a full apply which consumes def,
                         // then it isn't a deinit barrier.  Keep looking for
                         // barriers above it.
                         auto apply = FullApplySite::isa(inst);
                         if (!apply)
                           return true;
                         return liveness->isInterestingUser(inst) !=
                                PrunedLiveness::IsInterestingUser::
                                    LifetimeEndingUse;
                       });
  for (auto *barrier : barriers.instructions) {
    liveness->updateForUse(barrier, /*lifetimeEnding*/ false);
  }
  for (auto *barrier : barriers.phis) {
    for (auto *predecessor : barrier->getPredecessorBlocks()) {
      liveness->updateForUse(predecessor->getTerminator(),
                             /*lifetimeEnding*/ false);
    }
  }
  // Ignore barriers.edges.  The beginning of the targets of such edges should
  // not be added to liveness.  These edges will be rediscovered when computing
  // the liveness boundary.
}

// Return true if \p inst is an end_access whose access scope overlaps the end
// of the pruned live range. This means that a hoisted destroy might execute
// within the access scope which previously executed outside the access scope.
//
// Not overlapping (ignored):
//
//     %def
//     use %def     // pruned liveness ends here
//     begin_access // access scope unrelated to def
//     end_access
//
// Overlapping (must extend pruned liveness):
//
//     %def
//     begin_access // access scope unrelated to def
//     use %def     // pruned liveness ends here
//     end_access
//
// Overlapping (must extend pruned liveness):
//
//     begin_access // access scope unrelated to def
//     %def
//     use %def     // pruned liveness ends here
//     end_access
//
bool CanonicalizeOSSALifetime::
endsAccessOverlappingPrunedBoundary(SILInstruction *inst) {
  if (isa<EndUnpairedAccessInst>(inst)) {
    return true;
  }
  auto *endAccess = dyn_cast<EndAccessInst>(inst);
  if (!endAccess) {
    return false;
  }
  auto *beginAccess = endAccess->getBeginAccess();
  SILBasicBlock *beginBB = beginAccess->getParent();
  switch (liveness->getBlockLiveness(beginBB)) {
  case PrunedLiveBlocks::LiveOut:
    // Found partial overlap of the form:
    //     currentDef
    //     beginAccess
    //     br...
    //   bb...
    //     use
    //     endAccess
    return true;
  case PrunedLiveBlocks::LiveWithin:
    // Check for partial overlap of this form where beginAccess and the last use
    // are in the same block:
    //     currentDef
    //     beginAccess
    //     use
    //     endAccess
    if (std::find_if(std::next(beginAccess->getIterator()), beginBB->end(),
                     [this](SILInstruction &nextInst) {
                       return liveness->isInterestingUser(&nextInst)
                              != PrunedLiveness::NonUser;
                     })
        != beginBB->end()) {
      // An interesting use after the beginAccess means overlap.
      return true;
    }
    return false;
  case PrunedLiveBlocks::Dead:
    // Check for partial overlap of this form where beginAccess and currentDef
    // are in different blocks:
    //     beginAccess
    //     br...
    //  bb...
    //     currentDef
    //     endAccess
    //
    // Since beginAccess is not within the canonical live range, its access
    // scope overlaps only if there is a path from beginAccess to currentDef
    // that does not pass through endAccess. endAccess is dominated by
    // both currentDef and begin_access. Therefore, such a path only exists if
    // beginAccess dominates currentDef.
    return domTree->properlyDominates(beginAccess->getParent(),
                                      getCurrentDef()->getParentBlock());
  }
  llvm_unreachable("covered switch");
}

// Find all overlapping access scopes and extend pruned liveness to cover them:
//
// This may also unnecessarily, but conservatively extend liveness over some
// originally overlapping access, such as:
//
//     begin_access // access scope unrelated to def
//     %def
//     use %def
//     destroy %def
//     end_access
//
// Or:
//
//     %def
//     begin_access // access scope unrelated to def
//     use %def
//     destroy %def
//     end_access
//
// To minimize unnecessary lifetime extension, only search for end_access
// within dead blocks that are backward reachable from an original destroy.
//
// Note that lifetime extension is iterative because adding a new liveness use
// may create new overlapping access scopes. This can happen because there is no
// guarantee of strict stack discipline across unrelated access. For example:
//
//     %def
//     begin_access A
//     use %def        // Initial pruned lifetime boundary
//     begin_access B
//     end_access A    // Lifetime boundary after first extension
//     end_access B    // Lifetime boundary after second extension
//     destroy %def
//
// If the lifetime extension did not iterate, then def would be destroyed within
// B's access scope when originally it was destroyed outside that scope.
void CanonicalizeOSSALifetime::extendLivenessThroughOverlappingAccess() {
  this->accessBlocks = accessBlockAnalysis->get(getCurrentDef()->getFunction());

  // Visit each original consuming use or destroy as the starting point for a
  // backward CFG traversal. This traversal must only visit blocks within the
  // original extended lifetime.
  bool changed = true;
  while (changed) {
    changed = false;
    // The blocks in which we may have to extend liveness over access scopes.
    //
    // It must be populated first so that we can test membership during the loop
    // (see findLastConsume).
    BasicBlockSetVector blocksToVisit(getCurrentDef()->getFunction());
    for (auto *block : consumingBlocks) {
      blocksToVisit.insert(block);
    }
    for (auto iterator = blocksToVisit.begin(); iterator != blocksToVisit.end();
         ++iterator) {
      auto *bb = *iterator;
      // If the block isn't dead, then we won't need to extend liveness within
      // any of its predecessors (though we may within it).
      if (liveness->getBlockLiveness(bb) != PrunedLiveBlocks::Dead)
        continue;
      // Continue searching upward to find the pruned liveness boundary.
      for (auto *predBB : bb->getPredecessorBlocks()) {
        blocksToVisit.insert(predBB);
      }
    }
    for (auto *bb : blocksToVisit) {
      auto blockLiveness = liveness->getBlockLiveness(bb);
      // Ignore blocks within pruned liveness.
      if (blockLiveness == PrunedLiveBlocks::LiveOut) {
        continue;
      }
      if (blockLiveness == PrunedLiveBlocks::Dead) {
        // Otherwise, ignore dead blocks with no nonlocal end_access.
        if (!accessBlocks->containsNonLocalEndAccess(bb)) {
          continue;
        }
      }
      bool blockHasUse = (blockLiveness == PrunedLiveBlocks::LiveWithin);
      // Find the latest partially overlapping access scope, if one exists:
      //     use %def // pruned liveness ends here
      //     end_access

      // Whether to look for the last consume in the block.
      //
      // We need to avoid extending liveness over end_accesses that occur after
      // original liveness ended.
      bool findLastConsume =
          consumingBlocks.contains(bb)
          && llvm::none_of(bb->getSuccessorBlocks(), [&](auto *successor) {
               return blocksToVisit.contains(successor)
                      && liveness->getBlockLiveness(successor)
                             == PrunedLiveBlocks::Dead;
             });
      for (auto &inst : llvm::reverse(*bb)) {
        if (findLastConsume) {
          findLastConsume = !destroys.contains(&inst);
          continue;
        }
        // Stop at the latest use. An earlier end_access does not overlap.
        if (blockHasUse
            && liveness->isInterestingUser(&inst) != PrunedLiveness::NonUser) {
          break;
        }
        if (endsAccessOverlappingPrunedBoundary(&inst)) {
          liveness->updateForUse(&inst, /*lifetimeEnding*/ false);
          changed = true;
          break;
        }
      }
      // If liveness changed, might as well restart CFG traversal.
      if (changed) {
        break;
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// MARK: Step 2. Find the "original" (unextended) boundary determined by the
//               liveness built up in step 1.
//===----------------------------------------------------------------------===//

void CanonicalizeOSSALifetime::findOriginalBoundary(
    PrunedLivenessBoundary &boundary) {
  assert(boundary.lastUsers.size() == 0 && boundary.boundaryEdges.size() == 0 &&
         boundary.deadDefs.size() == 0);
  liveness->computeBoundary(boundary, consumingBlocks.getArrayRef());
}

//===----------------------------------------------------------------------===//
// MARK: Step 3. (Optional) Maximize lifetimes.
//===----------------------------------------------------------------------===//

/// At -Onone, there are some conflicting goals:
/// On the one hand: good debugging experience.
/// (1) do not shorten value's lifetime
/// On the other: demonstrate semantics.
/// (2) consume value at same places it will be consumed at -O
/// (3) ensure there are no more copies than there would be at -O
///
/// (2) and (3) are equivalent--extra (compared to -O) copies arise from failing
/// to end lifetimes at consuming uses (which then need their own copies).
///
/// We achieve (2) and (3) always.  We achieve (1) where possible.
///
/// Conceptually, the strategy is the following:
/// - Collect the blocks in which the value was live before canonicalization.
///   These are the "original" live blocks (originalLiveBlocks).
///   [Color these blocks green.]
/// - From within that collection, collect the blocks which contain a _final_
///   consuming, non-destroy use, and their iterative successors.
///   These are the "consumed" blocks (consumedAtExitBlocks).
///   [Color these blocks red.]
/// - Extend liveness down to the boundary between originalLiveBlocks and
///   consumedAtExitBlocks blocks.
///   [Extend liveness down to the boundary between green blocks and red.]
/// - In particular, in regions of originalLiveBlocks which have no boundary
///   with consumedAtExitBlocks, liveness should be extended to its original
///   extent.
///   [Extend liveness down to the boundary between green blocks and uncolored.]
void CanonicalizeOSSALifetime::extendUnconsumedLiveness(
    PrunedLivenessBoundary const &boundary) {
  auto currentDef = getCurrentDef();

  // First, collect the blocks that were _originally_ live.  We can't use
  // liveness here because it doesn't include blocks that occur before a
  // destroy_value.
  BasicBlockSet originalLiveBlocks(currentDef->getFunction());
  {
    // Some of the work here was already done by computeCanonicalLiveness.
    // Specifically, it already discovered all blocks containing (transitive)
    // uses and blocks that appear between them and the def.
    //
    // Seed the set with what it already discovered.
    for (auto *discoveredBlock : liveness->getDiscoveredBlocks())
      originalLiveBlocks.insert(discoveredBlock);

    // Start the walk from the consuming blocks (which includes destroys as well
    // as the other consuming uses).
    BasicBlockWorklist worklist(currentDef->getFunction());
    for (auto *consumingBlock : consumingBlocks) {
      worklist.push(consumingBlock);
    }

    // Walk backwards from consuming blocks.
    while (auto *block = worklist.pop()) {
      if (!originalLiveBlocks.insert(block))
        continue;
      for (auto *predecessor : block->getPredecessorBlocks()) {
        // If the block was discovered by liveness, we already added it to the
        // set.
        if (originalLiveBlocks.contains(predecessor))
          continue;
        worklist.pushIfNotVisited(predecessor);
      }
    }
  }

  // Second, collect the blocks which contain a _final_ consuming use and their
  // iterative successors within the originalLiveBlocks.
  BasicBlockSet consumedAtExitBlocks(currentDef->getFunction());
  // The subset of consumedAtExitBlocks which do not contain a _final_ consuming
  // use, i.e. the subset that is dead.
  StackList<SILBasicBlock *> consumedAtEntryBlocks(currentDef->getFunction());
  {
    // Start the forward walk from blocks which contain _final_ non-destroy
    // consumes. These are just the instructions on the boundary which aren't
    // destroys.
    BasicBlockWorklist worklist(currentDef->getFunction());
    for (auto *instruction : boundary.lastUsers) {
      if (destroys.contains(instruction))
        continue;
      if (liveness->isInterestingUser(instruction)
          != PrunedLiveness::IsInterestingUser::LifetimeEndingUse)
        continue;
      worklist.push(instruction->getParent());
    }
    while (auto *block = worklist.pop()) {
      consumedAtExitBlocks.insert(block);
      for (auto *successor : block->getSuccessorBlocks()) {
        if (!originalLiveBlocks.contains(successor))
          continue;
        worklist.pushIfNotVisited(successor);
        consumedAtEntryBlocks.push_back(successor);
      }
    }
  }

  // Third, find the blocks on the boundary between the originalLiveBlocks
  // blocks and the consumedAtEntryBlocks blocks.  Extend liveness "to the end"
  // of these blocks.
  for (auto *block : consumedAtEntryBlocks) {
    for (auto *predecessor : block->getPredecessorBlocks()) {
      if (consumedAtExitBlocks.contains(predecessor))
        continue;
      // Add "the instruction(s) before the terminator" of the predecessor to
      // liveness.
      if (auto *inst = predecessor->getTerminator()->getPreviousInstruction()) {
        liveness->updateForUse(inst, /*lifetimeEnding*/ false);
      } else {
        for (auto *grandPredecessor : predecessor->getPredecessorBlocks()) {
          liveness->updateForUse(grandPredecessor->getTerminator(),
                                 /*lifetimeEnding*/ false);
        }
      }
    }
  }

  // Finally, preserve the destroys which weren't in the consumed region in
  // place: hoisting such destroys would not avoid copies.
  for (auto *destroy : destroys) {
    auto *block = destroy->getParent();
    // If the destroy is in a consumed block or a final consuming block,
    // hoisting it would avoid a copy.
    if (consumedAtExitBlocks.contains(block))
      continue;
    liveness->updateForUse(destroy, /*lifetimeEnding*/ true);
  }
}

//===----------------------------------------------------------------------===//
// MARK: Step 4. Extend the "original" boundary from step 2 up to destroys that
//               aren't separated from it by "interesting" instructions.
//===----------------------------------------------------------------------===//

namespace {
/// Extends the boundary from PrunedLiveness down to preexisting destroys of the
/// def which aren't separated from the original boundary by "interesting"
/// instructions.
///
/// The motivation for extending the boundary is to avoid "churning" when
/// iterating to a fixed point by canonicalizing the lifetimes of several
/// values with overlapping live ranges and failing to find a fixed point
/// because their destroys are repeatedly hoisted over one another.
class ExtendBoundaryToDestroys final {
  using InstructionPredicate = llvm::function_ref<bool(SILInstruction *)>;
  SSAPrunedLiveness &liveness;
  PrunedLivenessBoundary const &originalBoundary;
  SILValue currentDef;
  BasicBlockSet seenMergePoints;
  InstructionPredicate isDestroy;

public:
  ExtendBoundaryToDestroys(SSAPrunedLiveness &liveness,
                           PrunedLivenessBoundary const &originalBoundary,
                           SILValue currentDef, InstructionPredicate isDestroy)
      : liveness(liveness), originalBoundary(originalBoundary),
        currentDef(currentDef), seenMergePoints(currentDef->getFunction()),
        isDestroy(isDestroy){};
  ExtendBoundaryToDestroys(ExtendBoundaryToDestroys const &) = delete;
  ExtendBoundaryToDestroys &
  operator=(ExtendBoundaryToDestroys const &) = delete;

  /// Compute the extended boundary by walking out from the original boundary
  /// (from PrunedLiveness::computeBoundary) down to any destroys that appear
  /// later but which aren't separated from the original boundary by
  /// "interesting" users.
  void extend(PrunedLivenessBoundary &boundary) {
    for (auto *def : originalBoundary.deadDefs) {
      extendBoundaryFromDef(def, boundary);
    }
    for (auto *destination : originalBoundary.boundaryEdges) {
      extendBoundaryFromBoundaryEdge(destination, boundary);
    }
    for (auto *user : originalBoundary.lastUsers) {
      extendBoundaryFromUser(user, boundary);
    }
  }

  /// Look past ignoreable instructions to find the _last_ destroy after the
  /// specified instruction that destroys \p def.
  static DestroyValueInst *findDestroyAfter(SILInstruction *previous,
                                            SILValue def,
                                            InstructionPredicate isDestroy) {
    DestroyValueInst *retval = nullptr;
    for (auto *instruction = previous->getNextInstruction(); instruction;
         instruction = instruction->getNextInstruction()) {
      if (!CanonicalizeOSSALifetime::ignoredByDestroyHoisting(
              instruction->getKind()))
        break;
      if (isDestroy(instruction))
        retval = cast<DestroyValueInst>(instruction);
    }
    return retval;
  }

  /// Look past ignoreable instructions to find the _last_ destroy at or after
  /// the specified instruction that destroys \p def.
  static DestroyValueInst *
  findDestroyAtOrAfter(SILInstruction *start, SILValue def,
                       InstructionPredicate isDestroy) {
    if (isDestroy(start))
      return cast<DestroyValueInst>(start);
    return findDestroyAfter(start, def, isDestroy);
  }

  /// Look past ignoreable instructions to find the _first_ destroy in \p
  /// destination that destroys \p def and isn't separated from the beginning
  /// by "interesting" instructions.
  static DestroyValueInst *
  findDestroyFromBlockBegin(SILBasicBlock *destination, SILValue def,
                            InstructionPredicate isDestroy) {
    return findDestroyAtOrAfter(&*destination->begin(), def, isDestroy);
  }

private:
  /// Compute the points on the extended boundary found by walking forward from
  /// the dead def (starting either with the top of the block in the case of a
  /// dead arg or the next instruction in the case of an instruction) down to
  /// any destroys that appear later but which aren't separated from the
  /// original boundary by "interesting" users.
  ///
  /// If a destroy is found, it becomes a last user.  Otherwise, the boundary
  /// stays in place and \p def remains a dead def.
  void extendBoundaryFromDef(SILNode *def, PrunedLivenessBoundary &boundary) {
    if (auto *arg = dyn_cast<SILArgument>(def)) {
      if (auto *dvi = findDestroyFromBlockBegin(arg->getParent(), currentDef,
                                                isDestroy)) {
        boundary.lastUsers.push_back(dvi);
        return;
      }
    } else {
      if (auto *dvi = findDestroyAfter(cast<SILInstruction>(def), currentDef,
                                       isDestroy)) {
        boundary.lastUsers.push_back(dvi);
        return;
      }
    }
    boundary.deadDefs.push_back(def);
  }

  /// Compute the points on the extended boundary found by walking down from the
  /// boundary edge in the original boundary (uniquely determined by the
  /// specified destination edge) down to any destroys that appear later but
  /// which aren't separated from the original boundary by "interesting" users.
  ///
  /// If a destroy is found, it becomes a last user.  Otherwise, the boundary
  /// stays in place and \p destination remains a boundary edge.
  void extendBoundaryFromBoundaryEdge(SILBasicBlock *destination,
                                      PrunedLivenessBoundary &boundary) {
    if (auto *dvi =
            findDestroyFromBlockBegin(destination, currentDef, isDestroy)) {
      boundary.lastUsers.push_back(dvi);
    } else {
      boundary.boundaryEdges.push_back(destination);
    }
  }

  /// Compute the points on the extended boundary found by walking down from the
  /// specified instruction in the original boundary down to any destroys that
  /// appear later but which aren't separated from the original boundary by
  /// "interesting" users.
  ///
  /// If the user is consuming, the boundary remains in place.
  ///
  /// If the user is a terminator, see extendBoundaryFromTerminator.
  ///
  /// If a destroy is found after the (non-consuming, non-terminator) \p user,
  /// it becomes a last user.  Otherwise, the boundary stays in place and \p
  /// user remains a last user.
  void extendBoundaryFromUser(SILInstruction *user,
                              PrunedLivenessBoundary &boundary) {
    if (isDestroy(user)) {
      auto *dvi = cast<DestroyValueInst>(user);
      auto *existingDestroy = findDestroyAtOrAfter(dvi, currentDef, isDestroy);
      assert(existingDestroy && "couldn't find a destroy at or after one!?");
      boundary.lastUsers.push_back(existingDestroy);
      return;
    }
    switch (liveness.isInterestingUser(user)) {
    case PrunedLiveness::IsInterestingUser::LifetimeEndingUse:
      // Even if we saw a destroy after this consuming use, we don't want to
      // add it to the boundary.  We will rewrite copies so that this user is
      // the final consuming user on this path.
      boundary.lastUsers.push_back(user);
      return;
    case PrunedLiveness::IsInterestingUser::NonLifetimeEndingUse:
    case PrunedLiveness::IsInterestingUser::NonUser:
      if (auto *terminator = dyn_cast<TermInst>(user)) {
        extendBoundaryFromTerminator(terminator, boundary);
        return;
      }
      if (auto *existingDestroy =
              findDestroyAfter(user, currentDef, isDestroy)) {
        boundary.lastUsers.push_back(existingDestroy);
        return;
      }
      boundary.lastUsers.push_back(user);
    }
  }

  /// Compute the points on the extended boundary by walking into \p user's
  /// parent's successors and looking for destroys.
  ///
  /// If any destroys are found, they become last users and all other successors
  /// (which lack destroys) become boundary edges.  If no destroys are found,
  /// the boundary stays in place and \p user remains a last user.
  void extendBoundaryFromTerminator(TermInst *user,
                                    PrunedLivenessBoundary &boundary) {
    auto *block = user->getParent();
    // Record the successors at the beginning of which we didn't find destroys.
    // If we found a destroy at the beginning of any other successor, then all
    // the other edges become boundary edges.
    SmallVector<SILBasicBlock *, 4> successorsWithoutDestroys;
    bool foundDestroy = false;
    for (auto *successor : block->getSuccessorBlocks()) {
      // If multiple terminators were live and had the same successor, only
      // record the boundary corresponding to that destination block once.
      if (!seenMergePoints.insert(successor)) {
        // Thanks to the lack of critical edges, having seen this successor
        // before means it has multiple predecessors, so this must be \p block's
        // unique successor.
        assert(block->getSingleSuccessorBlock() == successor);
        // When this merge point was encountered the first time, a
        // destroy_value was sought from its top.  If one was found, it was
        // added to the boundary. If no destroy_value was found, _that_ user
        // (i.e. the one on behalf of which extendBoundaryFromTerminator was
        // called which inserted successor into seenMergePoints) was added to
        // the boundary.
        //
        // This time, if a destroy was found, it's already in the boundary.  If
        // no destroy was found, though, _this_ user must be added to the
        // boundary.
        foundDestroy =
            findDestroyFromBlockBegin(successor, currentDef, isDestroy);
        continue;
      }
      if (auto *dvi =
              findDestroyFromBlockBegin(successor, currentDef, isDestroy)) {
        boundary.lastUsers.push_back(dvi);
        foundDestroy = true;
      } else {
        successorsWithoutDestroys.push_back(successor);
      }
    }
    if (foundDestroy) {
      // If we found a destroy in any successor, then every block at the
      // beginning of which we didn't find a destroy becomes a boundary edge.
      for (auto *successor : successorsWithoutDestroys) {
        boundary.boundaryEdges.push_back(successor);
      }
    } else {
      boundary.lastUsers.push_back(user);
    }
  }
};
} // anonymous namespace

void CanonicalizeOSSALifetime::findExtendedBoundary(
    PrunedLivenessBoundary const &originalBoundary,
    PrunedLivenessBoundary &boundary) {
  assert(boundary.lastUsers.size() == 0 && boundary.boundaryEdges.size() == 0 &&
         boundary.deadDefs.size() == 0);
  auto isDestroy = [&](auto *inst) { return destroys.contains(inst); };
  ExtendBoundaryToDestroys extender(*liveness, originalBoundary,
                                    getCurrentDef(), isDestroy);
  extender.extend(boundary);
}

//===----------------------------------------------------------------------===//
// MARK: Step 5. Insert destroys onto the boundary found in step 3 where needed.
//===----------------------------------------------------------------------===//

/// Create a new destroy_value instruction before the specified instruction and
/// record it as a final consume.
static void insertDestroyBeforeInstruction(SILInstruction *nextInstruction,
                                           SILValue currentDef,
                                           CanonicalOSSAConsumeInfo &consumes,
                                           InstModCallbacks &callbacks) {
  SILBuilderWithScope builder(nextInstruction);
  auto loc =
      RegularLocation::getAutoGeneratedLocation(nextInstruction->getLoc());
  auto *dvi = builder.createDestroyValue(loc, currentDef);
  callbacks.createdNewInst(dvi);
  consumes.recordFinalConsume(dvi);
  ++NumDestroysGenerated;
}

/// Inserts destroys along the boundary where needed and records all final
/// consuming uses.
///
/// Observations:
/// - currentDef must be postdominated by some subset of its
///   consuming uses, including destroys on all return paths.
/// - The postdominating consumes cannot be within nested loops.
/// - Any blocks in nested loops are now marked LiveOut.
void CanonicalizeOSSALifetime::insertDestroysOnBoundary(
    PrunedLivenessBoundary const &boundary) {
  BasicBlockSet seenMergePoints(getCurrentDef()->getFunction());
  for (auto *instruction : boundary.lastUsers) {
    if (destroys.contains(instruction)) {
      consumes.recordFinalConsume(instruction);
      continue;
    }
    switch (liveness->isInterestingUser(instruction)) {
    case PrunedLiveness::IsInterestingUser::LifetimeEndingUse:
      consumes.recordFinalConsume(instruction);
      continue;
    case PrunedLiveness::IsInterestingUser::NonLifetimeEndingUse:
    case PrunedLiveness::IsInterestingUser::NonUser:
      if (isa<TermInst>(instruction)) {
        auto *block = instruction->getParent();
        for (auto *successor : block->getSuccessorBlocks()) {
          if (!seenMergePoints.insert(successor)) {
            assert(block->getSingleSuccessorBlock() == successor);
            continue;
          }
          auto *insertionPoint = &*successor->begin();
          insertDestroyBeforeInstruction(insertionPoint, getCurrentDef(),
                                         consumes, getCallbacks());
          LLVM_DEBUG(llvm::dbgs()
                     << "  Destroy after terminator " << instruction
                     << " at beginning of " << successor << "\n");
        }
        continue;
      }
      auto *insertionPoint = instruction->getNextInstruction();
      insertDestroyBeforeInstruction(insertionPoint, getCurrentDef(), consumes,
                                     getCallbacks());
      LLVM_DEBUG(llvm::dbgs()
                 << "  Destroy at last use " << insertionPoint << "\n");
      continue;
    }
  }
  for (auto *edgeDestination : boundary.boundaryEdges) {
    auto *insertionPoint = &*edgeDestination->begin();
    insertDestroyBeforeInstruction(insertionPoint, getCurrentDef(), consumes,
                                   getCallbacks());
    LLVM_DEBUG(llvm::dbgs() << "  Destroy on edge " << edgeDestination << "\n");
  }
  for (auto *def : boundary.deadDefs) {
    if (auto *arg = dyn_cast<SILArgument>(def)) {
      auto *insertionPoint = &*arg->getParent()->begin();
      insertDestroyBeforeInstruction(insertionPoint, getCurrentDef(), consumes,
                                     getCallbacks());
      LLVM_DEBUG(llvm::dbgs()
                 << "  Destroy after dead def arg " << arg << "\n");
    } else {
      auto *instruction = cast<SILInstruction>(def);
      auto *insertionPoint = instruction->getNextInstruction();
      assert(insertionPoint && "def instruction was a terminator?!");
      insertDestroyBeforeInstruction(insertionPoint, getCurrentDef(), consumes,
                                     getCallbacks());
      LLVM_DEBUG(llvm::dbgs()
                 << "  Destroy after dead def inst " << instruction << "\n");
    }
  }
}

//===----------------------------------------------------------------------===//
// MARK: Step 6. Rewrite copies and destroys
//===----------------------------------------------------------------------===//

/// The lifetime extends beyond given consuming use. Copy the value.
///
/// This can set the operand value, but cannot invalidate the use iterator.
void swift::copyLiveUse(Operand *use, InstModCallbacks &instModCallbacks) {
  SILInstruction *user = use->getUser();
  SILBuilderWithScope builder(user->getIterator());

  auto loc = RegularLocation::getAutoGeneratedLocation(user->getLoc());
  auto *copy = builder.createCopyValue(loc, use->get());
  instModCallbacks.createdNewInst(copy);
  use->set(copy);

  ++NumCopiesGenerated;
  LLVM_DEBUG(llvm::dbgs() << "  Copying at last use " << *copy);
}

/// Revisit the def-use chain of currentDef. Mark unneeded original
/// copies and destroys for deletion. Insert new copies for interior uses that
/// require ownership of the used operand.
void CanonicalizeOSSALifetime::rewriteCopies() {
  assert(getCurrentDef()->getOwnershipKind() == OwnershipKind::Owned);

  InstructionSetVector instsToDelete(getCurrentDef()->getFunction());
  defUseWorklist.clear();

  // Visit each operand in the def-use chain.
  //
  // Return true if the operand can use the current definition. Return false if
  // it requires a copy.
  auto visitUse = [&](Operand *use) {
    auto *user = use->getUser();
    // Recurse through copies.
    if (auto *copy = dyn_cast<CopyValueInst>(user)) {
      defUseWorklist.insert(copy);
      return true;
    }
    if (destroys.contains(user)) {
      auto *destroy = cast<DestroyValueInst>(user);
      // If this destroy was marked as a final destroy, ignore it; otherwise,
      // delete it.
      if (!consumes.claimConsume(destroy)) {
        instsToDelete.insert(destroy);
        LLVM_DEBUG(llvm::dbgs() << "  Removing " << *destroy);
        ++NumDestroysEliminated;
      } else if (pruneDebugMode) {
        // If this destroy was marked as a final destroy, add it to liveness so
        // that we don't delete any debug instructions that occur before it.
        // (Only relevant in pruneDebugMode).
        liveness->updateForUse(destroy, /*lifetimeEnding*/ true);
      }
      return true;
    }

    // Nonconsuming uses do not need copies and cannot be marked as destroys.
    // A lifetime-ending use here must be a consume because EndBorrow/Reborrow
    // uses have been filtered out.
    if (!use->isLifetimeEnding())
      return true;

    // If this use was not marked as a final destroy *or* this is not the first
    // consumed operand we visited, then it needs a copy.
    if (!consumes.claimConsume(user)) {
      return false;
    }

    return true;
  };

  // Perform a def-use traversal, visiting each use operand.
  for (auto useIter = getCurrentDef()->use_begin(),
         endIter = getCurrentDef()->use_end(); useIter != endIter;) {
    Operand *use = *useIter++;
    if (!visitUse(use)) {
      copyLiveUse(use, getCallbacks());
    }
  }
  while (SILValue value = defUseWorklist.pop()) {
    CopyValueInst *srcCopy = cast<CopyValueInst>(value);
    // Recurse through copies while replacing their uses.
    Operand *reusedCopyOp = nullptr;
    for (auto useIter = srcCopy->use_begin(); useIter != srcCopy->use_end();) {
      Operand *use = *useIter++;
      if (!visitUse(use)) {
        if (!reusedCopyOp && srcCopy->getParent() == use->getParentBlock()) {
          reusedCopyOp = use;
        } else {
          copyLiveUse(use, getCallbacks());
        }
      }
    }
    if (!(reusedCopyOp && srcCopy->hasOneUse())) {
      getCallbacks().replaceValueUsesWith(srcCopy, srcCopy->getOperand());
      if (reusedCopyOp) {
        reusedCopyOp->set(srcCopy);
      } else {
        if (instsToDelete.insert(srcCopy)) {
          LLVM_DEBUG(llvm::dbgs() << "  Removing " << *srcCopy);
          ++NumCopiesAndMovesEliminated;
        }
      }
    }
  }
  assert(!consumes.hasUnclaimedConsumes());

  if (pruneDebugMode) {
    for (auto *dvi : debugValues) {
      if (!liveness->isWithinBoundary(dvi)) {
        LLVM_DEBUG(llvm::dbgs() << "  Removing debug_value: " << *dvi);
        deleter.forceDelete(dvi);
      }
    }
  }

  // Remove the leftover copy_value and destroy_value instructions.
  for (auto iter = instsToDelete.begin(), end = instsToDelete.end();
       iter != end; ++iter) {
    deleter.forceDelete(*iter);
  }
}

//===----------------------------------------------------------------------===//
//                            MARK: Top-Level API
//===----------------------------------------------------------------------===//

bool CanonicalizeOSSALifetime::computeLiveness() {
  if (currentDef->getOwnershipKind() != OwnershipKind::Owned)
    return false;

  LLVM_DEBUG(llvm::dbgs() << "  Canonicalizing: " << currentDef);

  // Note: There is no need to register callbacks with this utility. 'onDelete'
  // is the only one in use to handle dangling pointers, which could be done
  // instead be registering a temporary handler with the pass. Canonicalization
  // is only allowed to create and delete instructions that are associated with
  // this canonical def (copies and destroys). Each canonical def has a disjoint
  // extended lifetime. Any pass calling this utility should work at the level
  // canonical defs, not individual instructions.
  //
  // NotifyWillBeDeleted will not work because copy rewriting removes operands
  // before deleting instructions. Also prohibit setUse callbacks just because
  // that would simply be insane.
  assert(!getCallbacks().notifyWillBeDeletedFunc
         && !getCallbacks().setUseValueFunc && "unsupported");

  // Step 1: compute liveness
  if (!computeCanonicalLiveness()) {
    LLVM_DEBUG(llvm::errs() << "Failed to compute canonical liveness?!\n");
    clear();
    return false;
  }
  if (currentDef->isLexical()) {
    extendLivenessToDeinitBarriers();
  }
  if (accessBlockAnalysis) {
    extendLivenessThroughOverlappingAccess();
  }
  return true;
}

void CanonicalizeOSSALifetime::rewriteLifetimes() {
  // Step 2: compute original boundary
  PrunedLivenessBoundary originalBoundary;
  findOriginalBoundary(originalBoundary);
  PrunedLivenessBoundary extendedBoundary;
  if (maximizeLifetime) {
    // Step 3. (optional) maximize lifetimes
    extendUnconsumedLiveness(originalBoundary);
    originalBoundary.clear();
    // Step 2: (again) recompute the original boundary since we've extended
    //         liveness
    findOriginalBoundary(originalBoundary);
    // Step 4: extend boundary to destroys
    findExtendedBoundary(originalBoundary, extendedBoundary);
  } else {
    // Step 3: (skipped)
    // Step 4: extend boundary to destroys
    findExtendedBoundary(originalBoundary, extendedBoundary);
  }

  // Step 5: insert destroys and record consumes
  insertDestroysOnBoundary(extendedBoundary);
  // Step 6: rewrite copies and delete extra destroys
  rewriteCopies();

  clear();
  consumes.clear();
}

/// Canonicalize a single extended owned lifetime.
bool CanonicalizeOSSALifetime::canonicalizeValueLifetime(SILValue def) {
  LivenessState livenessState(*this, def);

  // Don't canonicalize the lifetimes of values of move-only type.  According to
  // language rules, they are fixed.
  if (def->getType().isMoveOnly()) {
    return false;
  }

  // Step 1: Compute liveness.
  if (!computeLiveness()) {
    LLVM_DEBUG(llvm::dbgs() << "Failed to compute liveness boundary!\n");
    return false;
  }

  // Steps 2-6. \see rewriteUses for explanation of steps 2-6.
  rewriteLifetimes();

  return true;
}

//===----------------------------------------------------------------------===//
//                              MARK: Debugging
//===----------------------------------------------------------------------===//

SWIFT_ASSERT_ONLY_DECL(
  void CanonicalOSSAConsumeInfo::dump() const {
    llvm::dbgs() << "Consumes:";
    for (auto &blockAndInst : finalBlockConsumes) {
      llvm::dbgs() << "  " << *blockAndInst.getSecond();
    }
  })
