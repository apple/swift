//===--- SILMoveOnlyWrappedTypeEliminator.cpp -----------------------------===//
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
/// This file contains an optimizer pass that lowers away move only types from
/// SIL. It can run on all types or just trivial types. It works by Walking all
/// values in the IR and unsafely converting their type to be without move
/// only. If a change is made, we add the defining instruction to a set list for
/// post-processing. Once we have updated all types in the function, we revisit
/// the instructions that we touched and update/delete them as appropriate.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-move-only-type-eliminator"

#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/ClosureScope.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                                  Visitor
//===----------------------------------------------------------------------===//

namespace {

struct SILMoveOnlyWrappedTypeEliminatorVisitor
    : SILInstructionVisitor<SILMoveOnlyWrappedTypeEliminatorVisitor, bool> {
  const SmallSetVector<SILArgument *, 8> &touchedArgs;

  SILMoveOnlyWrappedTypeEliminatorVisitor(
      const SmallSetVector<SILArgument *, 8> &touchedArgs)
      : touchedArgs(touchedArgs) {}

  bool visitSILInstruction(SILInstruction *inst) {
    llvm::errs() << "Unhandled SIL Instruction: " << *inst;
    llvm_unreachable("error");
  }

  bool eraseFromParent(SILInstruction *i) {
    LLVM_DEBUG(llvm::dbgs() << "Erasing Inst: " << *i);
    i->eraseFromParent();
    return true;
  }

  bool visitLoadInst(LoadInst *li) {
    if (!li->getType().isTrivial(*li->getFunction()))
      return false;
    li->setOwnershipQualifier(LoadOwnershipQualifier::Trivial);
    return true;
  }

  bool visitStoreInst(StoreInst *si) {
    if (!si->getSrc()->getType().isTrivial(*si->getFunction()))
      return false;
    si->setOwnershipQualifier(StoreOwnershipQualifier::Trivial);
    return true;
  }

  bool visitStoreBorrowInst(StoreBorrowInst *si) {
    if (!si->getSrc()->getType().isTrivial(*si->getFunction()))
      return false;
    SILBuilderWithScope b(si);
    b.emitStoreValueOperation(si->getLoc(), si->getSrc(), si->getDest(),
                              StoreOwnershipQualifier::Trivial);
    SmallVector<EndBorrowInst *, 4> endBorrows;
    for (auto *ebi : si->getUsersOfType<EndBorrowInst>()) {
      endBorrows.push_back(ebi);
    }
    for (auto *ebi : endBorrows) {
      eraseFromParent(ebi);
    }
    si->replaceAllUsesWith(si->getDest());
    return eraseFromParent(si);
  }

  bool visitLoadBorrowInst(LoadBorrowInst *li) {
    if (!li->getType().isTrivial(*li->getFunction()))
      return false;
    SILBuilderWithScope b(li);
    auto newVal = b.emitLoadValueOperation(li->getLoc(), li->getOperand(),
                                           LoadOwnershipQualifier::Trivial);
    li->replaceAllUsesWith(newVal);
    return eraseFromParent(li);
  }

#define RAUW_IF_TRIVIAL_RESULT(CLS)                                            \
  bool visit##CLS##Inst(CLS##Inst *inst) {                                     \
    if (!inst->getType().isTrivial(*inst->getFunction())) {                    \
      return false;                                                            \
    }                                                                          \
    inst->replaceAllUsesWith(inst->getOperand());                              \
    return eraseFromParent(inst);                                              \
  }
  RAUW_IF_TRIVIAL_RESULT(CopyValue)
  RAUW_IF_TRIVIAL_RESULT(MoveValue)
  RAUW_IF_TRIVIAL_RESULT(ExplicitCopyValue)
  RAUW_IF_TRIVIAL_RESULT(BeginBorrow)
#undef RAUW_IF_TRIVIAL_RESULT

#define RAUW_ALWAYS(CLS)                                                       \
  bool visit##CLS##Inst(CLS##Inst *inst) {                                     \
    inst->replaceAllUsesWith(inst->getOperand());                              \
    return eraseFromParent(inst);                                              \
  }
  RAUW_ALWAYS(MoveOnlyWrapperToCopyableValue)
  RAUW_ALWAYS(MoveOnlyWrapperToCopyableBox)
  RAUW_ALWAYS(MoveOnlyWrapperToCopyableAddr)
  RAUW_ALWAYS(CopyableToMoveOnlyWrapperValue)
  RAUW_ALWAYS(CopyableToMoveOnlyWrapperAddr)
#undef RAUW_ALWAYS

#define DELETE_IF_TRIVIAL_OP(CLS)                                              \
  bool visit##CLS##Inst(CLS##Inst *inst) {                                     \
    if (!inst->getOperand()->getType().isTrivial(*inst->getFunction())) {      \
      return false;                                                            \
    }                                                                          \
    return eraseFromParent(inst);                                              \
  }
  DELETE_IF_TRIVIAL_OP(DestroyValue)
  DELETE_IF_TRIVIAL_OP(EndBorrow)
#undef DELETE_IF_TRIVIAL_OP

#define NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_OP(CLS)                  \
  bool visit##CLS##Inst(CLS##Inst *inst) {                                     \
    if (!inst->getOperand()->getType().isTrivial(*inst->getFunction()))        \
      return false;                                                            \
    inst->setForwardingOwnershipKind(OwnershipKind::None);                     \
    return true;                                                               \
  }
  NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_OP(StructExtract)
  NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_OP(TupleExtract)
  NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_OP(UncheckedEnumData)
  NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_OP(SwitchEnum)
#undef NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_OP

#define NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_RESULT(CLS)              \
  bool visit##CLS##Inst(CLS##Inst *inst) {                                     \
    if (!inst->getType().isTrivial(*inst->getFunction()))                      \
      return false;                                                            \
    inst->setForwardingOwnershipKind(OwnershipKind::None);                     \
    return true;                                                               \
  }
  NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_RESULT(Enum)
  NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_RESULT(Struct)
  NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_RESULT(Tuple)
#undef NEED_TO_CONVERT_FORWARDING_TO_NONE_IF_TRIVIAL_RESULT

#define NO_UPDATE_NEEDED(CLS)                                                  \
  bool visit##CLS##Inst(CLS##Inst *inst) { return false; }
  NO_UPDATE_NEEDED(AllocStack)
  NO_UPDATE_NEEDED(AllocBox)
  NO_UPDATE_NEEDED(ProjectBox)
  NO_UPDATE_NEEDED(DebugValue)
  NO_UPDATE_NEEDED(StructElementAddr)
  NO_UPDATE_NEEDED(TupleElementAddr)
  NO_UPDATE_NEEDED(UncheckedTakeEnumDataAddr)
  NO_UPDATE_NEEDED(DestructureTuple)
  NO_UPDATE_NEEDED(DestructureStruct)
  NO_UPDATE_NEEDED(SelectEnum)
  NO_UPDATE_NEEDED(MarkDependence)
  NO_UPDATE_NEEDED(DestroyAddr)
  NO_UPDATE_NEEDED(DeallocStack)
  NO_UPDATE_NEEDED(Branch)
  NO_UPDATE_NEEDED(ExplicitCopyAddr)
  NO_UPDATE_NEEDED(CopyAddr)
  NO_UPDATE_NEEDED(RefElementAddr)
  NO_UPDATE_NEEDED(CheckedCastBranch)
  NO_UPDATE_NEEDED(Object)
  NO_UPDATE_NEEDED(OpenExistentialRef)
  NO_UPDATE_NEEDED(ConvertFunction)
  NO_UPDATE_NEEDED(RefToBridgeObject)
  NO_UPDATE_NEEDED(BridgeObjectToRef)
  NO_UPDATE_NEEDED(BeginAccess)
  NO_UPDATE_NEEDED(EndAccess)
  NO_UPDATE_NEEDED(ClassMethod)
#undef NO_UPDATE_NEEDED

  bool eliminateIdentityCast(SingleValueInstruction *svi) {
    if (svi->getOperand(0)->getType() != svi->getType())
      return false;
    svi->replaceAllUsesWith(svi->getOperand(0));
    eraseFromParent(svi);
    return true;
  }

#define ELIMINATE_POTENTIAL_IDENTITY_CAST(NAME) \
  bool visit##NAME##Inst(NAME##Inst *cast) { \
    return eliminateIdentityCast(cast); \
  }
  ELIMINATE_POTENTIAL_IDENTITY_CAST(Upcast)
  ELIMINATE_POTENTIAL_IDENTITY_CAST(UncheckedAddrCast)
  ELIMINATE_POTENTIAL_IDENTITY_CAST(UnconditionalCheckedCast)
#undef ELIMINATE_POTENTIAL_IDENTITY_CAST
  // We handle apply sites by just inserting a convert_function that converts
  // the original function type into a function type without move only. This is
  // safe since adding/removing moveonlywrapped types is ABI neutral.
  bool visitApplySite(ApplySite ai) {
    auto eliminateMoveOnlyWrapped =
        [&](TypeBase *type) -> llvm::Optional<Type> {
      if (auto *moveType = dyn_cast<SILMoveOnlyWrappedType>(type))
        return moveType->getInnerType();
      return llvm::None;
    };

    // First fix up the callee.
    auto origCalleeType = ai.getOrigCalleeType();
    auto newOrigCalleeType =
        origCalleeType.transformRec(eliminateMoveOnlyWrapped)
            ->getCanonicalType();
    auto newOrigCalleeSILType =
        SILType::getPrimitiveObjectType(newOrigCalleeType);
    SILBuilderWithScope b(*ai);
    auto *newCallee = b.createConvertFunction(
        ai->getLoc(), ai.getCallee(), newOrigCalleeSILType,
        false /*without actually escaping*/);
    ai.setCallee(newCallee);

    // Then fix up the subst function type.
    auto newSubstType = ai.getSubstCalleeType()
                            .transformRec(eliminateMoveOnlyWrapped)
                            ->getCanonicalType();
    ai.setSubstCalleeType(cast<SILFunctionType>(newSubstType));
    return true;
  }

  bool visitApplyInst(ApplyInst *ai) { return visitApplySite({ai}); }
  bool visitTryApplyInst(TryApplyInst *tai) { return visitApplySite({tai}); }
  bool visitBeginApplyInst(BeginApplyInst *bai) {
    return visitApplySite({bai});
  }
  bool visitPartialApplyInst(PartialApplyInst *pai) {
    return visitApplySite({pai});
  }
};

} // namespace

//===----------------------------------------------------------------------===//
//                             Top Levelish Code?
//===----------------------------------------------------------------------===//

namespace {

struct SILMoveOnlyWrappedTypeEliminator {
  SILFunction *fn;
  bool trivialOnly;

  SILMoveOnlyWrappedTypeEliminator(SILFunction *fn, bool trivialOnly)
      : fn(fn), trivialOnly(trivialOnly) {}

  bool process();
};

} // namespace

/// Returns true if this is a moveonlywrapped type whose underlying type is a
/// trivial type /or/ if this is a boxed type of that sort.
static bool isMoveOnlyWrappedTrivial(SILValue value) {
  auto *fn = value->getFunction();
  SILType type = value->getType();
  if (type.removingMoveOnlyWrapper().isTrivial(fn))
    return true;
  if (type.isBoxedMoveOnlyWrappedType(fn))
    return type.getSILBoxFieldType(fn).removingMoveOnlyWrapper().isTrivial(fn);
  return false;
}

bool SILMoveOnlyWrappedTypeEliminator::process() {
  bool madeChange = true;

  SmallSetVector<SILArgument *, 8> touchedArgs;
  SmallSetVector<SILInstruction *, 8> touchedInsts;

  for (auto &bb : *fn) {
    for (auto *arg : bb.getArguments()) {
      if (!arg->getType().isMoveOnlyWrapped() &&
          !arg->getType().isBoxedMoveOnlyWrappedType(fn))
        continue;

      // If we are looking at trivial only, skip non-trivial function args.
      if (trivialOnly &&
          !arg->getType().removingMoveOnlyWrapper().isTrivial(*fn))
        continue;

      arg->unsafelyEliminateMoveOnlyWrapper(fn);

      // If our new type is trivial, convert the arguments ownership to
      // None. Otherwise, preserve the ownership kind of the argument.
      if (arg->getType().isTrivial(*fn))
        arg->setOwnershipKind(OwnershipKind::None);
      touchedArgs.insert(arg);
      for (auto *use : arg->getNonTypeDependentUses())
        touchedInsts.insert(use->getUser());
    }

    for (auto &ii : bb) {
      for (SILValue v : ii.getResults()) {
        if (!v->getType().isMoveOnlyWrapped() &&
            !v->getType().isBoxedMoveOnlyWrappedType(fn))
          continue;

        if (trivialOnly &&
            !isMoveOnlyWrappedTrivial(v))
          continue;

        v->unsafelyEliminateMoveOnlyWrapper(fn);
        touchedInsts.insert(&ii);

        // Add all users as well. This ensures we visit things like
        // destroy_value and end_borrow.
        for (auto *use : v->getNonTypeDependentUses())
          touchedInsts.insert(use->getUser());
        madeChange = true;
      }
    }
  }

  SILMoveOnlyWrappedTypeEliminatorVisitor visitor(touchedArgs);
  while (!touchedInsts.empty()) {
    visitor.visit(touchedInsts.pop_back_val());
  }

  return madeChange;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

struct SILMoveOnlyWrappedTypeEliminatorPass : SILFunctionTransform {
  bool trivialOnly;

  SILMoveOnlyWrappedTypeEliminatorPass(bool trivialOnly)
      : SILFunctionTransform(), trivialOnly(trivialOnly) {}

  void run() override {
    auto *fn = getFunction();

    // Don't rerun on deserialized functions. We lower trivial things earlier
    // during Raw SIL.
    if (getFunction()->wasDeserializedCanonical())
      return;

    assert(fn->getModule().getStage() == SILStage::Raw &&
           "Should only run on Raw SIL");

    if (SILMoveOnlyWrappedTypeEliminator(getFunction(), trivialOnly)
            .process()) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }
  }
};

} // anonymous namespace

SILTransform *swift::createTrivialMoveOnlyTypeEliminator() {
  return new SILMoveOnlyWrappedTypeEliminatorPass(true /*trivial only*/);
}

SILTransform *swift::createMoveOnlyTypeEliminator() {
  return new SILMoveOnlyWrappedTypeEliminatorPass(false /*trivial only*/);
}
