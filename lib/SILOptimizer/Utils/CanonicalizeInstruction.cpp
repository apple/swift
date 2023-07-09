//===--- CanonicalizeInstruction.cpp - canonical SIL peepholes ------------===//
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
/// SSA-peephole transformations that yield a more canonical SIL representation.
///
/// A superset of simplifyInstruction.
///
//===----------------------------------------------------------------------===//

// CanonicalizeInstruction defines a default DEBUG_TYPE: "sil-canonicalize"

#include "swift/SILOptimizer/Utils/CanonicalizeInstruction.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/SimplifyInstruction.h"
#include "swift/SILOptimizer/Utils/DebugOptUtils.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"

using namespace swift;

// Tracing within the implementation can also be activated by the pass.
#define DEBUG_TYPE pass.debugType

// Vtable anchor.
CanonicalizeInstruction::~CanonicalizeInstruction() {}

// Helper to delete an instruction, or mark it for deletion.
//
// Return an iterator to the next non-deleted instruction. The incoming iterator
// may already have advanced beyond 'inst'.
static SILBasicBlock::iterator killInstruction(SILInstruction *inst,
                                               SILBasicBlock::iterator nextII,
                                               CanonicalizeInstruction &pass) {
  if (nextII == inst->getIterator())
    ++nextII;
  pass.killInstruction(inst);
  return nextII;
}

// Helper to delete, or mark for deletion, an instruction with potential debug
// or end of scope uses. All "real" uses must already be removed.
//
// fix_lifetime uses are not currently handled here. They are generally
// (incorrectly) treated as "incidental" uses, but no canonicalizations need
// them yet.
static SILBasicBlock::iterator
killInstAndIncidentalUses(SingleValueInstruction *inst,
                          SILBasicBlock::iterator nextII,
                          CanonicalizeInstruction &pass) {
  while (!inst->use_empty()) {
    auto *user = inst->use_begin()->getUser();
    assert(user->isDebugInstruction() || isEndOfScopeMarker(user));
    nextII = killInstruction(user, nextII, pass);
  }
  return killInstruction(inst, nextII, pass);
}

//===----------------------------------------------------------------------===//
//                          Instruction Simplification
//===----------------------------------------------------------------------===//

// If simplification is successful, return a valid iterator to the next
// instruction that wasn't erased.
static llvm::Optional<SILBasicBlock::iterator>
simplifyAndReplace(SILInstruction *inst, CanonicalizeInstruction &pass) {
  // Erase the simplified instruction and any instructions that end its
  // scope. Nothing needs to be added to the worklist except for Result,
  // because the instruction and all non-replaced users will be deleted.
  pass.callbacks.resetHadCallbackInvocation();
  auto result = simplifyAndReplaceAllSimplifiedUsesAndErase(
      inst, pass.callbacks, &pass.deadEndBlocks);
  if (!pass.callbacks.hadCallbackInvocation())
    return llvm::None;

  return result;
}

//===----------------------------------------------------------------------===//
//                        Canonicalize Memory Operations
//===----------------------------------------------------------------------===//

// Replace all uses of an original struct or tuple extract instruction with the
// given load instruction. The caller ensures that the load only loads the
// extracted field.
//
// \p extract has the form:
// (struct_extract (load %base), #field)
//
// \p loadInst has the form:
// (load (struct_element_addr %base, #field)
static void replaceUsesOfExtract(SingleValueInstruction *extract,
                                 LoadOperation loadInst,
                                 CanonicalizeInstruction &pass) {
  assert(extract->getType() == loadInst->getType());

  SingleValueInstruction *loadedVal = *loadInst;
  if (auto qual = loadInst.getOwnershipQualifier()) {
    if (*qual == LoadOwnershipQualifier::Copy) {
      // Borrow the load-copied subelement, with precisely the same scope as
      // the aggregate borrow.
      assert(extract->getNumOperands() == 1);
      auto *origBorrow = cast<BeginBorrowInst>(extract->getOperand(0));
      auto *newBorrow = SILBuilderWithScope(origBorrow)
                            .createBeginBorrow(loadInst->getLoc(), *loadInst);
      pass.notifyNewInstruction(newBorrow);

      assert(extract == origBorrow->getSingleNonEndingUse()->getUser());
      for (auto *origEnd : origBorrow->getEndBorrows()) {
        auto *endBorrow = SILBuilderWithScope(origEnd).createEndBorrow(
            origEnd->getLoc(), newBorrow);
        pass.notifyNewInstruction(endBorrow);
      }
      loadedVal = newBorrow;
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "Replacing " << *extract << "    with "
                          << *loadedVal << "\n");
  extract->replaceAllUsesWith(loadedVal);
}

// Given a load with multiple struct_extracts/tuple_extracts and no other uses,
// canonicalize the load into several (struct_element_addr (load)) pairs.
//
// (struct_extract (load %base))
//   ->
// (load (struct_element_addr %base, #field)
//
// TODO: Consider handling LoadBorrowInst.
static SILBasicBlock::iterator
splitAggregateLoad(LoadOperation loadInst, CanonicalizeInstruction &pass) {
  // Keep track of the next iterator after any newly added or to-be-deleted
  // instructions. This must be valid regardless of whether the pass immediately
  // deletes the instructions or simply records them for later deletion.
  auto nextII = std::next(loadInst->getIterator());

  bool needsBorrow;
  if (auto qual = loadInst.getOwnershipQualifier()) {
    switch (*qual) {
    case LoadOwnershipQualifier::Unqualified:
    case LoadOwnershipQualifier::Trivial:
      needsBorrow = false;
      break;
    case LoadOwnershipQualifier::Copy:
      needsBorrow = true;
      break;
    case LoadOwnershipQualifier::Take:
      // TODO: To handle a "take", we would need to generate additional destroys
      // for any fields that aren't already extracted. This would be
      // out-of-place for this transform, and I'm not sure if this a case that
      // needs to be handled in CanonicalizeInstruction.
      return nextII;
    }
  } else {
    // If we don't have a qual, we have a borrow.
    needsBorrow = false;
  }

  struct ProjInstPair {
    Projection proj;
    SingleValueInstruction *extract;

    // When sorting, just look at the projection and ignore the instruction.
    // Including the instruction address in the sort key would be
    // nondeterministic.
    bool operator<(const ProjInstPair &rhs) const { return proj < rhs.proj; }
  };

  // Add load projections to a projection list.
  llvm::SmallVector<ProjInstPair, 8> projections;
  llvm::SmallVector<BeginBorrowInst *, 8> borrows;
  llvm::SmallVector<SILInstruction *, 8> lifetimeEndingInsts;
  for (auto *use : getNonDebugUses(*loadInst)) {
    auto *user = use->getUser();
    if (needsBorrow) {
      if (auto *destroy = dyn_cast<DestroyValueInst>(user)) {
        lifetimeEndingInsts.push_back(destroy);
        continue;
      }
      auto *borrow = dyn_cast<BeginBorrowInst>(user);
      if (!borrow)
        return nextII;

      // The transformation below also assumes a single borrow use.
      auto *borrowedOper = borrow->getSingleNonEndingUse();
      if (!borrowedOper)
        return nextII;

      borrows.push_back(borrow);
      user = borrowedOper->getUser();
    } else {
      if (isa<EndBorrowInst>(user) &&
          !loadInst.getOwnershipQualifier().has_value()) {
        lifetimeEndingInsts.push_back(user);
        continue;
      }
    }

    // If we have any non SEI, TEI instruction, don't do anything here.
    if (!isa<StructExtractInst>(user) && !isa<TupleExtractInst>(user))
      return nextII;

    auto extract = cast<SingleValueInstruction>(user);
    projections.push_back({Projection(extract), extract});
  }
  // Sort the list so projections with the same value decl and tuples with the
  // same indices will be processed together. This makes it easy to reuse the
  // load from the first such projection for all subsequent projections on the
  // same value decl or index.
  std::sort(projections.begin(), projections.end());

  // If the original load is dead, then do not delete it before
  // diagnostics. Doing so would suppress DefiniteInitialization in cases like:
  //
  // struct S {
  //   let a: Int
  //   init() {
  //     _ = a // must be diagnosed as use before initialization
  //     a = 0
  //   }
  // }
  //
  // However, if the load has any projections, it must be deleted, otherwise
  // exclusivity checking is too strict:
  //
  // extension S {
  //   mutating func foo() {
  //     _ = a // Must be diagnosed as a read of self.a only not the whole self.
  //   }
  // }
  //
  // Also, avoid degrading debug info unless it is necessary for exclusivity
  // diagnostics.
  //
  // TODO: This logic subtly anticipates SILGen behavior. In the future, change
  // SILGen to avoid emitting the full load and never delete loads in Raw SIL.
  if (projections.empty() && loadInst->getModule().getStage() == SILStage::Raw)
    return nextII;

  // Create a new address projection instruction and load instruction for each
  // unique projection.
  Projection *lastProj = nullptr;
  llvm::Optional<LoadOperation> lastNewLoad;
  for (auto &pair : projections) {
    auto &proj = pair.proj;
    auto *extract = pair.extract;

    // If this projection is the same as the last projection we processed, just
    // replace all uses of the projection with the load we created previously.
    if (lastProj && proj == *lastProj) {
      replaceUsesOfExtract(extract, *lastNewLoad, pass);
      nextII = killInstruction(extract, nextII, pass);
      continue;
    }

    // This is a unique projection. Create the new address projection and load.
    lastProj = &proj;
    // Insert new instructions before the original load.
    SILBuilderWithScope LoadBuilder(*loadInst);
    auto *projInst =
        proj.createAddressProjection(LoadBuilder, loadInst->getLoc(),
                                     loadInst->getOperand(0))
            .get();
    pass.notifyNewInstruction(projInst);

    // When loading a trivial subelement, convert ownership.
    llvm::Optional<LoadOwnershipQualifier> loadOwnership =
        loadInst.getOwnershipQualifier();
    if (loadOwnership.has_value()) {
      if (*loadOwnership != LoadOwnershipQualifier::Unqualified &&
          projInst->getType().isTrivial(*projInst->getFunction()))
        loadOwnership = LoadOwnershipQualifier::Trivial;
    } else {
      if (projInst->getType().isTrivial(*projInst->getFunction()))
        loadOwnership = LoadOwnershipQualifier::Trivial;
    }

    if (loadOwnership) {
      lastNewLoad =
          LoadBuilder.createLoad(loadInst->getLoc(), projInst, *loadOwnership);
    } else {
      lastNewLoad = LoadBuilder.createLoadBorrow(loadInst->getLoc(), projInst);
    }
    pass.notifyNewInstruction(**lastNewLoad);

    if (loadOwnership) {
      if (*loadOwnership == LoadOwnershipQualifier::Copy) {
        // Destroy the loaded value wherever the aggregate load was destroyed.
        assert(loadInst.getOwnershipQualifier() ==
               LoadOwnershipQualifier::Copy);
        for (SILInstruction *destroy : lifetimeEndingInsts) {
          auto *newInst = SILBuilderWithScope(destroy).createDestroyValue(
              destroy->getLoc(), lastNewLoad->getLoadInst());
          pass.notifyNewInstruction(newInst);
        }
      }
    } else {
      for (SILInstruction *destroy : lifetimeEndingInsts) {
        auto *newInst = SILBuilderWithScope(destroy).createEndBorrow(
            destroy->getLoc(), **lastNewLoad);
        pass.notifyNewInstruction(newInst);
      }
    }
    replaceUsesOfExtract(extract, *lastNewLoad, pass);
    nextII = killInstruction(extract, nextII, pass);
  }

  // Preserve the original load's debug information.
  if (pass.preserveDebugInfo) {
    swift::salvageLoadDebugInfo(loadInst);
  }
  // Remove the now unused borrows.
  for (auto *borrow : borrows)
    nextII = killInstAndIncidentalUses(borrow, nextII, pass);

  // Erase the old load.
  for (auto *destroy : lifetimeEndingInsts)
    nextII = killInstruction(destroy, nextII, pass);

  // TODO: remove this hack to advance the iterator beyond debug_value and check
  // SILInstruction::isDeleted() in the caller instead.
  while (nextII != loadInst->getParent()->end()
         && nextII->isDebugInstruction()) {
    ++nextII;
  }
  deleteAllDebugUses(*loadInst, pass.getCallbacks());
  return killInstAndIncidentalUses(*loadInst, nextII, pass);
}

// Given a store within a single property struct, recursively form the parent
// struct values and promote the store to the outer struct type.
//
// (store (struct_element_addr %base) object)
//   ->
// (store %base (struct object))
//
// TODO: supporting enums here would be very easy. The main thing is adding
// support in `createAggFromFirstLevelProjections`.
// Note: we will not be able to support tuples because we cannot have a
// single-element tuple.
static SILBasicBlock::iterator
broadenSingleElementStores(StoreInst *storeInst,
                           CanonicalizeInstruction &pass) {
  // Keep track of the next iterator after any newly added or to-be-deleted
  // instructions. This must be valid regardless of whether the pass immediately
  // deletes the instructions or simply records them for later deletion.
  auto nextII = std::next(storeInst->getIterator());
  auto *f = storeInst->getFunction();

  ProjectionPath projections(storeInst->getDest()->getType());
  SILValue op = storeInst->getDest();
  while (isa<StructElementAddrInst>(op)) {
    auto *inst = cast<SingleValueInstruction>(op);
    SILValue baseAddr = inst->getOperand(0);
    SILType baseAddrType = baseAddr->getType();
    auto *decl = baseAddrType.getStructOrBoundGenericStruct();
    assert(
      !decl->isResilient(f->getModule().getSwiftModule(),
                         f->getResilienceExpansion()) &&
        "This code assumes resilient structs can not have fragile fields. If "
        "this assert is hit, this has been changed. Please update this code.");

    // Bail if the store's destination is not a struct_element_addr or if the
    // store's destination (%base) is not a loadable type. If %base is not a
    // loadable type, we can't create it as a struct later on.
    // If our aggregate has unreferenced storage then we can never prove if it
    // actually has a single field.
    if (!baseAddrType.isLoadable(*f) ||
        baseAddrType.aggregateHasUnreferenceableStorage() ||
        decl->getStoredProperties().size() != 1)
      break;

    // If the struct is a move-only type, even though the single element in
    // the struct is trivial, the struct would be non-trivial. In this case, we
    // need a much more compelx analysis to determine the store ownership
    // qualifier. Such an analysis is not suitable in the canonicalize pass. So,
    // bail out.
    if (baseAddrType.isMoveOnly()) {
      break;
    }
    projections.push_back(Projection(inst));
    op = baseAddr;
  }

  // If we couldn't create a projection, bail.
  if (projections.empty())
    return nextII;

  // Now work our way back up. At this point we know all operations we are going
  // to do succeed (cast<SingleValueInst>, createAggFromFirstLevelProjections,
  // etc.) so we can omit null checks. We should not bail at this point (we
  // could create a double consume, or worse).
  SILBuilderWithScope builder(storeInst);
  SILValue result = storeInst->getSrc();
  SILValue storeAddr = storeInst->getDest();
  for (Projection proj : llvm::reverse(projections)) {
    storeAddr = cast<SingleValueInstruction>(storeAddr)->getOperand(0);
    result = proj.createAggFromFirstLevelProjections(
                     builder, storeInst->getLoc(),
                     storeAddr->getType().getObjectType(), {result})
                 .get();
  }

  // Store the new struct-wrapped value into the final base address.
  builder.createStore(storeInst->getLoc(), result, storeAddr,
                      storeInst->getOwnershipQualifier());

  // Erase the original store.
  return killInstruction(storeInst, nextII, pass);
}

//===----------------------------------------------------------------------===//
//                            Simple ARC Peepholes
//===----------------------------------------------------------------------===//

/// "dead" copies are removed in OSSA, but this may shorten object lifetimes,
/// changing program semantics in unexpected ways by releasing weak references
/// and running deinitializers early. This copy may be the only thing keeping a
/// variable's reference alive. But just because the copy's current SSA value
/// contains no other uses does not mean that there aren't other uses that still
/// correspond to the original variable whose lifetime is protected by this
/// copy. The only way to guarantee the lifetime of a variable is to use a
/// borrow scope--copy/destroy is insufficient by itself.
///
/// FIXME: This removes debug_value instructions aggressively as part of
/// SILGenCleanup. Instead, debug_values should be canonicalized before copy
/// elimination so that we never see the pattern:
///   %b = begin_borrow
///   %c = copy %b
///   end_borrow %b
///   debug_value %c
///
/// FIXME: Technically this should be guarded by a compiler flag like
/// -enable-copy-propagation until SILGen protects scoped variables by
/// borrow scopes.
static SILBasicBlock::iterator
eliminateSimpleCopies(CopyValueInst *cvi, CanonicalizeInstruction &pass) {
  auto next = std::next(cvi->getIterator());

  // Eliminate copies that only have destroy_value uses.
  SmallVector<DestroyValueInst *, 8> destroys;
  for (Operand *use : cvi->getUses()) {
    if (auto *dvi = dyn_cast<DestroyValueInst>(use->getUser())) {
      destroys.push_back(dvi);
      continue;
    }
    if (!pass.preserveDebugInfo && isa<DebugValueInst>(use->getUser())) {
      continue;
    }
    return next;
  }

  while (!destroys.empty()) {
    next = killInstruction(destroys.pop_back_val(), next, pass);
  }
  return killInstAndIncidentalUses(cvi, next, pass);
}

/// Unlike dead copy elimination, dead borrows can be safely removed because the
/// semantics of a borrow scope
static SILBasicBlock::iterator
eliminateSimpleBorrows(BeginBorrowInst *bbi, CanonicalizeInstruction &pass) {
  auto next = std::next(bbi->getIterator());

  // Lexical borrow scopes can only be eliminated under certain circumstances:
  // (1) They can never be eliminated if the module is in the raw stage, because
  //     they may be needed for diagnostic.
  // (2) They can never be eliminated if there is no enclosing lexical scope
  //     which guarantees the lifetime of the value.
  if (bbi->isLexical() && (bbi->getModule().getStage() == SILStage::Raw ||
                           !isNestedLexicalBeginBorrow(bbi)))
    return next;

  // We know that our borrow is completely within the lifetime of its base value
  // if the borrow is never reborrowed. We check for reborrows and do not
  // optimize such cases. Otherwise, we can eliminate our borrow and instead use
  // our operand.
  auto base = bbi->getOperand();
  auto baseOwnership = base->getOwnershipKind();
  SmallVector<EndBorrowInst *, 8> endBorrows;
  for (auto *use : getNonDebugUses(bbi)) {
    if (auto *ebi = dyn_cast<EndBorrowInst>(use->getUser())) {
      endBorrows.push_back(ebi);
      continue;
    }

    // Otherwise, if we have a use that is non-lifetime ending and can accept
    // our base ownership, continue.
    if (!use->isLifetimeEnding() && use->canAcceptKind(baseOwnership))
      continue;

    return next;
  }

  while (!endBorrows.empty()) {
    next = killInstruction(endBorrows.pop_back_val(), next, pass);
  }
  bbi->replaceAllUsesWith(base);
  pass.notifyHasNewUsers(base);
  return killInstruction(bbi, next, pass);
}

/// Delete any result having forwarding instruction that only has destroy_value
/// and debug_value uses.
static SILBasicBlock::iterator
eliminateUnneededForwardingUnarySingleValueInst(SingleValueInstruction *inst,
                                                CanonicalizeInstruction &pass) {
  auto next = std::next(inst->getIterator());

  for (auto *use : getNonDebugUses(inst))
    if (!isa<DestroyValueInst>(use->getUser()))
      return next;
  deleteAllDebugUses(inst, pass.callbacks);
  SILValue op = inst->getOperand(0);
  inst->replaceAllUsesWith(op);
  pass.notifyHasNewUsers(op);
  return killInstruction(inst, next, pass);
}

static llvm::Optional<SILBasicBlock::iterator>
tryEliminateUnneededForwardingInst(SILInstruction *i,
                                   CanonicalizeInstruction &pass) {
  assert(ForwardingInstruction::isa(i) &&
         "Must be an ownership forwarding inst");
  if (auto *svi = dyn_cast<SingleValueInstruction>(i))
    if (svi->getNumOperands() == 1)
      return eliminateUnneededForwardingUnarySingleValueInst(svi, pass);

  return llvm::None;
}

//===----------------------------------------------------------------------===//
//                            Top-Level Entry Point
//===----------------------------------------------------------------------===//

SILBasicBlock::iterator
CanonicalizeInstruction::canonicalize(SILInstruction *inst) {
  if (auto nextII = simplifyAndReplace(inst, *this))
    return nextII.value();

  if (auto li = LoadOperation(inst)) {
    return splitAggregateLoad(li, *this);
  }
  if (auto *storeInst = dyn_cast<StoreInst>(inst)) {
    return broadenSingleElementStores(storeInst, *this);
  }

  if (auto *cvi = dyn_cast<CopyValueInst>(inst))
    return eliminateSimpleCopies(cvi, *this);

  if (auto *bbi = dyn_cast<BeginBorrowInst>(inst))
    return eliminateSimpleBorrows(bbi, *this);

  // If we have ownership and are not in raw SIL, eliminate unneeded forwarding
  // insts. We don't do this in raw SIL as not to disturb the codegen read by
  // diagnostics.
  //
  // TODO: fix tryEliminateUnneededForwardingInst to handle debug uses.
  auto *fn = inst->getFunction();
  if (!preserveDebugInfo && fn->hasOwnership()
      && fn->getModule().getStage() != SILStage::Raw) {
    if (ForwardingInstruction::isa(inst))
      if (auto newNext = tryEliminateUnneededForwardingInst(inst, *this))
        return *newNext;
  }

  // Skip ahead.
  return std::next(inst->getIterator());
}
