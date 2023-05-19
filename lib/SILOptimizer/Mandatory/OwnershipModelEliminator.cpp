//===--- OwnershipModelEliminator.cpp - Eliminate SILOwnership Instr. -----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
///  \file
///
///  This file contains a small pass that lowers SIL ownership instructions to
///  their constituent operations. This will enable us to separate
///  implementation
///  of Semantic ARC in SIL and SILGen from ensuring that all of the optimizer
///  passes respect Semantic ARC. This is done by running this pass right after
///  SILGen and as the pass pipeline is updated, moving this pass further and
///  further back in the pipeline.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-ownership-model-eliminator"

#include "swift/Basic/BlotSetVector.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/SimplifyInstruction.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/StackNesting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"

using namespace swift;

// Utility command line argument to dump the module before we eliminate
// ownership from it.
static llvm::cl::opt<std::string>
DumpBefore("sil-dump-before-ome-to-path", llvm::cl::Hidden);

//===----------------------------------------------------------------------===//
//                               Implementation
//===----------------------------------------------------------------------===//

namespace {

/// A high level SILInstruction visitor that lowers Ownership SSA from SIL.
///
/// NOTE: Erasing instructions must always be done by the method
/// eraseInstruction /and/ any instructions that are created in one visit must
/// not be deleted in the same visit since after each visit, we empty the
/// tracking list into the instructionsToSimplify array. We do this in order to
/// ensure that when we use inst-simplify on these instructions, we have
/// consistent non-ossa vs ossa code rather than an intermediate state.
struct OwnershipModelEliminatorVisitor
    : SILInstructionVisitor<OwnershipModelEliminatorVisitor, bool> {
  SmallVector<SILInstruction *, 8> trackingList;
  SmallBlotSetVector<SILInstruction *, 8> instructionsToSimplify;
  
  /// Points at either a user passed in SILBuilderContext or points at
  /// builderCtxStorage.
  SILBuilderContext builderCtx;

  /// Construct an OME visitor for eliminating ownership from \p fn.
  OwnershipModelEliminatorVisitor(SILFunction &fn)
      : trackingList(), instructionsToSimplify(),
        builderCtx(fn.getModule(), &trackingList) {
  }

  /// A "syntactic" high level function that combines our insertPt with a
  /// builder ctx.
  ///
  /// Since this is syntactic and we assume that our caller is passing in a
  /// lambda that if we inline will be eliminated, we mark this function always
  /// inline.
  template <typename ResultTy>
  ResultTy LLVM_ATTRIBUTE_ALWAYS_INLINE
  withBuilder(SILInstruction *insertPt,
              llvm::function_ref<ResultTy(SILBuilder &, SILLocation)> visitor) {
    SILBuilderWithScope builder(insertPt, builderCtx);
    return visitor(builder, insertPt->getLoc());
  }

  void drainTrackingList() {
    // Called before we visit a new instruction and before we ever erase an
    // instruction. This ensures that we can post-process instructions that need
    // simplification in a purely non-ossa world instead of an indeterminate
    // state mid elimination.
    while (!trackingList.empty()) {
      instructionsToSimplify.insert(trackingList.pop_back_val());
    }
  }

  void beforeVisit(SILInstruction *instToVisit) {
    // Add any elements to the tracking list that we currently have in the
    // tracking list that we haven't added yet.
    drainTrackingList();
  }

  void eraseInstruction(SILInstruction *i) {
    // Before we erase anything, drain the tracking list.
    drainTrackingList();

    // Make sure to blot our instruction.
    instructionsToSimplify.erase(i);
    i->eraseFromParent();
  }

  void eraseInstructionAndRAUW(SingleValueInstruction *i, SILValue newValue) {
    // Make sure to blot our instruction.
    i->replaceAllUsesWith(newValue);
    eraseInstruction(i);
  }

  bool visitSILInstruction(SILInstruction *inst) {
    // Make sure this wasn't a forwarding instruction in case someone adds a new
    // forwarding instruction but does not update this code.
    if (OwnershipForwardingMixin::isa(inst)) {
      llvm::errs() << "Found unhandled forwarding inst: " << *inst;
      llvm_unreachable("standard error handler");
    }
    return false;
  }

  bool visitLoadInst(LoadInst *li);
  bool visitStoreInst(StoreInst *si);
  bool visitStoreBorrowInst(StoreBorrowInst *si);
  bool visitCopyValueInst(CopyValueInst *cvi);
  bool visitExplicitCopyValueInst(ExplicitCopyValueInst *cvi);
  bool visitExplicitCopyAddrInst(ExplicitCopyAddrInst *cai);
  bool visitDestroyValueInst(DestroyValueInst *dvi);
  bool visitLoadBorrowInst(LoadBorrowInst *lbi);
  bool visitMoveValueInst(MoveValueInst *mvi) {
    eraseInstructionAndRAUW(mvi, mvi->getOperand());
    return true;
  }
  bool visitDropDeinitInst(DropDeinitInst *ddi) {
    eraseInstructionAndRAUW(ddi, ddi->getOperand());
    return true;
  }
  bool visitBeginBorrowInst(BeginBorrowInst *bbi) {
    eraseInstructionAndRAUW(bbi, bbi->getOperand());
    return true;
  }
  bool visitEndBorrowInst(EndBorrowInst *ebi) {
    eraseInstruction(ebi);
    return true;
  }
  bool visitEndLifetimeInst(EndLifetimeInst *eli) {
    eraseInstruction(eli);
    return true;
  }
  bool visitUncheckedOwnershipConversionInst(
      UncheckedOwnershipConversionInst *uoci) {
    eraseInstructionAndRAUW(uoci, uoci->getOperand());
    return true;
  }
  bool visitUnmanagedRetainValueInst(UnmanagedRetainValueInst *urvi);
  bool visitUnmanagedReleaseValueInst(UnmanagedReleaseValueInst *urvi);
  bool visitUnmanagedAutoreleaseValueInst(UnmanagedAutoreleaseValueInst *uavi);
  bool visitCheckedCastBranchInst(CheckedCastBranchInst *cbi);
  bool visitSwitchEnumInst(SwitchEnumInst *swi);
  bool visitDestructureStructInst(DestructureStructInst *dsi);
  bool visitDestructureTupleInst(DestructureTupleInst *dti);

  // We lower this to unchecked_bitwise_cast losing our assumption of layout
  // compatibility.
  bool visitUncheckedValueCastInst(UncheckedValueCastInst *uvci) {
    return withBuilder<bool>(uvci, [&](SILBuilder &b, SILLocation loc) {
      auto *newVal = b.createUncheckedBitwiseCast(loc, uvci->getOperand(),
                                                  uvci->getType());
      eraseInstructionAndRAUW(uvci, newVal);
      return true;
    });
  }
  
  bool visitPartialApplyInst(PartialApplyInst *pai);

  void splitDestructure(SILInstruction *destructure,
                        SILValue destructureOperand);

#define HANDLE_FORWARDING_INST(Cls)                                            \
  bool visit##Cls##Inst(Cls##Inst *i) {                                        \
    if (isa<SelectValueInst>(i)) {                                             \
      return true;                                                             \
    }                                                                          \
    OwnershipForwardingMixin::get(i)->setForwardingOwnershipKind(              \
        OwnershipKind::None);                                                  \
    return true;                                                               \
  }
  HANDLE_FORWARDING_INST(ConvertFunction)
  HANDLE_FORWARDING_INST(MoveOnlyWrapperToCopyableValue)
  HANDLE_FORWARDING_INST(Upcast)
  HANDLE_FORWARDING_INST(UncheckedRefCast)
  HANDLE_FORWARDING_INST(RefToBridgeObject)
  HANDLE_FORWARDING_INST(BridgeObjectToRef)
  HANDLE_FORWARDING_INST(ThinToThickFunction)
  HANDLE_FORWARDING_INST(UnconditionalCheckedCast)
  HANDLE_FORWARDING_INST(Struct)
  HANDLE_FORWARDING_INST(Object)
  HANDLE_FORWARDING_INST(Tuple)
  HANDLE_FORWARDING_INST(Enum)
  HANDLE_FORWARDING_INST(UncheckedEnumData)
  HANDLE_FORWARDING_INST(SelectEnum)
  HANDLE_FORWARDING_INST(SelectValue)
  HANDLE_FORWARDING_INST(OpenExistentialRef)
  HANDLE_FORWARDING_INST(InitExistentialRef)
  HANDLE_FORWARDING_INST(MarkDependence)
  HANDLE_FORWARDING_INST(DifferentiableFunction)
  HANDLE_FORWARDING_INST(LinearFunction)
  HANDLE_FORWARDING_INST(StructExtract)
  HANDLE_FORWARDING_INST(TupleExtract)
  HANDLE_FORWARDING_INST(LinearFunctionExtract)
  HANDLE_FORWARDING_INST(DifferentiableFunctionExtract)
  HANDLE_FORWARDING_INST(MarkUninitialized)
#undef HANDLE_FORWARDING_INST
};

} // end anonymous namespace

bool OwnershipModelEliminatorVisitor::visitLoadInst(LoadInst *li) {
  auto qualifier = li->getOwnershipQualifier();

  // If the qualifier is unqualified, there is nothing further to do
  // here. Just return.
  if (qualifier == LoadOwnershipQualifier::Unqualified)
    return false;

  auto result = withBuilder<SILValue>(li, [&](SILBuilder &b, SILLocation loc) {
    return b.emitLoadValueOperation(loc, li->getOperand(),
                                    li->getOwnershipQualifier());
  });

  // Then remove the qualified load and use the unqualified load as the def of
  // all of LI's uses.
  eraseInstructionAndRAUW(li, result);
  return true;
}

bool OwnershipModelEliminatorVisitor::visitStoreInst(StoreInst *si) {
  auto qualifier = si->getOwnershipQualifier();

  // If the qualifier is unqualified, there is nothing further to do
  // here. Just return.
  if (qualifier == StoreOwnershipQualifier::Unqualified)
    return false;

  withBuilder<void>(si, [&](SILBuilder &b, SILLocation loc) {
    b.emitStoreValueOperation(loc, si->getSrc(), si->getDest(),
                              si->getOwnershipQualifier());
  });

  // Then remove the qualified store.
  eraseInstruction(si);
  return true;
}

bool OwnershipModelEliminatorVisitor::visitStoreBorrowInst(
    StoreBorrowInst *si) {
  withBuilder<void>(si, [&](SILBuilder &b, SILLocation loc) {
    b.emitStoreValueOperation(loc, si->getSrc(), si->getDest(),
                              StoreOwnershipQualifier::Unqualified);
  });

  // Then remove the qualified store after RAUWing si with its dest. This
  // ensures that any uses of the interior pointer result of the store_borrow
  // are rewritten to be on the dest point.
  si->replaceAllUsesWith(si->getDest());
  eraseInstruction(si);
  return true;
}

bool OwnershipModelEliminatorVisitor::visitLoadBorrowInst(LoadBorrowInst *lbi) {
  // Break down the load borrow into an unqualified load.
  auto newLoad =
      withBuilder<SILValue>(lbi, [&](SILBuilder &b, SILLocation loc) {
        return b.createLoad(loc, lbi->getOperand(),
                            LoadOwnershipQualifier::Unqualified);
      });

  // Then remove the qualified load and use the unqualified load as the def of
  // all of LI's uses.
  eraseInstructionAndRAUW(lbi, newLoad);
  return true;
}

bool OwnershipModelEliminatorVisitor::visitCopyValueInst(CopyValueInst *cvi) {
  // A copy_value of an address-only type cannot be replaced.
  if (cvi->getType().isAddressOnly(*cvi->getFunction()))
    return false;

  // Nonescaping closures are represented ultimately as trivial pointers to
  // their context, but we use ownership to do borrow checking of their captures
  // in OSSA. Now that we're eliminating ownership, fold away copies.
  if (auto cvFnTy = cvi->getType().getAs<SILFunctionType>()) {
    if (cvFnTy->isTrivialNoEscape()) {
      // Erase any `destroy_value`s of this copy, so we don't mistake them for
      // the end of the original value's lifetime after we RAUW.
      SmallVector<DestroyValueInst *, 2> destroys;
      for (auto user : cvi->getUsersOfType<DestroyValueInst>()) {
        destroys.push_back(user);
      }
      for (auto destroy : destroys) {
        eraseInstruction(destroy);
      }
      eraseInstructionAndRAUW(cvi, cvi->getOperand());
      return true;
    }
  }

  // Now that we have set the unqualified ownership flag, emitCopyValueOperation
  // operation will delegate to the appropriate strong_release, etc.
  withBuilder<void>(cvi, [&](SILBuilder &b, SILLocation loc) {
    b.emitCopyValueOperation(loc, cvi->getOperand());
  });
  eraseInstructionAndRAUW(cvi, cvi->getOperand());
  return true;
}

bool OwnershipModelEliminatorVisitor::visitExplicitCopyValueInst(
    ExplicitCopyValueInst *cvi) {
  // A copy_value of an address-only type cannot be replaced.
  if (cvi->getType().isAddressOnly(*cvi->getFunction()))
    return false;

  // Now that we have set the unqualified ownership flag, destroy value
  // operation will delegate to the appropriate strong_release, etc.
  withBuilder<void>(cvi, [&](SILBuilder &b, SILLocation loc) {
    b.emitCopyValueOperation(loc, cvi->getOperand());
  });
  eraseInstructionAndRAUW(cvi, cvi->getOperand());
  return true;
}

bool OwnershipModelEliminatorVisitor::visitExplicitCopyAddrInst(
    ExplicitCopyAddrInst *ecai) {
  // Now that we have set the unqualified ownership flag, destroy value
  // operation will delegate to the appropriate strong_release, etc.
  withBuilder<void>(ecai, [&](SILBuilder &b, SILLocation loc) {
    b.createCopyAddr(loc, ecai->getSrc(), ecai->getDest(), ecai->isTakeOfSrc(),
                     ecai->isInitializationOfDest());
  });
  eraseInstruction(ecai);
  return true;
}

bool OwnershipModelEliminatorVisitor::visitUnmanagedRetainValueInst(
    UnmanagedRetainValueInst *urvi) {
  // Now that we have set the unqualified ownership flag, destroy value
  // operation will delegate to the appropriate strong_release, etc.
  withBuilder<void>(urvi, [&](SILBuilder &b, SILLocation loc) {
    b.emitCopyValueOperation(loc, urvi->getOperand());
  });
  eraseInstruction(urvi);
  return true;
}

bool OwnershipModelEliminatorVisitor::visitUnmanagedReleaseValueInst(
    UnmanagedReleaseValueInst *urvi) {
  // Now that we have set the unqualified ownership flag, destroy value
  // operation will delegate to the appropriate strong_release, etc.
  withBuilder<void>(urvi, [&](SILBuilder &b, SILLocation loc) {
    b.emitDestroyValueOperation(loc, urvi->getOperand());
  });
  eraseInstruction(urvi);
  return true;
}

bool OwnershipModelEliminatorVisitor::visitUnmanagedAutoreleaseValueInst(
    UnmanagedAutoreleaseValueInst *UAVI) {
  // Now that we have set the unqualified ownership flag, destroy value
  // operation will delegate to the appropriate strong_release, etc.
  withBuilder<void>(UAVI, [&](SILBuilder &b, SILLocation loc) {
    b.createAutoreleaseValue(loc, UAVI->getOperand(), UAVI->getAtomicity());
  });
  eraseInstruction(UAVI);
  return true;
}

// Poison every debug variable associated with \p value.
static void injectDebugPoison(DestroyValueInst *destroy) {
  // TODO: SILDebugVariable should define it's key. Until then, we try to be
  // consistent with IRGen.
  using StackSlotKey =
      std::pair<unsigned, std::pair<const SILDebugScope *, StringRef>>;
  // This DenseSet points to StringRef memory into the debug_value insts.
  llvm::SmallDenseSet<StackSlotKey> poisonedVars;

  SILValue destroyedValue = destroy->getOperand();
  for (Operand *use : getDebugUses(destroyedValue)) {
    auto debugVal = dyn_cast<DebugValueInst>(use->getUser());
    if (!debugVal || debugVal->poisonRefs())
      continue;

    const SILDebugScope *scope = debugVal->getDebugScope();
    auto loc = debugVal->getLoc();

    Optional<SILDebugVariable> varInfo = debugVal->getVarInfo();
    if (!varInfo)
      continue;

    unsigned argNo = varInfo->ArgNo;
    if (!poisonedVars.insert({argNo, {scope, varInfo->Name}}).second)
      continue;

    SILBuilder builder(destroy);
    // The poison DebugValue's DebugLocation must be identical to the original
    // DebugValue. The DebugScope is used to identify the variable's unique
    // shadow copy. The SILLocation is used to determine the VarDecl, which is
    // necessary in some cases to derive a unique variable name.
    //
    // This debug location is obviously inconsistent with surrounding code, but
    // IRGen is responsible for fixing this.
    builder.setCurrentDebugScope(scope);
    auto *newDebugVal = builder.createDebugValue(loc, destroyedValue, *varInfo,
                                                 /*poisonRefs*/ true);
    assert(*(newDebugVal->getVarInfo()) == *varInfo && "lost in translation");
    (void)newDebugVal;
  }
}

bool OwnershipModelEliminatorVisitor::visitPartialApplyInst(
    PartialApplyInst *inst) {
  // Escaping closures don't need attention beyond what we already perform.
  if (!inst->isOnStack())
    return false;
  
  // A nonescaping closure borrows its captures, but now that we've lowered
  // those borrows away, we need to make those dependence relationships explicit
  // so that the optimizer continues respecting them.
  MarkDependenceInst *firstNewMDI = nullptr;
  auto newValue = withBuilder<SILValue>(inst->getNextInstruction(),
                                        [&](SILBuilder &b, SILLocation loc) {
    SILValue newValue = inst;
    for (auto op : inst->getArguments()) {
      // Trivial types have infinite lifetimes already.
      if (op->getType().isTrivial(*inst->getFunction())) {
        break;
      }
      // Address operands should already have their dependence marked, since
      // borrowing doesn't model values in memory.
      if (op->getType().isAddress()) {
        break;
      }
      
      // If this is a nontrivial value argument, insert the mark_dependence.
      auto mdi = b.createMarkDependence(loc, newValue, op);
      if (!firstNewMDI)
        firstNewMDI = mdi;
      newValue = mdi;
    }
    return newValue;
  });
  
  // Rewrite all uses other than the root of the new dependence chain, and a
  // `dealloc_stack` of the partial_apply instruction we may have already
  // created, to go through the dependence chain, if there is one.
  if (firstNewMDI) {
    while (!inst->use_empty()) {
      auto opI = inst->use_begin();
      while ((*opI)->getUser() == firstNewMDI
             || isa<DeallocStackInst>((*opI)->getUser())) {
        ++opI;
        if (opI == inst->use_end()) {
          goto done_rewriting;
        }
      }
      (*opI)->set(newValue);
    }
done_rewriting:
    return true;
  }
  
  return false;
}

bool OwnershipModelEliminatorVisitor::visitDestroyValueInst(
    DestroyValueInst *dvi) {
  // Nonescaping closures are represented ultimately as trivial pointers to
  // their context, but we use ownership to do borrow checking of their captures
  // in OSSA. Now that we're eliminating ownership, fold away destroys.
  auto operand = dvi->getOperand();
  auto operandTy = operand->getType();
  if (auto operandFnTy = operandTy.getAs<SILFunctionType>()){
    if (operandFnTy->isTrivialNoEscape()) {
      eraseInstruction(dvi);
      return true;
    }
  }
    
  // A destroy_value of an address-only type cannot be replaced.
  //
  // TODO: When LowerAddresses runs before this, we can remove this case.
  if (operandTy.isAddressOnly(*dvi->getFunction()))
    return false;

  // Now that we have set the unqualified ownership flag,
  // emitDestroyValueOperation will insert the appropriate instruction.
  withBuilder<void>(dvi, [&](SILBuilder &b, SILLocation loc) {
    b.emitDestroyValueOperation(loc, operand);
  });
  if (dvi->poisonRefs()) {
    injectDebugPoison(dvi);
  }
  eraseInstruction(dvi);
  return true;
}

bool OwnershipModelEliminatorVisitor::visitCheckedCastBranchInst(
    CheckedCastBranchInst *cbi) {
  cbi->setForwardingOwnershipKind(OwnershipKind::None);

  // In ownership qualified SIL, checked_cast_br must pass its argument to the
  // fail case so we can clean it up. In non-ownership qualified SIL, we expect
  // no argument from the checked_cast_br in the default case. The way that we
  // handle this transformation is that:
  //
  // 1. We replace all uses of the argument to the false block with a use of the
  // checked cast branch's operand.
  // 2. We delete the argument from the false block.
  SILBasicBlock *failureBlock = cbi->getFailureBB();
  if (failureBlock->getNumArguments() == 0)
    return false;
  failureBlock->getArgument(0)->replaceAllUsesWith(cbi->getOperand());
  failureBlock->eraseArgument(0);
  return true;
}

bool OwnershipModelEliminatorVisitor::visitSwitchEnumInst(
    SwitchEnumInst *swei) {
  swei->setForwardingOwnershipKind(OwnershipKind::None);

  // In ownership qualified SIL, switch_enum must pass its argument to the fail
  // case so we can clean it up. In non-ownership qualified SIL, we expect no
  // argument from the switch_enum in the default case. The way that we handle
  // this transformation is that:
  //
  // 1. We replace all uses of the argument to the false block with a use of the
  // checked cast branch's operand.
  // 2. We delete the argument from the false block.
  if (!swei->hasDefault())
    return false;

  SILBasicBlock *defaultBlock = swei->getDefaultBB();
  if (defaultBlock->getNumArguments() == 0)
    return false;
  defaultBlock->getArgument(0)->replaceAllUsesWith(swei->getOperand());
  defaultBlock->eraseArgument(0);
  return true;
}

void OwnershipModelEliminatorVisitor::splitDestructure(
    SILInstruction *destructureInst, SILValue destructureOperand) {
  assert((isa<DestructureStructInst>(destructureInst) ||
          isa<DestructureTupleInst>(destructureInst)) &&
         "Only destructure operations can be passed to splitDestructure");

  // First before we destructure anything, see if we can simplify any of our
  // instruction operands.
  SILModule &M = destructureInst->getModule();
  SILType opType = destructureOperand->getType();

  llvm::SmallVector<Projection, 8> projections;
  Projection::getFirstLevelProjections(
      opType, M, TypeExpansionContext(*destructureInst->getFunction()),
      projections);
  assert(projections.size() == destructureInst->getNumResults());

  auto destructureResults = destructureInst->getResults();
  for (unsigned index : indices(destructureResults)) {
    SILValue result = destructureResults[index];

    // If our result doesnt have any uses, do not emit instructions, just skip
    // it.
    if (result->use_empty())
      continue;

    // Otherwise, create a projection.
    const auto &proj = projections[index];
    auto *projInst = withBuilder<SingleValueInstruction *>(
        destructureInst, [&](SILBuilder &b, SILLocation loc) {
          return proj.createObjectProjection(b, loc, destructureOperand).get();
        });

    // First RAUW Result with ProjInst. This ensures that we have a complete IR
    // before we perform any simplifications.
    result->replaceAllUsesWith(projInst);
  }

  // Now that all of its uses have been eliminated, erase the destructure.
  eraseInstruction(destructureInst);
}

bool OwnershipModelEliminatorVisitor::visitDestructureStructInst(
    DestructureStructInst *dsi) {
  splitDestructure(dsi, dsi->getOperand());
  return true;
}

bool OwnershipModelEliminatorVisitor::visitDestructureTupleInst(
    DestructureTupleInst *dti) {
  splitDestructure(dti, dti->getOperand());
  return true;
}

//===----------------------------------------------------------------------===//
//                           Top Level Entry Point
//===----------------------------------------------------------------------===//

static bool stripOwnership(SILFunction &func) {
  // If F is an external declaration, do not process it.
  if (func.isExternalDeclaration())
    return false;

  llvm::DenseMap<PartialApplyInst *, SmallVector<SILInstruction *>>
      lifetimeEnds;

  // Nonescaping closures are represented ultimately as trivial pointers to
  // their context, but we use ownership to do borrow checking of their captures
  // in OSSA. Now that we're eliminating ownership, we need to dealloc_stack the
  // context at its lifetime ends.
  // partial_apply's lifetime ends has to be gathered before we begin to leave
  // OSSA, but no dealloc_stack can be emitted until after we leave OSSA.
  for (auto &block : func) {
    for (auto &ii : block) {
      auto *pai = dyn_cast<PartialApplyInst>(&ii);
      if (!pai || !pai->isOnStack()) {
        continue;
      }
      pai->visitOnStackLifetimeEnds([&](Operand *op) {
        lifetimeEnds[pai].push_back(op->getUser());
        return true;
      });
    }
  }

  // Set F to have unqualified ownership.
  func.setOwnershipEliminated();

  // Now that we are in non-ossa, create dealloc_stack at partial_apply's
  // lifetime ends
  for (auto &it : lifetimeEnds) {
    auto *pai = it.first;
    for (auto *lifetimeEnd : it.second) {
      SILBuilderWithScope(lifetimeEnd->getNextInstruction())
          .createDeallocStack(lifetimeEnd->getLoc(), pai);
    }
  }

  bool madeChange = false;
  SmallVector<SILInstruction *, 32> createdInsts;
  OwnershipModelEliminatorVisitor visitor(func);

  for (auto &block : func) {
    // Change all arguments to have OwnershipKind::None.
    for (auto *arg : block.getArguments()) {
      arg->setOwnershipKind(OwnershipKind::None);
    }

    for (auto ii = block.begin(), ie = block.end(); ii != ie;) {
      // Since we are going to be potentially removing instructions, we need
      // to make sure to increment our iterator before we perform any
      // visits.
      SILInstruction *inst = &*ii;
      ++ii;

      madeChange |= visitor.visit(inst);
    }
  }

  // Once we have finished processing all instructions, we should be
  // consistently in non-ossa form meaning that it is now safe for us to invoke
  // utilities that assume that they are in a consistent ossa or non-ossa form
  // such as inst simplify. Now go through any instructions and simplify using
  // inst simplify!
  //
  // DISCUSSION: We want our utilities to be able to assume if f.hasOwnership()
  // is false then the utility is allowed to assume the function the utility is
  // invoked within is in non-ossa form structurally (e.x.: non-ossa does not
  // have arguments on the default result of checked_cast_br).
  while (!visitor.instructionsToSimplify.empty()) {
    auto value = visitor.instructionsToSimplify.pop_back_val();
    if (!value.has_value())
      continue;
    auto callbacks =
        InstModCallbacks().onDelete([&](SILInstruction *instToErase) {
          visitor.eraseInstruction(instToErase);
        });
    // We are no longer in OSSA, so we don't need to pass in a deBlocks.
    simplifyAndReplaceAllSimplifiedUsesAndErase(*value, callbacks);
    madeChange |= callbacks.hadCallbackInvocation();
  }
  
  if (madeChange) {
    StackNesting::fixNesting(&func);
  }
  
  return madeChange;
}

static void prepareNonTransparentSILFunctionForOptimization(ModuleDecl *,
                                                            SILFunction *f) {
  if (!f->hasOwnership() || f->isTransparent())
    return;

  LLVM_DEBUG(llvm::dbgs() << "After deserialization, stripping ownership in:"
                          << f->getName() << "\n");

  stripOwnership(*f);
}

static void prepareSILFunctionForOptimization(ModuleDecl *, SILFunction *f) {
  if (!f->hasOwnership())
    return;

  LLVM_DEBUG(llvm::dbgs() << "After deserialization, stripping ownership in:"
                          << f->getName() << "\n");

  stripOwnership(*f);
}

namespace {

struct OwnershipModelEliminator : SILFunctionTransform {
  bool skipTransparent;
  bool skipStdlibModule;

  OwnershipModelEliminator(bool skipTransparent, bool skipStdlibModule)
      : skipTransparent(skipTransparent), skipStdlibModule(skipStdlibModule) {}

  void run() override {
    if (DumpBefore.size()) {
      getFunction()->dump(DumpBefore.c_str());
    }

    auto *f = getFunction();
    auto &mod = getFunction()->getModule();

    // If we are supposed to skip the stdlib module and we are in the stdlib
    // module bail.
    if (skipStdlibModule && mod.isStdlibModule()) {
      return;
    }

    if (!f->hasOwnership())
      return;

    // If we were asked to not strip ownership from transparent functions in
    // /our/ module, return.
    if (skipTransparent && f->isTransparent())
      return;

    // Verify here to make sure ownership is correct before we strip.
    {
      // Add a pretty stack trace entry to tell users who see a verification
      // failure triggered by this verification check that they need to re-run
      // with -sil-verify-all to actually find the pass that introduced the
      // verification error.
      //
      // DISCUSSION: This occurs due to the crash from the verification
      // failure happening in the pass itself. This causes us to dump the
      // SILFunction and emit a msg that this pass (OME) is the culprit. This
      // is generally correct for most passes, but not for OME since we are
      // verifying before we have even modified the function to ensure that
      // all ownership invariants have been respected before we lower
      // ownership from the function.
      llvm::PrettyStackTraceString silVerifyAllMsgOnFailure(
          "Found verification error when verifying before lowering "
          "ownership. Please re-run with -sil-verify-all to identify the "
          "actual pass that introduced the verification error.");
      f->verify(getPassManager());
    }

    if (stripOwnership(*f)) {
      auto InvalidKind = SILAnalysis::InvalidationKind::BranchesAndInstructions;
      invalidateAnalysis(InvalidKind);
    }

    // If we were asked to strip transparent, we are at the beginning of the
    // performance pipeline. In such a case, we register a handler so that all
    // future things we deserialize have ownership stripped.
    using NotificationHandlerTy =
        FunctionBodyDeserializationNotificationHandler;
    std::unique_ptr<DeserializationNotificationHandler> ptr;
    if (skipTransparent) {
      if (!mod.hasRegisteredDeserializationNotificationHandlerForNonTransparentFuncOME()) {
        ptr.reset(new NotificationHandlerTy(
            prepareNonTransparentSILFunctionForOptimization));
        mod.registerDeserializationNotificationHandler(std::move(ptr));
        mod.setRegisteredDeserializationNotificationHandlerForNonTransparentFuncOME();
      }
    } else {
      if (!mod.hasRegisteredDeserializationNotificationHandlerForAllFuncOME()) {
        ptr.reset(new NotificationHandlerTy(prepareSILFunctionForOptimization));
        mod.registerDeserializationNotificationHandler(std::move(ptr));
        mod.setRegisteredDeserializationNotificationHandlerForAllFuncOME();
      }
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createOwnershipModelEliminator() {
  return new OwnershipModelEliminator(false /*skip transparent*/,
                                      false /*ignore stdlib*/);
}

SILTransform *swift::createNonTransparentFunctionOwnershipModelEliminator() {
  return new OwnershipModelEliminator(true /*skip transparent*/,
                                      false /*ignore stdlib*/);
}
