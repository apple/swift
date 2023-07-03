//===--- AccessStorageAnalysis.cpp  - Accessed Storage Analysis ---------===//
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

#define DEBUG_TYPE "sil-sea"

#include "swift/SILOptimizer/Analysis/AccessStorageAnalysis.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/Analysis/FunctionOrder.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"

using namespace swift;

// -----------------------------------------------------------------------------
// MARK: Accessing the results.
// -----------------------------------------------------------------------------

bool AccessStorageResult::hasNoNestedConflict(
    const AccessStorage &otherStorage) const {
  assert(otherStorage.isUniquelyIdentified());
  assert(!hasUnidentifiedAccess());

  return getStorageAccessInfo(otherStorage).hasNoNestedConflict();
}

bool AccessStorageResult::mayConflictWith(
    SILAccessKind otherAccessKind, const AccessStorage &otherStorage) const {
  if (hasUnidentifiedAccess()
      && accessKindMayConflict(otherAccessKind,
                               unidentifiedAccess.value())) {
    return true;
  }
  for (auto &storageAccess : storageAccessSet) {
    assert(storageAccess && "FunctionAccessStorage mapped invalid storage.");

    if (!accessKindMayConflict(otherAccessKind, storageAccess.getAccessKind()))
      continue;

    if (!otherStorage.isDistinctFrom(storageAccess))
      return true;
  }
  return false;
}

StorageAccessInfo AccessStorageResult::getStorageAccessInfo(
    const AccessStorage &otherStorage) const {
  // Construct a fake StorageAccessInfo to do a hash lookup for the real
  // StorageAccessInfo. The DenseSet key is limited to the AccessStorage base
  // class members.
  StorageAccessInfo storageKey(otherStorage, SILAccessKind::Read, false);
  auto iter = storageAccessSet.find(storageKey);
  assert(iter != storageAccessSet.end());
  return *iter;
}

// -----------------------------------------------------------------------------
// MARK: Constructing the results.
// -----------------------------------------------------------------------------

static bool updateAccessKind(SILAccessKind &LHS, SILAccessKind RHS) {
  bool changed = false;
  // Assume we don't track Init/Deinit.
  if (LHS == SILAccessKind::Read && RHS == SILAccessKind::Modify) {
    LHS = RHS;
    changed = true;
  }
  return changed;
}

static bool updateOptionalAccessKind(llvm::Optional<SILAccessKind> &LHS,
                                     llvm::Optional<SILAccessKind> RHS) {
  if (RHS == llvm::None)
    return false;

  if (LHS == llvm::None) {
    LHS = RHS;
    return true;
  }
  return updateAccessKind(LHS.value(), RHS.value());
}

bool StorageAccessInfo::mergeFrom(const StorageAccessInfo &RHS) {
  bool changed = false;
  SILAccessKind accessKind = getAccessKind();
  assert(accessKind == SILAccessKind::Read
         || accessKind == SILAccessKind::Modify && "uninitialized info");
  if (updateAccessKind(accessKind, RHS.getAccessKind())) {
    setAccessKind(accessKind);
    changed = true;
  }
  if (hasNoNestedConflict() && !RHS.hasNoNestedConflict()) {
    setNoNestedConflict(false);
    changed = true;
  }
  return changed;
}

bool AccessStorageResult::updateUnidentifiedAccess(SILAccessKind accessKind) {
  if (unidentifiedAccess == llvm::None) {
    unidentifiedAccess = accessKind;
    return true;
  }
  return updateAccessKind(unidentifiedAccess.value(), accessKind);
}

// Merge the given AccessStorageResult in `other` into this
// AccessStorageResult. Use the given `transformStorage` to map `other`
// AccessStorage into this context. If `other` is from a callee, argument
// substitution will be performed if possible. However, there's no guarantee
// that the merged access values will belong to this region.
//
// Return true if these results changed, requiring further propagation through
// the call graph.
bool AccessStorageResult::mergeAccesses(
    const AccessStorageResult &other,
    std::function<StorageAccessInfo(const StorageAccessInfo &)>
        transformStorage) {

  // The cost of BottomUpIPAnalysis can be quadratic for large recursive call
  // graphs. That cost is multiplied by the size of storageAccessSet. Slowdowns
  // can occur ~1000 elements. 200 is large enough to cover "normal" code,
  // while ensuring compile time isn't affected.
  if (storageAccessSet.size() > 200) {
    setWorstEffects();
    return true;
  }
  // To save compile time, if this storage already has worst-case effects, avoid
  // growing its storageAccessSet.
  if (hasWorstEffects())
    return false;

  // When `this` == `other` (for self-recursion), insertion in DenseMap
  // invalidates the iterator. We still need to propagate and merge in that case
  // because arguments can be recursively dependent. The alternative would be
  // treating all self-recursion conservatively.
  const AccessStorageResult *otherRegionAccesses = &other;
  AccessStorageResult regionAccessCopy;
  if (this == &other) {
    regionAccessCopy = other;
    otherRegionAccesses = &regionAccessCopy;
  }
  bool changed = false;
  // Nondeterministically iterate for the sole purpose of inserting into another
  // unordered set.
  for (auto &rawStorageInfo : otherRegionAccesses->storageAccessSet) {
    const StorageAccessInfo &otherStorageInfo =
        transformStorage(rawStorageInfo);
    // If transformStorage() returns invalid storage object for local storage,
    // that should not be merged with the caller.
    if (!otherStorageInfo)
      continue;

    if (otherStorageInfo.getKind() == AccessStorage::Unidentified) {
      changed |= updateUnidentifiedAccess(otherStorageInfo.getAccessKind());
      continue;
    }
    // Attempt to add identified AccessStorage to this map.
    auto result = insertStorageAccess(otherStorageInfo);
    if (result.second) {
      // A new AccessStorage key was added to this map.
      changed = true;
      continue;
    }
    // Merge StorageAccessInfo into already-mapped AccessStorage.
    changed |= result.first->mergeFrom(otherStorageInfo);
  }
  if (other.unidentifiedAccess != llvm::None)
    changed |= updateUnidentifiedAccess(other.unidentifiedAccess.value());

  return changed;
}

bool AccessStorageResult::mergeFrom(const AccessStorageResult &other) {
  // Merge accesses from other. Both `this` and `other` are either from the same
  // function or are both callees of the same call site, so their parameters
  // indices coincide. transformStorage is the identity function.
  return mergeAccesses(other, [](const StorageAccessInfo &s) { return s; });
}

/// Returns the argument of the full apply or partial apply corresponding to the
/// callee's parameter index, or returns an invalid SILValue if the applied
/// closure cannot be found. This walks up the apply chain starting at the given
/// `fullApply` to find the applied argument.
static SILValue getCallerArg(FullApplySite fullApply, unsigned paramIndex) {
  if (paramIndex < fullApply.getNumArguments())
    return fullApply.getArgument(paramIndex);

  SILValue callee = fullApply.getCalleeOrigin();
  auto *PAI = dyn_cast<PartialApplyInst>(callee);
  if (!PAI)
    return SILValue();

  unsigned appliedIndex =
    paramIndex - ApplySite(PAI).getCalleeArgIndexOfFirstAppliedArg();
  if (appliedIndex < PAI->getNumArguments())
    return PAI->getArgument(appliedIndex);

  // This must be a chain of partial_applies. We don't expect this in practice,
  // so handle it conservatively.
  return SILValue();
}

/// Transform AccessStorage from a callee into the caller context. If this is
/// uniquely identified local storage, then return an invalid storage object.
///
/// For correctness, AccessEnforcementOpts relies on all Argument access to
/// either be mapped into the caller's context or marked as an unidentified
/// access at the call site.
///
/// Note: This does *not* map the storage index into the caller function's index
/// range. (When the storage value doesn't need to be remapped, it returns the
/// original storage value.) It's simpler to set the storage index later when it
/// is actually added to the function's storageAccessSet.
static StorageAccessInfo
transformCalleeStorage(const StorageAccessInfo &storage,
                       FullApplySite fullApply) {
  if (storage.isLocal()) {
    // Do not merge local storage.
    return StorageAccessInfo(AccessStorage(), storage);
  }
  // Remap reference storage. The caller's argument becomes the new object. The
  // old storage info is inherited.
  if (storage.isReference()) {
    auto object = storage.getObject();
    if (auto *arg = dyn_cast<SILFunctionArgument>(object)) {
      if (SILValue argVal = getCallerArg(fullApply, arg->getIndex())) {
        // Remap this storage info. The argument source value is now the new
        // object. The old storage info is inherited.
        auto callerStorage = storage.transformReference(argVal);
        return StorageAccessInfo(callerStorage, storage);
      }
    }
    // Continue using the callee value as the storage object.
    return storage;
  }
  switch (storage.getKind()) {
  case AccessStorage::Box:
  case AccessStorage::Class:
  case AccessStorage::Tail:
  case AccessStorage::Stack:
    llvm_unreachable("Handled immediately above");
  case AccessStorage::Nested:
    llvm_unreachable("Nested storage should not be used here");
  case AccessStorage::Global:
    // Global accesses is universal.
    return storage;
  case AccessStorage::Yield:
    // Continue to hold on to yields from the callee because we don't have
    // any better placeholder in the callee.
    return storage;
  case AccessStorage::Unidentified:
    // For unidentified storage, continue to reference the value in the callee
    // because we don't have any better placeholder for a callee-defined object.
    return storage;
  case AccessStorage::Argument: {
    // Transitively search for the storage base in the caller.
    if (SILValue argVal = getCallerArg(fullApply, storage.getParamIndex())) {
      // Remap the argument source value and inherit the old storage info.
      if (auto calleeStorage = AccessStorage::compute(argVal))
        return StorageAccessInfo(calleeStorage, storage);
    }
    // If the argument can't be transformed, demote it to an unidentified
    // access.
    //
    // This is an untested bailout. It is only reachable if the call graph
    // contains an edge that getCallerArg is unable to analyze OR if
    // AccessStorage::compute returns an invalid SILValue, which won't
    // pass SIL verification.
    //
    // TODO: To handle invalid argVal, consider allowing Unidentified access for
    // invalid values. This may be useful for partially invalidating results.
    return StorageAccessInfo(
      AccessStorage(storage.getValue(), AccessStorage::Unidentified),
      storage);
  }
  }
  llvm_unreachable("unhandled kind");
}

bool AccessStorageResult::mergeFromApply(
    const AccessStorageResult &calleeAccess, FullApplySite fullApply) {
  // Merge accesses from calleeAccess. Transform any Argument type
  // AccessStorage into the caller context to be added to `this` storage map.
  return mergeAccesses(calleeAccess, [&fullApply](const StorageAccessInfo &s) {
    return transformCalleeStorage(s, fullApply);
  });
}

template <typename B>
void AccessStorageResult::visitBeginAccess(B *beginAccess) {
  if (beginAccess->getEnforcement() != SILAccessEnforcement::Dynamic)
    return;

  auto storage = AccessStorage::compute(beginAccess->getSource());

  if (storage.getKind() == AccessStorage::Unidentified) {
    // This also catches invalid storage.
    updateOptionalAccessKind(unidentifiedAccess, beginAccess->getAccessKind());
    return;
  }
  StorageAccessInfo storageAccess(storage, beginAccess);
  auto result = insertStorageAccess(storageAccess);
  if (!result.second)
    result.first->mergeFrom(storageAccess);
}

void AccessStorageResult::analyzeInstruction(SILInstruction *I) {
  assert(!FullApplySite::isa(I) && "caller should merge");

  if (auto *BAI = dyn_cast<BeginAccessInst>(I))
    visitBeginAccess(BAI);
  else if (auto *BUAI = dyn_cast<BeginUnpairedAccessInst>(I))
    visitBeginAccess(BUAI);
}

void StorageAccessInfo::print(raw_ostream &os) const {
  os << "  [" << getSILAccessKindName(getAccessKind()) << "] ";
  if (hasNoNestedConflict())
    os << "[no_nested_conflict] ";
  AccessStorage::print(os);
}

void StorageAccessInfo::dump() const { print(llvm::dbgs()); }

void AccessStorageResult::print(raw_ostream &os) const {
  for (auto &storageAccess : storageAccessSet)
    storageAccess.print(os);

  if (unidentifiedAccess != llvm::None) {
    os << "  unidentified accesses: "
       << getSILAccessKindName(unidentifiedAccess.value()) << "\n";
  }
}

void AccessStorageResult::dump() const { print(llvm::dbgs()); }

// -----------------------------------------------------------------------------
// MARK: FunctionAccessStorage, implementation of
// GenericFunctionEffectAnalysis.
// -----------------------------------------------------------------------------

bool FunctionAccessStorage::summarizeFunction(SILFunction *F) {
  assert(accessResult.isEmpty() && "expected uninitialized results.");

  if (F->isDefinition())
    return false;

  // If the function definition is unavailable, set unidentifiedAccess to a
  // conservative value, since analyzeInstruction will never be called.
  //
  // If FunctionSideEffects can be summarized, use that information.

  auto b = F->getMemoryBehavior(/*observeRetains*/ false);
  if (b == MemoryBehavior::MayHaveSideEffects) {
    setWorstEffects();
    // May as well consider this a successful summary since there are no
    // instructions to visit anyway.
    return true;
  }
  if (b >= MemoryBehavior::MayWrite) {
    accessResult.setUnidentifiedAccess(SILAccessKind::Modify);
  } else if (b == MemoryBehavior::MayRead) {
    accessResult.setUnidentifiedAccess(SILAccessKind::Read);
  }

  // If function side effects is "readnone" then this result will have an empty
  // storageAccessSet and unidentifiedAccess == None.
  return true;
}

bool FunctionAccessStorage::summarizeCall(FullApplySite fullApply) {
  assert(accessResult.isEmpty() && "expected uninitialized results.");
  
  if (SILFunction *callee = fullApply.getReferencedFunctionOrNull()) {
    if (callee->getName() == "_swift_stdlib_malloc_size" ||
        callee->getName() == "_swift_stdlib_has_malloc_size") {
      return true;
    }
  }
  
  return false;
}

void AccessStorageAnalysis::initialize(
    SILPassManager *PM) {
  BCA = PM->getAnalysis<BasicCalleeAnalysis>();
}

void AccessStorageAnalysis::invalidate() {
  functionInfoMap.clear();
  allocator.DestroyAll();
  LLVM_DEBUG(llvm::dbgs() << "invalidate all\n");
}

void AccessStorageAnalysis::invalidate(
    SILFunction *F, InvalidationKind K) {
  if (FunctionInfo *FInfo = functionInfoMap.lookup(F)) {
    LLVM_DEBUG(llvm::dbgs() << "  invalidate " << FInfo->F->getName() << '\n');
    invalidateIncludingAllCallers(FInfo);
  }
}

void AccessStorageAnalysis::getCalleeEffects(
    FunctionAccessStorage &calleeEffects, FullApplySite fullApply) {
  if (calleeEffects.summarizeCall(fullApply))
    return;

  auto callees = BCA->getCalleeList(fullApply);
  if (!callees.allCalleesVisible() ||
      // @callee_owned function calls implicitly release the context, which
      // may call deinits of boxed values.
      // TODO: be less conservative about what destructors might be called.
      fullApply.getOrigCalleeType()->isCalleeConsumed()) {
    calleeEffects.setWorstEffects();
    return;
  }

  // We can see all the callees, so merge the effects from all of them.
  for (auto *callee : callees)
    calleeEffects.mergeFrom(getEffects(callee));
}

void AccessStorageAnalysis::analyzeFunction(
    FunctionInfo *functionInfo, FunctionOrder &bottomUpOrder,
    int recursionDepth) {
  functionInfo->needUpdateCallers = true;

  if (bottomUpOrder.prepareForVisiting(functionInfo))
    return;

  auto *F = functionInfo->F;
  if (functionInfo->functionEffects.summarizeFunction(F))
    return;

  LLVM_DEBUG(llvm::dbgs() << "  >> analyze " << F->getName() << '\n');

  // Check all instructions of the function
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto fullApply = FullApplySite::isa(&I))
        analyzeCall(functionInfo, fullApply, bottomUpOrder, recursionDepth);
      else
        functionInfo->functionEffects.analyzeInstruction(&I);
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "  << finished " << F->getName() << '\n');
}

void AccessStorageAnalysis::analyzeCall(
    FunctionInfo *functionInfo, FullApplySite fullApply,
    FunctionOrder &bottomUpOrder, int recursionDepth) {

  FunctionAccessStorage applyEffects;
  if (applyEffects.summarizeCall(fullApply)) {
    functionInfo->functionEffects.mergeFromApply(applyEffects, fullApply);
    return;
  }

  if (recursionDepth >= MaxRecursionDepth) {
    functionInfo->functionEffects.setWorstEffects();
    return;
  }
  CalleeList callees = BCA->getCalleeList(fullApply);
  if (!callees.allCalleesVisible() ||
      // @callee_owned function calls implicitly release the context, which
      // may call deinits of boxed values.
      // TODO: be less conservative about what destructors might be called.
      fullApply.getOrigCalleeType()->isCalleeConsumed()) {
    functionInfo->functionEffects.setWorstEffects();
    return;
  }
  // Derive the effects of the apply from the known callees.
  // Defer merging callee effects until the callee is scheduled
  for (SILFunction *callee : callees) {
    FunctionInfo *calleeInfo = getFunctionInfo(callee);
    calleeInfo->addCaller(functionInfo, fullApply);
    if (!calleeInfo->isVisited()) {
      // Recursively visit the called function.
      analyzeFunction(calleeInfo, bottomUpOrder, recursionDepth + 1);
      bottomUpOrder.tryToSchedule(calleeInfo);
    }
  }
}

void AccessStorageAnalysis::recompute(
    FunctionInfo *initialInfo) {
  allocNewUpdateID();

  LLVM_DEBUG(llvm::dbgs() << "recompute function-effect analysis with UpdateID "
                          << getCurrentUpdateID() << '\n');

  // Collect and analyze all functions to recompute, starting at initialInfo.
  FunctionOrder bottomUpOrder(getCurrentUpdateID());
  analyzeFunction(initialInfo, bottomUpOrder, 0);

  // Build the bottom-up order.
  bottomUpOrder.tryToSchedule(initialInfo);
  bottomUpOrder.finishScheduling();

  // Second step: propagate the side-effect information up the call-graph until
  // it stabilizes.
  bool needAnotherIteration;
  do {
    LLVM_DEBUG(llvm::dbgs() << "new iteration\n");
    needAnotherIteration = false;

    for (FunctionInfo *functionInfo : bottomUpOrder) {
      if (!functionInfo->needUpdateCallers)
        continue;

      LLVM_DEBUG(llvm::dbgs() << "  update callers of "
                              << functionInfo->F->getName() << '\n');
      functionInfo->needUpdateCallers = false;

      // Propagate the function effects to all callers.
      for (const auto &E : functionInfo->getCallers()) {
        assert(E.isValid());

        // Only include callers which we are actually recomputing.
        if (!bottomUpOrder.wasRecomputedWithCurrentUpdateID(E.Caller))
          continue;

        LLVM_DEBUG(llvm::dbgs() << "    merge into caller "
                                << E.Caller->F->getName() << '\n');

        if (E.Caller->functionEffects.mergeFromApply(
                functionInfo->functionEffects, FullApplySite(E.FAS))) {
          E.Caller->needUpdateCallers = true;
          if (!E.Caller->isScheduledAfter(functionInfo)) {
            // This happens if we have a cycle in the call-graph.
            needAnotherIteration = true;
          }
        }
      }
    }
  } while (needAnotherIteration);
}

SILAnalysis *swift::createAccessStorageAnalysis(SILModule *) {
  return new AccessStorageAnalysis();
}
