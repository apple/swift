//===--- ClosureLifetimeFixup.cpp - Fixup the lifetime of closures --------===//
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

#define DEBUG_TYPE "closure-lifetime-fixup"

#include "swift/Basic/Defer.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/SILSSAUpdater.h"
#include "swift/SILOptimizer/Utils/StackNesting.h"

#include "llvm/Support/CommandLine.h"

llvm::cl::opt<bool> DisableConvertEscapeToNoEscapeSwitchEnumPeephole(
    "sil-disable-convert-escape-to-noescape-switch-peephole",
    llvm::cl::init(false),
    llvm::cl::desc(
        "Disable the convert_escape_to_noescape switch enum peephole. "),
    llvm::cl::Hidden);

using namespace swift;

/// Given an optional diamond, return the bottom of the diamond.
///
/// That is given that sei is in bb0,
///
///   /---> bb1 ---\
///  /              \
/// bb0              ---> bb3
///  \              /
///   \---> bb2 ---/
///
/// this routine will return bb3.
static SILBasicBlock *getOptionalDiamondSuccessor(SwitchEnumInst *sei) {
  auto numSuccs = sei->getNumSuccessors();
  if (numSuccs != 2)
    return nullptr;
  auto *succSome = sei->getCase(0).second;
  auto *succNone = sei->getCase(1).second;
  if (succSome->args_size() != 1)
    std::swap(succSome, succNone);

  if (succSome->args_size() != 1 || succNone->args_size() != 0)
    return nullptr;

  auto *succ = succSome->getSingleSuccessorBlock();
  if (!succ)
    return nullptr;

  if (succNone == succ)
    return succ;

  succNone = succNone->getSingleSuccessorBlock();
  if (succNone == succ)
    return succ;

  if (succNone == nullptr)
    return nullptr;

  succNone = succNone->getSingleSuccessorBlock();
  if (succNone == succ)
    return succ;

  return nullptr;
}

/// Find a safe insertion point for closure destruction. We might create a
/// closure that captures self in deinit of self. In this situation it is not
/// safe to destroy the closure after we called super deinit. We have to place
/// the closure destruction before that call.
///
///  %deinit = objc_super_method %0 : $C, #A.deinit!deallocator.foreign
///  %super = upcast %0 : $C to $A
///  apply %deinit(%super) : $@convention(objc_method) (A) -> ()
///  end_lifetime %super : $A
static SILInstruction *getDeinitSafeClosureDestructionPoint(SILBasicBlock *bb) {
  for (auto &i : llvm::reverse(*bb)) {
    if (auto *endLifetime = dyn_cast<EndLifetimeInst>(&i)) {
      auto *superInstance = endLifetime->getOperand()->getDefiningInstruction();
      assert(superInstance && "Expected an instruction");
      return superInstance;
    }
  }
  return bb->getTerminator();
}

static void findReachableExitBlocks(SILInstruction *i,
                                    SmallVectorImpl<SILBasicBlock *> &result) {
  BasicBlockWorklist worklist(i->getParent());

  while (SILBasicBlock *bb = worklist.pop()) {
    if (bb->getTerminator()->isFunctionExiting()) {
      result.push_back(bb);
      continue;
    }
    for (SILBasicBlock *succ : bb->getSuccessors()) {
      worklist.pushIfNotVisited(succ);
    }
  }
}

/// We use this to ensure that we properly handle recursive cases by revisiting
/// phi nodes that we are tracking. This just makes it easier to reproduce in a
/// test case.
static llvm::cl::opt<bool> ReverseInitialWorklist(
    "sil-closure-lifetime-fixup-reverse-phi-order", llvm::cl::init(false),
    llvm::cl::desc(
        "Reverse the order in which we visit phis for testing purposes"),
    llvm::cl::Hidden);

// Finally, we need to prune phis inserted by the SSA updater that
// only take the .none from the entry block. This means that they are
// not actually reachable from the .some() so we know that we do not
// need to lifetime extend there at all. As an additional benefit, we
// eliminate the need to balance these arguments to satisfy the
// ownership verifier. This occurs since arguments are a place in SIL
// where the trivialness of an enums case is erased.
static void
cleanupDeadTrivialPhiArgs(SILValue initialValue,
                          SmallVectorImpl<SILPhiArgument *> &insertedPhis) {
  // Just for testing purposes.
  if (ReverseInitialWorklist) {
    std::reverse(insertedPhis.begin(), insertedPhis.end());
  }
  SmallVector<SILArgument *, 8> worklist(insertedPhis.begin(),
                                         insertedPhis.end());
  sortUnique(insertedPhis);
  SmallVector<SILValue, 8> incomingValues;

  while (!worklist.empty()) {
    // Clear the incoming values array after each iteration.
    SWIFT_DEFER { incomingValues.clear(); };

    auto *phi = worklist.pop_back_val();
    {
      auto it = lower_bound(insertedPhis, phi);
      if (it == insertedPhis.end() || *it != phi)
        continue;
    }

    // TODO: When we split true phi arguments from transformational terminators,
    // this will always succeed and the assert can go away.
    bool foundPhiValues = phi->getIncomingPhiValues(incomingValues);
    (void)foundPhiValues;
    assert(foundPhiValues && "Should always have 'true' phi arguments since "
                             "these were inserted by the SSA updater.");
    if (llvm::any_of(incomingValues,
                     [&](SILValue v) { return v != initialValue; }))
      continue;

    // Remove it from our insertedPhis list to prevent us from re-visiting this.
    {
      auto it = lower_bound(insertedPhis, phi);
      assert((it != insertedPhis.end() && *it == phi) &&
             "Should have found the phi");
      insertedPhis.erase(it);
    }

    // See if any of our users are branch or cond_br. If so, we may have
    // exposed additional unneeded phis. Add it back to the worklist in such a
    // case.
    for (auto *op : phi->getUses()) {
      auto *user = op->getUser();

      if (!isa<BranchInst>(user) && !isa<CondBranchInst>(user))
        continue;

      auto *termInst = cast<TermInst>(user);
      for (auto succBlockArgList : termInst->getSuccessorBlockArgumentLists()) {
        llvm::copy_if(succBlockArgList, std::back_inserter(worklist),
                      [&](SILArgument *succArg) -> bool {
                        auto it = lower_bound(insertedPhis, succArg);
                        return it != insertedPhis.end() && *it == succArg;
                      });
      }
    }

    // Then RAUW the phi with the entryBlockOptionalNone and erase the
    // argument.
    phi->replaceAllUsesWith(initialValue);
    erasePhiArgument(phi->getParent(), phi->getIndex());
  }
}

/// Extend the lifetime of the convert_escape_to_noescape's operand to the end
/// of the function.
///
/// NOTE: Since we are lifetime extending a copy that we have introduced, we do
/// not need to consider destroy_value emitted by SILGen unlike
/// copy_block_without_escaping which consumes its sentinel parameter. Unlike
/// that case where we have to consider that destroy_value, we have a simpler
/// time here.
static void extendLifetimeToEndOfFunction(SILFunction &fn,
                                          ConvertEscapeToNoEscapeInst *cvt,
                                          SILSSAUpdater &updater) {
  auto escapingClosure = cvt->getOperand();
  auto escapingClosureTy = escapingClosure->getType();
  auto optionalEscapingClosureTy = SILType::getOptionalType(escapingClosureTy);
  auto loc = RegularLocation::getAutoGeneratedLocation();

  // If our Cvt is in the initial block, we do not need to use the SSA updater
  // since we know Cvt can not be in a loop and must dominate all exits
  // (*). Just insert a copy of the escaping closure at the Cvt and destroys at
  // the exit blocks of the function.
  //
  // (*) In fact we can't use the SILSSAUpdater::GetValueInMiddleOfBlock.
  if (cvt->getParent() == cvt->getFunction()->getEntryBlock()) {
    auto *innerCVI =
        SILBuilderWithScope(cvt).createCopyValue(loc, escapingClosure);
    cvt->setLifetimeGuaranteed();
    cvt->setOperand(innerCVI);
    SmallVector<SILBasicBlock *, 4> exitingBlocks;
    fn.findExitingBlocks(exitingBlocks);

    for (auto *block : exitingBlocks) {
      auto *safeDestructPoint = getDeinitSafeClosureDestructionPoint(block);
      SILBuilderWithScope(safeDestructPoint).createDestroyValue(loc, innerCVI);
    }
    return;
  }

  // Ok. At this point we know that Cvt is not in the entry block... so we can
  // use SILSSAUpdater::GetValueInMiddleOfBlock() to extend the object's
  // lifetime respecting loops.
  SmallVector<SILPhiArgument *, 8> insertedPhis;
  updater.setInsertedPhis(&insertedPhis);
  updater.initialize(optionalEscapingClosureTy, fn.hasOwnership()
                                                    ? OwnershipKind::Owned
                                                    : OwnershipKind::None);

  // Create an Optional<() -> ()>.none in the entry block of the function and
  // add it as an available value to the SSAUpdater.
  //
  // Since we know that Cvt is not in the entry block and this must be, we know
  // that it is safe to use the SSAUpdater's getValueInMiddleOfBlock with this
  // value.
  SILValue entryBlockOptionalNone = [&]() -> SILValue {
    SILBuilderWithScope b(fn.getEntryBlock()->begin());
    return b.createOptionalNone(loc, optionalEscapingClosureTy);
  }();
  updater.addAvailableValue(fn.getEntryBlock(), entryBlockOptionalNone);

  // Create a copy of the convert_escape_to_no_escape and add it as an available
  // value to the SSA updater.
  //
  // NOTE: The SSAUpdater does not support providing multiple values in the same
  // block without extra work. So the fact that Cvt is not in the entry block
  // means that we don't have to worry about overwriting the .none value.
  auto *cvi = [&]() -> CopyValueInst * {
    auto *innerCVI =
        SILBuilderWithScope(cvt).createCopyValue(loc, escapingClosure);
    cvt->setLifetimeGuaranteed();
    cvt->setOperand(innerCVI);
    SILBuilderWithScope b(std::next(cvt->getIterator()));
    updater.addAvailableValue(
        cvt->getParent(),
        b.createOptionalSome(loc, innerCVI, optionalEscapingClosureTy));
    return innerCVI;
  }();

  // Then we use the SSA updater to insert a destroy_value before the cvt and at
  // the reachable exit blocks.
  SmallVector<SILBasicBlock *, 4> exitingBlocks;
  findReachableExitBlocks(cvt, exitingBlocks);

  {
    // Before the copy value, insert an extra destroy_value to handle
    // loops. Since we used our enum value this is safe.
    SILValue v = updater.getValueInMiddleOfBlock(cvi->getParent());
    SILBuilderWithScope(cvi).createDestroyValue(loc, v);
  }

  for (auto *block : exitingBlocks) {
    auto *safeDestructionPt = getDeinitSafeClosureDestructionPoint(block);
    SILValue v = updater.getValueAtEndOfBlock(block);
    SILBuilderWithScope(safeDestructionPt).createDestroyValue(loc, v);
  }

  // Finally, we need to prune phis inserted by the SSA updater that only take
  // the .none from the entry block.
  //
  // TODO: Should we sort inserted phis before or after we initialize
  // the worklist or maybe backwards? We should investigate how the
  // SSA updater adds phi nodes to this list to resolve this question.
  cleanupDeadTrivialPhiArgs(entryBlockOptionalNone, insertedPhis);
}

static SILInstruction *lookThroughRebastractionUsers(
    SILInstruction *inst,
    llvm::DenseMap<SILInstruction *, SILInstruction *> &memoized) {
  if (inst == nullptr)
    return nullptr;

  // Try a cached lookup.
  auto res = memoized.find(inst);
  if (res != memoized.end())
    return res->second;

  // Cache recursive results.
  auto memoizeResult = [&](SILInstruction *from, SILInstruction *toResult) {
    memoized[from] = toResult;
    return toResult;
  };
  
  auto getSingleNonDebugNonRefCountUser =
    [](SILValue v) -> SILInstruction* {
      SILInstruction *singleNonDebugNonRefCountUser = nullptr;
      for (auto *use : getNonDebugUses(v)) {
        auto *user = use->getUser();
        if (onlyAffectsRefCount(user))
          continue;
        if (singleNonDebugNonRefCountUser) {
          return nullptr;
        }
        singleNonDebugNonRefCountUser = user;
      }
      return singleNonDebugNonRefCountUser;
    };

  // If we have a convert_function, just look at its user.
  if (auto *cvt = dyn_cast<ConvertFunctionInst>(inst))
    return memoizeResult(inst, lookThroughRebastractionUsers(
                             getSingleNonDebugNonRefCountUser(cvt), memoized));
  if (auto *cvt = dyn_cast<ConvertEscapeToNoEscapeInst>(inst))
    return memoizeResult(inst, lookThroughRebastractionUsers(
                             getSingleNonDebugNonRefCountUser(cvt), memoized));

  // If we have a partial_apply user look at its single (non release) user.
  if (auto *pa = dyn_cast<PartialApplyInst>(inst))
    return memoizeResult(inst, lookThroughRebastractionUsers(
                             getSingleNonDebugNonRefCountUser(pa), memoized));

  return inst;
}

/// Insert a mark_dependence for any non-trivial argument of a partial_apply.
static SILValue insertMarkDependenceForCapturedArguments(PartialApplyInst *pai,
                                                         SILBuilder &b) {
  SILValue curr(pai);
  // Mark dependence on all non-trivial arguments.
  for (auto &arg : pai->getArgumentOperands()) {
    if (arg.get()->getType().isTrivial(*pai->getFunction()))
      continue;
    curr = b.createMarkDependence(pai->getLoc(), curr, arg.get());
  }

  return curr;
}

/// Returns the (single) "endAsyncLetLifetime" builtin if \p startAsyncLet is a
/// "startAsyncLetWithLocalBuffer" builtin.
static BuiltinInst *getEndAsyncLet(BuiltinInst *startAsyncLet) {
  if (startAsyncLet->getBuiltinKind() != BuiltinValueKind::StartAsyncLetWithLocalBuffer)
    return nullptr;

  BuiltinInst *endAsyncLet = nullptr;
  for (Operand *op : startAsyncLet->getUses()) {
    auto *endBI = dyn_cast<BuiltinInst>(op->getUser());
    if (endBI && endBI->getBuiltinKind() == BuiltinValueKind::EndAsyncLetLifetime) {
      // At this stage of the pipeline, it's always the case that a
      // startAsyncLet has an endAsyncLet: that's how SILGen generates it.
      // Just to be on the safe side, do this check.
      if (endAsyncLet)
        return nullptr;
      endAsyncLet = endBI;
    }
  }
  return endAsyncLet;
}

/// Call the \p insertFn with a builder at all insertion points after
/// a closure is used by \p closureUser.
static void insertAfterClosureUser(SILInstruction *closureUser,
                                   function_ref<void(SILBuilder &)> insertFn) {
  // Don't insert any destroy or deallocation right before an unreachable.
  // It's not needed an will only add up to code size.
  auto insertAtNonUnreachable = [&](SILBuilder &builder) {
    if (isa<UnreachableInst>(builder.getInsertionPoint()))
      return;
    insertFn(builder);
  };

  if (auto *startAsyncLet = dyn_cast<BuiltinInst>(closureUser)) {
    BuiltinInst *endAsyncLet = getEndAsyncLet(startAsyncLet);
    if (!endAsyncLet)
      return;
    SILBuilderWithScope builder(std::next(endAsyncLet->getIterator()));
    insertAtNonUnreachable(builder);
    return;
  }
  FullApplySite fas = FullApplySite::isa(closureUser);
  assert(fas);
  fas.insertAfterFullEvaluation(insertAtNonUnreachable);
}

static SILValue skipConvert(SILValue v) {
  auto *cvt = dyn_cast<ConvertFunctionInst>(v);
  if (!cvt)
    return v;
  auto *pa = dyn_cast<PartialApplyInst>(cvt->getOperand());
  if (!pa || !pa->hasOneUse())
    return v;
  return pa;
}

/// Rewrite a partial_apply convert_escape_to_noescape sequence with a single
/// apply/try_apply user to a partial_apply [stack] terminated with a
/// dealloc_stack placed after the apply.
///
///   %p = partial_apply %f(%a, %b)
///   %ne = convert_escape_to_noescape %p
///   apply %f2(%p)
///   destroy_value %p
///
///    =>
///
///   %p = partial_apply [stack] %f(%a, %b)
///   %md = mark_dependence %p on %a
///   %md2 = mark_dependence %md on %b
///   apply %f2(%md2)
///   dealloc_stack %p
///   destroy_value %a
///   destroy_value %b
///
/// Note: If the rewrite succeeded we have inserted a dealloc_stack. This
/// dealloc_stack still needs to be balanced with other dealloc_stacks i.e the
/// caller needs to use the StackNesting utility to update the dealloc_stack
/// nesting.
static SILValue tryRewriteToPartialApplyStack(
    ConvertEscapeToNoEscapeInst *cvt,
    SILInstruction *closureUser, InstructionDeleter &deleter,
    llvm::DenseMap<SILInstruction *, SILInstruction *> &memoized) {

  auto *origPA = dyn_cast<PartialApplyInst>(skipConvert(cvt->getOperand()));
  if (!origPA)
    return SILValue();

  auto *convertOrPartialApply = cast<SingleValueInstruction>(origPA);
  if (cvt->getOperand() != origPA)
    convertOrPartialApply = cast<ConvertFunctionInst>(cvt->getOperand());

  // Whenever we delete an instruction advance the iterator and remove the
  // instruction from the memoized map.
  auto saveDeleteInst = [&](SILInstruction *i) {
    memoized.erase(i);
    deleter.forceDelete(i);
  };

  // Look for a single non ref count user of the partial_apply.
  SmallVector<SILInstruction *, 8> refCountInsts;
  SILInstruction *singleNonDebugNonRefCountUser = nullptr;
  for (auto *use : getNonDebugUses(convertOrPartialApply)) {
    auto *user = use->getUser();
    if (onlyAffectsRefCount(user)) {
      refCountInsts.push_back(user);
      continue;
    }
    if (singleNonDebugNonRefCountUser)
      return SILValue();
    singleNonDebugNonRefCountUser = user;
  }
  
  SILBuilderWithScope b(cvt);

  // The convert_escape_to_noescape is the only user of the partial_apply.
  // Convert to a partial_apply [stack].
  SmallVector<SILValue, 8> args;
  for (auto &arg : origPA->getArgumentOperands())
    args.push_back(arg.get());
  auto newPA = b.createPartialApply(
      origPA->getLoc(), origPA->getCallee(), origPA->getSubstitutionMap(), args,
      origPA->getType().getAs<SILFunctionType>()->getCalleeConvention(),
      PartialApplyInst::OnStackKind::OnStack);

  // Insert mark_dependence for any non-trivial operand to the partial_apply.
  auto closure = insertMarkDependenceForCapturedArguments(newPA, b);

  // Optionally, replace the convert_function instruction.
  if (auto *convert = dyn_cast<ConvertFunctionInst>(convertOrPartialApply)) {
    auto origTy = convert->getType().castTo<SILFunctionType>();
    auto origWithNoEscape = SILType::getPrimitiveObjectType(
        origTy->getWithExtInfo(origTy->getExtInfo().withNoEscape()));
    closure = b.createConvertFunction(convert->getLoc(), closure,
                                      origWithNoEscape, false);
    convert->replaceAllUsesWith(closure);
  }

  // Replace the convert_escape_to_noescape uses with the new
  // partial_apply [stack].
  cvt->replaceAllUsesWith(closure);
  saveDeleteInst(cvt);

  // Delete the ref count operations on the original partial_apply.
  for (auto *refInst : refCountInsts)
    saveDeleteInst(refInst);
  convertOrPartialApply->replaceAllUsesWith(newPA);
  if (convertOrPartialApply != origPA)
    saveDeleteInst(convertOrPartialApply);
  saveDeleteInst(origPA);

  ApplySite site(newPA);
  SILFunctionConventions calleeConv(site.getSubstCalleeType(),
                                      newPA->getModule());

  // Since we create temporary allocation for in_guaranteed captures during SILGen,
  // the dealloc_stack of it can occur before the apply due to conversion scopes.
  // When we insert destroy_addr of the in_guaranteed capture after the apply,
  // we may end up with a situation when the dealloc_stack occurs before the destroy_addr.
  // The code below proactively removes the dealloc_stack of in_guaranteed capture,
  // so that it can be reinserted at the correct place after the destroy_addr below.
  for (auto &arg : newPA->getArgumentOperands()) {
    unsigned calleeArgumentIndex = site.getCalleeArgIndex(arg);
    assert(calleeArgumentIndex >= calleeConv.getSILArgIndexOfFirstParam());
    auto paramInfo = calleeConv.getParamInfoForSILArg(calleeArgumentIndex);
    if (paramInfo.getConvention() == ParameterConvention::Indirect_In_Guaranteed) {
      // go over all the dealloc_stack, remove it
      SmallVector<Operand*, 16> Uses(arg.get()->getUses());
      for (auto use : Uses)
        if (auto *deallocInst = dyn_cast<DeallocStackInst>(use->getUser()))
          deleter.forceDelete(deallocInst);
    }
  }

  // Insert destroys of arguments after the closure user and the dealloc_stack.
  insertAfterClosureUser(closureUser, [newPA](SILBuilder &builder) {
    auto loc = RegularLocation(builder.getInsertionPointLoc());
    builder.createDeallocStack(loc, newPA);
    insertDestroyOfCapturedArguments(newPA, builder);
    // dealloc_stack of the in_guaranteed capture is inserted
    insertDeallocOfCapturedArguments(newPA, builder);
  });
  return closure;
}

static bool tryExtendLifetimeToLastUse(
    ConvertEscapeToNoEscapeInst *cvt,
    llvm::DenseMap<SILInstruction *, SILInstruction *> &memoized,
    InstructionDeleter &deleter) {
  // If there is a single user that is an apply this is simple: extend the
  // lifetime of the operand until after the apply.
  auto *singleUser = lookThroughRebastractionUsers(cvt, memoized);
  if (!singleUser)
    return false;

  // Handle apply instructions and startAsyncLet.
  BuiltinInst *endAsyncLet = nullptr;
  if (FullApplySite::isa(singleUser)) {
    // TODO: Enable begin_apply/end_apply. It should work, but is not tested yet.
    if (isa<BeginApplyInst>(singleUser))
      return false;
  } else if (auto *bi = dyn_cast<BuiltinInst>(singleUser)) {
    endAsyncLet = getEndAsyncLet(bi);
    if (!endAsyncLet)
      return false;
  } else {
    return false;
  }

  if (SILValue closure = tryRewriteToPartialApplyStack(cvt, singleUser,
                                                       deleter, memoized)) {
    if (auto *cfi = dyn_cast<ConvertFunctionInst>(closure))
      closure = cfi->getOperand();
    if (endAsyncLet && isa<MarkDependenceInst>(closure)) {
      // Add the top-level mark_dependence (which keeps the closure arguments
      // alive) as a second operand to the endAsyncLet builtin.
      // This ensures that the closure arguments are kept alive until the
      // endAsyncLet builtin.
      assert(endAsyncLet->getNumOperands() == 1);
      SILBuilderWithScope builder(endAsyncLet);
      builder.createBuiltin(endAsyncLet->getLoc(), endAsyncLet->getName(),
        endAsyncLet->getType(), endAsyncLet->getSubstitutions(),
        {endAsyncLet->getOperand(0), closure});
      deleter.forceDelete(endAsyncLet);
    }
    return true;
  }

  // Insert a copy at the convert_escape_to_noescape [not_guaranteed] and
  // change the instruction to the guaranteed form.
  auto escapingClosure = cvt->getOperand();
  auto *closureCopy =
      SILBuilderWithScope(cvt).createCopyValue(cvt->getLoc(), escapingClosure);
  cvt->setLifetimeGuaranteed();
  cvt->setOperand(closureCopy);

  insertAfterClosureUser(singleUser, [closureCopy](SILBuilder &builder) {
    auto loc = RegularLocation(builder.getInsertionPointLoc());
    builder.createDestroyValue(loc, closureCopy);
  });
  return true;
}

/// Ensure the lifetime of the closure across a two step optional conversion
/// from:
///
///   optional<@escaping () -> ()>
///
/// to:
///
///   optional<@noescape () -> ()>
///
/// to:
///
///   optional<@noescape @convention(block) () -> ()>
///
/// and all uses of the block. The pattern that we are looking for is:
///
///                            switch_enum %optional_closure           (1)
///                           /           \
///   %trivial_closure = CVT %closure     nil                          (2)
///                           \           /
///                          switch_enum %optional_trivial_closure     (3)
///                           /            \
/// %b = convertToBlock %trivial_closure   nil                         (4)
///                           \            /
///                   ... uses of %optional_block ...
///                    destroy_value %optional_block
///
/// where CVT is convert_escape_to_no_escape [not_guaranteed]. We assume that
/// the %optional_block is going through a conversion sequence in SILGen meaning
/// that we should only have a single destroy of the optional block.
///
/// NOTE: There is a *lifetime gap* during the usage of the trivial_closure!
/// This means we must be careful when lifetime extending. We can only assume
/// that the underlying closure is alive immediately at the CVT. So to perform
/// our lifetime extend, we do the following:
///
/// 1. We copy and borrow optional_closure, right before the switch_enum in
/// (1).
///
/// 2. We rewrite the convert_escape_to_no_escape guaranteed to use the copy
/// instead.
///
/// 3. To make sure that even after ossa is complete, we do not move any
/// destroys above the convert_escape_to_no_escape by putting a mark_dependence
/// on %closure
///
/// 4. We insert an end_borrow, destroy for the copy at the destroy of the
/// optional block.
static bool trySwitchEnumPeephole(ConvertEscapeToNoEscapeInst *cvt) {
  auto *blockArg = dyn_cast<SILArgument>(cvt->getOperand());
  if (!blockArg)
    return false;
  auto *convertSuccessorBlock = cvt->getParent()->getSingleSuccessorBlock();
  if (!convertSuccessorBlock)
    return false;
  auto *predBB = cvt->getParent()->getSinglePredecessorBlock();
  if (!predBB)
    return false;
  auto *switchEnum1 = dyn_cast<SwitchEnumInst>(predBB->getTerminator());
  if (!switchEnum1)
    return false;
  auto *diamondSucc = getOptionalDiamondSuccessor(switchEnum1);
  if (!diamondSucc)
    return false;
  auto *switchEnum2 = dyn_cast<SwitchEnumInst>(diamondSucc->getTerminator());
  if (!switchEnum2)
    return false;
  auto *diamondSucc2 = getOptionalDiamondSuccessor(switchEnum2);
  if (!diamondSucc2)
    return false;
  if (diamondSucc2->getNumArguments() != 1)
    return false;

  // Look for the last and only destroy of the diamond succ 2's argument. This
  // is going to be the place where we destroy the lifetime extending copy.
  SILInstruction *onlyDestroy = [&]() -> SILInstruction * {
    SILInstruction *lastDestroy = nullptr;
    for (auto *use : diamondSucc2->getArgument(0)->getUses()) {
      SILInstruction *user = use->getUser();
      if (isa<ReleaseValueInst>(user) || isa<StrongReleaseInst>(user) ||
          isa<DestroyValueInst>(user)) {
        if (lastDestroy)
          return nullptr;
        lastDestroy = user;
      }
    }
    return lastDestroy;
  }();
  if (!onlyDestroy)
    return false;

  // Extend the lifetime.
  auto loc = RegularLocation::getAutoGeneratedLocation();
  SILValue copy, borrow;
  std::tie(copy, borrow) = ([&]() -> std::pair<SILValue, SILValue> {
    SILBuilderWithScope builder(switchEnum1);
    auto copy = builder.emitCopyValueOperation(loc, switchEnum1->getOperand());
    auto borrow = builder.emitBeginBorrowOperation(loc, copy);
    return {copy, borrow};
  })(); // end std::tie(copy, borrow).

  {
    SILBuilderWithScope builder(cvt);
    auto value = builder.emitExtractOptionalPayloadOperation(loc, borrow);
    cvt->setOperand(value);
    cvt->setLifetimeGuaranteed();
  }

  {
    SILBuilderWithScope builder(onlyDestroy);
    builder.emitEndBorrowOperation(loc, borrow);
    builder.emitDestroyValueOperation(loc, copy);
  }

  return true;
}

/// Look for a single destroy user and possibly unowned apply uses.
static SILInstruction *getOnlyDestroy(CopyBlockWithoutEscapingInst *cb) {
  SILInstruction *onlyDestroy = nullptr;

  for (auto *use : getNonDebugUses(cb)) {
    SILInstruction *inst = use->getUser();

    // If this an apply use, only handle unowned parameters.
    if (auto apply = FullApplySite::isa(inst)) {
      SILArgumentConvention conv = apply.getArgumentConvention(*use);
      if (conv != SILArgumentConvention::Direct_Unowned)
        return nullptr;
      continue;
    }

    // We have already seen one destroy.
    if (onlyDestroy)
      return nullptr;

    if (isa<DestroyValueInst>(inst) || isa<ReleaseValueInst>(inst) ||
        isa<StrongReleaseInst>(inst)) {
      onlyDestroy = inst;
      continue;
    }

    // Some other instruction.
    return nullptr;
  }

  if (!onlyDestroy)
    return nullptr;

  // Now look at whether the dealloc_stack or the destroy postdominates and
  // return the post dominator.
  auto *blockInit = dyn_cast<InitBlockStorageHeaderInst>(cb->getBlock());
  if (!blockInit)
    return nullptr;

  auto *asi = dyn_cast<AllocStackInst>(blockInit->getBlockStorage());
  if (!asi)
    return nullptr;
  auto *dealloc = asi->getSingleDeallocStack();
  if (!dealloc || dealloc->getParent() != onlyDestroy->getParent())
    return nullptr;

  // Return the later instruction.
  for (auto it = SILBasicBlock::iterator(onlyDestroy),
            ie = dealloc->getParent()->end();
       it != ie; ++it) {
    if (&*it == dealloc)
      return dealloc;
  }
  return onlyDestroy;
}

/// Lower a copy_block_without_escaping instruction.
///
///    This involves replacing:
///
///      %copy = copy_block_without_escaping %block withoutEscaping %closure
///
///      ...
///      destroy_value %copy
///
///    by (roughly) the instruction sequence:
///
///      %copy = copy_block %block
///
///      ...
///      destroy_value %copy
///      %e = is_escaping %closure
///      cond_fail %e
///      destroy_value %closure
static bool fixupCopyBlockWithoutEscaping(CopyBlockWithoutEscapingInst *cb,
                                          InstructionDeleter &deleter,
                                          bool &modifiedCFG) {
  // Find the end of the lifetime of the copy_block_without_escaping
  // instruction.
  auto &fn = *cb->getFunction();

  // If we find a single destroy, this destroy is going to be a destroy that may
  // be in the same block as CB. It is important that we make sure that the
  // destroy is in a different block than CB or any terminating blocks to ensure
  // that we can use the SSAUpdater if needed.
  auto *singleDestroy = getOnlyDestroy(cb);
  if (singleDestroy && singleDestroy->getParent() == cb->getParent()) {
    modifiedCFG = true;
    {
      SILBuilderWithScope b(singleDestroy);
      splitBasicBlockAndBranch(b, singleDestroy, nullptr, nullptr);
    }

    {
      SILBuilderWithScope b(singleDestroy);
      auto *term = singleDestroy->getParent()->getTerminator();
      if (term->isFunctionExiting()) {
        splitBasicBlockAndBranch(b, &*std::next(singleDestroy->getIterator()),
                                 nullptr, nullptr);
      }
    }
  }

  auto sentinelClosure = cb->getClosure();
  auto loc = cb->getLoc();

  // At this point, we transform our copy_block_without_escaping into a
  // copy_block. This has a few important implications:
  //
  // 1. copy_block_without_escaping takes the sentinel value at +1. We will need
  //    to balance that +1.
  // 2. The destroy_value associated with the copy_block_without_escaping will
  //    be on the copy_block value.
  SILBuilderWithScope b(cb);
  auto *newCB = b.createCopyBlock(loc, cb->getBlock());
  cb->replaceAllUsesWith(newCB);
  deleter.forceDelete(cb);

  auto autoGenLoc = RegularLocation::getAutoGeneratedLocation();

  // If CB is in the entry block, we know that our definition of SentinelClosure
  // must be as well. Thus we know that we do not need to worry about loops or
  // dominance issues and can just insert destroy_values for the sentinel at the
  // lifetime end points.
  if (newCB->getParent() == newCB->getFunction()->getEntryBlock()) {
    // Our single destroy must not be in the entry block since if so, we would
    // have inserted an edge to appease the SSA updater.
    if (singleDestroy) {
      SILBuilderWithScope b(std::next(singleDestroy->getIterator()));
      SILValue v = sentinelClosure;
      SILValue isEscaping = b.createIsEscapingClosure(
          loc, v, IsEscapingClosureInst::ObjCEscaping);
      b.createCondFail(loc, isEscaping, "non-escaping closure has escaped");
      b.createDestroyValue(loc, v);
      return true;
    }

    // If we couldn't find a specific destroy_value, lifetime extend to the end
    // of the function.
    SmallVector<SILBasicBlock *, 4> ExitingBlocks;
    fn.findExitingBlocks(ExitingBlocks);
    for (auto *Block : ExitingBlocks) {
      SILBuilderWithScope B(Block->getTerminator());
      SILValue V = sentinelClosure;
      SILValue isEscaping = B.createIsEscapingClosure(
          loc, V, IsEscapingClosureInst::ObjCEscaping);
      B.createCondFail(loc, isEscaping, "non-escaping closure has escaped");
      B.createDestroyValue(loc, V);
    }

    return true;
  }

  // Otherwise, we need to be more careful since we can have loops and may not
  // transitively dominate all uses of the closure. So we:
  //
  // 1. Create an Optional<T>.none at the entry.
  // 2. Create a destroy_value(val), val = Optional<T>.some(sentinel) in the cvt
  // block.
  // 3. Create a destroy_value at all exits of the value.
  //
  // and then use the SSAUpdater to ensure that we handle loops correctly.
  auto optionalEscapingClosureTy =
      SILType::getOptionalType(sentinelClosure->getType());

  SmallVector<SILPhiArgument *, 8> insertedPhis;
  SILSSAUpdater updater(&insertedPhis);
  updater.initialize(optionalEscapingClosureTy, fn.hasOwnership()
                                                    ? OwnershipKind::Owned
                                                    : OwnershipKind::None);

  // Create the Optional.none as the beginning available value.
  SILValue entryBlockOptionalNone;
  {
    SILBuilderWithScope b(fn.getEntryBlock()->begin());
    entryBlockOptionalNone =
        b.createOptionalNone(autoGenLoc, optionalEscapingClosureTy);
    updater.addAvailableValue(fn.getEntryBlock(), entryBlockOptionalNone);
  }
  assert(entryBlockOptionalNone);

  // Then create the Optional.some(closure sentinel).
  //
  // NOTE: We return the appropriate insertion point to insert the destroy_value
  // before the value (to ensure we handle loops). We need to get all available
  // values first though.
  auto *initialValue = [&]() -> EnumInst * {
    SILBuilderWithScope b(newCB);
    // Create the closure sentinel (the copy_block_without_escaping closure
    // operand consumed at +1, so we don't need a copy) to it.
    auto *result = b.createOptionalSome(autoGenLoc, sentinelClosure,
                                        optionalEscapingClosureTy);
    updater.addAvailableValue(result->getParent(), result);
    return result;
  }();

  // If we had a single destroy, creating a .none after it and add that as a
  // value to the SSA updater.
  if (singleDestroy) {
    SILBuilderWithScope b(std::next(singleDestroy->getIterator()));
    auto *result = b.createOptionalNone(autoGenLoc, optionalEscapingClosureTy);
    updater.addAvailableValue(result->getParent(), result);
  }

  // Now that we have all of our available values, insert a destroy_value before
  // the initial Optional.some value using the SSA updater to ensure that we
  // handle loops correctly.
  {
    SILValue v = updater.getValueInMiddleOfBlock(initialValue->getParent());
    SILBuilderWithScope(initialValue).createDestroyValue(autoGenLoc, v);
  }

  // And insert an is_escaping_closure, cond_fail, destroy_value at each of the
  // lifetime end points. This ensures we do not expand our lifetime too much.
  if (singleDestroy) {
    SILBuilderWithScope b(std::next(singleDestroy->getIterator()));
    SILValue v = updater.getValueInMiddleOfBlock(singleDestroy->getParent());
    SILValue isEscaping =
        b.createIsEscapingClosure(loc, v, IsEscapingClosureInst::ObjCEscaping);
    b.createCondFail(loc, isEscaping, "non-escaping closure has escaped");
    b.createDestroyValue(loc, v);
  }

  // Then to be careful with regards to loops, insert at each of the destroy
  // blocks destroy_value to ensure that we obey ownership invariants.
  {
    SmallVector<SILBasicBlock *, 4> exitingBlocks;
    findReachableExitBlocks(newCB, exitingBlocks);

    for (auto *block : exitingBlocks) {
      auto *safeDestructionPt = getDeinitSafeClosureDestructionPoint(block);
      SILValue v = updater.getValueAtEndOfBlock(block);
      SILBuilderWithScope(safeDestructionPt).createDestroyValue(autoGenLoc, v);
    }
  }

  // Finally, we need to prune phis inserted by the SSA updater that only take
  // the .none from the entry block.
  //
  // TODO: Should we sort inserted phis before or after we initialize
  // the worklist or maybe backwards? We should investigate how the
  // SSA updater adds phi nodes to this list to resolve this question.
  cleanupDeadTrivialPhiArgs(entryBlockOptionalNone, insertedPhis);

  return true;
}

static bool fixupClosureLifetimes(SILFunction &fn, bool &checkStackNesting,
                                  bool &modifiedCFG) {
  bool changed = false;

  // tryExtendLifetimeToLastUse uses a cache of recursive instruction use
  // queries.
  llvm::DenseMap<SILInstruction *, SILInstruction *> memoizedQueries;

  for (auto &block : fn) {
    SILSSAUpdater updater;
    for (SILInstruction *inst : updater.getDeleter().updatingRange(&block)) {

      // Handle, copy_block_without_escaping instructions.
      if (auto *cb = dyn_cast<CopyBlockWithoutEscapingInst>(inst)) {
        if (fixupCopyBlockWithoutEscaping(cb, updater.getDeleter(), modifiedCFG)) {
          changed = true;
        }
        continue;
      }

      // Otherwise, look at convert_escape_to_noescape [not_guaranteed]
      // instructions.
      auto *cvt = dyn_cast<ConvertEscapeToNoEscapeInst>(inst);
      if (!cvt || cvt->isLifetimeGuaranteed())
        continue;

      // First try to peephole a known pattern.
      if (!DisableConvertEscapeToNoEscapeSwitchEnumPeephole) {
        if (trySwitchEnumPeephole(cvt)) {
          changed = true;
          continue;
        }
      }

      if (tryExtendLifetimeToLastUse(cvt, memoizedQueries, updater.getDeleter())) {
        changed = true;
        checkStackNesting = true;
        continue;
      }

      // Otherwise, extend the lifetime of the operand to the end of the
      // function.
      extendLifetimeToEndOfFunction(fn, cvt, updater);
      changed = true;
    }
  }
  return changed;
}

/// Fix-up the lifetime of the escaping closure argument of
/// convert_escape_to_noescape [not_guaranteed] instructions.
///
/// convert_escape_to_noescape [not_guaranteed] assume that someone guarantees
/// the lifetime of the operand for the duration of the trivial closure result.
/// SILGen does not guarantee this for '[not_guaranteed]' instructions so we
/// ensure it here.
namespace {
class ClosureLifetimeFixup : public SILFunctionTransform {

  /// The entry point to the transformation.
  void run() override {
    // Don't rerun diagnostics on deserialized functions.
    if (getFunction()->wasDeserializedCanonical())
      return;

    // Fixup convert_escape_to_noescape [not_guaranteed] and
    // copy_block_without_escaping instructions.

    bool checkStackNesting = false;
    bool modifiedCFG = false;

    if (fixupClosureLifetimes(*getFunction(), checkStackNesting, modifiedCFG)) {
      if (checkStackNesting){
        modifiedCFG |=
          StackNesting::fixNesting(getFunction()) == StackNesting::Changes::CFG;
      }
      if (modifiedCFG)
        invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
      else
        invalidateAnalysis(SILAnalysis::InvalidationKind::CallsAndInstructions);
    }
    LLVM_DEBUG(getFunction()->verify());

  }

};
} // end anonymous namespace

SILTransform *swift::createClosureLifetimeFixup() {
  return new ClosureLifetimeFixup();
}
