//===------ AccessEnforcementDom.cpp - dominated access removal opt -------===//
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
/// This function pass removes dominated accesses in two ways:
///
/// 1) Remove accesses dominated by an existing access
///
/// General case:
/// begin_access A (may or may not have no_nested_conflict)
/// load/store
/// end_access
/// ...
/// begin_access A [no_nested_conflict] // dominated by the first access
/// load/store
/// end_access A
///
/// The second access scope does not need to be emitted.
///
/// 2) Add a new dominating accesses to loop's preheader
///
/// General case:
/// <loop preheader>
/// A = ref_element_addr
/// <loop>
/// begin_access A [dynamic] [no_nested_conflict]
///
/// Adding an empty begin_access A in the preheader would allow us to
/// turn the loop's access to [static]
///
/// Warning: This optimization requires that all points within this function
/// that begin an access can be identified. Failure to recognize the beginning
/// of an access scope could weaken dynamic enforcement.
///
/// FIXME: This pass currently only runs in the last-chance pipeline, with a
/// guarantee that no access marker removal is done after it. This happens to
/// work but is dangerous and violates SIL semantics. We should instead add a
/// flag for accesses to give them the semantics that they may guard memory
/// operations other than those enclosed by the access scope.
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "access-enforcement-dom"

#include "swift/Basic/Assertions.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/LoopAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/DepthFirstIterator.h"

using namespace swift;

// =============================================================================
// DominatedAccessAnalysis

namespace swift {
/// Information about each dynamic access with valid storage.
///
/// This is a pass-specific subclass of AccessStorage with identical layout.
/// An instance is created for each BeginAccess in the current function. In
/// additional to identifying the access' storage location, it associates that
/// access with pass-specific data in reserved bits. The reserved bits do not
/// participate in equality or hash lookup.
class DomAccessStorage : public AccessStorage {
public:
  DomAccessStorage() {}

  explicit DomAccessStorage(const AccessStorage &storage)
      : AccessStorage(storage) {
    Bits.DomAccessStorage.isInner = false;
    Bits.DomAccessStorage.containsRead = false;
  }

  // Is this dynamic, identifiable access scope potentially contained within any
  // kind of outer scope. The outer scope may be static and/or have unidentified
  // storage. For example:
  //
  // A1: [static] [read]
  // A2: [dynamic] [read]
  // A3: [dynamic] [modify]
  //
  // A2 cannot be promoted to modify if its scope overlaps with A1.
  bool isInner() const { return Bits.DomAccessStorage.isInner; }

  void setIsInner() { Bits.DomAccessStorage.isInner = true; }

  /// Is this dynamic, identifiable access scope a [read], and does it
  /// potentially contain another non-distinct [read] access of any kind?
  bool containsRead() const { return Bits.DomAccessStorage.containsRead; }

  void setContainsRead() { Bits.DomAccessStorage.containsRead = true; }

  void dump() const {
    AccessStorage::dump();
    llvm::dbgs() << "<" << (isInner() ? "" : "inner")
                 << (containsRead() ? "" : "containsRead") << ">\n";
  }
};
} // namespace swift

namespace {
// An analysis that maps each valid dynamic BeginAccess to
// DomAccessStorage. Performs a trivial data flow analysis to populate the map
// with information about nested accesses. Data flow is needed to track open
// access scopes, but only flow-insensitive information is recorded in the
// result.
//
// Note that all access scopes are tracked during data flow, but only valid
// dynamic are mapped to results.
//
// TODO: Separate this into a shared analysis and factor it with
// AccessEnforcementOpts. The optimization that merges accesses would also
// benefit.
//
// TODO: This could be made more precise by querying AccessStorageAnalysis.
class DominatedAccessAnalysis {
public:
  // The result records information for all dynamic accesses in this
  // function. If an UnpairedAccess exists, then the result will be
  // conservatively empty.
  struct Result {
    llvm::SmallDenseMap<BeginAccessInst *, DomAccessStorage, 32> accessMap;
  };

private:
  // The data flow state for each block. It is only valid between the time at
  // least one predecesor block has been finished and before its own block
  // has been finished.
  //
  // The isBottom flag allows the analysis to avoid quadratic behavior where
  // each open access must be updated at each conservatively handled
  // instruction. Instead, the accesses are only conservatively updated once
  // until the next scope is entered. So the complexity is (#OpenScopes)^2
  // instead of (#OpenScopes)*(#Applies).
  struct BBState {
    using DenseAccessSet = llvm::SmallDenseSet<BeginAccessInst *, 4>;
    using DenseCoroutineSet = llvm::SmallDenseSet<BeginApplyInst *, 4>;

    DenseAccessSet inScopeAccesses;
    DenseCoroutineSet inScopeCoroutines;
    bool isBottom = false;
  };
  using BlockStateMap = llvm::DenseMap<SILBasicBlock *, BBState>;

  PostOrderFunctionInfo *PO;

  Result result; // Flow-insensitive analysis result.

  BlockStateMap blockStateMap; // Data flow state.

public:
  DominatedAccessAnalysis(PostOrderFunctionInfo *PO) : PO(PO) {}

  Result analyze() &&;

protected:
  void setBottom(BBState &state);
  void analyzeAccess(BeginAccessInst *BAI, BBState &state);
};
} // namespace

// Set information for all in-scope accesses to the worst case "bottom".
// The isInner flag is not affected because it only applies to scopes inside the
// currently open scopes, not the currently open scopes themselves.
void DominatedAccessAnalysis::setBottom(BBState &state) {
  if (state.isBottom)
    return;

  // Unordered iteration over the in-access scopes.
  llvm::for_each(state.inScopeAccesses, [this](BeginAccessInst *BAI) {
    if (auto &domStorage = result.accessMap[BAI])
      domStorage.setContainsRead();
  });
}

// Perform the analysis and return the Result, relinquishing internal state.
DominatedAccessAnalysis::Result DominatedAccessAnalysis::analyze() && {
  // A single RPO traversal is sufficient to visit access scopes in order. The
  // set of open accesses entering a block cannot grow as a result of loops, and
  // within an open scope the results are flow-insensitive.
  for (auto *BB : PO->getReversePostOrder()) {
    BBState state = blockStateMap[BB];
    for (auto &I : *BB) {
      if (auto *BAI = dyn_cast<BeginAccessInst>(&I)) {
        analyzeAccess(BAI, state);
        continue;
      }
      if (auto *EAI = dyn_cast<EndAccessInst>(&I)) {
        bool erased = state.inScopeAccesses.erase(EAI->getBeginAccess());
        (void)erased;
        assert(erased);
        continue;
      }
      // Check for BeginApply before checking FullApplySite below.
      if (auto *beginApply = dyn_cast<BeginApplyInst>(&I)) {
        auto iterAndInserted = state.inScopeCoroutines.insert(beginApply);
        (void)iterAndInserted;
        assert(iterAndInserted.second);
        continue;
      }
      if (auto *endApply = dyn_cast<EndApplyInst>(&I)) {
        bool erased = state.inScopeCoroutines.erase(endApply->getBeginApply());
        (void)erased;
        assert(erased);
        continue;
      }
      if (FullApplySite::isa(&I)) {
        setBottom(state);
        continue;
      }
      if (isa<BeginUnpairedAccessInst>(&I)) {
        // Unpaired accesses could be tracked, but are ignored because they are
        // mostly irrelevant and hard to test. Completely bail on this function.
        result.accessMap.clear();
        return std::move(result);
      }
    }
    auto successors = BB->getTerminator()->getSuccessors();
    unsigned numSucc = successors.size();
    for (unsigned succIdx : indices(successors)) {
      SILBasicBlock *succBB = successors[succIdx].getBB();
      if (succBB == BB)
        continue;

      if (succIdx != numSucc - 1)
        blockStateMap.try_emplace(succBB, state);
      else
        // Move the state into the last successor to avoid copying sets.
        blockStateMap.try_emplace(succBB, std::move(state));
    }
  }
  return std::move(result);
}

// The data flow transfer function for BeginAccess. Creates the
// DomAccessStorage for this access and inserts it in the result.
void DominatedAccessAnalysis::analyzeAccess(BeginAccessInst *BAI,
                                            BBState &state) {
  DomAccessStorage domStorage;
  // Only track dynamic access in the result. Static accesses still need to be
  // tracked by data flow, but they can't be optimized as "dominating".
  if (BAI->getEnforcement() == SILAccessEnforcement::Dynamic) {
    auto storage = AccessStorage::compute(BAI->getSource());
    // Copy the AccessStorage into DomAccessStorage. All pass-specific bits
    // are initialized to zero.
    domStorage = DomAccessStorage(storage);
  }
  // Continue to handle both untracked access and invalid domStorage
  // conservatively below...

  // unordered set iteration...
  llvm::for_each(state.inScopeAccesses, [&](BeginAccessInst *outerBegin) {
    auto &outerInfo = result.accessMap[outerBegin];
    // If the current access is mapped, set its isInner flag.
    if (domStorage && !domStorage.isInner()) {
      if (domStorage.isDistinctFrom(outerInfo))
        return;

      domStorage.setIsInner();
    }
    // The results for tracked in-scope accesses still need to be
    // updated even if the current access is not be tracked.
    if (outerInfo && BAI->getAccessKind() == SILAccessKind::Read
        && outerBegin->getAccessKind() == SILAccessKind::Read) {
      outerInfo.setContainsRead();
    }
  });
  // Track this access even if it is invalid or unmapped.
  {
    auto iterAndInserted = state.inScopeAccesses.insert(BAI);
    (void)iterAndInserted;
    assert(iterAndInserted.second);
  }
  // Update the results if this access will be mapped.
  if (!domStorage)
    return;

  // Set the current access isInner flag if it's inside a coroutine scope.
  if (!state.inScopeCoroutines.empty())
    domStorage.setIsInner();

  // Map the current access.
  {
    auto iterAndInserted = result.accessMap.try_emplace(BAI, domStorage);
    (void)iterAndInserted;
    assert(iterAndInserted.second);
  }
  state.isBottom = false;
}

// =============================================================================
// DominatedAccessRemoval optimization.

namespace {
using DomTreeNode = llvm::DomTreeNodeBase<SILBasicBlock>;

// Visit the dominator tree top down, tracking the current set of dominating
// dynamic accesses. Dominated dynamic accesses with identical storage are
// marked static during traversal. If a dynamic access inside a loop has no
// dominating access, insert a new access in the preheader.
class DominatedAccessRemoval {
  // Record the first access of a given storage location and the dominator node
  // in which the access occurred.
  struct DominatingAccess {
    BeginAccessInst *beginAccess;
    DomTreeNode *domNode;

    DominatingAccess(BeginAccessInst *beginAccess, DomTreeNode *domNode)
        : beginAccess(beginAccess), domNode(domNode) {}
  };
  using StorageToDomMap = llvm::DenseMap<AccessStorage, DominatingAccess>;

  SILFunction &func;
  DominanceInfo *domInfo;
  SILLoopInfo *loopInfo;
  DominatedAccessAnalysis::Result &DAA;

  // Hash map from each storage location to the dominating access.
  StorageToDomMap storageToDomMap;

  DomTreeNode *currDomNode = nullptr;

  bool hasChanged = false;

public:
  DominatedAccessRemoval(SILFunction &func, DominanceInfo *domInfo,
                         SILLoopInfo *loopInfo,
                         DominatedAccessAnalysis::Result &DAA)
      : func(func), domInfo(domInfo), loopInfo(loopInfo), DAA(DAA) {}

  bool optimize();

protected:
  void visitBeginAccess(BeginAccessInst *BAI);
  bool checkDominatedAccess(BeginAccessInst *BAI,
                            DomAccessStorage currDomStorage);
  bool optimizeDominatedAccess(BeginAccessInst *currBegin,
                               DomAccessStorage currDomStorage,
                               const DominatingAccess &domAccess);
  void tryInsertLoopPreheaderAccess(BeginAccessInst *BAI,
                                    DomAccessStorage currDomStorage);
};
} // namespace

// Optimize the current function, and return true if any optimization was
// performed.
bool DominatedAccessRemoval::optimize() {
  DomTreeNode *entryNode = domInfo->getNode(func.getEntryBlock());
  for (DomTreeNode *domNode : llvm::depth_first(entryNode)) {
    currDomNode = domNode;

    // Optimize dominated accesses in this block.
    for (auto &instr : *domNode->getBlock()) {
      if (auto *BAI = dyn_cast<BeginAccessInst>(&instr))
        visitBeginAccess(BAI);
    }
  }
  return hasChanged;
}

// Visit a BeginAccessInst once-and-only-once in domtree order.
// Attempt to find a dominating access with identical storage.
// If that fails, attempt to insert a new dominating access in the preheader.
void DominatedAccessRemoval::visitBeginAccess(BeginAccessInst *BAI) {
  if (BAI->getEnforcement() != SILAccessEnforcement::Dynamic)
    return;

  DomAccessStorage currDomStorage = DAA.accessMap.lookup(BAI);
  if (!currDomStorage)
    return;

  // Only track "identifiable" storage.
  if (currDomStorage.isFormalAccessBase()) {
    if (checkDominatedAccess(BAI, currDomStorage))
      return;
  }
  tryInsertLoopPreheaderAccess(BAI, currDomStorage);
}

// Track this identifiable dynamic access in storageToDomMap, and optimize it if
// possible. Return true if the optimization succeeds.
bool DominatedAccessRemoval::checkDominatedAccess(
    BeginAccessInst *BAI, DomAccessStorage currDomStorage) {
  // Attempt to add this access to storageToDomMap using its base storage
  // location as the key.
  //
  // Cast this DomAccessStorage back to a plain storage location. The
  // pass-specific bits will be ignored, but reset them anyway for soundness.
  AccessStorage storage = static_cast<AccessStorage>(currDomStorage);
  storage.resetSubclassData();
  auto iterAndInserted =
      storageToDomMap.try_emplace(storage, DominatingAccess(BAI, currDomNode));
  if (iterAndInserted.second)
    return false;

  // An access has already been recorded for this storage.
  // If the previous domNode does not dominate currDomNode, then replace it.
  DominatingAccess &domAccess = iterAndInserted.first->second;
  if (!domInfo->dominates(domAccess.domNode, currDomNode)) {
    domAccess = DominatingAccess(BAI, currDomNode);
    return false;
  }
  // The previously mapped access still dominates this block, so the current
  // access can potentially be optimized.
  return optimizeDominatedAccess(BAI, currDomStorage, domAccess);
}

// If possible, optimize the current access by converting it to [static]. Return
// true if the optimization succeeds.
//
// This function is not allowed to add or erase instructions, only change
// instruction flags.
//
// The four required conditions for converting this access to static are:
//
// 1. A closed dominating access has identical storage.
//
// The caller looked up this access' storage in storageToDomMap and checked
// that the previously seen access dominates this block. As long as this access'
// isInner flag from DominatedAccessAnalysis is not set, the dominating access
// must be closed.
//
// 2. There is no open (overlapping) access with nondistinct storage (that isn't
// also open at the dominating access).
//
// The isInner flag from DominatedAccessAnalysis indicates no enclosing
// nondistinct scopes within this function. Any outer scope would enclose the
// whole function, thereby also enclosing the dominating scope.
//
// 3. This access has no_nested_conflict.
//
// This is a direct check on this BeginAccessInst flag.
//
// 4. The current and dominating access kinds are compatible:
//
// read -> read: OK
//
// modify -> read: OK
//
// modify -> modify: OK
//
// read -> modify: Requires promoting the dominating access to a modify. This
// can be done as long as the dominating access does not contain another
// non-distinct read and isn't contained by another non-distinct read. The
// containsRead and isInner flags from in DominatedAccessAnalysis answer this
// conservatively.
//
// Note: Promoting an earlier access to a modify could cause a program to
// trap when optimized even if the unoptimized program does not trap; the
// original modify access may be on an unreachable code path. This is acceptable
// because:
//
// (a) in theory, exclusivity violations do not need to be executed to be
// considered program violations. Promoting the access does not introduce any
// new conflict where one didn't already exist statically. Catching these
// violations at runtime is only an implementation compromise, and the more true
// violations are caught, the better.
//
// (b) in practice, this situation is so exceedingly unlikely that it won't
// cause any pervasive usability problem where programs have stronger
// enforcement only when optimized.
bool DominatedAccessRemoval::optimizeDominatedAccess(
    BeginAccessInst *BAI, DomAccessStorage currAccessInfo,
    const DominatingAccess &domAccess) {
  // 1. and 2. If any nondistinct scopes are open, it must remain dynamic.
  if (currAccessInfo.isInner())
    return false;

  // 3. If BAI may have a nested conflict, it must remain dynamic.
  if (!BAI->hasNoNestedConflict())
    return false;

  // 4. Promoting a read to a modify is only safe with no nested reads.
  if (domAccess.beginAccess->getAccessKind() == SILAccessKind::Read
      && BAI->getAccessKind() == SILAccessKind::Modify) {
    DomAccessStorage domStorage = DAA.accessMap[domAccess.beginAccess];
    if (domStorage.containsRead() || domStorage.isInner())
      return false;

    LLVM_DEBUG(llvm::dbgs()
               << "Promoting to modify: " << *domAccess.beginAccess << "\n");
    domAccess.beginAccess->setAccessKind(SILAccessKind::Modify);
  }
  LLVM_DEBUG(llvm::dbgs() << "Setting static enforcement: " << *BAI << "\n");
  LLVM_DEBUG(llvm::dbgs() << "Dominated by: " << *domAccess.beginAccess
                          << "\n");
  BAI->setEnforcement(SILAccessEnforcement::Static);

  hasChanged = true;
  return true;
}

// Attempt to insert a new access in the loop preheader. If successful, insert
// the new access in DominatedAccessAnalysis so it can be used to dominate other
// accesses. Also convert the current access to static and update the current
// storageToDomMap since the access may already have been recorded (when it was
// still dynamic).
//
// This function cannot add or remove instructions in the current block, but
// may add instructions to the current loop's preheader.
//
// The required conditions for inserting a new dominating access are:
//
// 1. The new preheader access is not enclosed in another scope that doesn't
// also enclose the current scope.
//
// This is inferred from the loop structure; any scope that encloses the
// preheader must also enclose the entire loop.
//
// 2. The current access is not enclosed in another scope that doesn't also
// enclose the preheader.
//
// As before, it is sufficient to check this access' isInner flags in
// DominatedAccessAnalysis; if this access isn't enclosed by any scope within
// the function, then it can't be enclosed within a scope inside the loop.
//
// 3. The current header has no nested conflict within its scope.
//
// 4. The access' source operand is available in the loop preheader.
void DominatedAccessRemoval::tryInsertLoopPreheaderAccess(
    BeginAccessInst *BAI, DomAccessStorage currAccessInfo) {
  // 2. the current access may be enclosed.
  if (currAccessInfo.isInner())
    return;

  // 3. the current access must be instantaneous.
  if (!BAI->hasNoNestedConflict())
    return;

  SILLoop *currLoop = loopInfo->getLoopFor(BAI->getParent());
  if (!currLoop)
    return;
  SILBasicBlock *preheader = currLoop->getLoopPreheader();
  if (!preheader)
    return;

  // 4. The source operand must be available in the preheader.
  auto sourceOperand = BAI->getOperand();
  auto *sourceBB = sourceOperand->getParentBlock();
  if (!domInfo->dominates(sourceBB, preheader))
    return;

  // Insert a new access scope immediately before the
  // preheader's terminator.
  TermInst *preheaderTerm = preheader->getTerminator();
  SILBuilderWithScope scopeBuilder(preheaderTerm);
  BeginAccessInst *newBegin = scopeBuilder.createBeginAccess(
      preheaderTerm->getLoc(), sourceOperand, BAI->getAccessKind(),
      SILAccessEnforcement::Dynamic, true /*no nested conflict*/,
      BAI->isFromBuiltin());
  scopeBuilder.createEndAccess(preheaderTerm->getLoc(), newBegin, false);
  LLVM_DEBUG(llvm::dbgs() << "Created loop preheader access: " << *newBegin
                          << "\n"
                          << "dominating: " << *BAI << "\n");
  BAI->setEnforcement(SILAccessEnforcement::Static);

  hasChanged = true;

  // Insert the new dominating instruction in both DominatedAccessAnalysis and
  // storageToDomMap if it has uniquely identifiable storage.
  if (!currAccessInfo.isFormalAccessBase())
    return;

  AccessStorage storage = static_cast<AccessStorage>(currAccessInfo);
  storage.resetSubclassData();

  // Create a DomAccessStorage for the new access with no flags set.
  DAA.accessMap.try_emplace(newBegin, DomAccessStorage(storage));

  // Track the new access as long as no other accesses from the same storage are
  // already tracked. This also necessarily replaces the current access, which
  // was just made static.
  DominatingAccess newDomAccess(newBegin, domInfo->getNode(preheader));
  auto iterAndInserted = storageToDomMap.try_emplace(storage, newDomAccess);
  if (!iterAndInserted.second) {
    DominatingAccess &curDomAccess = iterAndInserted.first->second;
    if (curDomAccess.beginAccess == BAI)
      curDomAccess = newDomAccess;
  }
}

namespace {
struct AccessEnforcementDom : public SILFunctionTransform {
  void run() override;
};
} // namespace

void AccessEnforcementDom::run() {
  SILFunction *func = getFunction();
  if (func->empty())
    return;

  PostOrderFunctionInfo *PO = getAnalysis<PostOrderAnalysis>()->get(func);
  auto DAA = DominatedAccessAnalysis(PO).analyze();

  DominanceAnalysis *domAnalysis = getAnalysis<DominanceAnalysis>();
  DominanceInfo *domInfo = domAnalysis->get(func);
  SILLoopAnalysis *loopAnalysis = PM->getAnalysis<SILLoopAnalysis>();
  SILLoopInfo *loopInfo = loopAnalysis->get(func);

  DominatedAccessRemoval eliminationPass(*func, domInfo, loopInfo, DAA);
  if (eliminationPass.optimize())
    invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
}

SILTransform *swift::createAccessEnforcementDom() {
  return new AccessEnforcementDom();
}
