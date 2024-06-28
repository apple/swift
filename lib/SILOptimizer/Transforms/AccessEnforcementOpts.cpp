//===------ AccessEnforcementOpts.cpp - Optimize access enforcement -------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// Pass order dependencies:
///
/// - Will benefit from running after AccessEnforcementSelection.
///
/// - Should run immediately before the AccessEnforcementWMO to share
///   AccessStorageAnalysis results.
///
/// - Benefits from running after AccessEnforcementReleaseSinking.
///
/// This pass optimizes access enforcement as follows:
///
/// **Access marker folding**
///
/// Find begin/end access scopes that are uninterrupted by a potential
/// conflicting access. Flag those as [nontracking] access.
///
/// Folding must prove that no dynamic conflicts occur inside of an access
/// scope. That is, a scope has no "nested inner conflicts". The access itself
/// may still conflict with an outer scope. If successful, folding simply sets
/// the [no_nested_conflict] attribute on the begin_[unpaired_]access
/// instruction and removes all corresponding end_[unpaired_]access
/// instructions.
///
/// This analysis is conceptually similar to DiagnoseStaticExclusivity. The
/// difference is that it conservatively considers any dynamic access that may
/// alias, as opposed to only the obviously aliasing accesses (it is the
/// complement of the static diagnostic pass in that respect). This makes a
/// considerable difference in the implementation. For example,
/// DiagnoseStaticExclusivity must be able to fully analyze all @inout_aliasable
/// parameters because they aren't dynamically enforced. This optimization
/// completely ignores @inout_aliasable parameters because it only cares about
/// dynamic enforcement. This optimization also does not attempt to
/// differentiate accesses on disjoint subaccess paths, because it should not
/// weaken enforcement in any way--a program that traps at -Onone should also
/// trap at -O.
///
/// Access folding is a forward data flow analysis that tracks open accesses. If
/// any path to an access' end of scope has a potentially conflicting access,
/// then that access is marked as a nested conflict.
///
/// **Local access marker removal**
///
/// When none of the local accesses on local storage (box/stack) have nested
/// conflicts, then all the local accesses may be disabled by setting their
/// enforcement to `static`. This is somewhat rare because static diagnostics
/// already promote the obvious cases to static checks. However, there are two
/// reasons that dynamic local markers may be disabled: (1) inlining may cause
/// closure access to become local access (2) local storage may truly escape,
/// but none of the local access scopes cross a call site.
///
/// TODO: Perform another run of AccessEnforcementSelection immediately before
/// this pass. Currently, that pass only works well when run before
/// AllocBox2Stack. Ideally all such closure analysis passes are combined into a
/// shared analysis with a set of associated optimizations that can be rerun at
/// any point in the pipeline. Until then, we could settle for a partially
/// working AccessEnforcementSelection, or expand it somewhat to handle
/// alloc_stack.
///
/// **Access marker merger**
///
/// When a pair of non-overlapping accesses, where the first access dominates
/// the second and there are no conflicts on the same storage in the paths
/// between them, and they are part of the same sub-region
/// be it the same block or the sample loop, merge those accesses to create
/// a new, larger, scope with a single begin_access for the accesses.
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "access-enforcement-opts"

#include "swift/Basic/Assertions.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SILOptimizer/Analysis/AccessStorageAnalysis.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/LoopRegionAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "swift/SILOptimizer/Utils/OwnershipOptUtils.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SCCIterator.h"

using namespace swift;

namespace swift {
/// Information about each dynamic access with valid storage.
///
/// This is a pass-specific subclass of AccessStorage with identical layout.
/// An instance is created for each BeginAccess in the current function. In
/// additional to identifying the access' storage location, it associates that
/// access with pass-specific data in reserved bits. The reserved bits do not
/// participate in equality or hash lookup.
///
/// Aliased to AccessInfo in this file; the fully descriptive class name allows
/// forward declaration in order to define bitfields in AccessStorage.
class AccessEnforcementOptsInfo : public AccessStorage {
public:
  AccessEnforcementOptsInfo(const AccessStorage &storage)
    : AccessStorage(storage) {
    Bits.AccessEnforcementOptsInfo.beginAccessIndex = 0;
    Bits.AccessEnforcementOptsInfo.seenNestedConflict = false;
  }

  /// Get a unique index for this access within its function.
  unsigned getAccessIndex() const {
    return Bits.AccessEnforcementOptsInfo.beginAccessIndex;
  }

  void setAccessIndex(unsigned index) {
    Bits.AccessEnforcementOptsInfo.beginAccessIndex = index;
    assert(unsigned(Bits.AccessEnforcementOptsInfo.beginAccessIndex) == index);
  }

  /// Has the analysis seen a conflicting nested access on any path within this
  /// access' scope.
  bool seenNestedConflict() const {
    return Bits.AccessEnforcementOptsInfo.seenNestedConflict;
  }

  void setSeenNestedConflict() {
    Bits.AccessEnforcementOptsInfo.seenNestedConflict = 1;
  }

  /// Did a PostOrder walk previously find another access to the same
  /// storage. If so, then this access could be merged with a subsequent access
  /// after checking for conflicts.
  bool seenIdenticalStorage() const {
    return Bits.AccessEnforcementOptsInfo.seenIdenticalStorage;
  }

  void setSeenIdenticalStorage() {
    Bits.AccessEnforcementOptsInfo.seenIdenticalStorage = 1;
  }

  void dump() const {
    AccessStorage::dump();
    llvm::dbgs() << "  access index: " << getAccessIndex() << " <"
                 << (seenNestedConflict() ? "" : "no ") << "conflict> <"
                 << (seenIdenticalStorage() ? "" : "not ") << "seen identical>"
                 << "\n";
  }
};
using AccessInfo = AccessEnforcementOptsInfo;
} // namespace swift

namespace {
/// A dense map of (index, begin_access instructions) as a compact vector.
/// Reachability results are stored here because very few accesses are
/// typically in-progress at a particular program point,
/// particularly at block boundaries.
using DenseAccessSet = llvm::SmallSetVector<BeginAccessInst *, 4>;

// Tracks the local data flow result for a basic block
struct RegionState {
  DenseAccessSet inScopeConflictFreeAccesses;
  DenseAccessSet outOfScopeConflictFreeAccesses;

public:
  RegionState(unsigned size) {
    // FIXME: llvm::SetVector should have a reserve API.
    // inScopeConflictFreeAccesses.reserve(size);
    // outOfScopeConflictFreeAccesses.reserve(size);
  }

  void reset() {
    inScopeConflictFreeAccesses.clear();
    outOfScopeConflictFreeAccesses.clear();
  }

  const DenseAccessSet &getInScopeAccesses() {
    return inScopeConflictFreeAccesses;
  }

  const DenseAccessSet &getOutOfScopeAccesses() {
    return outOfScopeConflictFreeAccesses;
  }
};

/// Analyze a function's formal accesses.
/// determines nested conflicts and mergeable accesses.
///
/// Maps each begin access instruction to its AccessInfo, which:
/// - identifies the accessed memory for conflict detection
/// - contains a pass-specific reachability set index
/// - contains a pass-specific flag that indicates the presence of a conflict
///   on any path.
///
/// If, after computing reachability, an access' conflict flag is still not set,
/// then all paths in its scope are conflict free. Reachability begins at a
/// begin_access instruction and ends either at a potential conflict
/// or at the end_access instruction that is associated with the
/// begin_access.
///
/// Forward data flow computes `BlockRegionState` for each region's blocks.
/// Loops are processed bottom-up.
/// Control flow within a loop or function top level is processed in RPO order.
/// At a block's control flow merge, this analysis forms an intersection of
/// reachable accesses on each path inside the region.
/// Before a block is visited, it has no `BlockRegionState` entry.
/// Blocks are processed in RPO order, and a single begin_access dominates
/// all associated end_access instructions. Consequently,
/// when a block is first visited, its storage accesses contains the maximal
/// reachability set. Further iteration would only reduce this set.
///
/// The only results of this analysis are:
//// 1) The seenNestedConflict flags in AccessInfo. For Each begin_access
///     Since reducing a reachability set cannot further detect
///     conflicts, there is no need to iterate to a reachability fix point.
///     This is derived from a block's in-scope accesses
///  2) A deterministic order map of out-of-scope instructions that we can
///     merge. The way we construct this map guarantees the accesses within
///     it are mergeable.
///
// Example:
// %1 = begin_access X
// %1 is in-scope
// ...
// %2 = begin_access Y // conflict with %1 if X (may-)aliases Y
// If it conflicts - seenNestedConflict
// ...
// end_access %1
// %1 is out-of-scope
// ...
// %3 = begin_access X // %1 reaches %3 -> we can merge
class AccessConflictAndMergeAnalysis {
public:
  using AccessMap = llvm::SmallDenseMap<BeginAccessInst *, AccessInfo, 32>;
  using AccessStorageSet = llvm::SmallDenseSet<AccessStorage, 8>;
  using LoopRegionToAccessStorage =
      llvm::SmallDenseMap<unsigned, AccessStorageResult>;
  using RegionIDToLocalStateMap = llvm::DenseMap<unsigned, RegionState>;
  // Instruction pairs we can merge from dominating instruction to dominated
  using MergeablePairs =
      llvm::SmallVector<std::pair<BeginAccessInst *, BeginAccessInst *>, 64>;
  // This result of this analysis is a map from all BeginAccessInst in this
  // function to AccessInfo.
  struct Result {
    /// Map each begin access to its AccessInfo with index, data, and flags.
    /// Iterating over this map is nondeterministic. If it is necessary to order
    /// the accesses, then AccessInfo::getAccessIndex() can be used.
    /// This maps contains every dynamic begin_access instruction,
    /// even those with invalid storage:
    /// We would like to keep track of unrecognized or invalid storage locations
    /// Because they affect our decisions for recognized locations,
    /// be it nested conflict or merging out of scope accesses.
    /// The access map is just a “cache” of accesses.
    /// Keeping those invalid ones just makes the lookup faster
    AccessMap accessMap;

    /// Instruction pairs we can merge the scope of
    MergeablePairs mergePairs;

    /// Convenience.
    ///
    /// Note: If AccessInfo has already been retrieved, get the index directly
    /// from it instead of calling this to avoid additional hash lookup.
    unsigned getAccessIndex(BeginAccessInst *beginAccess) const {
      return getAccessInfo(beginAccess).getAccessIndex();
    }

    /// Get the AccessInfo for a BeginAccessInst within this function. All
    /// accesses are mapped by identifyBeginAccesses().
    AccessInfo &getAccessInfo(BeginAccessInst *beginAccess) {
      auto iter = accessMap.find(beginAccess);
      assert(iter != accessMap.end());
      return iter->second;
    }
    const AccessInfo &getAccessInfo(BeginAccessInst *beginAccess) const {
      return const_cast<Result &>(*this).getAccessInfo(beginAccess);
    }
  };

private:
  LoopRegionFunctionInfo *LRFI;
  PostOrderFunctionInfo *PO;
  AccessStorageAnalysis *ASA;

  // Unique storage locations seen in this function.
  AccessStorageSet storageSet;

  Result result;

public:
  AccessConflictAndMergeAnalysis(LoopRegionFunctionInfo *LRFI,
                                 PostOrderFunctionInfo *PO,
                                 AccessStorageAnalysis *ASA)
    : LRFI(LRFI), PO(PO), ASA(ASA) {}

  bool analyze();

  const Result &getResult() { return result; }

protected:
  bool identifyBeginAccesses();

  void
  propagateAccessSetsBottomUp(LoopRegionToAccessStorage &regionToStorageMap,
                              const llvm::SmallVector<unsigned, 16> &worklist);

  void calcBottomUpOrder(llvm::SmallVectorImpl<unsigned> &worklist);

  void visitBeginAccess(BeginAccessInst *beginAccess, RegionState &state);

  void visitEndAccess(EndAccessInst *endAccess, RegionState &state);

  void visitFullApply(FullApplySite fullApply, RegionState &state);

  void visitMayRelease(SILInstruction *instr, RegionState &state);

  RegionState &mergePredAccesses(unsigned regionID,
                                 RegionIDToLocalStateMap &localRegionStates);

  void localDataFlowInBlock(RegionState &state, SILBasicBlock *bb);

private:
  void recordInScopeConflicts(RegionState &state,
                              const AccessStorage &currStorage,
                              SILAccessKind currKind);
  bool removeConflicts(DenseAccessSet &accessSet,
                       const AccessStorage &currStorage);
  void recordUnknownConflict(RegionState &state);
  void recordConflicts(RegionState &state,
                       const AccessStorageResult &accessedStorage);
  BeginAccessInst *findMergeableOutOfScopeAccess(RegionState &state,
                                                 BeginAccessInst *beginAccess);
  void insertOutOfScopeAccess(RegionState &state, BeginAccessInst *beginAccess,
                              AccessInfo &currStorageInfo);
  void mergeAccessSet(DenseAccessSet &accessSet, const DenseAccessSet &otherSet,
                      bool isInitialized);
  void mergeState(RegionState &state, const RegionState &otherState,
                  bool isInitialized);
};
} // namespace

// Mark any in-scope access that conflicts with an access to 'currStorage' for
// the given 'beginAccess' as having a nested conflict.
void AccessConflictAndMergeAnalysis::recordInScopeConflicts(
    RegionState &state, const AccessStorage &currStorage,
    SILAccessKind currKind) {
  // It is tempting to combine this loop with the loop in removeConflicts, which
  // also checks isDistinctFrom for each element. However, since SetVector does
  // not support 'llvm::erase_if', it is actually more efficient to do the
  // removal in a separate 'remove_if' loop.
  llvm::for_each(state.inScopeConflictFreeAccesses, [&](BeginAccessInst *bai) {
    auto &accessInfo = result.getAccessInfo(bai);
    if (accessKindMayConflict(currKind, bai->getAccessKind())
        && !accessInfo.isDistinctFrom(currStorage)) {

      accessInfo.setSeenNestedConflict();
      LLVM_DEBUG(llvm::dbgs() << "  may conflict with:\n"; accessInfo.dump());
    }
  });
}

// Remove any accesses in accessSet that may conflict with the given storage
// location, currStorageInfo.
//
// Return true if any set elements were removed.
bool AccessConflictAndMergeAnalysis::removeConflicts(
    DenseAccessSet &accessSet, const AccessStorage &currStorage) {
  return accessSet.remove_if([&](BeginAccessInst *bai) {
    auto &storage = result.getAccessInfo(bai);
    return !storage.isDistinctFrom(currStorage);
  });
}

void AccessConflictAndMergeAnalysis::recordUnknownConflict(RegionState &state) {
  // Mark all open scopes as having a nested conflict.
  llvm::for_each(state.inScopeConflictFreeAccesses, [&](BeginAccessInst *bai) {
    auto &accessInfo = result.getAccessInfo(bai);
    accessInfo.setSeenNestedConflict();
    LLVM_DEBUG(llvm::dbgs() << "  may conflict with:\n"; accessInfo.dump());
  });
  // Clear data flow.
  state.reset();
}

// Update data flow `state` by removing accesses that conflict with the
// currently accessed `storage`. For in-scope accesses, also mark conflicting
// scopes with SeenNestedConflict.
//
// Removing access from the out-of-scope set is important for two reasons:
//
// 1. Let A & B be conflicting out-of-scope, where A's scope ends before B. If
// data flow then encounters scope C with the same storage as B, it should be
// able to merge them. This is safe regardless of whether A & B overlap because
// it doesn't introduce any conflict that wasn't already present. However,
// leaving A in the out-of-scope set means that we won't be able to merge B & C
// based on this dataflow.
//
// 2. Without removing conflicting scopes, the access set is unbounded and this
// data flow could scale quadratically with the function size.
void AccessConflictAndMergeAnalysis::recordConflicts(
    RegionState &state, const AccessStorageResult &accessedStorage) {

  if (accessedStorage.hasUnidentifiedAccess()) {
    recordUnknownConflict(state);
    return;
  }
  for (const StorageAccessInfo &currStorage : accessedStorage.getStorageSet()) {

    recordInScopeConflicts(state, currStorage, currStorage.getAccessKind());

    removeConflicts(state.inScopeConflictFreeAccesses, currStorage);

    removeConflicts(state.outOfScopeConflictFreeAccesses, currStorage);
  }
}

// Check if the current BeginAccessInst has identical storage with an
// out-of-scope access. If so, remove the access from the set and return it.
BeginAccessInst *AccessConflictAndMergeAnalysis::findMergeableOutOfScopeAccess(
    RegionState &state, BeginAccessInst *beginAccess) {

  auto currStorageInfo = result.getAccessInfo(beginAccess);

  // Before removing any conflicting accesses, find one with identical storage.
  auto identicalStorageIter = llvm::find_if(
      state.outOfScopeConflictFreeAccesses, [&](BeginAccessInst *bai) {
        auto storageInfo = result.getAccessInfo(bai);
        return storageInfo.hasIdenticalStorage(currStorageInfo);
      });
  if (identicalStorageIter == state.outOfScopeConflictFreeAccesses.end())
    return nullptr;

  // Remove the matching access before checking for other conflicts.  Since we
  // only check for a single identical storage access above, leaving multiple
  // accesses of the same storage in the set would appear as a conflict in the
  // check below when processing subsequent mergeable accesses.
  BeginAccessInst *mergeableAccess = *identicalStorageIter;
  state.outOfScopeConflictFreeAccesses.erase(identicalStorageIter);

  // Given a mergeableAccess, 'A', another out-of-scope access, 'B', and the
  // current access, 'C' which has identical storage as 'A', the only situation
  // in which it is illegal to merge 'A' with 'C' is when 'B' has non-distinct
  // storage from 'A'/'C', 'B' begins after 'A', and 'B' ends before
  // 'C'. Merging 'A' with 'C' would then introduce a false conflict. Since it
  // is impossible to determine here whether 'A' and 'B' overlap, we assume they
  // do not and simply avoid merging whenever 'B' and 'C' overlap. It is not
  // important to optimize the case in which 'A' and 'B' overlap because
  // potential conflicts like that are unlikely.
  if (llvm::any_of(state.outOfScopeConflictFreeAccesses,
                   [&](BeginAccessInst *bai) {
                     auto storageInfo = result.getAccessInfo(bai);
                     return !storageInfo.isDistinctFrom(currStorageInfo);
                   })) {
    return nullptr;
  }
  return mergeableAccess;
}

// Add the given access to the out-of-scope set, replacing any existing
// out-of-scope access on the same storage. An access to the same storage may
// already be out-of-scope, for example, if there are nested reads:
//
// %4 = begin_access [read] [dynamic] %0 : $*X
// %5 = load %4 : $*X
// %7 = begin_access [read] [dynamic] %0 : $*X
// %8 = load %7 : $*X
// end_access %7 : $*X
// end_access %4 : $*X
//
// The inner scope needs to be replaced with the outer scope so that scope
// nesting is preserved when merging scopes.
void AccessConflictAndMergeAnalysis::insertOutOfScopeAccess(
    RegionState &state, BeginAccessInst *beginAccess,
    AccessInfo &currStorageInfo) {

  if (!currStorageInfo.seenIdenticalStorage()) {
    LLVM_DEBUG(llvm::dbgs() << "Ignoring unmergeable access: " << *beginAccess);
    return;
  }

  auto identicalStorageIter = llvm::find_if(
      state.outOfScopeConflictFreeAccesses, [&](BeginAccessInst *bai) {
        auto storageInfo = result.getAccessInfo(bai);
        return storageInfo.hasIdenticalStorage(currStorageInfo);
      });
  if (identicalStorageIter == state.outOfScopeConflictFreeAccesses.end())
    state.outOfScopeConflictFreeAccesses.insert(beginAccess);
  else {
    state.outOfScopeConflictFreeAccesses.erase(identicalStorageIter);
    state.outOfScopeConflictFreeAccesses.insert(beginAccess);
  }
}

// Top-level driver for AccessConflictAndMergeAnalysis
//
// Returns true if the analysis succeeded.
bool AccessConflictAndMergeAnalysis::analyze() {
  if (!identifyBeginAccesses()) {
    LLVM_DEBUG(llvm::dbgs() << "Skipping AccessConflictAndMergeAnalysis...\n");
    return false;
  }
  LoopRegionToAccessStorage accessSetsOfRegions;
  // Populate a worklist of regions such that the top of the worklist is the
  // innermost loop and the bottom of the worklist is the entry block.
  llvm::SmallVector<unsigned, 16> worklist;
  calcBottomUpOrder(worklist);
  propagateAccessSetsBottomUp(accessSetsOfRegions, worklist);

  LLVM_DEBUG(llvm::dbgs() << "Processing Function: "
                          << LRFI->getFunction()->getName() << "\n");
  while (!worklist.empty()) {
    auto regionID = worklist.pop_back_val();
    LLVM_DEBUG(llvm::dbgs() << "Processing Sub-Region: " << regionID << "\n");
    auto *region = LRFI->getRegion(regionID);
    RegionIDToLocalStateMap localRegionStates;
    // This is RPO order of the sub-regions
    for (auto subID : region->getSubregions()) {
      RegionState &state = mergePredAccesses(subID, localRegionStates);

      auto *subRegion = LRFI->getRegion(subID);
      if (subRegion->isBlock()) {
        localDataFlowInBlock(state, subRegion->getBlock());
      } else {
        assert(subRegion->isLoop() && "Expected a loop sub-region");

        const AccessStorageResult &loopStorage = accessSetsOfRegions[subID];
        recordConflicts(state, loopStorage);
      }
    }
  }
  return true;
}

// Find all begin access operations in this function. Map each access to
// AccessInfo, which includes its identified memory location, identifying
// index, and analysis result flags.
//
// Also, add the storage location to the function's RegionStorage
//
// Returns true if it is worthwhile to continue the analysis.
//
// TODO: begin_unpaired_access is not tracked. Even though begin_unpaired_access
// isn't explicitly paired, it may be possible after devirtualization and
// inlining to find all uses of the scratch buffer. However, this doesn't
// currently happen in practice (rdar://40033735).
bool AccessConflictAndMergeAnalysis::identifyBeginAccesses() {
  bool seenPossibleNestedConflict = false;
  bool seenIdenticalStorage = false;
  // Scan blocks in PostOrder (bottom-up) to mark any accesses with identical
  // storage to another reachable access. The earlier access must be marked
  // because this analysis does forward data flow to find conflicts.
  for (auto *BB : PO->getPostOrder()) {
    for (auto &I : llvm::reverse(*BB)) {
      auto *beginAccess = dyn_cast<BeginAccessInst>(&I);
      if (!beginAccess)
        continue;

      if (beginAccess->getEnforcement() != SILAccessEnforcement::Dynamic)
        continue;

      if (!beginAccess->hasNoNestedConflict())
        seenPossibleNestedConflict = true;

      // The accessed base is expected to be valid for begin_access, but for
      // now, since this optimization runs at the end of the pipeline, we
      // gracefully ignore unrecognized source address patterns, which show up
      // here as an invalid `storage` value.
      auto storage = AccessStorage::compute(beginAccess->getSource());

      auto iterAndInserted = storageSet.insert(storage);

      // After inserting it in storageSet, this storage object can be downcast
      // to AccessInfo to use the pass-specific bits.
      auto &accessInfo = static_cast<AccessInfo &>(storage);

      // If the same location was seen later in the CFG, mark this access as one
      // to check for merging.
      if (!iterAndInserted.second) {
        seenIdenticalStorage = true;
        accessInfo.setSeenIdenticalStorage();
      }

      auto iterAndSuccess =
        result.accessMap.try_emplace(beginAccess, accessInfo);
      (void)iterAndSuccess;
      assert(iterAndSuccess.second);

      // Add a pass-specific access index to the mapped storage object.
      AccessInfo &info = iterAndSuccess.first->second;
      info.setAccessIndex(result.accessMap.size() - 1);
      assert(!info.seenNestedConflict());
    }
  }
  return seenPossibleNestedConflict || seenIdenticalStorage;
}

// Returns a mapping from each loop sub-region to all its access storage
// Propagates access sets bottom-up from nested regions
void AccessConflictAndMergeAnalysis::propagateAccessSetsBottomUp(
    LoopRegionToAccessStorage &regionToStorageMap,
    const llvm::SmallVector<unsigned, 16> &worklist) {
  for (unsigned regionID : reverse(worklist)) {
    auto *region = LRFI->getRegion(regionID);
    auto iterAndInserted =
        regionToStorageMap.try_emplace(regionID, AccessStorageResult());
    assert(iterAndInserted.second && "Should not process a region twice");
    AccessStorageResult &accessResult = iterAndInserted.first->second;
    for (auto subID : region->getSubregions()) {
      auto *subRegion = LRFI->getRegion(subID);
      if (subRegion->isLoop()) {
        // propagate access sets bottom-up from nested loops.
        auto subRegionResultIter = regionToStorageMap.find(subID);
        assert(subRegionResultIter != regionToStorageMap.end()
               && "Should have processed sub-region");
        accessResult.mergeFrom(subRegionResultIter->second);
      } else {
        assert(subRegion->isBlock() && "Expected a block region");
        auto *bb = subRegion->getBlock();
        for (auto &instr : *bb) {
          if (auto fullApply = FullApplySite::isa(&instr)) {
            FunctionAccessStorage calleeAccess;
            // Instead of calling getCallSiteEffects, call getCalleeEffects and
            // merge ourselves to avoid an extra merge step.
            ASA->getCalleeEffects(calleeAccess, fullApply);
            accessResult.mergeFrom(calleeAccess.getResult());
            continue;
          }
          // FIXME: Treat may-release conservatively in the analysis itself by
          // adding a mayRelease flag, in addition to the unidentified flag.
          accessResult.analyzeInstruction(&instr);
        }
      }
    }
  }
}

// Helper function for calcBottomUpOrder
static void calcBottomUpOrderRecurse(LoopRegion *region,
                                     llvm::SmallVectorImpl<unsigned> &worklist,
                                     LoopRegionFunctionInfo *LRFI) {
  worklist.push_back(region->getID());
  for (auto regionIndex : region->getReverseSubregions()) {
    auto *region = LRFI->getRegion(regionIndex);
    if (region->isBlock())
      continue;
    calcBottomUpOrderRecurse(region, worklist, LRFI);
  }
}

// Returns a worklist of loop IDs is bottom-up order.
void AccessConflictAndMergeAnalysis::calcBottomUpOrder(
    llvm::SmallVectorImpl<unsigned> &worklist) {
  auto *topRegion = LRFI->getTopLevelRegion();
  calcBottomUpOrderRecurse(topRegion, worklist, LRFI);
}

void AccessConflictAndMergeAnalysis::visitBeginAccess(
    BeginAccessInst *beginAccess, RegionState &state) {
  if (beginAccess->getEnforcement() != SILAccessEnforcement::Dynamic)
    return;

  // Get the Access info:
  auto &beginAccessInfo = result.getAccessInfo(beginAccess);
  if (beginAccessInfo.getKind() == AccessStorage::Unidentified) {
    recordUnknownConflict(state);
    return;
  }

  // Mark in-scope accesses that now have nested conflicts.
  recordInScopeConflicts(state, beginAccessInfo, beginAccess->getAccessKind());
  // Remove in-scope conflicts to avoid checking them again.
  removeConflicts(state.inScopeConflictFreeAccesses, beginAccessInfo);

  if (!beginAccess->hasNoNestedConflict()) {
    // Record the current access as in-scope. It can potentially be folded to
    // [no_nested_conflict] independent of any enclosing access conflicts.
    bool inserted = state.inScopeConflictFreeAccesses.insert(beginAccess);
    (void)inserted;
    assert(inserted && "the begin_access should not have been seen yet.");
  }

  // Find an out-of-scope access that is mergeable with this access. This is
  // done at the BeginAccess because it doesn't matter whether the merged access
  // has any nested conflicts. Consider the following mergeable accesses:
  //
  // begin_access %x
  // end_access %x
  // begin_access %x
  // conflict
  // end_access %x
  if (BeginAccessInst *mergeableAccess =
          findMergeableOutOfScopeAccess(state, beginAccess)) {
    LLVM_DEBUG(llvm::dbgs() << "Found mergeable pair: " << *mergeableAccess
                            << "  with " << *beginAccess << "\n");
    result.mergePairs.emplace_back(mergeableAccess, beginAccess);
  }
  // For the purpose of data-flow, removing the out-of-scope access does not
  // need to be done until the corresponding EndAccess is seen.
}

void AccessConflictAndMergeAnalysis::visitEndAccess(EndAccessInst *endAccess,
                                                    RegionState &state) {
  auto *beginAccess = endAccess->getBeginAccess();
  if (beginAccess->getEnforcement() != SILAccessEnforcement::Dynamic)
    return;

  // Remove the corresponding in-scope access (it is no longer in-scope).
  if (state.inScopeConflictFreeAccesses.remove(beginAccess)) {
    LLVM_DEBUG(llvm::dbgs() << "No conflict on one path from " << *beginAccess
                            << " to " << *endAccess);
  }

  // Any out-of-scope access with non-distinct storage is now longer mergeable.
  // If this access doesn't currently overlap with it, then merging it with
  // another later access could introduce a conflict with this access.
  auto currStorageInfo = result.getAccessInfo(beginAccess);
  removeConflicts(state.outOfScopeConflictFreeAccesses, currStorageInfo);

  // This access is now out-of-scope access; inform data flow.
  insertOutOfScopeAccess(state, beginAccess, currStorageInfo);
}

void AccessConflictAndMergeAnalysis::visitFullApply(FullApplySite fullApply,
                                                    RegionState &state) {
  FunctionAccessStorage callSiteAccesses;
  ASA->getCallSiteEffects(callSiteAccesses, fullApply);

  LLVM_DEBUG(llvm::dbgs() << "Visiting: " << *fullApply.getInstruction()
                          << "  call site accesses:\n";
             callSiteAccesses.dump());
  recordConflicts(state, callSiteAccesses.getResult());
}

void AccessConflictAndMergeAnalysis::visitMayRelease(SILInstruction *instr,
                                                     RegionState &state) {
  // TODO Introduce "Pure Swift" deinitializers
  // We can then make use of alias information for instr's operands
  // If they don't alias - we might get away with not recording a conflict
  LLVM_DEBUG(llvm::dbgs() << "MayRelease Instruction: " << *instr);

  // This is similar to recordUnknownConflict, but only class and global
  // accesses can be affected by a deinitializer.
  auto isHeapAccess = [](AccessStorage::Kind accessKind) {
    return accessKind == AccessStorage::Class || accessKind == AccessStorage::Global;
  };
  // Mark the in-scope accesses as having a nested conflict
  llvm::for_each(state.inScopeConflictFreeAccesses, [&](BeginAccessInst *bai) {
    auto &accessInfo = result.getAccessInfo(bai);
    if (isHeapAccess(accessInfo.getKind())) {
      accessInfo.setSeenNestedConflict();
      LLVM_DEBUG(llvm::dbgs() << "  may conflict with:\n"; accessInfo.dump());
    }
  });

  // Remove both in-scope and out-of-scope accesses from
  // the data flow state.
  state.inScopeConflictFreeAccesses.remove_if([&](BeginAccessInst *bai) {
    auto &accessInfo = result.getAccessInfo(bai);
    return isHeapAccess(accessInfo.getKind());
  });
  state.outOfScopeConflictFreeAccesses.remove_if([&](BeginAccessInst *bai) {
    auto &accessInfo = result.getAccessInfo(bai);
    return isHeapAccess(accessInfo.getKind());
  });
}

// Merge the data flow result in 'otherSet' into 'accessSet'.  If 'accessSet' is
// not initialized, simply copy 'otherSet'; otherwise, "merge" the results by
// deleting any accesses that aren't in common.
void AccessConflictAndMergeAnalysis::mergeAccessSet(
    DenseAccessSet &accessSet, const DenseAccessSet &otherSet,
    bool isInitialized) {
  if (!isInitialized) {
    accessSet.insert(otherSet.begin(), otherSet.end());
    return;
  }
  accessSet.remove_if(
      [&](BeginAccessInst *bai) { return !otherSet.count(bai); });
}

// Merge the data flow result in `otherState` into `state`.
void AccessConflictAndMergeAnalysis::mergeState(RegionState &state,
                                                const RegionState &otherState,
                                                bool isInitialized) {
  mergeAccessSet(state.inScopeConflictFreeAccesses,
                 otherState.inScopeConflictFreeAccesses, isInitialized);
  mergeAccessSet(state.outOfScopeConflictFreeAccesses,
                 otherState.outOfScopeConflictFreeAccesses, isInitialized);
}

RegionState &AccessConflictAndMergeAnalysis::mergePredAccesses(
    unsigned regionID, RegionIDToLocalStateMap &localRegionStates) {
  auto regionStateIterAndInserted = localRegionStates.try_emplace(
      regionID, RegionState(result.accessMap.size()));
  assert(regionStateIterAndInserted.second && "only visit each region once");
  RegionState &state = regionStateIterAndInserted.first->second;

  auto *region = LRFI->getRegion(regionID);
  auto bbRegionParentID = region->getParentID();
  bool isInitialized = false;
  for (auto pred : region->getPreds()) {
    auto *predRegion = LRFI->getRegion(pred);
    assert((predRegion->getParentID() == bbRegionParentID) &&
           "predecessor is not part of the parent region - unhandled control "
           "flow");
    (void)predRegion;
    (void)bbRegionParentID;
    auto predStateIter = localRegionStates.find(pred);
    if (predStateIter == localRegionStates.end()) {
      // Backedge / irreducible control flow - bail
      state.reset();
      break;
    }
    mergeState(state, predStateIter->second, isInitialized);
    isInitialized = true;
  }
  return state;
}

void AccessConflictAndMergeAnalysis::localDataFlowInBlock(RegionState &state,
                                                          SILBasicBlock *bb) {
  for (auto &instr : *bb) {
    if (auto *beginAccess = dyn_cast<BeginAccessInst>(&instr)) {
      visitBeginAccess(beginAccess, state);
      continue;
    }
    if (auto *endAccess = dyn_cast<EndAccessInst>(&instr)) {
      visitEndAccess(endAccess, state);
      continue;
    }
    if (auto fullApply = FullApplySite::isa(&instr)) {
      visitFullApply(fullApply, state);
      continue;
    }
    if (instr.mayRelease()) {
      visitMayRelease(&instr, state);
    }
  }
}

// -----------------------------------------------------------------------------
// MARK: Access Enforcement Optimization
// -----------------------------------------------------------------------------

/// Perform access folding.
///
/// Data-flow analysis is now complete. Any begin_access that has seen a
/// conflict can be given the [no_nested_conflict] instruction attribute.
///
/// Note: If we later support marking begin_unpaired_access
/// [no_nested_conflict], then we also need to remove any corresponding
/// end_unpaired_access. That can be done either by recording the
/// end_unpaired_access instructions during analysis and deleting them here in
/// the same order, or sorting them here by their begin_unpaired_access index.
static bool
foldNonNestedAccesses(AccessConflictAndMergeAnalysis::AccessMap &accessMap) {
  bool changed = false;
  // Iteration over accessMap is nondeterministic. Setting the conflict flags
  // can be done in any order.
  for (auto &beginAccessAndInfo : accessMap) {
    BeginAccessInst *beginAccess = beginAccessAndInfo.first;
    AccessInfo &info = beginAccessAndInfo.second;
    if (info.seenNestedConflict())
      continue;

    // Optimize this begin_access by setting [no_nested_conflict].
    beginAccess->setNoNestedConflict(true);
    changed = true;
    LLVM_DEBUG(llvm::dbgs() << "Folding " << *beginAccess);
  }
  return changed;
}

/// Perform local access marker elimination.
///
/// Disable access checks for uniquely identified local storage for which no
/// accesses can have nested conflicts. This is only valid if the function's
/// local storage cannot be potentially modified by unidentified access:
///
/// - Arguments cannot alias with local storage, so accessing an argument has no
///   effect on analysis of the current function. When a callee accesses an
///   argument, AccessStorageAnalysis will either map the accessed storage to
///   a value in the caller's function, or mark it as unidentified.
///
/// - Stack or Box local storage could potentially be accessed via Unidentified
///   access. (Some Unidentified accesses are for initialization or for
///   temporary storage instead, but those should never have Dynamic
///   enforcement). These accesses can only be eliminated when there is no
///   Unidentified access within the function without the [no_nested_conflict]
///   flag.
static bool
removeLocalNonNestedAccess(const AccessConflictAndMergeAnalysis::Result &result,
                           const FunctionAccessStorage &functionAccess) {
  if (functionAccess.hasUnidentifiedAccess())
    return false;

  bool changed = false;
  SmallVector<BeginAccessInst *, 8> deadAccesses;
  for (auto &beginAccessAndInfo : result.accessMap) {
    BeginAccessInst *beginAccess = beginAccessAndInfo.first;
    const AccessInfo &info = beginAccessAndInfo.second;
    if (info.seenNestedConflict() || !info.isLocal())
      continue;

    // This particular access to local storage is marked
    // [no_nested_conflict]. Now check FunctionAccessStorage to determine if
    // that is true for all access to the same storage.
    if (functionAccess.hasNoNestedConflict(info)) {
      LLVM_DEBUG(llvm::dbgs() << "Disabling dead access " << *beginAccess);
      beginAccess->setEnforcement(SILAccessEnforcement::Static);
      changed = true;
    }
  }
  return changed;
}

// TODO: support multi-end access cases
static EndAccessInst *getSingleEndAccess(BeginAccessInst *inst) {
  EndAccessInst *end = nullptr;
  for (auto *currEnd : inst->getEndAccesses()) {
    if (end == nullptr)
      end = currEnd;
    else
      return nullptr;
  }
  return end;
}

struct SCCInfo {
  unsigned id;
  bool hasLoop;
};

static void mergeEndAccesses(BeginAccessInst *parentIns,
                             BeginAccessInst *childIns) {
  auto *endP = getSingleEndAccess(parentIns);
  if (!endP)
    llvm_unreachable("not supported");
  auto *endC = getSingleEndAccess(childIns);
  if (!endC)
    llvm_unreachable("not supported");

  endC->setOperand(parentIns);
  endP->eraseFromParent();
}

static bool canMergeEnd(BeginAccessInst *parentIns, BeginAccessInst *childIns) {
  auto *endP = getSingleEndAccess(parentIns);
  if (!endP)
    return false;

  auto *endC = getSingleEndAccess(childIns);
  if (!endC)
    return false;

  return true;
}

// TODO: support other merge patterns
static bool
canMergeBegin(PostDominanceInfo *postDomTree,
              const llvm::DenseMap<SILBasicBlock *, SCCInfo> &blockToSCCMap,
              BeginAccessInst *parentIns, BeginAccessInst *childIns) {
  if (!postDomTree->properlyDominates(childIns, parentIns)) {
    return false;
  }
  auto parentSCCIt = blockToSCCMap.find(parentIns->getParent());
  assert(parentSCCIt != blockToSCCMap.end() && "Expected block in SCC Map");
  auto childSCCIt = blockToSCCMap.find(childIns->getParent());
  assert(childSCCIt != blockToSCCMap.end() && "Expected block in SCC Map");
  auto parentSCC = parentSCCIt->getSecond();
  auto childSCC = childSCCIt->getSecond();
  if (parentSCC.id == childSCC.id) {
    return true;
  }
  if (parentSCC.hasLoop) {
    return false;
  }
  if (childSCC.hasLoop) {
    return false;
  }
  return true;
}

static bool
canMerge(PostDominanceInfo *postDomTree,
         const llvm::DenseMap<SILBasicBlock *, SCCInfo> &blockToSCCMap,
         BeginAccessInst *parentIns, BeginAccessInst *childIns) {
  // A [read] access cannot be converted to a [modify] without potentially
  // introducing new conflicts that were previously ignored. Merging read/modify
  // will require additional data flow information.
  if (childIns->getAccessKind() != parentIns->getAccessKind())
    return false;

  if (!canMergeBegin(postDomTree, blockToSCCMap, parentIns, childIns))
    return false;

  return canMergeEnd(parentIns, childIns);
}

static bool extendOwnership(BeginAccessInst *parentInst,
                            BeginAccessInst *childInst,
                            InstructionDeleter &deleter,
                            DeadEndBlocks &deBlocks) {
  GuaranteedOwnershipExtension extension(deleter, deBlocks,
                                         parentInst->getFunction());
  auto status = extension.checkAddressOwnership(parentInst, childInst);
  switch (status) {
  case GuaranteedOwnershipExtension::Invalid:
    return false;
  case GuaranteedOwnershipExtension::Valid:
    return true;
  case GuaranteedOwnershipExtension::ExtendLifetime:
  case GuaranteedOwnershipExtension::ExtendBorrow:
    break;
  }
  extension.transform(status);
  return true;
}

/// Perform access merging.
static bool
mergeAccesses(SILFunction *F, PostDominanceInfo *postDomTree,
              const AccessConflictAndMergeAnalysis::MergeablePairs &mergePairs,
              DeadEndBlocks &deBlocks) {

  if (mergePairs.empty()) {
    LLVM_DEBUG(llvm::dbgs() << "Skipping SCC Analysis...\n");
    return false;
  }

  bool changed = false;

  // Compute a map from each block to its SCC -
  // For now we can't merge cross SCC boundary
  llvm::DenseMap<SILBasicBlock *, SCCInfo> blockToSCCMap;
  SCCInfo info;
  info.id = 0;
  for (auto sccIt = scc_begin(F); !sccIt.isAtEnd(); ++sccIt) {
    ++info.id;
    info.hasLoop = sccIt.hasCycle();
    for (auto *bb : *sccIt) {
      blockToSCCMap.insert(std::make_pair(bb, info));
    }
  }
  // make a temporary reverse copy to work on:
  // It is in reverse order just to make it easier to debug / follow
  AccessConflictAndMergeAnalysis::MergeablePairs workPairs;
  workPairs.append(mergePairs.rbegin(), mergePairs.rend());

  // Assume the result contains two access pairs to be merged:
  // (begin_access %1, begin_access %2)
  // = merge end_access %1 with begin_access %2
  // (begin_access %2, begin_access %3)
  // = merge end_access %2 with begin_access %3
  // After merging the first pair, begin_access %2 is removed,
  // so the second pair in the result list points to a to-be-deleted
  // begin_access instruction. We store (begin_access %2 -> begin_access %1)
  // to re-map a merged begin_access to it's replaced instruction.
  llvm::DenseMap<BeginAccessInst *, BeginAccessInst *> oldToNewMap;
  InstructionDeleter deleter;

  while (!workPairs.empty()) {
    auto curr = workPairs.pop_back_val();
    auto *parentIns = curr.first;
    auto *childIns = curr.second;
    if (oldToNewMap.count(parentIns) != 0) {
      parentIns = oldToNewMap[parentIns];
    }
    assert(oldToNewMap.count(childIns) == 0 &&
           "Can't have same child instruction twice in map");

    // The optimization might not currently support every mergeable pair
    // If the current pattern is not supported - skip
    if (!canMerge(postDomTree, blockToSCCMap, parentIns, childIns))
      continue;

    if (!extendOwnership(parentIns, childIns, deleter, deBlocks))
      return false;

    LLVM_DEBUG(llvm::dbgs()
               << "Merging " << *childIns << " into " << *parentIns << "\n");

    // Change the no nested conflict of parent if the child has a nested
    // conflict.
    if (!childIns->hasNoNestedConflict())
      parentIns->setNoNestedConflict(false);

    // remove end accesses and create new ones that cover bigger scope:
    mergeEndAccesses(parentIns, childIns);

    // In case the child instruction is at the map,
    // updated the oldToNewMap to reflect that we are getting rid of it:
    oldToNewMap.insert(std::make_pair(childIns, parentIns));

    // Modify the users of child instruction to use the parent:
    childIns->replaceAllUsesWith(parentIns);

    changed = true;
  }

  // Delete all old instructions from parent scopes:
  while (!oldToNewMap.empty()) {
    auto curr = oldToNewMap.begin();
    auto *oldIns = curr->getFirst();
    oldToNewMap.erase(oldIns);
    deleter.forceDelete(oldIns);
  }
  deleter.cleanupDeadInstructions();
  return changed;
}

namespace {
struct AccessEnforcementOpts : public SILFunctionTransform {
  void run() override {
    SILFunction *F = getFunction();
    if (F->empty())
      return;

    LLVM_DEBUG(llvm::dbgs() << "Running local AccessEnforcementOpts on "
                            << F->getName() << "\n");

    LoopRegionFunctionInfo *LRFI = getAnalysis<LoopRegionAnalysis>()->get(F);
    PostOrderFunctionInfo *PO = getAnalysis<PostOrderAnalysis>()->get(F);
    DeadEndBlocksAnalysis *deBlocksAnalysis =
        PM->getAnalysis<DeadEndBlocksAnalysis>();
    AccessStorageAnalysis *ASA = getAnalysis<AccessStorageAnalysis>();
    AccessConflictAndMergeAnalysis a(LRFI, PO, ASA);
    if (!a.analyze())
      return;

    auto result = a.getResult();

    // Perform access folding by setting the [no_nested_conflict] flag on
    // begin_access instructions.
    if (foldNonNestedAccesses(result.accessMap)) {
      // Recompute AccessStorageAnalysis, just for this function, to update the
      // StorageAccessInfo::noNestedConflict status for each accessed storage.
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }

    // Use the updated AccessStorageAnalysis to find any uniquely identified
    // local storage that has no nested conflict on any of its accesses within
    // this function. All the accesses can be marked as statically enforced.
    //
    // Note that the storage address may be passed as an argument and there may
    // be nested conflicts within that call, but none of the accesses within
    // this function will overlap.
    const FunctionAccessStorage &functionAccess = ASA->getEffects(F);
    if (removeLocalNonNestedAccess(result, functionAccess))
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);

    // Perform the access merging
    // The inital version of the optimization requires a postDomTree
    PostDominanceAnalysis *postDomAnalysis =
        getAnalysis<PostDominanceAnalysis>();
    PostDominanceInfo *postDomTree = postDomAnalysis->get(F);
    DeadEndBlocks *deBlocks = deBlocksAnalysis->get(F);
    if (mergeAccesses(F, postDomTree, result.mergePairs, *deBlocks))
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
};
} // namespace

SILTransform *swift::createAccessEnforcementOpts() {
  return new AccessEnforcementOpts();
}
