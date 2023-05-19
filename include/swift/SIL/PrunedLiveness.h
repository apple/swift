//===--- PrunedLiveness.hpp - Compute liveness from selected uses ---------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// Incrementally compute and represent basic block liveness of a single live
/// range. The live range is defined by points in the CFG, independent of any
/// particular SSA value. The client initializes liveness with a set of
/// definition blocks, typically a single block. The client then incrementally
/// updates liveness by providing a set of "interesting" uses one at a time.
///
/// This supports discovery of pruned liveness during control flow traversal. It
/// is not tied to a single SSA value and allows the client to select
/// interesting uses while ignoring other uses.
///
/// The PrunedLiveBlocks result maps each block to its current liveness state:
/// Dead, LiveWithin, LiveOut.
///
/// A LiveWithin block has a liveness boundary within the block. The client can
/// determine the boundary's instruction position by searching for the last use.
///
/// LiveOut indicates that liveness extends into a successor edges, therefore,
/// no uses within that block can be on the liveness boundary, unless that use
/// occurs before a def in the same block.
///
/// All blocks are initially assumed Dead. Initializing a definition block marks
/// that block LiveWithin. Each time an interesting use is discovered, blocks
/// liveness may undergo one of these transitions:
///
/// - Dead -> LiveWithin
/// - Dead -> LiveOut
/// - LiveWithin -> LiveOut
///
/// Example 1. Local liveness.
///
///  -----
/// |     | [Dead]
///  -----
///    |
///  -----
/// | Def | [LiveWithin]
/// | Use |
///  -----
///    |
///  -----
/// |     | [Dead]
///  -----
///
/// Example 2. Cross-block liveness.
///
/// Initial State after initializing a def block:
///
///  -----
/// | Def | [LiveWithin]
///  -----
///    |
///  -----
/// |     | [Dead]
///  -----
///    |
///  -----
/// | Use | [Dead]
///  -----
///
/// Later state after updateForUse is applied to the use:
///
///  -----
/// | Def | [LiveOut]
///  -----
///    |
///  -----
/// |     | [LiveOut]
///  -----
///    |
///  -----
/// | Use | [LiveWithin]
///  -----
///
/// ---------------------------------------------------------------------------
///
/// "Use points" are the instructions that "generate" liveness for a given
/// operand.
///
/// ** Lifetime-ending uses **
///
/// When PrunedLiveness records uses, it caches the "lifetime-ending"
/// state. This flag has _zero effect_ on liveness. It refers to the use's
/// ownership constraint. But liveness does not map cleanly to ownership
/// lifetime. For extended live ranges, lifetime ending uses may occur in the
/// middle of liveness (a live-out block may contain a "lifetime-ending"
/// use). For incomplete ownership lifetimes, and for guaranteed phis,
/// non-lifetime ending uses may end liveness. This deliberate abstraction
/// leakage is only done for efficiency. Note that use-points are recorded as
/// instructions, not operands. Caching "lifetime-ending" state avoids the need
/// to visit all operands when computing liveness and, in the common case,
/// avoids the need for a separate operand map in the client.
///
/// ** Scoped operations **
///
/// Handling uses that must be live over a scope requires treating all the
/// scope-ending points as "use points". See
/// PrunedLiveRange<LivenessWithDefs>::recursivelyUpdateForDef for an example of
/// handling scopes.
///
/// ** Phis **
///
/// PrunedLiveness has no way to know whether a phi is intended to end a live
/// range. It consistently models all phi operands as uses in the predecessor
/// block (the branch is the use point). A guaranteed phi may or may not
/// actually end liveness depending on whether its enclosing def is an outer
/// adjacent phi. Liveness cannot, therefore, distinguish between a guaranteed
/// phi that ends liveness, and a dead guaranteed phi that does not end
/// liveness. In the later case, predecessor blocks are confusingly marked
/// live-within instead of live-out. visitInsertionPoints compensates by moving
/// the insertion point to the successor block.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_PRUNEDLIVENESS_H
#define SWIFT_SILOPTIMIZER_UTILS_PRUNEDLIVENESS_H

#include "swift/AST/TypeExpansionContext.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/NodeDatastructures.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

class DeadEndBlocks;

/// Discover "pruned" liveness for an arbitrary set of uses. The client builds
/// liveness by first initializing "def" blocks, then incrementally feeding uses
/// to updateForUse().
///
/// Incrementally building liveness is important for algorithms that create an
/// initial live region, perform some analysis on that, then expand the live
/// region by adding new uses before continuing the analysis.
///
/// Initializing "def blocks" restricts liveness on any path through those def
/// blocks to the blocks that occur on or after the def block. If any uses is
/// not dominated by a def block, then liveness will include the entry block,
/// as if defined by a function argument
///
/// TODO: For efficiency, use BasicBlockBitfield rather than SmallDenseMap.
class PrunedLiveBlocks {
public:
  /// Per-block liveness state computed during backward dataflow propagation.
  /// All unvisited blocks are considered Dead. As the are visited, blocks
  /// transition through these states in one direction:
  ///
  /// Dead -> LiveWithin -> LiveOut
  ///
  /// Dead blocks are either outside of the def's pruned liveness region, or
  /// they have not yet been discovered by the liveness computation.
  ///
  /// LiveWithin blocks have at least one use and/or def within the block, but
  /// are not (yet) LiveOut.
  ///
  /// LiveOut blocks are live on at least one successor path. LiveOut blocks may
  /// or may not contain defs or uses.
  ///
  /// NOTE: The values below for Dead, LiveWithin, LiveOut were picked to
  /// establish a lattice such that:
  /// - Dead is the initial state (zero bitfield)
  /// - Merging liveness information is a bitwise-or
  enum IsLive {
    Dead = 0,
    LiveWithin = 1,
    LiveOut = 3,
  };

private:
  /// Map all blocks to an IsLive state.
  BasicBlockBitfield liveBlocks;

  /// Optional vector of live blocks for clients that deterministically iterate.
  SmallVectorImpl<SILBasicBlock *> *discoveredBlocks = nullptr;

  /// Once the first def has been initialized, uses can be added.
  bool initializedFlag = false;

public:
  PrunedLiveBlocks(SILFunction *function,
                   SmallVectorImpl<SILBasicBlock *> *discoveredBlocks = nullptr)
      : liveBlocks(function, 2), discoveredBlocks(discoveredBlocks) {
    assert(!discoveredBlocks || discoveredBlocks->empty());
  }

  bool isInitialized() const { return initializedFlag; }

  void initializeDiscoveredBlocks(
      SmallVectorImpl<SILBasicBlock *> *discoveredBlocks) {
    assert(!isInitialized() && "cannot reinitialize after blocks are live");

    this->discoveredBlocks = discoveredBlocks;
  }

  /// If the constructor was provided with a vector to populate, then this
  /// returns the list of all live blocks with no duplicates.
  ArrayRef<SILBasicBlock *> getDiscoveredBlocks() const {
    return *discoveredBlocks;
  }

  void initializeDefBlock(SILBasicBlock *defBB) {
    initializedFlag = true;
    markBlockLive(defBB, LiveWithin);
  }

  /// Update this liveness result for a single use.
  ///
  /// \p isUseBeforeDef is true if \p user occures before the first def in this
  /// block. This indicates "liveness holes" inside the block, causing liveness
  /// to propagate to predecessors.
  IsLive updateForUse(SILInstruction *user, bool isUseBeforeDef) {
    assert(isInitialized() && "at least one definition must be initialized");

    auto *block = user->getParent();
    if (!isUseBeforeDef) {
      auto liveness = getBlockLiveness(block);
      // If a block is already marked live, it must either "kill" liveness, or
      // liveness was already propagated to its predecessors.
      if (liveness != Dead)
        return liveness;
    }
    computeUseBlockLiveness(block);
    return getBlockLiveness(block);
  }

  IsLive getBlockLiveness(SILBasicBlock *bb) const {
    assert(isInitialized());
    return (IsLive)liveBlocks.get(bb);
  }

  llvm::StringRef getStringRef(IsLive isLive) const;

  void print(llvm::raw_ostream &OS) const;

  void dump() const;

protected:
  void markBlockLive(SILBasicBlock *bb, IsLive isLive) {
    assert(isLive != Dead && "erasing live blocks isn't implemented.");
    auto state = (IsLive)liveBlocks.get(bb);
    liveBlocks.set(bb, state | isLive);
    if (state == IsLive::Dead) {
      if (discoveredBlocks)
        discoveredBlocks->push_back(bb);
    }
  }

private:
  void computeUseBlockLiveness(SILBasicBlock *userBB);
};

/// If inner borrows are 'Contained', then liveness is fully described by the
/// scope-ending instructions of any inner borrow scopes, and those scope-ending
/// uses are dominated by the current def. This is known as a "simple" live
/// range.
///
/// If nested borrows are 'Reborrowed' then simple liveness computed here based
/// on dominated uses is not sufficient to guarantee the value's lifetime. To do
/// that, the client needs to consider the reborrow scopes. OSSALiveness handles
/// those details.
///
/// Reborrows are only relevant when they apply to the first level of borrow
/// scope. Reborrows within nested borrows scopes are already summarized by the
/// outer borrow scope.
enum class InnerBorrowKind {
  Contained, // any borrows are fully contained within this live range
  Reborrowed // at least one immediately nested borrow is reborrowed
};

inline InnerBorrowKind meet(InnerBorrowKind lhs, InnerBorrowKind rhs) {
  return (lhs > rhs) ? lhs : rhs;
}

/// Summarize reborrows and pointer escapes that affect a live range. Reborrows
/// and pointer escapes that are encapsulated in a nested borrow don't affect
/// the outer live range.
struct LiveRangeSummary {
  InnerBorrowKind innerBorrowKind;
  AddressUseKind addressUseKind;

  LiveRangeSummary()
      : innerBorrowKind(InnerBorrowKind::Contained),
        addressUseKind(AddressUseKind::NonEscaping) {}

  void meet(const InnerBorrowKind lhs) {
    innerBorrowKind = swift::meet(innerBorrowKind, lhs);
  }
  void meet(const AddressUseKind lhs) {
    addressUseKind = swift::meet(addressUseKind, lhs);
  }
  void meet(const LiveRangeSummary lhs) {
    meet(lhs.innerBorrowKind);
    meet(lhs.addressUseKind);
  }
};

/// PrunedLiveness tracks PrunedLiveBlocks along with "interesting" use
/// points. The set of interesting uses is a superset of all uses on the
/// liveness boundary. Filtering out uses that are obviously not on the liveness
/// boundary improves efficiency over tracking all uses.
///
/// The "interesting use" set flags potentially "lifetime-ending" uses. This
/// merely caches Operand::isLifetimeEnding() for efficiency. It has no effect
/// on liveness computation. These instructions are always included in the set
/// of interesting use points, even if they don't occur on the liveness
/// boundary. The client may later use that information to figure out how to
/// "extend" a lifetime, for example by inserting copies.
///
/// Consequently, a branch intruction may be marked as a non-lifetime-ending
/// use, but modeled as as a use point in the predecessor block. This can
/// confusingly result in liveness that ends *before* value's the lifetime ends:
///
///     left:           // live-within
///       br merge(%)
///     right:          // live-within
///       br merge(%p)
///     merge(%deadPhi) // dead
///
/// If deadPhi has guaranteed ownership, and has no outer adjacent phi that
/// provides a separate borrow scope, then one would expect its phi operands to
/// be live-out of the predecessors. visitInsertionPoints compensates by
/// creating a "shared" insertion point in the merge block.
///
/// Note: unlike OwnershipLiveRange, this represents a lifetime in terms of the
/// CFG boundary rather that the use set, and, because it is "pruned", it only
/// includes liveness generated by select uses. For example, it does not
/// necessarily include liveness up to destroy_value or end_borrow
/// instructions.
class PrunedLiveness {
protected:
  PrunedLiveBlocks liveBlocks;

  // Map all "interesting" user instructions in this def's live range to a flag
  // indicating whether they must end the lifetime.
  //
  // Lifetime-ending users are always on the boundary so are always interesting.
  //
  // Non-lifetime-ending uses within a LiveWithin block are interesting because
  // they may be the last use in the block.
  //
  // Non-lifetime-ending within a LiveOut block are uninteresting.
  llvm::SmallMapVector<SILInstruction *, bool, 8> users;

public:
  PrunedLiveness(SILFunction *function,
                 SmallVectorImpl<SILBasicBlock *> *discoveredBlocks = nullptr)
      : liveBlocks(function, discoveredBlocks) {}

  bool isInitialized() const { return liveBlocks.isInitialized(); }

  bool empty() const { return users.empty(); }

  void initializeDiscoveredBlocks(
      SmallVectorImpl<SILBasicBlock *> *discoveredBlocks) {
    liveBlocks.initializeDiscoveredBlocks(discoveredBlocks);
  }

  /// If the constructor was provided with a vector to populate, then this
  /// returns the list of all live blocks with no duplicates.
  ArrayRef<SILBasicBlock *> getDiscoveredBlocks() const {
    return liveBlocks.getDiscoveredBlocks();
  }

  void initializeDefBlock(SILBasicBlock *defBB) {
    liveBlocks.initializeDefBlock(defBB);
  }

  PrunedLiveBlocks::IsLive getBlockLiveness(SILBasicBlock *bb) const {
    return liveBlocks.getBlockLiveness(bb);
  }

  enum IsInterestingUser {
    NonUser = 0,
    NonLifetimeEndingUse,
    LifetimeEndingUse
  };

  /// Return a result indicating whether the given user was identified as an
  /// interesting use of the current def and whether it ends the lifetime.
  IsInterestingUser isInterestingUser(SILInstruction *user) const {
    auto useIter = users.find(user);
    if (useIter == users.end())
      return NonUser;
    return useIter->second ? LifetimeEndingUse : NonLifetimeEndingUse;
  }

  using ConstUserRange =
      iterator_range<const std::pair<SILInstruction *, bool> *>;
  ConstUserRange getAllUsers() const {
    return llvm::make_range(users.begin(), users.end());
  }

  /// A namespace containing helper functors for use with various mapped
  /// ranges. Intended to be used to hide these noise types when working in an
  /// IDE.
  struct RangeIterationHelpers {
    struct MapFunctor {
      SILInstruction *
      operator()(const std::pair<SILInstruction *, bool> &pair) const {
        // Strip off the const to ease use with other APIs.
        return const_cast<SILInstruction *>(pair.first);
      }
    };

    struct LifetimeEnding {
      struct FilterFunctor {
        bool operator()(const std::pair<SILInstruction *, bool> &pair) const {
          return pair.second;
        }
      };

      using MapFilterIter = llvm::mapped_iterator<
          llvm::filter_iterator<const std::pair<SILInstruction *, bool> *,
                                FilterFunctor>,
          MapFunctor>;
    };

    struct NonLifetimeEnding {
      struct FilterFunctor {
        bool operator()(const std::pair<SILInstruction *, bool> &pair) const {
          return !pair.second;
        }
      };

      using MapFilterIter = llvm::mapped_iterator<
          llvm::filter_iterator<const std::pair<SILInstruction *, bool> *,
                                FilterFunctor>,
          MapFunctor>;
    };
  };
  using LifetimeEndingUserRange = llvm::iterator_range<
      RangeIterationHelpers::LifetimeEnding::MapFilterIter>;

  /// Return a range consisting of the current set of consuming users fed into
  /// this PrunedLiveness instance.
  LifetimeEndingUserRange getLifetimeEndingUsers() const {
    return map_range(
        llvm::make_filter_range(
            getAllUsers(),
            RangeIterationHelpers::LifetimeEnding::FilterFunctor()),
        RangeIterationHelpers::MapFunctor());
  }

  using NonLifetimeEndingUserRange = llvm::iterator_range<
      RangeIterationHelpers::NonLifetimeEnding::MapFilterIter>;

  /// Return a range consisting of the current set of non lifetime ending users
  /// fed into this PrunedLiveness instance.
  NonLifetimeEndingUserRange getNonLifetimeEndingUsers() const {
    return map_range(
        llvm::make_filter_range(
            getAllUsers(),
            RangeIterationHelpers::NonLifetimeEnding::FilterFunctor()),
        RangeIterationHelpers::MapFunctor());
  }

  void print(llvm::raw_ostream &OS) const;
  void dump() const;
};

/// Record the last use points and CFG edges that form the boundary of
/// PrunedLiveness.
///
/// Dead defs may occur even when the liveness result has uses for every
/// definition because those uses may occur in unreachable blocks. A dead def
/// must either be a SILInstruction or SILArgument. This supports memory
/// location liveness, so there isn't necessary a defining SILValue.
///
/// Each boundary edge is identified by its target block. The source of the edge
/// is the target block's single predecessor which must have at least one other
/// non-boundary successor.
struct PrunedLivenessBoundary {
  SmallVector<SILInstruction *, 8> lastUsers;
  SmallVector<SILBasicBlock *, 8> boundaryEdges;
  SmallVector<SILNode *, 1> deadDefs;

  void clear() {
    lastUsers.clear();
    boundaryEdges.clear();
    deadDefs.clear();
  }

  /// Visit the point at which a lifetime-ending instruction must be inserted,
  /// excluding dead-end blocks. This is only useful when it is known that none
  /// of the lastUsers ends the lifetime, for example when creating a new borrow
  /// scope to enclose all uses.
  void visitInsertionPoints(
      llvm::function_ref<void(SILBasicBlock::iterator insertPt)> visitor,
      DeadEndBlocks *deBlocks = nullptr);

  void print(llvm::raw_ostream &OS) const;
  void dump() const;
};

/// PrunedLiveness with information about defs for computing the live range
/// boundary.
///
/// LivenessWithDefs implements:
///
///   bool isInitialized() const
///
///   bool isDef(SILInstruction *inst) const
///
///   bool isDefBlock(SILBasicBlock *block) const
///
template <typename LivenessWithDefs>
class PrunedLiveRange : public PrunedLiveness {
protected:
  const LivenessWithDefs &asImpl() const {
    return static_cast<const LivenessWithDefs &>(*this);
  }

  PrunedLiveRange(SILFunction *function,
                  SmallVectorImpl<SILBasicBlock *> *discoveredBlocks = nullptr)
      : PrunedLiveness(function, discoveredBlocks) {}

  LiveRangeSummary recursivelyUpdateForDef(SILValue initialDef,
                                           ValueSet &visited,
                                           SILValue value);

public:
  /// For flexibility, \p lifetimeEnding is provided by the
  /// caller. PrunedLiveness makes no assumptions about the def-use
  /// relationships that generate liveness. For example, use->isLifetimeEnding()
  /// cannot distinguish the end of the borrow scope that defines this extended
  /// live range vs. a nested borrow scope within the extended live range.
  void updateForUse(SILInstruction *user, bool lifetimeEnding);

  /// Updates the liveness for a whole borrow scope, beginning at \p op.
  /// Returns false if this cannot be done. This assumes that nested OSSA
  /// lifetimes are complete.
  InnerBorrowKind updateForBorrowingOperand(Operand *operand);

  /// Update liveness for an interior pointer use. These are normally handled
  /// like an instantaneous use. But if \p operand "borrows" a value for the
  /// duration of a scoped address (store_borrow), then update liveness for the
  /// entire scope. This assumes that nested OSSA lifetimes are complete.
  AddressUseKind checkAndUpdateInteriorPointer(Operand *operand);

  /// Update this liveness to extend across the given liveness.
  void extendAcrossLiveness(PrunedLiveness &otherLiveness);

  /// Update liveness for all direct uses of \p def. Transitively follows
  /// guaranteed forwards up to but not including guaranteed phis. If \p def is
  /// used by a guaranteed phi return InnerBorrowKind::Reborrowed.
  LiveRangeSummary updateForDef(SILValue def);

  /// Check if \p inst occurs in between the definition this def and the
  /// liveness boundary.
  bool isWithinBoundary(SILInstruction *inst) const;

  /// Returns true when all \p uses are between this def and the liveness
  /// boundary \p deadEndBlocks is optional.
  bool areUsesWithinBoundary(ArrayRef<Operand *> uses,
                             DeadEndBlocks *deadEndBlocks) const;

  /// Returns true if any of the \p uses are before this def or after the
  /// liveness boundary
  /// \p deadEndBlocks is optional.
  bool areUsesOutsideBoundary(ArrayRef<Operand *> uses,
                              DeadEndBlocks *deadEndBlocks) const;

  /// Compute the boundary from the blocks discovered during liveness analysis.
  ///
  /// Precondition: \p liveness.getDiscoveredBlocks() is a valid list of all
  /// live blocks with no duplicates.
  ///
  /// The computed boundary will completely post-dominate, including dead end
  /// paths. The client should query DeadEndBlocks to ignore those dead end
  /// paths.
  void computeBoundary(PrunedLivenessBoundary &boundary) const;

  /// Compute the boundary from a backward CFG traversal from a known set of
  /// jointly post-dominating blocks. Avoids the need to record an ordered list
  /// of live blocks during liveness analysis. It's ok if postDomBlocks has
  /// duplicates or extraneous blocks, as long as they jointly post-dominate all
  /// live blocks that aren't on dead-end paths.
  ///
  /// If the jointly post-dominating destroys do not include dead end paths,
  /// then any uses on those paths will not be included in the boundary. The
  /// resulting partial boundary will have holes along those paths. The dead end
  /// successors of blocks in this live set on are not necessarily identified
  /// by DeadEndBlocks.
  void computeBoundary(PrunedLivenessBoundary &boundary,
                       ArrayRef<SILBasicBlock *> postDomBlocks) const;
};

// Singly-defined liveness.
//
// An SSA def results in pruned liveness with a contiguous liverange.
//
// An unreachable self-loop might result in a "gap" between the last use above
// the def in the same block.
//
// For SSA live ranges, a single "def" block dominates all uses. If no def
// block is provided, liveness is computed as if defined by a function
// argument. If the client does not provide a single, dominating def block,
// then the client must at least ensure that no uses precede the first
// definition in a def block. Since this analysis does not remember the
// positions of defs, it assumes that, within a block, uses follow
// defs. Breaking this assumption will result in a "hole" in the live range in
// which the def block's predecessors incorrectly remain dead. This situation
// could be handled by adding an updateForUseBeforeFirstDef() API.
class SSAPrunedLiveness : public PrunedLiveRange<SSAPrunedLiveness> {
  SILValue def;
  SILInstruction *defInst = nullptr; // nullptr for argument defs.

public:
  SSAPrunedLiveness(
      SILFunction *function,
      SmallVectorImpl<SILBasicBlock *> *discoveredBlocks = nullptr)
      : PrunedLiveRange(function, discoveredBlocks) {}

  SILValue getDef() const { return def; }

  void initializeDef(SILValue def) {
    assert(!this->def && "reinitialization");

    this->def = def;
    defInst = def->getDefiningInstruction();
    initializeDefBlock(def->getParentBlock());
  }

  bool isInitialized() const { return bool(def); }

  bool isDef(SILInstruction *inst) const { return inst == defInst; }

  bool isDefBlock(SILBasicBlock *block) const {
    return def->getParentBlock() == block;
  }

  /// In SSA, uses never occur before the single def.
  bool isUserBeforeDef(SILInstruction *user) const { return false; }

  /// SSA implementation of computeBoundary.
  void findBoundariesInBlock(SILBasicBlock *block, bool isLiveOut,
                             PrunedLivenessBoundary &boundary) const;

  /// Compute liveness for a single SSA definition. The lifetime-ending uses are
  /// also recorded--destroy_value or end_borrow.
  ///
  /// This only handles simple liveness in which all uses are dominated by the
  /// definition. If the returned summary includes InnerBorrowKind::Reborrow,
  /// then the resulting liveness does not includes potentially non-dominated
  /// uses within the reborrow scope. If the summary returns something other
  /// than AddressUseKind::NonEscaping, then the resulting liveness does not
  /// necessarilly encapsulate value ownership.
  ///
  /// Warning: If OSSA lifetimes are incomplete, then destroy_values might not
  /// jointly-post dominate if dead-end blocks are present. Nested scopes may
  /// also lack scope-ending instructions, so the liveness of their nested uses
  /// may be ignored.
  LiveRangeSummary computeSimple() {
    assert(def && "SSA def uninitialized");
    return updateForDef(def);
  }
};

/// MultiDefPrunedLiveness is computed incrementally by calling updateForUse.
///
/// Defs should be initialized before calling updatingForUse on any def
/// that reaches the use.
class MultiDefPrunedLiveness : public PrunedLiveRange<MultiDefPrunedLiveness> {
  NodeSetVector defs;
  BasicBlockSet defBlocks;

  void initializeDefNode(SILNode *def) {
    defs.insert(def);
    auto *block = def->getParentBlock();
    defBlocks.insert(block);
    initializeDefBlock(block);
  }

public:
  MultiDefPrunedLiveness(
      SILFunction *function,
      SmallVectorImpl<SILBasicBlock *> *discoveredBlocks = nullptr)
      : PrunedLiveRange(function, discoveredBlocks), defs(function),
        defBlocks(function) {}

  void initializeDef(SILInstruction *defInst) {
    initializeDefNode(cast<SILNode>(defInst));
  }

  void initializeDef(SILArgument *defArg) { initializeDefNode(defArg); }

  void initializeDef(SILValue value) {
    if (auto arg = dyn_cast<SILArgument>(value)) {
      initializeDefNode(arg);
    } else {
      initializeDef(value->getDefiningInstruction());
    }
  }

  bool isInitialized() const { return !defs.empty(); }

  NodeSetVector::iterator defBegin() const { return defs.begin(); }
  NodeSetVector::iterator defEnd() const { return defs.end(); }

  bool isDef(SILInstruction *inst) const {
    return defs.contains(cast<SILNode>(inst));
  }

  bool isDef(SILArgument *arg) const {
    return defs.contains(arg);
  }

  bool isDefBlock(SILBasicBlock *block) const {
    return defBlocks.contains(block);
  }

  /// Return true if \p user occurs before the first def in the same basic
  /// block. In classical liveness dataflow terms, gen/kill conditions over all
  /// users in 'bb' are:
  ///
  ///   Gen(bb)  |= !isDefBlock(bb) || isUserBeforeDef(bb)
  ///   Kill(bb) &= isDefBlock(bb) && !isUserBeforeDef(bb)
  ///
  /// If 'bb' has no users, it is neither a Gen nor Kill. Otherwise, Gen and
  /// Kill are complements.
  bool isUserBeforeDef(SILInstruction *user) const;

  /// Multi-Def implementation of computeBoundary.
  void findBoundariesInBlock(SILBasicBlock *block, bool isLiveOut,
                             PrunedLivenessBoundary &boundary) const;

  /// Compute liveness for a all currently initialized definitions. The
  /// lifetime-ending uses are also recorded--destroy_value or
  /// end_borrow. However destroy_values might not jointly-post dominate if
  /// dead-end blocks are present.
  ///
  /// This only handles simple liveness in which all uses are dominated by the
  /// definition. If the returned summary includes InnerBorrowKind::Reborrow,
  /// then the resulting liveness does not includes potentially non-dominated
  /// uses within the reborrow scope. If the summary returns something other
  /// than AddressUseKind::NonEscaping, then the resulting liveness does not
  /// necessarilly encapsulate value ownership.
  ///
  /// Warning: If OSSA lifetimes are incomplete, then destroy_values might not
  /// jointly-post dominate if dead-end blocks are present. Nested scopes may
  /// also lack scope-ending instructions, so the liveness of their nested uses
  /// may be ignored.
  LiveRangeSummary computeSimple();
};

//===----------------------------------------------------------------------===//
//                          DiagnosticPrunedLiveness
//===----------------------------------------------------------------------===//

// FIXME: it isn't clear what this is for or what nonLifetimeEndingUseInLiveOut
// means precisely.
class DiagnosticPrunedLiveness : public SSAPrunedLiveness {
  /// A side array that stores any non lifetime ending uses we find in live out
  /// blocks. This is used to enable our callers to emit errors on non-lifetime
  /// ending uses that extend liveness into a loop body.
  SmallSetVector<SILInstruction *, 8> *nonLifetimeEndingUsesInLiveOut;

public:
  DiagnosticPrunedLiveness(
      SILFunction *function,
      SmallVectorImpl<SILBasicBlock *> *discoveredBlocks = nullptr,
      SmallSetVector<SILInstruction *, 8> *nonLifetimeEndingUsesInLiveOut =
          nullptr)
      : SSAPrunedLiveness(function, discoveredBlocks),
        nonLifetimeEndingUsesInLiveOut(nonLifetimeEndingUsesInLiveOut) {}

  void updateForUse(SILInstruction *user, bool lifetimeEnding);

  using NonLifetimeEndingUsesInLiveOutRange =
      iterator_range<SILInstruction *const *>;

  NonLifetimeEndingUsesInLiveOutRange
  getNonLifetimeEndingUsesInLiveOut() const {
    assert(nonLifetimeEndingUsesInLiveOut &&
           "Called without passing in nonLifetimeEndingUsesInLiveOut to "
           "constructor?!");
    return llvm::make_range(nonLifetimeEndingUsesInLiveOut->begin(),
                            nonLifetimeEndingUsesInLiveOut->end());
  }

  using NonLifetimeEndingUsesInLiveOutBlocksRange =
      TransformRange<NonLifetimeEndingUsesInLiveOutRange,
                     function_ref<SILBasicBlock *(const SILInstruction *&)>>;
  NonLifetimeEndingUsesInLiveOutBlocksRange
  getNonLifetimeEndingUsesInLiveOutBlocks() const {
    function_ref<SILBasicBlock *(const SILInstruction *&)> op;
    op = [](const SILInstruction *&ptr) -> SILBasicBlock * {
      return ptr->getParent();
    };
    return NonLifetimeEndingUsesInLiveOutBlocksRange(
        getNonLifetimeEndingUsesInLiveOut(), op);
  }
};

} // namespace swift

#endif
