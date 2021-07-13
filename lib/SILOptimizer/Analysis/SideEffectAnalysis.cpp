//===--- SideEffectAnalysis.cpp - SIL Side Effect Analysis ----------------===//
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
#include "swift/SILOptimizer/Analysis/SideEffectAnalysis.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SILOptimizer/Analysis/AccessedStorageAnalysis.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/Analysis/FunctionOrder.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"

using namespace swift;

// -----------------------------------------------------------------------------
// GenericFunctionEffectAnalysis
// -----------------------------------------------------------------------------

template <typename FunctionEffects>
void GenericFunctionEffectAnalysis<FunctionEffects>::initialize(
    SILPassManager *PM) {
  BCA = PM->getAnalysis<BasicCalleeAnalysis>();
}

template <typename FunctionEffects>
void GenericFunctionEffectAnalysis<FunctionEffects>::invalidate() {
  functionInfoMap.clear();
  allocator.DestroyAll();
  LLVM_DEBUG(llvm::dbgs() << "invalidate all\n");
}

template <typename FunctionEffects>
void GenericFunctionEffectAnalysis<FunctionEffects>::invalidate(
    SILFunction *F, InvalidationKind K) {
  if (FunctionInfo *FInfo = functionInfoMap.lookup(F)) {
    LLVM_DEBUG(llvm::dbgs() << "  invalidate " << FInfo->F->getName() << '\n');
    invalidateIncludingAllCallers(FInfo);
  }
}

template <typename FunctionEffects>
void GenericFunctionEffectAnalysis<FunctionEffects>::getCalleeEffects(
    FunctionEffects &calleeEffects, FullApplySite fullApply) {
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

template <typename FunctionEffects>
void GenericFunctionEffectAnalysis<FunctionEffects>::analyzeFunction(
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

template <typename FunctionEffects>
void GenericFunctionEffectAnalysis<FunctionEffects>::analyzeCall(
    FunctionInfo *functionInfo, FullApplySite fullApply,
    FunctionOrder &bottomUpOrder, int recursionDepth) {

  FunctionEffects applyEffects;
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

template <typename FunctionEffects>
void GenericFunctionEffectAnalysis<FunctionEffects>::recompute(
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

// Instantiate template members.
template class swift::GenericFunctionEffectAnalysis<FunctionSideEffects>;
template class swift::GenericFunctionEffectAnalysis<FunctionAccessedStorage>;

// -----------------------------------------------------------------------------
// FunctionSideEffects
// -----------------------------------------------------------------------------

using MemoryBehavior = SILInstruction::MemoryBehavior;

MemoryBehavior
FunctionSideEffects::getMemBehavior(RetainObserveKind ScanKind) const {

  bool Observe = (ScanKind == RetainObserveKind::ObserveRetains);
  if ((Observe && mayAllocObjects()) || mayReadRC())
    return MemoryBehavior::MayHaveSideEffects;
  
  // Start with the global effects.
  auto Behavior = GlobalEffects.getMemBehavior(ScanKind);
  
  // Add effects from the parameters.
  for (auto &ParamEffect : ParamEffects) {
    MemoryBehavior ArgBehavior = ParamEffect.getMemBehavior(ScanKind);

    Behavior = combineMemoryBehavior(Behavior, ArgBehavior);

    // Stop the scan if we've reached the highest level of side effect.
    if (Behavior == MemoryBehavior::MayHaveSideEffects)
      break;
  }
  return Behavior;
}

MemoryBehavior
FunctionSideEffects::getArgumentBehavior(FullApplySite applySite,
                                         unsigned argIdx) {
  // The overall argument effect is the combination of the argument and the
  // global effects.
  MemoryBehavior behavior =
    GlobalEffects.getMemBehavior(RetainObserveKind::IgnoreRetains);
  MemoryBehavior argBehavior =
    ParamEffects[argIdx].getMemBehavior(RetainObserveKind::IgnoreRetains);

  behavior = combineMemoryBehavior(behavior, argBehavior);

  if (behavior > MemoryBehavior::MayRead &&
      applySite.getArgumentConvention(applySite.getArgumentRef(argIdx)) ==
        SILArgumentConvention::Indirect_In_Guaranteed) {
    // Even if side-effect analysis doesn't know anything about the called
    // called function, the in_guaranteed convention guarantees that the
    // argument is never written to.
    return MemoryBehavior::MayRead;
  }
  return behavior;
}

bool FunctionSideEffects::mergeFrom(const FunctionSideEffects &RHS) {
  bool Changed = mergeFlags(RHS);
  Changed |= GlobalEffects.mergeFrom(RHS.GlobalEffects);
  Changed |= LocalEffects.mergeFrom(RHS.LocalEffects);
  // In case of an external function, the RHS may have 0 arguments.
  unsigned NumArgs = RHS.ParamEffects.size();
  for (unsigned Idx = 0; Idx < NumArgs; Idx++) {
    // In case of a partial_apply, the RHS (= callee) may have more arguments
    // than the apply instruction.
    if (Idx < ParamEffects.size()) {
      Changed |= ParamEffects[Idx].mergeFrom(RHS.ParamEffects[Idx]);
    } else {
      Changed |= GlobalEffects.mergeFrom(RHS.ParamEffects[Idx]);
    }
  }
  return Changed;
}

bool FunctionSideEffects::mergeFromApply(
    const FunctionSideEffects &ApplyEffects, FullApplySite FAS) {
  bool Changed = mergeFlags(ApplyEffects);
  Changed |= GlobalEffects.mergeFrom(ApplyEffects.GlobalEffects);
  unsigned numCallerArgs = FAS.getNumArguments();
  unsigned numCalleeArgs = ApplyEffects.ParamEffects.size();
  assert(numCalleeArgs >= numCallerArgs);
  for (unsigned Idx = 0; Idx < numCalleeArgs; Idx++) {
    // Map the callee argument effects to parameters of this function.
    // If there are more callee parameters than arguments it means that the
    // callee is the result of a partial_apply.
    FunctionSideEffectFlags *E = (Idx < numCallerArgs
                                  ? getEffectsOn(FAS.getArgument(Idx))
                                  : &GlobalEffects);
    Changed |= E->mergeFrom(ApplyEffects.ParamEffects[Idx]);
  }
  return Changed;
}

void FunctionSideEffects::dump() const { llvm::errs() << *this << '\n'; }

static SILValue skipAddrProjections(SILValue V) {
  for (;;) {
    switch (V->getKind()) {
      case ValueKind::IndexAddrInst:
      case ValueKind::IndexRawPointerInst:
      case ValueKind::StructElementAddrInst:
      case ValueKind::TupleElementAddrInst:
      case ValueKind::RefElementAddrInst:
      case ValueKind::RefTailAddrInst:
      case ValueKind::ProjectBoxInst:
      case ValueKind::UncheckedTakeEnumDataAddrInst:
      case ValueKind::PointerToAddressInst:
        V = cast<SingleValueInstruction>(V)->getOperand(0);
        break;
      default:
        return V;
    }
  }
  llvm_unreachable("there is no escape from an infinite loop");
}

static SILValue skipValueProjections(SILValue V) {
  for (;;) {
    switch (V->getKind()) {
      case ValueKind::StructExtractInst:
      case ValueKind::TupleExtractInst:
      case ValueKind::UncheckedEnumDataInst:
      case ValueKind::UncheckedTrivialBitCastInst:
      case ValueKind::UncheckedRefCastInst:
        V = cast<SingleValueInstruction>(V)->getOperand(0);
        break;
      default:
        return V;
    }
  }
  llvm_unreachable("there is no escape from an infinite loop");
}

FunctionSideEffectFlags *FunctionSideEffects::getEffectsOn(SILValue Addr) {
  SILValue BaseAddr = skipValueProjections(skipAddrProjections(Addr));
  switch (BaseAddr->getKind()) {
  case swift::ValueKind::SILFunctionArgument: {
    // Can we associate the address to a function parameter?
    auto *Arg = cast<SILFunctionArgument>(BaseAddr);
    return &ParamEffects[Arg->getIndex()];
    break;
    }
    case ValueKind::AllocStackInst:
    case ValueKind::AllocRefInst:
    case ValueKind::AllocRefDynamicInst:
    case ValueKind::AllocBoxInst:
      // Effects on locally allocated storage.
      return &LocalEffects;
    default:
      break;
  }
  // Everything else.
  return &GlobalEffects;
}

// Return true if the given function has defined effects that were successfully
// recorded in this FunctionSideEffects object.
bool FunctionSideEffects::setDefinedEffects(SILFunction *F) {
  if (F->hasSemanticsAttr(SEMANTICS_PROGRAMTERMINATION_POINT)) {
    Traps = true;
    return true;
  }
  switch (F->getEffectsKind()) {
    case EffectsKind::ReleaseNone:
      GlobalEffects.Reads = true;
      GlobalEffects.Writes = true;
      GlobalEffects.Releases = false;
      return true;
    case EffectsKind::ReadNone:
      return true;
    case EffectsKind::ReadOnly:
      for (auto i :
           range(F->getLoweredFunctionType()->getNumIndirectFormalResults())) {
        ParamEffects[i].Writes = true;
      }
      // @_effects(readonly) is worthless if we have owned parameters, because
      // the release inside the callee may call a deinit, which itself can do
      // anything.
      if (!F->hasOwnedParameters()) {
        GlobalEffects.Reads = true;
        return true;
      }
      break;
    default:
      break;
  }

  return false;
}

// Return true if this function's effects have been fully summarized in this
// FunctionSideEffects object without visiting its body.
bool FunctionSideEffects::summarizeFunction(SILFunction *F) {
  assert(ParamEffects.empty() && "Expect uninitialized effects.");

  if (F->isDynamicallyReplaceable()) {
    LLVM_DEBUG(llvm::dbgs()
               << "  -- is dynamically_replaceable " << F->getName() << '\n');
    setWorstEffects();
    return true;
  }

  if (!F->empty())
    ParamEffects.resize(F->getArguments().size());

  // Handle @_effects attributes
  if (setDefinedEffects(F)) {
    LLVM_DEBUG(llvm::dbgs() << "  -- has defined effects " << F->getName()
                            << '\n');
    return true;
  }

  if (!F->isDefinition()) {
    // We can't assume anything about external functions.
    LLVM_DEBUG(llvm::dbgs() << "  -- is external " << F->getName() << '\n');
    setWorstEffects();
    return true;
  }
  return false;
}

// Return true if the side effects of this semantic call are fully known without
// visiting the callee and have been recorded in this FunctionSideEffects
// object.
bool FunctionSideEffects::setSemanticEffects(ArraySemanticsCall ASC) {
  assert(ASC.hasSelf());
  auto &SelfEffects = ParamEffects[ParamEffects.size() - 1];

  // Currently we only handle array semantics.
  // TODO: also handle other semantic functions.
  
  switch (ASC.getKind()) {
    case ArrayCallKind::kGetCount:
    case ArrayCallKind::kGetCapacity:
      if (!ASC.mayHaveBridgedObjectElementType()) {
        SelfEffects.Reads = true;
        SelfEffects.Releases |= !ASC.hasGuaranteedSelf();
        return true;
      }
      return false;

    case ArrayCallKind::kCheckSubscript:
    case ArrayCallKind::kCheckIndex:
      if (!ASC.mayHaveBridgedObjectElementType()) {
        SelfEffects.Reads = true;
        SelfEffects.Releases |= !ASC.hasGuaranteedSelf();
        Traps = true;
        return true;
      }
      return false;

    case ArrayCallKind::kGetElement:
      if (!ASC.mayHaveBridgedObjectElementType()) {
        SelfEffects.Reads = true;
        SelfEffects.Releases |= !ASC.hasGuaranteedSelf();
        for (auto i : range(((ApplyInst *)ASC)
                                ->getOrigCalleeConv()
                                .getNumIndirectSILResults())) {
          assert(!ASC.hasGetElementDirectResult());
          ParamEffects[i].Writes = true;
        }
        return true;
      }
      return false;

    case ArrayCallKind::kArrayPropsIsNativeTypeChecked:
      SelfEffects.Releases |= !ASC.hasGuaranteedSelf();
      // The isNative checks evaluate to a constant (no read!) if the array
      // cannot be bridged.
      if (ASC.mayHaveBridgedObjectElementType())
        SelfEffects.Reads = true;
      return true;

    case ArrayCallKind::kGetElementAddress:
      SelfEffects.Reads = true;
      SelfEffects.Releases |= !ASC.hasGuaranteedSelf();
      return true;

    case ArrayCallKind::kMakeMutable:
      if (!ASC.mayHaveBridgedObjectElementType()) {
        SelfEffects.Writes = true;
        GlobalEffects.Releases = true;
        AllocsObjects = true;
        ReadsRC = true;
        return true;
      }
      return false;

    default:
      return false;
  }
}

// Summarize the callee side effects of a call instruction using this
// FunctionSideEffects object without analyzing the callee function bodies or
// scheduling the callees for bottom-up propagation.
//
// Return true if this call-site's effects are summarized without visiting the
// callee.
bool FunctionSideEffects::summarizeCall(FullApplySite fullApply) {
  assert(ParamEffects.empty() && "Expect uninitialized effects.");
  ParamEffects.resize(fullApply.getNumArguments());

  // Is this a call to a semantics function?
  if (auto apply = dyn_cast<ApplyInst>(fullApply.getInstruction())) {
    ArraySemanticsCall ASC(apply);
    if (ASC && ASC.hasSelf()) {
      if (setSemanticEffects(ASC))
        return true;
    }
  }

  if (SILFunction *SingleCallee = fullApply.getReferencedFunctionOrNull()) {
    // Does the function have any @_effects?
    if (setDefinedEffects(SingleCallee))
      return true;
  }
  return false;
}

void FunctionSideEffects::analyzeInstruction(SILInstruction *I) {
  // Handle some kind of instructions specially.
  switch (I->getKind()) {
  case SILInstructionKind::FixLifetimeInst:
    // A fix_lifetime instruction acts like a read on the operand. Retains can
    // move after it but the last release can't move before it.
    getEffectsOn(I->getOperand(0))->Reads = true;
    return;
  case SILInstructionKind::AllocStackInst:
  case SILInstructionKind::DeallocStackInst:
    return;
#define UNCHECKED_REF_CAST(Name, ...)                                          \
  case SILInstructionKind::Name##RetainValueInst:                              \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  case SILInstructionKind::Name##RetainInst:                                   \
  case SILInstructionKind::StrongRetain##Name##Inst:                           \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::StrongRetainInst:
  case SILInstructionKind::RetainValueInst:
    getEffectsOn(I->getOperand(0))->Retains = true;
    return;
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  case SILInstructionKind::Name##ReleaseValueInst:
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case SILInstructionKind::Name##ReleaseInst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::StrongReleaseInst:
  case SILInstructionKind::ReleaseValueInst:
    getEffectsOn(I->getOperand(0))->Releases = true;
    return;
  case SILInstructionKind::UnconditionalCheckedCastInst:
    getEffectsOn(cast<UnconditionalCheckedCastInst>(I)->getOperand())->Reads =
        true;
    Traps = true;
    return;
  case SILInstructionKind::LoadBorrowInst: {
    auto *effects = getEffectsOn(cast<LoadBorrowInst>(I)->getOperand());
    effects->Reads = true;
    return;
  }
  case SILInstructionKind::LoadInst: {
    auto *li = cast<LoadInst>(I);
    auto *effects = getEffectsOn(cast<LoadInst>(I)->getOperand());
    effects->Reads = true;
    if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Take)
      effects->Writes = true;
    if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Copy)
      effects->Retains = true;
    return;
  }
  case SILInstructionKind::StoreInst: {
    auto *si = cast<StoreInst>(I);
    auto *effects = getEffectsOn(si->getDest());
    effects->Writes = true;
    if (si->getOwnershipQualifier() == StoreOwnershipQualifier::Assign)
      effects->Releases = true;
    return;
  }
  case SILInstructionKind::CondFailInst:
    Traps = true;
    return;
  case SILInstructionKind::PartialApplyInst: {
    AllocsObjects = true;
    auto *PAI = cast<PartialApplyInst>(I);
    auto Args = PAI->getArguments();
    auto Params = PAI->getSubstCalleeType()->getParameters();
    Params = Params.slice(Params.size() - Args.size(), Args.size());
    for (unsigned Idx : indices(Args)) {
      if (isIndirectFormalParameter(Params[Idx].getConvention()))
        getEffectsOn(Args[Idx])->Reads = true;
    }
    return;
  }
  case SILInstructionKind::BuiltinInst: {
    auto *BInst = cast<BuiltinInst>(I);
    auto &BI = BInst->getBuiltinInfo();
    switch (BI.ID) {
    case BuiltinValueKind::IsUnique:
      // TODO: derive this information in a more general way, e.g. add it
      // to Builtins.def
      ReadsRC = true;
      break;
    case BuiltinValueKind::CondUnreachable:
      Traps = true;
      return;
    default:
      break;
    }
    const IntrinsicInfo &IInfo = BInst->getIntrinsicInfo();
    if (IInfo.ID == llvm::Intrinsic::trap) {
      Traps = true;
      return;
    }
    // Detailed memory effects of builtins are handled below by checking the
    // memory behavior of the instruction.
    break;
  }
  default:
    break;
  }

  if (isa<AllocationInst>(I)) {
    // Excluding AllocStackInst (which is handled above).
    AllocsObjects = true;
  }
  
  // Check the general memory behavior for instructions we didn't handle above.
  switch (I->getMemoryBehavior()) {
  case MemoryBehavior::None:
    break;
  case MemoryBehavior::MayRead:
    GlobalEffects.Reads = true;
    break;
  case MemoryBehavior::MayWrite:
    GlobalEffects.Writes = true;
    break;
  case MemoryBehavior::MayReadWrite:
    GlobalEffects.Reads = true;
    GlobalEffects.Writes = true;
    break;
  case MemoryBehavior::MayHaveSideEffects:
    setWorstEffects();
    break;
  }
  if (I->mayTrap())
    Traps = true;
}

SILAnalysis *swift::createSideEffectAnalysis(SILModule *M) {
  return new SideEffectAnalysis();
}
