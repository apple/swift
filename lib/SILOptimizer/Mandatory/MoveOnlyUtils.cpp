//===--- MoveOnlyUtils.cpp ------------------------------------------------===//
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

#define DEBUG_TYPE "sil-move-only-checker"

#include "swift/AST/AccessScope.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/Basic/SmallBitVector.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockData.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/Consumption.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/FieldSensitivePrunedLiveness.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILArgumentConvention.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/Analysis/ClosureScope.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

#include "MoveOnlyDiagnostics.h"
#include "MoveOnlyUtils.h"

using namespace swift;
using namespace swift::siloptimizer;

//===----------------------------------------------------------------------===//
//                        MARK: Missed Copy Diagnostic
//===----------------------------------------------------------------------===//

/// A small diagnostic helper that causes us to emit a diagnostic error upon any
/// copies we did not eliminate and ask the user for a test case.
void swift::siloptimizer::emitCheckerMissedCopyOfNonCopyableTypeErrors(
    SILFunction *fn, DiagnosticEmitter &diagnosticEmitter) {
  for (auto &block : *fn) {
    for (auto &inst : block) {
      if (auto *cvi = dyn_cast<CopyValueInst>(&inst)) {
        if (cvi->getOperand()->getType().isMoveOnly()) {
          LLVM_DEBUG(llvm::dbgs()
                     << "Emitting missed copy error for: " << *cvi);
          diagnosticEmitter.emitCheckedMissedCopyError(cvi);
        }
        continue;
      }

      if (auto *li = dyn_cast<LoadInst>(&inst)) {
        if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Copy &&
            li->getType().isMoveOnly()) {
          LLVM_DEBUG(llvm::dbgs() << "Emitting missed copy error for: " << *li);
          diagnosticEmitter.emitCheckedMissedCopyError(li);
        }
        continue;
      }

      if (auto *copyAddr = dyn_cast<CopyAddrInst>(&inst)) {
        if (!copyAddr->isTakeOfSrc() &&
            copyAddr->getSrc()->getType().isMoveOnly()) {
          LLVM_DEBUG(llvm::dbgs()
                     << "Emitting missed copy error for: " << *copyAddr);
          diagnosticEmitter.emitCheckedMissedCopyError(copyAddr);
        }
        continue;
      }
    }
  }
}

//===----------------------------------------------------------------------===//
//                  MARK: Cleanup After Emitting Diagnostic
//===----------------------------------------------------------------------===//

bool swift::siloptimizer::cleanupNonCopyableCopiesAfterEmittingDiagnostic(
    SILFunction *fn) {
  bool changed = false;
  for (auto &block : *fn) {
    for (auto ii = block.begin(), ie = block.end(); ii != ie;) {
      auto *inst = &*ii;
      ++ii;

      // Convert load [copy] *MoveOnly -> load_borrow + explicit_copy_value.
      if (auto *li = dyn_cast<LoadInst>(inst)) {
        if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Copy) {
          if (!li->getType().isMoveOnly())
            continue;

          SILBuilderWithScope builder(li);
          auto *lbi = builder.createLoadBorrow(li->getLoc(), li->getOperand());
          auto *cvi = builder.createExplicitCopyValue(li->getLoc(), lbi);
          builder.createEndBorrow(li->getLoc(), lbi);
          li->replaceAllUsesWith(cvi);
          li->eraseFromParent();
          changed = true;
        }
      }

      // Convert copy_addr !take MoveOnly ... -> explicit_copy_addr ...same...
      // so we don't error.
      if (auto *copyAddr = dyn_cast<CopyAddrInst>(inst)) {
        if (!copyAddr->isTakeOfSrc()) {
          if (!copyAddr->getSrc()->getType().isMoveOnly())
            continue;

          SILBuilderWithScope builder(copyAddr);
          builder.createExplicitCopyAddr(
              copyAddr->getLoc(), copyAddr->getSrc(), copyAddr->getDest(),
              IsTake_t(copyAddr->isTakeOfSrc()),
              IsInitialization_t(copyAddr->isInitializationOfDest()));
          copyAddr->eraseFromParent();
          changed = true;
        }
      }

      // Convert any copy_value of MoveOnly type -> explicit_copy_value.
      if (auto *cvi = dyn_cast<CopyValueInst>(inst)) {
        if (!cvi->getOperand()->getType().isMoveOnly())
          continue;

        SILBuilderWithScope b(cvi);
        auto *expCopy =
            b.createExplicitCopyValue(cvi->getLoc(), cvi->getOperand());
        cvi->replaceAllUsesWith(expCopy);
        cvi->eraseFromParent();
        changed = true;
        continue;
      }

      if (auto *mmci = dyn_cast<MarkMustCheckInst>(inst)) {
        mmci->replaceAllUsesWith(mmci->getOperand());
        mmci->eraseFromParent();
        changed = true;
        continue;
      }
    }
  }

  return changed;
}

//===----------------------------------------------------------------------===//
//                           MARK: Memory Utilities
//===----------------------------------------------------------------------===//

bool noncopyable::memInstMustInitialize(Operand *memOper) {
  SILValue address = memOper->get();

  SILInstruction *memInst = memOper->getUser();

  switch (memInst->getKind()) {
  default:
    return false;

  case SILInstructionKind::CopyAddrInst: {
    auto *CAI = cast<CopyAddrInst>(memInst);
    return CAI->getDest() == address && CAI->isInitializationOfDest();
  }
  case SILInstructionKind::ExplicitCopyAddrInst: {
    auto *CAI = cast<ExplicitCopyAddrInst>(memInst);
    return CAI->getDest() == address && CAI->isInitializationOfDest();
  }
  case SILInstructionKind::MarkUnresolvedMoveAddrInst: {
    return cast<MarkUnresolvedMoveAddrInst>(memInst)->getDest() == address;
  }
  case SILInstructionKind::InitExistentialAddrInst:
  case SILInstructionKind::InitEnumDataAddrInst:
  case SILInstructionKind::InjectEnumAddrInst:
    return true;

  case SILInstructionKind::BeginApplyInst:
  case SILInstructionKind::TryApplyInst:
  case SILInstructionKind::ApplyInst: {
    FullApplySite applySite(memInst);
    return applySite.isIndirectResultOperand(*memOper);
  }
  case SILInstructionKind::StoreInst: {
    auto qual = cast<StoreInst>(memInst)->getOwnershipQualifier();
    return qual == StoreOwnershipQualifier::Init ||
           qual == StoreOwnershipQualifier::Trivial;
  }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)             \
  case SILInstructionKind::Store##Name##Inst:                                  \
    return cast<Store##Name##Inst>(memInst)->isInitializationOfDest();
#include "swift/AST/ReferenceStorage.def"
  }
}

bool noncopyable::memInstMustReinitialize(Operand *memOper) {
  SILValue address = memOper->get();

  SILInstruction *memInst = memOper->getUser();

  switch (memInst->getKind()) {
  default:
    return false;

  case SILInstructionKind::CopyAddrInst: {
    auto *CAI = cast<CopyAddrInst>(memInst);
    return CAI->getDest() == address && !CAI->isInitializationOfDest();
  }
  case SILInstructionKind::ExplicitCopyAddrInst: {
    auto *CAI = cast<ExplicitCopyAddrInst>(memInst);
    return CAI->getDest() == address && !CAI->isInitializationOfDest();
  }
  case SILInstructionKind::YieldInst: {
    auto *yield = cast<YieldInst>(memInst);
    return yield->getYieldInfoForOperand(*memOper).isIndirectInOut();
  }
  case SILInstructionKind::BeginApplyInst:
  case SILInstructionKind::TryApplyInst:
  case SILInstructionKind::ApplyInst: {
    FullApplySite applySite(memInst);
    return applySite.getArgumentOperandConvention(*memOper).isInoutConvention();
  }
  case SILInstructionKind::StoreInst:
    return cast<StoreInst>(memInst)->getOwnershipQualifier() ==
           StoreOwnershipQualifier::Assign;

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)             \
  case SILInstructionKind::Store##Name##Inst:                                  \
    return !cast<Store##Name##Inst>(memInst)->isInitializationOfDest();
#include "swift/AST/ReferenceStorage.def"
  }
}

bool noncopyable::memInstMustConsume(Operand *memOper) {
  SILValue address = memOper->get();

  SILInstruction *memInst = memOper->getUser();

  // FIXME: drop_deinit must be handled here!
  switch (memInst->getKind()) {
  default:
    return false;

  case SILInstructionKind::CopyAddrInst: {
    auto *CAI = cast<CopyAddrInst>(memInst);
    return (CAI->getSrc() == address && CAI->isTakeOfSrc()) ||
           (CAI->getDest() == address && !CAI->isInitializationOfDest());
  }
  case SILInstructionKind::ExplicitCopyAddrInst: {
    auto *CAI = cast<ExplicitCopyAddrInst>(memInst);
    return (CAI->getSrc() == address && CAI->isTakeOfSrc()) ||
           (CAI->getDest() == address && !CAI->isInitializationOfDest());
  }
  case SILInstructionKind::BeginApplyInst:
  case SILInstructionKind::TryApplyInst:
  case SILInstructionKind::ApplyInst: {
    FullApplySite applySite(memInst);
    return applySite.getArgumentOperandConvention(*memOper).isOwnedConvention();
  }
  case SILInstructionKind::PartialApplyInst: {
    // If we are on the stack or have an inout convention, we do not
    // consume. Otherwise, we do.
    auto *pai = cast<PartialApplyInst>(memInst);
    if (pai->isOnStack())
      return false;
    ApplySite applySite(pai);
    auto convention = applySite.getArgumentConvention(*memOper);
    return !convention.isInoutConvention();
  }
  case SILInstructionKind::DestroyAddrInst:
    return true;
  case SILInstructionKind::LoadInst:
    return cast<LoadInst>(memInst)->getOwnershipQualifier() ==
           LoadOwnershipQualifier::Take;
  }
}

//===----------------------------------------------------------------------===//
//                  Simple Temporary AllocStack Elimination
//===----------------------------------------------------------------------===//

static bool isLetAllocation(MarkMustCheckInst *mmci) {
  if (auto *pbi = dyn_cast<ProjectBoxInst>(mmci)) {
    auto *box = cast<AllocBoxInst>(stripBorrow(pbi->getOperand()));
    return !box->getBoxType()->getLayout()->isMutable();
  }

  if (auto *asi = dyn_cast<AllocStackInst>(mmci->getOperand()))
    if (auto varInfo = asi->getVarInfo())
      return varInfo->isLet();

  return false;
}

static bool walkUseToDefsStructTupleProjections(
    SILValue startPoint, SILValue endPoint,
    llvm::function_ref<void(SingleValueInstruction *)> visitor) {
  while (startPoint != endPoint) {
    if (auto *sei = dyn_cast<StructElementAddrInst>(startPoint)) {
      visitor(sei);
      startPoint = sei->getOperand();
      continue;
    }

    if (auto *tei = dyn_cast<TupleElementAddrInst>(startPoint)) {
      visitor(tei);
      startPoint = tei->getOperand();
      continue;
    }

    return false;
  }

  return true;
}

namespace {

struct SimpleTemporaryAllocStackElimState {
  SmallVector<SingleValueInstruction *, 8> projectionList;
  StackList<SILInstruction *> instsToDelete;

  /// We use this set to walk from our initial copy to our final use and ensure
  /// that there aren't any instructions we did not visit in between them. This
  /// is to ensure that there aren't any instructions we didn't scan and
  /// analyze.
  InstructionSet visitedInsts;

  SILValue rootAddress;
  Operand *finalUse = nullptr;

  SimpleTemporaryAllocStackElimState(SILValue rootAddress)
      : instsToDelete(rootAddress->getFunction()),
        visitedInsts(rootAddress->getFunction()), rootAddress(rootAddress) {}

  bool setFinalUser(Operand *newFinalUse) {
    if (finalUse)
      return false;
    finalUse = newFinalUse;
    return true;
  }

  /// Walk from use->def pattern matching struct_element_addr/tuple_element_addr
  /// from \p useAddress until it is \p allocationAddress. If we see a different
  /// instruction, we return false to signal failure. Returns true if all
  /// instructions along use->def walk are said instructions.
  bool appendProjections(SILValue useAddress, SILValue allocationAddress) {
    return walkUseToDefsStructTupleProjections(
        useAddress, allocationAddress,
        [&](SingleValueInstruction *sei) { projectionList.push_back(sei); });
  }
};

struct SimpleTemporaryAllocStackElimVisitor final
    : public TransitiveAddressWalker {
  SimpleTemporaryAllocStackElimState &state;
  CopyAddrInst *caiToVisit;
  CopyAddrInst *&nextCAI;

  SimpleTemporaryAllocStackElimVisitor(
      SimpleTemporaryAllocStackElimState &state, CopyAddrInst *cai,
      CopyAddrInst *&nextCAI)
      : state(state), caiToVisit(cai), nextCAI(nextCAI) {
    assert(nextCAI == nullptr);
  }

  AllocStackInst *getAllocation() const {
    return cast<AllocStackInst>(caiToVisit->getDest());
  }

  bool setNextCAI(CopyAddrInst *newCAI) {
    // If we already have a CAI, bail. We should only ever have one.
    if (nextCAI)
      return false;
    nextCAI = newCAI;
    return true;
  }

  bool visitUse(Operand *op) override {
    LLVM_DEBUG(llvm::dbgs() << "SimpleTemporaryAllocStackElimVisitor visiting: "
                            << *op->getUser());

    state.visitedInsts.insert(op->getUser());

    // We do not care about type dependent uses.
    if (op->isTypeDependent())
      return true;

    auto *user = op->getUser();

    // We should never see a debug_value use since this should be a temporary.
    if (user->isDebugInstruction()) {
      LLVM_DEBUG(llvm::dbgs() << "Found a debug_value?! This should be a "
                                 "temporary which implies no debug info!\n");
      return false;
    }

    // If we are visiting our own copy_addr, then just return true. We do not
    // need to do any further work. If we successfully process this, then we
    // shouldn't need anything.
    if (user == caiToVisit) {
      return true;
    }

    // Skip destroy_addr and dealloc_stack. We will remove them if we succeed in
    // our mission. We require they are directly on the temporary allocation.
    if (isa<DestroyAddrInst>(user) || isa<DeallocStackInst>(user)) {
      if (op->get() != getAllocation())
        return false;
      return true;
    }

    // If our operand is an initialization, we bail. We never have multiple
    // initializations for these sorts of temporaries. Our initial copy_addr is
    // our single initialization.
    if (noncopyable::memInstMustInitialize(op)) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Found extra initializer! Bailing!: " << *user);
      return false;
    }

    // We do not allow for reinitialization since we are working with
    // specifically lets.
    if (noncopyable::memInstMustReinitialize(op)) {
      LLVM_DEBUG(llvm::dbgs() << "Found reinit: " << *user);
      return false;
    }

    // If we see a consuming instruction, then this must be a final allocation
    // in our chain of temporary allocations.
    if (noncopyable::memInstMustConsume(op)) {
      // If we already found a CAI, bail.
      if (nextCAI)
        return false;

      // We do not append projections here since we handle the projections
      // associated with the final user once we visit everything. This ensures
      // that if we have multiple projections on the final allocation, we can
      // tell these projections apart from projections from earlier allocations.
      return state.setFinalUser(state.finalUse);
    }

    // If we see a load operation, stash it. This load operation will become a
    // copy.
    if (auto *li = dyn_cast<LoadInst>(user)) {
      // If we already found a CAI, bail.
      if (nextCAI)
        return false;

      // We do not handle takes for now.
      if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Take)
        return false;

      // We do not append projections here since we handle the projections
      // associated with the final user once we visit everything. This ensures
      // that if we have multiple projections on the final allocation, we can
      // tell these projections apart from projections from earlier allocations.
      return state.setFinalUser(op);
    }

    if (auto *cai = dyn_cast<CopyAddrInst>(user)) {
      // If we already found a copy, bail. We always only visit one of these
      // regardless if they are a final user or a temporary copy.
      if (!setNextCAI(cai))
        return false;

      // If our address is not the src or we are taking the src, bail. We do not
      // handle this.
      if (cai->getSrc() != op->get() || cai->isTakeOfSrc() ||
          cai->getSrc() == cai->getDest())
        return false;

      // If we are not initializing our dest, then we know that we do not have
      // another iterative temporary. Treat this as a final user.
      if (!cai->isInitializationOfDest()) {
        return state.setFinalUser(op);
      }

      // Ok, we are initializing some other memory. Check if our dest is another
      // temporary alloc_stack. If not, treat this as the final user.
      auto *newTemp = dyn_cast<AllocStackInst>(cai->getDest());
      if (!newTemp || newTemp->isLexical()) {
        return state.setFinalUser(op);
      }

      // Ok, we have another temporary allocation copy, add the copy_addr to the
      // worklist and add this allocation to the dead allocation list.
      if (!state.appendProjections(cai->getSrc(), caiToVisit->getDest()))
        return false;
      state.instsToDelete.push_back(getAllocation());
      return true;
    }

    // Otherwise, we have a potential liveness use. If the use writes to memory,
    // bail, we do not support it.
    if (user->mayWriteToMemory()) {
      return false;
    }

    // Liveness use.
    state.setFinalUser(op);

    // We found an instruction that we did not understand.
    return true;
  }
};

} // namespace

/// Returns false if we saw something we did not understand and the copy_addr
/// should be inserted into UseState::copyInst to be conservative.
bool siloptimizer::eliminateTemporaryAllocationsFromLet(
    MarkMustCheckInst *markedInst) {
  if (!isLetAllocation(markedInst))
    return false;

  StackList<CopyAddrInst *> copiesToVisit(markedInst->getFunction());
  struct FindCopyAddrWalker final : public TransitiveAddressWalker {
    StackList<CopyAddrInst *> &copiesToVisit;
    FindCopyAddrWalker(StackList<CopyAddrInst *> &copiesToVisit)
        : TransitiveAddressWalker(), copiesToVisit(copiesToVisit) {}

    bool visitUse(Operand *op) override {
      auto *cai = dyn_cast<CopyAddrInst>(op->getUser());
      // We want copy_addr that are not a take of src and are an init of their
      // dest.
      if (!cai || cai->isTakeOfSrc() || !cai->isInitializationOfDest())
        return true;
      copiesToVisit.push_back(cai);
      return true;
    };
  };
  FindCopyAddrWalker walker(copiesToVisit);
  std::move(walker).walk(markedInst);

  bool madeChange = false;

  while (!copiesToVisit.empty()) {
    auto *initialCAI = copiesToVisit.pop_back_val();
    SimpleTemporaryAllocStackElimState state(markedInst);
    auto *asi = dyn_cast<AllocStackInst>(initialCAI->getDest());
    if (!asi || asi->isLexical())
      continue;

    if ( // If we have that our dest/src are the same, just bail. We shouldn't
         // see this, but lets just be careful.
        initialCAI->getSrc() == initialCAI->getDest())
      continue;

    // For now, just handle if we have an init/no take. We should add support
    // for a take in the future.
    if (!initialCAI->isInitializationOfDest() || initialCAI->isTakeOfSrc())
      continue;

    AllocStackInst *finalAllocation = nullptr;
    CopyAddrInst *nextCAI = initialCAI;
    unsigned numLastProjections = 0;
    unsigned numProjectionsPrevIteration = 0;
    do {
      auto *cai = nextCAI;
      nextCAI = nullptr;
      SimpleTemporaryAllocStackElimVisitor visitor(state, cai, nextCAI);

      if (AddressUseKind::Unknown == std::move(visitor).walk(cai->getDest()))
        return false;

      // If we did not find a nextCAI, do not have a final use, and we already
      // saw at least one allocation, make cai our last user. We treat that last
      // allocation as our true last allocation, so we break.
      //
      // DISCUSSION: This occurs if we have a temporary copy that ends in a
      // copyable address only type.
      if (!nextCAI && !state.finalUse && finalAllocation) {
        state.finalUse = &cai->getAllOperands()[CopyLikeInstruction::Src];
        state.projectionList.pop_back_n(state.projectionList.size() -
                                        numProjectionsPrevIteration);
        break;
      }

      finalAllocation = cast<AllocStackInst>(cai->getDest());
      numProjectionsPrevIteration = numLastProjections;
      numLastProjections = state.projectionList.size();
    } while (nextCAI);

    assert(finalAllocation);

    // If we did not actually find a final use, just bail. We can't rewrite.
    auto *finalUse = state.finalUse;
    if (!finalUse)
      continue;

    // Then check that our final use and initialCAI are in the same block and
    // that all instructions in between them with side-effects are instructions
    // that we visited. This is a sanity check.
    if (finalUse->getParentBlock() != initialCAI->getParent() ||
        llvm::any_of(llvm::make_range(initialCAI->getIterator(),
                                      finalUse->getUser()->getIterator()),
                     [&](SILInstruction &inst) {
                       return !state.visitedInsts.contains(&inst) &&
                              inst.mayHaveSideEffects();
                     }))
      continue;

    // Now that we have succeeded in our analysis, we perform our
    // transformation. First if we do not have a finalUse, just bail.

    // Otherwise, begin rewriting. First see if we our final use is directly on
    // the final allocation or if it has intervening projections. If it has
    // intervening projections, we need to rewrite the final projection.
    SingleValueInstruction *finalProjection = nullptr;
    if (!walkUseToDefsStructTupleProjections(
            finalUse->get(), finalAllocation,
            [&](SingleValueInstruction *proj) { finalProjection = proj; }))
      continue;

    // Now that we have looked through all potential projections on our final
    // use, now walk and fix up the projections.
    auto &projList = state.projectionList;

    madeChange = true;

    // First set our initial projection to initialCAI.
    if (!projList.empty()) {
      (*projList.begin())->setOperand(0, initialCAI->getSrc());
    } else {
      if (finalProjection) {
        finalProjection->setOperand(0, initialCAI->getSrc());
      } else {
        finalUse->set(initialCAI->getSrc());
      }
    }

    for (auto ii = projList.begin(), ie = projList.end(); ii != ie;) {
      auto *proj = *ii;
      ++ii;

      // If next ii is ie, then make finalProjection/final use, use this
      // value.
      if (ii == ie) {
        if (finalProjection) {
          finalProjection->setOperand(0, proj);
        } else {
          finalUse->set(proj);
        }
        break;
      }

      // Otherwise, see if the next projection has this projection as its
      // operand. If so, just continue, we do not need to update
      // anything.
      if ((*ii)->getOperand(0) == SILValue(proj))
        continue;

      // Otherwise, we jumped to another allocation, set ii's operand to proj.
      (*ii)->setOperand(0, proj);
    }

    // Now go through all of the instructions to delete and delete them. They
    // should consist only of alloc_stack, destroy_addr, and dealloc_stack.
    InstructionDeleter deleter;
    while (!state.instsToDelete.empty())
      deleter.forceDeleteWithUsers(state.instsToDelete.pop_back_val());
    deleter.forceDeleteWithUsers(finalAllocation);
  }

  return madeChange;
}
