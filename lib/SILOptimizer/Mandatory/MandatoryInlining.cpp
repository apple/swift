//===--- MandatoryInlining.cpp - Perform inlining of "transparent" sites --===//
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

#define DEBUG_TYPE "mandatory-inlining"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/Basic/BlotSetVector.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/BranchPropagatedUser.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/Devirtualize.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/StackNesting.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/ImmutableSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"

using namespace swift;

using DenseFunctionSet = llvm::DenseSet<SILFunction *>;
using ImmutableFunctionSet = llvm::ImmutableSet<SILFunction *>;

STATISTIC(NumMandatoryInlines,
          "Number of function application sites inlined by the mandatory "
          "inlining pass");

template<typename...T, typename...U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
                     U &&...args) {
  Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

/// Fixup reference counts after inlining a function call (which is a no-op
/// unless the function is a thick function).
///
/// It is important to note that, we can not assume that the partial apply, the
/// apply site, or the callee value are control dependent in any way. This
/// requires us to need to be very careful. See inline comments.
static void fixupReferenceCounts(
    PartialApplyInst *pai, FullApplySite applySite, SILValue calleeValue,
    ArrayRef<ParameterConvention> captureArgConventions,
    MutableArrayRef<SILValue> capturedArgs, bool isCalleeGuaranteed) {

  // We assume that we were passed a slice of our actual argument array. So we
  // can use this to copy if we need to.
  assert(captureArgConventions.size() == capturedArgs.size());

  SmallPtrSet<SILBasicBlock *, 8> visitedBlocks;
  // FIXME: Can we cache this in between inlining invocations?
  DeadEndBlocks deadEndBlocks(pai->getFunction());
  SmallVector<SILBasicBlock *, 4> leakingBlocks;

  auto errorBehavior = ownership::ErrorBehaviorKind::ReturnFalse;

  // Add a copy of each non-address type capture argument to lifetime extend the
  // captured argument over at least the inlined function and till the end of a
  // box if we have an address. This deals with the possibility of the closure
  // being destroyed by an earlier application and thus cause the captured
  // argument to be destroyed.
  auto loc = RegularLocation::getAutoGeneratedLocation();

  for (unsigned i : indices(captureArgConventions)) {
    auto convention = captureArgConventions[i];
    SILValue &v = capturedArgs[i];
    if (v->getType().isAddress()) {
      // FIXME: What about indirectly owned parameters? The invocation of the
      // closure would perform an indirect copy which we should mimick here.
      assert(convention != ParameterConvention::Indirect_In &&
             "Missing indirect copy");
      continue;
    }

    auto *f = applySite.getFunction();

    // See if we have a trivial value. In such a case, just continue. We do not
    // need to fix up anything.
    if (v->getType().isTrivial(*f))
      continue;

    bool hasOwnership = f->hasOwnership();

    switch (convention) {
    case ParameterConvention::Indirect_In:
    case ParameterConvention::Indirect_In_Constant:
    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Indirect_InoutAliasable:
    case ParameterConvention::Indirect_In_Guaranteed:
      llvm_unreachable("Should be handled above");

    case ParameterConvention::Direct_Guaranteed: {
      // If we have a direct_guaranteed value, the value is being taken by the
      // partial_apply at +1, but we are going to invoke the value at +0. So we
      // need to copy/borrow the value before the pai and then
      // end_borrow/destroy_value at the apply site.
      SILValue copy = SILBuilderWithScope(pai).emitCopyValueOperation(loc, v);
      SILValue argument = copy;
      if (hasOwnership) {
        argument = SILBuilderWithScope(pai).createBeginBorrow(loc, argument);
      }

      visitedBlocks.clear();

      // If we need to insert compensating destroys, do so.
      //
      // NOTE: We use pai here since in non-ossa code emitCopyValueOperation
      // returns the operand of the strong_retain which may have a ValueBase
      // that is not in the same block. An example of where this is important is
      // if we are performing emitCopyValueOperation in non-ossa code on an
      // argument when the partial_apply is not in the entrance block. In truth,
      // the linear lifetime checker does not /actually/ care what the value is
      // (ignoring diagnostic error msgs that we do not care about here), it
      // just cares about the block the value is in. In a forthcoming commit, I
      // am going to change this to use a different API on the linear lifetime
      // checker that makes this clearer.
      LinearLifetimeChecker checker(visitedBlocks, deadEndBlocks);
      auto error = checker.checkValue(
          pai, {BranchPropagatedUser(applySite.getCalleeOperand())}, {},
          errorBehavior, &leakingBlocks);
      if (error.getFoundLeak()) {
        while (!leakingBlocks.empty()) {
          auto *leakingBlock = leakingBlocks.pop_back_val();
          auto loc = RegularLocation::getAutoGeneratedLocation();
          SILBuilderWithScope builder(leakingBlock->begin());
          if (hasOwnership) {
            builder.createEndBorrow(loc, argument);
          }
          builder.emitDestroyValueOperation(loc, copy);
        }
      }

      // If we found an over consume it means that our value is consumed within
      // the loop. That means our leak code will have lifetime extended the
      // value over the loop. So we should /not/ insert a destroy after the
      // apply site. In contrast, if we do not have an over consume, we must
      // have been compensating for uses in the top of a diamond and need to
      // insert a destroy after the apply since the leak will just cover the
      // other path.
      if (!error.getFoundOverConsume()) {
        applySite.insertAfterInvocation([&](SILBasicBlock::iterator iter) {
          if (hasOwnership) {
            SILBuilderWithScope(iter).createEndBorrow(loc, argument);
          }
          SILBuilderWithScope(iter).emitDestroyValueOperation(loc, copy);
        });
      }
      v = argument;
      break;
    }

    // TODO: Do we need to lifetime extend here?
    case ParameterConvention::Direct_Unowned: {
      v = SILBuilderWithScope(pai).emitCopyValueOperation(loc, v);
      visitedBlocks.clear();

      // If we need to insert compensating destroys, do so.
      //
      // NOTE: We use pai here since in non-ossa code emitCopyValueOperation
      // returns the operand of the strong_retain which may have a ValueBase
      // that is not in the same block. An example of where this is important is
      // if we are performing emitCopyValueOperation in non-ossa code on an
      // argument when the partial_apply is not in the entrance block. In truth,
      // the linear lifetime checker does not /actually/ care what the value is
      // (ignoring diagnostic error msgs that we do not care about here), it
      // just cares about the block the value is in. In a forthcoming commit, I
      // am going to change this to use a different API on the linear lifetime
      // checker that makes this clearer.
      LinearLifetimeChecker checker(visitedBlocks, deadEndBlocks);
      auto error = checker.checkValue(pai, {applySite.getCalleeOperand()}, {},
                                      errorBehavior, &leakingBlocks);
      if (error.getFoundError()) {
        while (!leakingBlocks.empty()) {
          auto *leakingBlock = leakingBlocks.pop_back_val();
          auto loc = RegularLocation::getAutoGeneratedLocation();
          SILBuilderWithScope builder(leakingBlock->begin());
          builder.emitDestroyValueOperation(loc, v);
        }
      }

      applySite.insertAfterInvocation([&](SILBasicBlock::iterator iter) {
        SILBuilderWithScope(iter).emitDestroyValueOperation(loc, v);
      });
      break;
    }

    // If we have an owned value, we insert a copy here for two reasons:
    //
    // 1. To balance the consuming argument.
    // 2. To lifetime extend the value over the call site in case our partial
    // apply has another use that would destroy our value first.
    case ParameterConvention::Direct_Owned: {
      v = SILBuilderWithScope(pai).emitCopyValueOperation(loc, v);
      visitedBlocks.clear();

      // If we need to insert compensating destroys, do so.
      //
      // NOTE: We use pai here since in non-ossa code emitCopyValueOperation
      // returns the operand of the strong_retain which may have a ValueBase
      // that is not in the same block. An example of where this is important is
      // if we are performing emitCopyValueOperation in non-ossa code on an
      // argument when the partial_apply is not in the entrance block. In truth,
      // the linear lifetime checker does not /actually/ care what the value is
      // (ignoring diagnostic error msgs that we do not care about here), it
      // just cares about the block the value is in. In a forthcoming commit, I
      // am going to change this to use a different API on the linear lifetime
      // checker that makes this clearer.
      LinearLifetimeChecker checker(visitedBlocks, deadEndBlocks);
      auto error = checker.checkValue(pai, {applySite.getCalleeOperand()}, {},
                                      errorBehavior, &leakingBlocks);
      if (error.getFoundError()) {
        while (!leakingBlocks.empty()) {
          auto *leakingBlock = leakingBlocks.pop_back_val();
          auto loc = RegularLocation::getAutoGeneratedLocation();
          SILBuilderWithScope builder(leakingBlock->begin());
          builder.emitDestroyValueOperation(loc, v);
        }
      }

      break;
    }
    }
  }

  // Destroy the callee as the apply would have done if our function is not
  // callee guaranteed.
  if (!isCalleeGuaranteed) {
    applySite.insertAfterInvocation([&](SILBasicBlock::iterator iter) {
      SILBuilderWithScope(iter).emitDestroyValueOperation(loc, calleeValue);
    });
  }
}

static void collectPartiallyAppliedArguments(
    PartialApplyInst *PAI,
    SmallVectorImpl<ParameterConvention> &CapturedArgConventions,
    SmallVectorImpl<SILValue> &FullArgs) {
  ApplySite Site(PAI);
  SILFunctionConventions CalleeConv(Site.getSubstCalleeType(),
                                    PAI->getModule());
  for (auto &Arg : PAI->getArgumentOperands()) {
    unsigned CalleeArgumentIndex = Site.getCalleeArgIndex(Arg);
    assert(CalleeArgumentIndex >= CalleeConv.getSILArgIndexOfFirstParam());
    auto ParamInfo = CalleeConv.getParamInfoForSILArg(CalleeArgumentIndex);
    CapturedArgConventions.push_back(ParamInfo.getConvention());
    FullArgs.push_back(Arg.get());
  }
}

static SILValue getLoadedCalleeValue(LoadInst *li) {
  auto *pbi = dyn_cast<ProjectBoxInst>(li->getOperand());
  if (!pbi)
    return SILValue();

  auto *abi = dyn_cast<AllocBoxInst>(pbi->getOperand());
  if (!abi)
    return SILValue();

  PointerUnion<StrongReleaseInst *, DestroyValueInst *> destroy =
      static_cast<StrongReleaseInst *>(nullptr);

  // Look through uses of the alloc box the load is loading from to find up to
  // one store and up to one destroy.
  for (auto *use : abi->getUses()) {
    auto *user = use->getUser();

    // Look for our single destroy. If we find it... continue.
    if (destroy.isNull()) {
      if (auto *sri = dyn_cast<StrongReleaseInst>(user)) {
        destroy = sri;
        continue;
      }

      if (auto *dvi = dyn_cast<DestroyValueInst>(user)) {
        destroy = dvi;
        continue;
      }
    }

    // Ignore our pbi if we find one.
    if (user == pbi)
      continue;

    // Otherwise, we have something that we do not understand. Return
    // SILValue().
    //
    // NOTE: We purposely allow for strong_retain, retain_value, copy_value to
    // go down this path since we only want to consider simple boxes that have a
    // single post-dominating destroy. So if we have a strong_retain,
    // retain_value, or copy_value, we want to bail.
    return SILValue();
  }

  // Make sure that our project_box has a single store user and our load user.
  StoreInst *si = nullptr;
  for (Operand *use : pbi->getUses()) {
    // If this use is our load... continue.
    if (use->getUser() == li)
      continue;

    // Otherwise, see if we have a store...
    if (auto *useSI = dyn_cast_or_null<StoreInst>(use->getUser())) {
      // If we already have a store, we have a value that is initialized
      // multiple times... bail.
      if (si)
        return SILValue();

      // If we do not have a store yet, make sure that it is in the same basic
      // block as box. Otherwise bail.
      if (useSI->getParent() != abi->getParent())
        return SILValue();

      // Ok, we found a store in the same block as the box and for which we have
      // so far only found one. Stash the store.
      si = useSI;
      continue;
    }

    // Otherwise, we have something we do not support... bail.
    return SILValue();
  }

  // If we did not find a store, bail.
  if (!si)
    return SILValue();

  // Otherwise, we have found our callee... the source of our store.
  return si->getSrc();
}

/// Returns the callee SILFunction called at a call site, in the case
/// that the call is transparent (as in, both that the call is marked
/// with the transparent flag and that callee function is actually transparently
/// determinable from the SIL) or nullptr otherwise. This assumes that the SIL
/// is already in SSA form.
///
/// In the case that a non-null value is returned, FullArgs contains effective
/// argument operands for the callee function.
static SILFunction *
getCalleeFunction(SILFunction *F, FullApplySite AI, bool &IsThick,
                  SmallVectorImpl<ParameterConvention> &CapturedArgConventions,
                  SmallVectorImpl<SILValue> &FullArgs,
                  PartialApplyInst *&PartialApply) {
  IsThick = false;
  PartialApply = nullptr;
  CapturedArgConventions.clear();
  FullArgs.clear();

  // First grab our basic arguments from our apply.
  for (const auto &Arg : AI.getArguments())
    FullArgs.push_back(Arg);

  // Then grab a first approximation of our apply by stripping off all copy
  // operations.
  SILValue CalleeValue = stripCopiesAndBorrows(AI.getCallee());

  // If after stripping off copy_values, we have a load then see if we the
  // function we want to inline has a simple available value through a simple
  // alloc_box. Bail otherwise.
  if (auto *li = dyn_cast<LoadInst>(CalleeValue)) {
    CalleeValue = getLoadedCalleeValue(li);
    if (!CalleeValue)
      return nullptr;
    CalleeValue = stripCopiesAndBorrows(CalleeValue);
  }

  // PartialApply/ThinToThick -> ConvertFunction patterns are generated
  // by @noescape closures.
  //
  // FIXME: We don't currently handle mismatched return types, however, this
  // would be a good optimization to handle and would be as simple as inserting
  // a cast.
  auto skipFuncConvert = [](SILValue CalleeValue) {
    // Skip any copies that we see.
    CalleeValue = stripCopiesAndBorrows(CalleeValue);

    // We can also allow a thin @escape to noescape conversion as such:
    // %1 = function_ref @thin_closure_impl : $@convention(thin) () -> ()
    // %2 = convert_function %1 :
    //      $@convention(thin) () -> () to $@convention(thin) @noescape () -> ()
    // %3 = thin_to_thick_function %2 :
    //  $@convention(thin) @noescape () -> () to
    //            $@noescape @callee_guaranteed () -> ()
    // %4 = apply %3() : $@noescape @callee_guaranteed () -> ()
    if (auto *ThinToNoescapeCast = dyn_cast<ConvertFunctionInst>(CalleeValue)) {
      auto FromCalleeTy =
          ThinToNoescapeCast->getOperand()->getType().castTo<SILFunctionType>();
      if (FromCalleeTy->getExtInfo().hasContext())
        return CalleeValue;
      auto ToCalleeTy = ThinToNoescapeCast->getType().castTo<SILFunctionType>();
      auto EscapingCalleeTy = ToCalleeTy->getWithExtInfo(
          ToCalleeTy->getExtInfo().withNoEscape(false));
      if (FromCalleeTy != EscapingCalleeTy)
        return CalleeValue;
      return stripCopiesAndBorrows(ThinToNoescapeCast->getOperand());
    }

    // Ignore mark_dependence users. A partial_apply [stack] uses them to mark
    // the dependence of the trivial closure context value on the captured
    // arguments.
    if (auto *MD = dyn_cast<MarkDependenceInst>(CalleeValue)) {
      while (MD) {
        CalleeValue = MD->getValue();
        MD = dyn_cast<MarkDependenceInst>(CalleeValue);
      }
      return CalleeValue;
    }

    auto *CFI = dyn_cast<ConvertEscapeToNoEscapeInst>(CalleeValue);
    if (!CFI)
      return stripCopiesAndBorrows(CalleeValue);

    // TODO: Handle argument conversion. All the code in this file needs to be
    // cleaned up and generalized. The argument conversion handling in
    // optimizeApplyOfConvertFunctionInst should apply to any combine
    // involving an apply, not just a specific pattern.
    //
    // For now, just handle conversion that doesn't affect argument types,
    // return types, or throws. We could trivially handle any other
    // representation change, but the only one that doesn't affect the ABI and
    // matters here is @noescape, so just check for that.
    auto FromCalleeTy = CFI->getOperand()->getType().castTo<SILFunctionType>();
    auto ToCalleeTy = CFI->getType().castTo<SILFunctionType>();
    auto EscapingCalleeTy =
      ToCalleeTy->getWithExtInfo(ToCalleeTy->getExtInfo().withNoEscape(false));
    if (FromCalleeTy != EscapingCalleeTy)
      return stripCopiesAndBorrows(CalleeValue);

    return stripCopiesAndBorrows(CFI->getOperand());
  };

  // Look through a escape to @noescape conversion.
  CalleeValue = skipFuncConvert(CalleeValue);

  // We are allowed to see through exactly one "partial apply" instruction or
  // one "thin to thick function" instructions, since those are the patterns
  // generated when using auto closures.
  if (auto *PAI = dyn_cast<PartialApplyInst>(CalleeValue)) {
    // Collect the applied arguments and their convention.
    collectPartiallyAppliedArguments(PAI, CapturedArgConventions, FullArgs);

    CalleeValue = stripCopiesAndBorrows(PAI->getCallee());
    IsThick = true;
    PartialApply = PAI;
  } else if (auto *TTTFI = dyn_cast<ThinToThickFunctionInst>(CalleeValue)) {
    CalleeValue = stripCopiesAndBorrows(TTTFI->getOperand());
    IsThick = true;
  }

  CalleeValue = skipFuncConvert(CalleeValue);

  auto *FRI = dyn_cast<FunctionRefInst>(CalleeValue);
  if (!FRI)
    return nullptr;

  SILFunction *CalleeFunction = FRI->getReferencedFunctionOrNull();

  switch (CalleeFunction->getRepresentation()) {
  case SILFunctionTypeRepresentation::Thick:
  case SILFunctionTypeRepresentation::Thin:
  case SILFunctionTypeRepresentation::Method:
  case SILFunctionTypeRepresentation::Closure:
  case SILFunctionTypeRepresentation::WitnessMethod:
    break;
    
  case SILFunctionTypeRepresentation::CFunctionPointer:
  case SILFunctionTypeRepresentation::ObjCMethod:
  case SILFunctionTypeRepresentation::Block:
    return nullptr;
  }

  // If the CalleeFunction is a not-transparent definition, we can not process
  // it.
  if (CalleeFunction->isTransparent() == IsNotTransparent)
    return nullptr;

  // If CalleeFunction is a declaration, see if we can load it.
  if (CalleeFunction->empty())
    AI.getModule().loadFunction(CalleeFunction);

  // If we fail to load it, bail.
  if (CalleeFunction->empty())
    return nullptr;

  if (F->isSerialized() &&
      !CalleeFunction->hasValidLinkageForFragileInline()) {
    if (!CalleeFunction->hasValidLinkageForFragileRef()) {
      llvm::errs() << "caller: " << F->getName() << "\n";
      llvm::errs() << "callee: " << CalleeFunction->getName() << "\n";
      llvm_unreachable("Should never be inlining a resilient function into "
                       "a fragile function");
    }
    return nullptr;
  }

  return CalleeFunction;
}

static SILInstruction *tryDevirtualizeApplyHelper(FullApplySite InnerAI,
                                                  ClassHierarchyAnalysis *CHA) {
  auto NewInst = tryDevirtualizeApply(InnerAI, CHA);
  if (!NewInst)
    return InnerAI.getInstruction();

  deleteDevirtualizedApply(InnerAI);

  // FIXME: Comments at the use of this helper indicate that devirtualization
  // may return SILArgument. Yet here we assert that it must return an
  // instruction.
  auto newApplyAI = NewInst.getInstruction();
  assert(newApplyAI && "devirtualized but removed apply site?");

  return newApplyAI;
}

/// Inlines all mandatory inlined functions into the body of a function,
/// first recursively inlining all mandatory apply instructions in those
/// functions into their bodies if necessary.
///
/// \param F the function to be processed
/// \param AI nullptr if this is being called from the top level; the relevant
///   ApplyInst requiring the recursive call when non-null
/// \param FullyInlinedSet the set of all functions already known to be fully
///   processed, to avoid processing them over again
/// \param SetFactory an instance of ImmutableFunctionSet::Factory
/// \param CurrentInliningSet the set of functions currently being inlined in
///   the current call stack of recursive calls
///
/// \returns true if successful, false if failed due to circular inlining.
static bool
runOnFunctionRecursively(SILOptFunctionBuilder &FuncBuilder,
			 SILFunction *F, FullApplySite AI,
                         DenseFunctionSet &FullyInlinedSet,
                         ImmutableFunctionSet::Factory &SetFactory,
                         ImmutableFunctionSet CurrentInliningSet,
                         ClassHierarchyAnalysis *CHA) {
  // Avoid reprocessing functions needlessly.
  if (FullyInlinedSet.count(F))
    return true;

  // Prevent attempt to circularly inline.
  if (CurrentInliningSet.contains(F)) {
    // This cannot happen on a top-level call, so AI should be non-null.
    assert(AI && "Cannot have circular inline without apply");
    SILLocation L = AI.getLoc();
    assert(L && "Must have location for transparent inline apply");
    diagnose(F->getModule().getASTContext(), L.getStartSourceLoc(),
             diag::circular_transparent);
    return false;
  }

  // Add to the current inlining set (immutably, so we only affect the set
  // during this call and recursive subcalls).
  CurrentInliningSet = SetFactory.add(CurrentInliningSet, F);

  SmallVector<ParameterConvention, 16> CapturedArgConventions;
  SmallVector<SILValue, 32> FullArgs;
  bool needUpdateStackNesting = false;

  // Visiting blocks in reverse order avoids revisiting instructions after block
  // splitting, which would be quadratic.
  for (auto BI = F->rbegin(), BE = F->rend(), nextBB = BI; BI != BE;
       BI = nextBB) {
    // After inlining, the block iterator will be adjusted to point to the last
    // block containing inlined instructions. This way, the inlined function
    // body will be reprocessed within the caller's context without revisiting
    // any original instructions.
    nextBB = std::next(BI);

    // While iterating over this block, instructions are inserted and deleted.
    // To avoid quadratic block splitting, instructions must be processed in
    // reverse order (block splitting reassigned the parent pointer of all
    // instructions below the split point).
    for (auto II = BI->rbegin(); II != BI->rend(); ++II) {
      FullApplySite InnerAI = FullApplySite::isa(&*II);
      if (!InnerAI)
        continue;

      // *NOTE* If devirtualization succeeds, devirtInst may not be InnerAI,
      // but a casted result of InnerAI or even a block argument due to
      // abstraction changes when calling the witness or class method.
      auto *devirtInst = tryDevirtualizeApplyHelper(InnerAI, CHA);
      // Restore II to the current apply site.
      II = devirtInst->getReverseIterator();
      // If the devirtualized call result is no longer a invalid FullApplySite,
      // then it has succeeded, but the result is not immediately inlinable.
      InnerAI = FullApplySite::isa(devirtInst);
      if (!InnerAI)
        continue;

      SILValue CalleeValue = InnerAI.getCallee();
      bool IsThick;
      PartialApplyInst *PAI;
      SILFunction *CalleeFunction = getCalleeFunction(
          F, InnerAI, IsThick, CapturedArgConventions, FullArgs, PAI);

      if (!CalleeFunction)
        continue;

      // Then recursively process it first before trying to inline it.
      if (!runOnFunctionRecursively(FuncBuilder, CalleeFunction, InnerAI,
                                    FullyInlinedSet, SetFactory,
                                    CurrentInliningSet, CHA)) {
        // If we failed due to circular inlining, then emit some notes to
        // trace back the failure if we have more information.
        // FIXME: possibly it could be worth recovering and attempting other
        // inlines within this same recursive call rather than simply
        // propagating the failure.
        if (AI) {
          SILLocation L = AI.getLoc();
          assert(L && "Must have location for transparent inline apply");
          diagnose(F->getModule().getASTContext(), L.getStartSourceLoc(),
                   diag::note_while_inlining);
        }
        return false;
      }

      // Get our list of substitutions.
      auto Subs = (PAI
                   ? PAI->getSubstitutionMap()
                   : InnerAI.getSubstitutionMap());

      SILOpenedArchetypesTracker OpenedArchetypesTracker(F);
      F->getModule().registerDeleteNotificationHandler(
          &OpenedArchetypesTracker);
      // The callee only needs to know about opened archetypes used in
      // the substitution list.
      OpenedArchetypesTracker.registerUsedOpenedArchetypes(
          InnerAI.getInstruction());
      if (PAI) {
        OpenedArchetypesTracker.registerUsedOpenedArchetypes(PAI);
      }

      SILInliner Inliner(FuncBuilder, SILInliner::InlineKind::MandatoryInline,
                         Subs, OpenedArchetypesTracker);
      if (!Inliner.canInlineApplySite(InnerAI))
        continue;

      // Inline function at I, which also changes I to refer to the first
      // instruction inlined in the case that it succeeds. We purposely
      // process the inlined body after inlining, because the inlining may
      // have exposed new inlining opportunities beyond those present in
      // the inlined function when processed independently.
      LLVM_DEBUG(llvm::errs() << "Inlining @" << CalleeFunction->getName()
                              << " into @" << InnerAI.getFunction()->getName()
                              << "\n");

      // If we intend to inline a partial_apply function that is not on the
      // stack, then we need to balance the reference counts for correctness.
      //
      // NOTE: If our partial apply is on the stack, it only has point uses (and
      // hopefully eventually guaranteed) uses of the captured arguments.
      //
      // NOTE: If we have a thin_to_thick_function, we do not need to worry
      // about such things since a thin_to_thick_function does not capture any
      // arguments.
      if (PAI && PAI->isOnStack() == PartialApplyInst::NotOnStack) {
        bool IsCalleeGuaranteed =
            PAI->getType().castTo<SILFunctionType>()->isCalleeGuaranteed();
        auto CapturedArgs = MutableArrayRef<SILValue>(FullArgs).take_back(
            CapturedArgConventions.size());
        // We need to insert the copies before the partial_apply since if we can
        // not remove the partial_apply the captured values will be dead by the
        // time we hit the call site.
        fixupReferenceCounts(PAI, InnerAI, CalleeValue, CapturedArgConventions,
                             CapturedArgs, IsCalleeGuaranteed);
      }

      needUpdateStackNesting |= Inliner.needsUpdateStackNesting(InnerAI);

      // Inlining deletes the apply, and can introduce multiple new basic
      // blocks. After this, CalleeValue and other instructions may be invalid.
      // nextBB will point to the last inlined block
      auto firstInlinedInstAndLastBB =
          Inliner.inlineFunction(CalleeFunction, InnerAI, FullArgs);
      nextBB = firstInlinedInstAndLastBB.second->getReverseIterator();
      ++NumMandatoryInlines;

      // The IR is now valid, and trivial dead arguments are removed. However,
      // we may be able to remove dead callee computations (e.g. dead
      // partial_apply closures). Those will be removed with mandatory combine.

      // Resume inlining within nextBB, which contains only the inlined
      // instructions and possibly instructions in the original call block that
      // have not yet been visited.
      break;
    }
  }

  if (needUpdateStackNesting) {
    StackNesting().correctStackNesting(F);
  }

  // Keep track of full inlined functions so we don't waste time recursively
  // reprocessing them.
  FullyInlinedSet.insert(F);
  return true;
}

//===----------------------------------------------------------------------===//
//                          Top Level Driver
//===----------------------------------------------------------------------===//

namespace {

class MandatoryInlining : public SILModuleTransform {
  /// The entry point to the transformation.
  void run() override {
    ClassHierarchyAnalysis *CHA = getAnalysis<ClassHierarchyAnalysis>();
    SILModule *M = getModule();
    bool ShouldCleanup = !getOptions().DebugSerialization;
    bool SILVerifyAll = getOptions().VerifyAll;
    DenseFunctionSet FullyInlinedSet;
    ImmutableFunctionSet::Factory SetFactory;

    SILOptFunctionBuilder FuncBuilder(*this);
    for (auto &F : *M) {
      // Don't inline into thunks, even transparent callees.
      if (F.isThunk())
        continue;

      // Skip deserialized functions.
      if (F.wasDeserializedCanonical())
        continue;

      runOnFunctionRecursively(FuncBuilder, &F,
                               FullApplySite(), FullyInlinedSet, SetFactory,
                               SetFactory.getEmptySet(), CHA);

      // The inliner splits blocks at call sites. Re-merge trivial branches
      // to reestablish a canonical CFG.
      mergeBasicBlocks(&F);

      // If we are asked to perform SIL verify all, perform that now so that we
      // can discover the immediate inlining trigger of the problematic
      // function.
      if (SILVerifyAll) {
        F.verify();
      }
    }

    if (!ShouldCleanup)
      return;

    // Now that we've inlined some functions, clean up.  If there are any
    // transparent functions that are deserialized from another module that are
    // now unused, just remove them from the module.
    //
    // We do this with a simple linear scan, because transparent functions that
    // reference each other have already been flattened.
    for (auto FI = M->begin(), E = M->end(); FI != E; ) {
      SILFunction &F = *FI++;

      invalidateAnalysis(&F, SILAnalysis::InvalidationKind::Everything);

      if (F.getRefCount() != 0) continue;

      // Leave non-transparent functions alone.
      if (!F.isTransparent())
        continue;

      // We discard functions that don't have external linkage,
      // e.g. deserialized functions, internal functions, and thunks.
      // Being marked transparent controls this.
      if (F.isPossiblyUsedExternally()) continue;

      // ObjC functions are called through the runtime and are therefore alive
      // even if not referenced inside SIL.
      if (F.getRepresentation() == SILFunctionTypeRepresentation::ObjCMethod)
        continue;

      // Okay, just erase the function from the module.
      FuncBuilder.eraseFunction(&F);
    }
  }

};
} // end anonymous namespace

SILTransform *swift::createMandatoryInlining() {
  return new MandatoryInlining();
}
