//===--- SILInliner.cpp - Inlines SIL functions ---------------------------===//
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

#define DEBUG_TYPE "sil-inliner"

#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "swift/AST/Builtins.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/TypeSubstCloner.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"

using namespace swift;

static bool canInlineBeginApply(BeginApplyInst *BA) {
  // Don't inline if we have multiple resumption sites (i.e. end_apply or
  // abort_apply instructions).  The current implementation clones a single
  // copy of the end_apply and abort_apply paths, so it can't handle values
  // that might be live in the caller across different resumption sites.  To
  // handle this in general, we'd need to separately clone the resume/unwind
  // paths into each end/abort.
  bool hasEndApply = false, hasAbortApply = false;
  for (auto tokenUse : BA->getTokenResult()->getUses()) {
    auto user = tokenUse->getUser();
    if (isa<EndApplyInst>(user)) {
      if (hasEndApply) return false;
      hasEndApply = true;
    } else {
      assert(isa<AbortApplyInst>(user));
      if (hasAbortApply) return false;
      hasAbortApply = true;
    }
  }

  // Don't inline a coroutine with multiple yields.  The current
  // implementation doesn't clone code from the caller, so it can't handle
  // values that might be live in the callee across different yields.
  // To handle this in general, we'd need to clone code in the caller,
  // both between the begin_apply and the resumption site and then
  // potentially after the resumption site when there are un-mergeable
  // values alive across it.
  bool hasYield = false;
  for (auto &B : *BA->getReferencedFunctionOrNull()) {
    if (isa<YieldInst>(B.getTerminator())) {
      if (hasYield) return false;
      hasYield = true;
    }
  }
  // Note that zero yields is fine; it just means the begin_apply is
  // basically noreturn.

  return true;
}

bool SILInliner::canInlineApplySite(FullApplySite apply) {
  if (!apply.canOptimize())
    return false;

  if (auto BA = dyn_cast<BeginApplyInst>(apply))
    return canInlineBeginApply(BA);

  return true;
}

namespace {

/// Utility class for rewiring control-flow of inlined begin_apply functions.
class BeginApplySite {
  SILLocation Loc;
  SILBuilder *Builder;
  BeginApplyInst *BeginApply;
  bool HasYield = false;

  EndApplyInst *EndApply = nullptr;
  SILBasicBlock *EndApplyBB = nullptr;
  SILBasicBlock *EndApplyReturnBB = nullptr;

  AbortApplyInst *AbortApply = nullptr;
  SILBasicBlock *AbortApplyBB = nullptr;
  SILBasicBlock *AbortApplyReturnBB = nullptr;

public:
  BeginApplySite(BeginApplyInst *BeginApply, SILLocation Loc,
                 SILBuilder *Builder)
      : Loc(Loc), Builder(Builder), BeginApply(BeginApply) {}

  static llvm::Optional<BeginApplySite> get(FullApplySite AI, SILLocation Loc,
                                            SILBuilder *Builder) {
    auto *BeginApply = dyn_cast<BeginApplyInst>(AI);
    if (!BeginApply)
      return llvm::None;
    return BeginApplySite(BeginApply, Loc, Builder);
  }

  void preprocess(SILBasicBlock *returnToBB,
                  SmallVectorImpl<SILInstruction *> &endBorrowInsertPts) {
    SmallVector<EndApplyInst *, 1> endApplyInsts;
    SmallVector<AbortApplyInst *, 1> abortApplyInsts;
    BeginApply->getCoroutineEndPoints(endApplyInsts, abortApplyInsts);
    while (!endApplyInsts.empty()) {
      auto *endApply = endApplyInsts.pop_back_val();
      collectEndApply(endApply);
      endBorrowInsertPts.push_back(&*std::next(endApply->getIterator()));
    }
    while (!abortApplyInsts.empty()) {
      auto *abortApply = abortApplyInsts.pop_back_val();
      collectAbortApply(abortApply);
      endBorrowInsertPts.push_back(&*std::next(abortApply->getIterator()));
    }
  }

  // Split the basic block before the end/abort_apply. We will insert code
  // to jump to the resume/unwind blocks depending on the integer token
  // later. And the inlined resume/unwind return blocks will jump back to
  // the merge blocks.
  void collectEndApply(EndApplyInst *End) {
    assert(!EndApply);
    EndApply = End;
    EndApplyBB = EndApply->getParent();
    EndApplyReturnBB = EndApplyBB->split(SILBasicBlock::iterator(EndApply));
  }
  void collectAbortApply(AbortApplyInst *Abort) {
    assert(!AbortApply);
    AbortApply = Abort;
    AbortApplyBB = AbortApply->getParent();
    AbortApplyReturnBB = AbortApplyBB->split(SILBasicBlock::iterator(Abort));
  }

  /// Perform special processing for the given terminator if necessary.
  ///
  /// \return false to use the normal inlining logic
  bool processTerminator(
      TermInst *terminator, SILBasicBlock *returnToBB,
      llvm::function_ref<SILBasicBlock *(SILBasicBlock *)> remapBlock,
      llvm::function_ref<SILValue(SILValue)> getMappedValue) {
    // A yield branches to the begin_apply return block passing the yielded
    // results as branch arguments. Collect the yields target block for
    // resuming later. Pass an integer token to the begin_apply return block
    // to mark the yield we came from.
    if (auto *yield = dyn_cast<YieldInst>(terminator)) {
      assert(!HasYield);
      HasYield = true;

      // Pairwise replace the yielded values of the BeginApply with the
      // values that were yielded.
      auto calleeYields = yield->getYieldedValues();
      auto callerYields = BeginApply->getYieldedValues();
      assert(calleeYields.size() == callerYields.size());
      SmallVector<BeginBorrowInst *, 2> guaranteedYields;
      for (auto i : indices(calleeYields)) {
        auto remappedYield = getMappedValue(calleeYields[i]);
        // When owned values are yielded @guaranteed, the mapped value must be
        // borrowed and the result be substituted in place of the originally
        // yielded value.  Otherwise, there could be uses of the original value
        // which require an @guaranteed operand into which we'd be attempting to
        // substitute an @owned operand.
        if (calleeYields[i]->getOwnershipKind() == OwnershipKind::Owned &&
            !yield->getOperandRef(i).isConsuming() &&
            Builder->getFunction().hasOwnership()) {
          auto *bbi = Builder->createBeginBorrow(Loc, remappedYield);
          guaranteedYields.push_back(bbi);
          remappedYield = bbi;
        }
        callerYields[i]->replaceAllUsesWith(remappedYield);
      }
      Builder->createBranch(Loc, returnToBB);

      // Add branches at the resumption sites to the resume/unwind block.
      if (EndApply) {
        SavedInsertionPointRAII savedIP(*Builder, EndApplyBB);
        auto resumeBB = remapBlock(yield->getResumeBB());
        for (auto *bbi : guaranteedYields) {
          Builder->createEndBorrow(EndApply->getLoc(), bbi);
        }
        Builder->createBranch(EndApply->getLoc(), resumeBB);
      }
      if (AbortApply) {
        SavedInsertionPointRAII savedIP(*Builder, AbortApplyBB);
        auto unwindBB = remapBlock(yield->getUnwindBB());
        for (auto *bbi : guaranteedYields) {
          Builder->createEndBorrow(EndApply->getLoc(), bbi);
        }
        Builder->createBranch(AbortApply->getLoc(), unwindBB);
      }
      return true;
    }

    // 'return' and 'unwind' instructions turn into branches to the
    // end_apply/abort_apply return blocks, respectively.  If those blocks
    // are null, it's because there weren't any of the corresponding
    // instructions in the caller.  That means this entire path is
    // unreachable.
    if (isa<ReturnInst>(terminator) || isa<UnwindInst>(terminator)) {
      bool isNormal = isa<ReturnInst>(terminator);
      auto returnBB = isNormal ? EndApplyReturnBB : AbortApplyReturnBB;
      if (returnBB) {
        Builder->createBranch(Loc, returnBB);
      } else {
        Builder->createUnreachable(Loc);
      }
      return true;
    }

    assert(!isa<ThrowInst>(terminator) &&
           "Unexpected throw instruction in yield_once function");

    // Otherwise, we just map the instruction normally.
    return false;
  }

  /// Complete the begin_apply-specific inlining work. Delete vestiges of the
  /// apply site except the callee value. Return a valid iterator after the
  /// original begin_apply.
  void complete() {
    // If there was no yield in the coroutine, then control never reaches
    // the end of the begin_apply, so all the downstream code is unreachable.
    // Make sure the function is well-formed, since we otherwise rely on
    // having visited a yield instruction.
    if (!HasYield) {
      // Make sure the split resumption blocks have terminators.
      if (EndApplyBB) {
        SavedInsertionPointRAII savedIP(*Builder, EndApplyBB);
        Builder->createUnreachable(Loc);
      }
      if (AbortApplyBB) {
        SavedInsertionPointRAII savedIP(*Builder, AbortApplyBB);
        Builder->createUnreachable(Loc);
      }

      // Replace all the yielded values in the callee with undef.
      for (auto calleeYield : BeginApply->getYieldedValues()) {
        calleeYield->replaceAllUsesWith(
            SILUndef::get(calleeYield->getType(), Builder->getFunction()));
      }
    }

    // Remove the resumption sites.
    if (EndApply)
      EndApply->eraseFromParent();
    if (AbortApply)
      AbortApply->eraseFromParent();

    assert(!BeginApply->hasUsesOfAnyResult());
  }
};

} // end anonymous namespace

namespace swift {
class SILInlineCloner
    : public TypeSubstCloner<SILInlineCloner, SILOptFunctionBuilder> {
  friend class SILInstructionVisitor<SILInlineCloner>;
  friend class SILCloner<SILInlineCloner>;
  using SuperTy = TypeSubstCloner<SILInlineCloner, SILOptFunctionBuilder>;
  using InlineKind = SILInliner::InlineKind;

  SILOptFunctionBuilder &FuncBuilder;
  InlineKind IKind;

  // The original, noninlined apply site. These become invalid after fixUp,
  // which is called as the last step in SILCloner::cloneFunctionBody.
  FullApplySite Apply;
  llvm::Optional<BeginApplySite> BeginApply;

  InstructionDeleter &deleter;

  /// The location representing the inlined instructions.
  ///
  /// This location wraps the call site AST node that is being inlined.
  /// Alternatively, it can be the SIL file location of the call site (in case
  /// of SIL-to-SIL transformations).
  SILLocation Loc;
  const SILDebugScope *CallSiteScope = nullptr;
  llvm::SmallDenseMap<const SILDebugScope *, const SILDebugScope *, 8>
      InlinedScopeCache;

  // Block in the original caller serving as the successor of the inlined
  // control path.
  SILBasicBlock *ReturnToBB = nullptr;

public:
  SILInlineCloner(SILFunction *CalleeFunction, FullApplySite Apply,
                  SILOptFunctionBuilder &FuncBuilder, InlineKind IKind,
                  SubstitutionMap ApplySubs,
                  InstructionDeleter &deleter);

  SILFunction *getCalleeFunction() const { return &Original; }

  void cloneInline(ArrayRef<SILValue> AppliedArgs);

protected:
  SILValue borrowFunctionArgument(SILValue callArg, unsigned index);
  SILValue moveFunctionArgument(SILValue callArg, unsigned index);

  void visitDebugValueInst(DebugValueInst *Inst);
  void visitHopToExecutorInst(HopToExecutorInst *Inst);

  void visitTerminator(SILBasicBlock *BB);
  void visitBuiltinInst(BuiltinInst *BI);

  /// This hook is called after either of the top-level visitors:
  /// cloneReachableBlocks or cloneSILFunction.
  ///
  /// After `preFixUp` is called `commonFixUp` will be called.
  void preFixUp(SILFunction *calleeFunction);

  /// After postFixUp, the SIL must be valid and semantically equivalent to the
  /// SIL before cloning.
  void postFixUp(SILFunction *calleeFunction);

  const SILDebugScope *getOrCreateInlineScope(const SILDebugScope *DS);

  void postProcess(SILInstruction *Orig, SILInstruction *Cloned) {
    // We just updated the debug scope information. Intentionally
    // don't call SILClonerWithScopes<SILInlineCloner>::postProcess().
    SILCloner<SILInlineCloner>::postProcess(Orig, Cloned);
  }

  SILLocation remapLocation(SILLocation InLoc) {
    // For performance inlining return the original location.
    if (IKind == InlineKind::PerformanceInline)
      return InLoc;
    // Inlined location wraps the call site that is being inlined, regardless of
    // the input location.
    return Loc;
  }

  const SILDebugScope *remapScope(const SILDebugScope *DS) {
    if (IKind == InlineKind::MandatoryInline)
      // Transparent functions are absorbed into the call
      // site. No soup, err, debugging for you!
      return CallSiteScope;
    else
      // Create an inlined version of the scope.
      return getOrCreateInlineScope(DS);
  }
};
} // namespace swift

SILBasicBlock *
SILInliner::inlineFunction(SILFunction *calleeFunction, FullApplySite apply,
                           ArrayRef<SILValue> appliedArgs) {
  PrettyStackTraceSILFunction calleeTraceRAII("inlining", calleeFunction);
  PrettyStackTraceSILFunction callerTraceRAII("...into", apply.getFunction());
  assert(canInlineApplySite(apply)
         && "Asked to inline function that is unable to be inlined?!");

  SILInlineCloner cloner(calleeFunction, apply, FuncBuilder, IKind, ApplySubs,
                         deleter);
  cloner.cloneInline(appliedArgs);
  return cloner.getLastClonedBB();
}

SILBasicBlock *
SILInliner::inlineFullApply(FullApplySite apply,
                            SILInliner::InlineKind inlineKind,
                            SILOptFunctionBuilder &funcBuilder,
                            InstructionDeleter &deleter) {
  assert(apply.canOptimize());
  assert(!apply.getFunction()->hasOwnership() ||
         apply.getReferencedFunctionOrNull()->hasOwnership());

  SmallVector<SILValue, 8> appliedArgs;
  for (const auto arg : apply.getArguments())
    appliedArgs.push_back(arg);

  SILInliner Inliner(funcBuilder, deleter, inlineKind, apply.getSubstitutionMap());
  return Inliner.inlineFunction(apply.getReferencedFunctionOrNull(), apply,
                                appliedArgs);
}

static SILLocation selectLoc(bool mandatory, SILLocation orig) {
  // Compute the SILLocation which should be used by all the inlined
  // instructions.
  if (mandatory)
    return MandatoryInlinedLocation(orig);
  else {
    return InlinedLocation(orig);
  }
}

SILInlineCloner::SILInlineCloner(
    SILFunction *calleeFunction, FullApplySite apply,
    SILOptFunctionBuilder &funcBuilder, InlineKind inlineKind,
    SubstitutionMap applySubs,
    InstructionDeleter &deleter)
    : SuperTy(*apply.getFunction(), *calleeFunction, applySubs,
              /*DT=*/nullptr, /*Inlining=*/true),
      FuncBuilder(funcBuilder), IKind(inlineKind), Apply(apply),
      deleter(deleter),
      Loc(selectLoc(inlineKind == InlineKind::MandatoryInline, apply.getLoc())) {

  SILFunction &F = getBuilder().getFunction();
  assert(apply.getFunction() && apply.getFunction() == &F
         && "Inliner called on apply instruction in wrong function?");
  assert(((calleeFunction->getRepresentation()
               != SILFunctionTypeRepresentation::ObjCMethod
           && calleeFunction->getRepresentation()
                  != SILFunctionTypeRepresentation::CFunctionPointer)
          || IKind == InlineKind::PerformanceInline)
         && "Cannot inline Objective-C methods or C functions in mandatory "
            "inlining");

  auto applyScope = apply.getDebugScope();
  // FIXME: Turn this into an assertion instead.
  if (!applyScope)
    applyScope = apply.getFunction()->getDebugScope();

  if (IKind == InlineKind::MandatoryInline) {
    // Mandatory inlining: every instruction inherits scope/location
    // from the call site.
    CallSiteScope = applyScope;
  } else {
    // Performance inlining. Construct a proper inline scope pointing
    // back to the call site.
    CallSiteScope = new (F.getModule()) SILDebugScope(
        apply.getLoc(), nullptr, applyScope, applyScope->InlinedCallSite);
  }
  assert(CallSiteScope && "call site has no scope");
  assert(CallSiteScope->getParentFunction() == &F);

  // Set up the coroutine-specific inliner if applicable.
  BeginApply = BeginApplySite::get(apply, Loc, &getBuilder());
}

// Clone the entire callee function into the caller function at the apply site.
// Delete the original apply and all dead arguments except the callee.
void SILInlineCloner::cloneInline(ArrayRef<SILValue> AppliedArgs) {
  assert(getCalleeFunction()->getArguments().size() == AppliedArgs.size()
         && "Unexpected number of callee arguments.");

  getBuilder().setInsertionPoint(Apply.getInstruction());

  SmallVector<SILValue, 4> entryArgs;
  entryArgs.reserve(AppliedArgs.size());

  auto calleeConv = getCalleeFunction()->getConventions();
  SmallBitVector borrowedArgs(AppliedArgs.size());
  SmallBitVector copiedArgs(AppliedArgs.size());
  if (!Apply->getFunction()->hasOwnership()) {

    for (auto p : llvm::enumerate(AppliedArgs)) {
      SILValue callArg = p.value();
      entryArgs.push_back(callArg);
    }
  } else {
    for (auto p : llvm::enumerate(AppliedArgs)) {
      SILValue callArg = p.value();
      SWIFT_DEFER { entryArgs.push_back(callArg); };
      unsigned idx = p.index();
      if (idx >= calleeConv.getSILArgIndexOfFirstParam()) {
        auto paramInfo = calleeConv.getParamInfoForSILArg(idx);
        if (callArg->getType().isAddress()) {
          // If lexical lifetimes are enabled, any alloc_stacks in the caller
          // that are passed to the callee being inlined (except mutating
          // exclusive accesses) need to be promoted to be lexical.  Otherwise,
          // destroy_addrs could be hoisted through the body of the newly
          // inlined function without regard to the deinit barriers it contains.
          //
          // TODO: [begin_borrow_addr] Instead of marking the alloc_stack as a
          //       whole lexical, just mark the inlined range lexical via
          //       begin_borrow_addr [lexical]/end_borrow_addr just as is done
          //       with values.
          auto &module = Apply.getFunction()->getModule();
          auto enableLexicalLifetimes =
              module.getASTContext().SILOpts.supportsLexicalLifetimes(module);
          if (!enableLexicalLifetimes)
            continue;

          // Exclusive mutating accesses don't entail a lexical scope.
          if (paramInfo.getConvention() == ParameterConvention::Indirect_Inout)
            continue;

          auto storage = AccessStorageWithBase::compute(callArg);
          if (auto *asi = dyn_cast_or_null<AllocStackInst>(storage.base))
            asi->setIsLexical();
        } else {
          // Insert begin/end borrow for guaranteed arguments.
          if (paramInfo.isGuaranteed()) {
            if (SILValue newValue = borrowFunctionArgument(callArg, idx)) {
              callArg = newValue;
              borrowedArgs[idx] = true;
            }
          } else if (paramInfo.isConsumed()) {
            if (SILValue newValue = moveFunctionArgument(callArg, idx)) {
              callArg = newValue;
            }
          }
        }
      }
    }
  }

  // Create the return block and set ReturnToBB for use in visitTerminator
  // callbacks.
  SILBasicBlock *callerBlock = Apply.getParent();
  SmallVector<SILInstruction *, 1> endBorrowInsertPts;

  switch (Apply.getKind()) {
  case FullApplySiteKind::ApplyInst: {
    auto *AI = dyn_cast<ApplyInst>(Apply);

    // Split the BB and do NOT create a branch between the old and new
    // BBs; we will create the appropriate terminator manually later.
    ReturnToBB =
        callerBlock->split(std::next(Apply.getInstruction()->getIterator()));
    endBorrowInsertPts.push_back(&*ReturnToBB->begin());

    // Create an argument on the return-to BB representing the returned value.
    auto *retArg =
        ReturnToBB->createPhiArgument(AI->getType(), OwnershipKind::Owned);
    // Replace all uses of the ApplyInst with the new argument.
    AI->replaceAllUsesWith(retArg);
    break;
  }
  case FullApplySiteKind::BeginApplyInst: {
    ReturnToBB =
        callerBlock->split(std::next(Apply.getInstruction()->getIterator()));
    // For begin_apply, we insert the end_borrow in the end_apply, abort_apply
    // blocks to ensure that our borrowed values live over both the body and
    // resume block of our coroutine.
    BeginApply->preprocess(ReturnToBB, endBorrowInsertPts);
    break;
  }
  case FullApplySiteKind::TryApplyInst: {
    auto *tai = cast<TryApplyInst>(Apply);
    ReturnToBB = tai->getNormalBB();
    endBorrowInsertPts.push_back(&*ReturnToBB->begin());
    endBorrowInsertPts.push_back(&*tai->getErrorBB()->begin());
    break;
  }
  }

  // Then insert end_borrow in our end borrow block and in the throw
  // block if we have one.
  if (borrowedArgs.any()) {
    for (unsigned i : indices(AppliedArgs)) {
      if (!borrowedArgs.test(i)) {
        continue;
      }

      for (auto *insertPt : endBorrowInsertPts) {
        SILBuilderWithScope returnBuilder(insertPt, getBuilder());
        returnBuilder.createEndBorrow(Apply.getLoc(), entryArgs[i]);
      }
    }
  }

  // Visit original BBs in depth-first preorder, starting with the
  // entry block, cloning all instructions and terminators.
  cloneFunctionBody(getCalleeFunction(), callerBlock, entryArgs);

  // For non-throwing applies, the inlined body now unconditionally branches to
  // the returned-to-code, which was previously part of the call site's basic
  // block. We could trivially merge these blocks now, however, this would be
  // quadratic: O(num-calls-in-block * num-instructions-in-block). Also,
  // guaranteeing that caller instructions following the inlined call are in a
  // separate block gives the inliner control over revisiting only the inlined
  // instructions.
  //
  // Once all calls in a function are inlined, unconditional branches are
  // eliminated by mergeBlocks.
}

void SILInlineCloner::visitTerminator(SILBasicBlock *BB) {
  TermInst *Terminator = BB->getTerminator();
  // Coroutine terminators need special handling.
  if (BeginApply) {
    if (BeginApply->processTerminator(
            Terminator, ReturnToBB,
            [=](SILBasicBlock *Block) -> SILBasicBlock * {
              return this->remapBasicBlock(Block);
            },
            [=](SILValue Val) -> SILValue {
              return this->getMappedValue(Val);
            }))
      return;
  }

  // Modify return terminators to branch to the return-to BB, rather
  // than trying to clone the ReturnInst.  Because of that, the scope
  // needs to be remapped manually.
  getBuilder().setCurrentDebugScope(getOpScope(Terminator->getDebugScope()));
  if (auto *RI = dyn_cast<ReturnInst>(Terminator)) {
    auto returnedValue = getMappedValue(RI->getOperand());
    getBuilder().createBranch(getOpLocation(RI->getLoc()), ReturnToBB,
                              returnedValue);
    return;
  }

  // Modify throw terminators to branch to the error-return BB, rather than
  // trying to clone the ThrowInst.
  if (auto *TI = dyn_cast<ThrowInst>(Terminator)) {
    SILLocation Loc = getOpLocation(TI->getLoc());
    switch (Apply.getKind()) {
    case FullApplySiteKind::ApplyInst:
      assert(cast<ApplyInst>(Apply)->isNonThrowing()
             && "apply of a function with error result must be non-throwing");
      getBuilder().createUnreachable(Loc);
      return;
    case FullApplySiteKind::BeginApplyInst:
      assert(cast<BeginApplyInst>(Apply)->isNonThrowing()
             && "apply of a function with error result must be non-throwing");
      getBuilder().createUnreachable(Loc);
      return;
    case FullApplySiteKind::TryApplyInst:
      auto tryAI = cast<TryApplyInst>(Apply);
      auto returnedValue = getMappedValue(TI->getOperand());
      getBuilder().createBranch(Loc, tryAI->getErrorBB(), returnedValue);
      return;
    }
  }
  // Otherwise use normal visitor, which clones the existing instruction
  // but remaps basic blocks and values.
  visit(Terminator);
}

void SILInlineCloner::preFixUp(SILFunction *calleeFunction) {
  // "Completing" the BeginApply only fixes the end of the apply scope. The
  // begin_apply itself lingers.
  if (BeginApply)
    BeginApply->complete();
}

void SILInlineCloner::postFixUp(SILFunction *calleeFunction) {
  deleter.getCallbacks().notifyWillBeDeleted(Apply.getInstruction());
  deleter.forceDelete(Apply.getInstruction());
}

namespace {

enum class Scope : uint8_t {
  None,
  Bare,
  Lexical,
};
Scope scopeForArgument(Scope nonlexicalScope, SILValue callArg, unsigned index,
                       SILFunction *caller, SILFunction *callee) {
  if (!caller->hasOwnership()) {
    // The function isn't in OSSA.  Borrows/moves are not meaningful.
    return Scope::None;
  }

  auto &mod = caller->getModule();
  auto enableLexicalLifetimes =
      mod.getASTContext().SILOpts.supportsLexicalLifetimes(mod);
  SILFunctionArgument *argument =
      cast<SILFunctionArgument>(callee->getEntryBlock()->getArgument(index));
  if (!enableLexicalLifetimes) {
    // Lexical lifetimes are disabled.  Use the non-lexical scope:
    // - for borrows, do an ownership conversion.
    // - for moves, do nothing.
    return nonlexicalScope;
  }
  if (!argument->getLifetime().isLexical()) {
    // The same applies if lexical lifetimes are enabled but the function
    // argument is not lexical.  There is no lexical lifetime to maintain.  Use
    // the non-lexical scope.
    return nonlexicalScope;
  }
  // Lexical lifetimes are enabled and the function argument is lexical.
  // During inlining, we need to ensure that the lifetime is maintained.
  if (callArg->isLexical()) {
    // The caller's value is already lexical.  It will maintain the lifetime of
    // the argument.  Just do an ownership conversion if needed.
    return nonlexicalScope;
  }
  // Lexical lifetimes are enabled, the function argument's lifetime is
  // lexical, but the caller's value is not lexical.  Extra care is required to
  // maintain the function argument's lifetime.  We need to add a lexical
  // scope.
  return Scope::Lexical;
}

} // anonymous namespace

SILValue SILInlineCloner::borrowFunctionArgument(SILValue callArg,
                                                 unsigned index) {
  // The "minimal" borrow scope:  Guaranteed values are valid operands to some
  // instructions that owned values are not.  If the caller's value is owned,
  // it must be converted (via a "bare" begin_borrow) to a guaranteed value so
  // that it can be used in place of the original guaranteed value in the
  // instructions that are being inlined.
  auto scopeForOwnership = callArg->getOwnershipKind() == OwnershipKind::Owned
                               ? Scope::Bare
                               : Scope::None;
  auto scope = scopeForArgument(scopeForOwnership, callArg, index,
                                Apply.getFunction(), getCalleeFunction());
  bool isLexical;
  switch (scope) {
  case Scope::None:
    return SILValue();
  case Scope::Bare:
    isLexical = false;
    break;
  case Scope::Lexical:
    isLexical = true;
    break;
  }
  SILBuilderWithScope beginBuilder(Apply.getInstruction(), getBuilder());
  return beginBuilder.createBeginBorrow(Apply.getLoc(), callArg, isLexical);
}

SILValue SILInlineCloner::moveFunctionArgument(SILValue callArg,
                                               unsigned index) {
  auto scope = scopeForArgument(Scope::None, callArg, index,
                                Apply.getFunction(), getCalleeFunction());
  bool isLexical;
  switch (scope) {
  case Scope::None:
    return SILValue();
  case Scope::Bare:
    assert(false && "Non-lexical move produced during inlining!?");
    isLexical = false;
    break;
  case Scope::Lexical:
    isLexical = true;
    break;
  }
  SILBuilderWithScope beginBuilder(Apply.getInstruction(), getBuilder());
  return beginBuilder.createMoveValue(Apply.getLoc(), callArg, isLexical);
}

void SILInlineCloner::visitDebugValueInst(DebugValueInst *Inst) {
  // The mandatory inliner drops debug_value instructions when inlining, as if
  // it were a "nodebug" function in C.
  if (IKind == InlineKind::MandatoryInline) return;

  return SILCloner<SILInlineCloner>::visitDebugValueInst(Inst);
}

void SILInlineCloner::visitHopToExecutorInst(HopToExecutorInst *Inst) {
  // Drop hop_to_executor in non async functions.
  if (!Apply.getFunction()->isAsync()) {
    assert(Apply.isNonAsync());
    return;
  }

  return SILCloner<SILInlineCloner>::visitHopToExecutorInst(Inst);
}

const SILDebugScope *
SILInlineCloner::getOrCreateInlineScope(const SILDebugScope *CalleeScope) {
  if (!CalleeScope)
    return CallSiteScope;
  auto it = InlinedScopeCache.find(CalleeScope);
  if (it != InlinedScopeCache.end())
    return it->second;

  auto &M = getBuilder().getModule();
  auto InlinedAt =
      getOrCreateInlineScope(CalleeScope->InlinedCallSite);

  auto *ParentFunction = CalleeScope->Parent.dyn_cast<SILFunction *>();
  if (ParentFunction)
    ParentFunction = remapParentFunction(
        FuncBuilder, M, ParentFunction, SubsMap,
        getCalleeFunction()->getLoweredFunctionType()
                           ->getInvocationGenericSignature(),
        ForInlining);

  auto *ParentScope = CalleeScope->Parent.dyn_cast<const SILDebugScope *>();
  auto *InlinedScope = new (M) SILDebugScope(
      CalleeScope->Loc, ParentFunction,
      ParentScope ? getOrCreateInlineScope(ParentScope) : nullptr, InlinedAt);
  InlinedScopeCache.insert({CalleeScope, InlinedScope});
  return InlinedScope;
}

template <typename... T, typename... U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
                     U &&...args) {
  Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

void SILInlineCloner::visitBuiltinInst(BuiltinInst *Inst) {
  if (IKind == InlineKind::MandatoryInline) {
    if (auto kind = Inst->getBuiltinKind()) {
      if (*kind == BuiltinValueKind::Copy) {
        auto otherResultAddr = getOpValue(Inst->getOperand(0));
        auto otherSrcAddr = getOpValue(Inst->getOperand(1));
        auto otherType = otherSrcAddr->getType();

        if (!otherType.isLoadable(*Inst->getFunction())) {
          // If otherType is not loadable, emit a diagnostic since it was used
          // on a generic or existential value.
          diagnose(Inst->getModule().getASTContext(),
                   getOpLocation(Inst->getLoc()).getSourceLoc(),
                   diag::copy_operator_used_on_generic_or_existential_value);
          return SILCloner<SILInlineCloner>::visitBuiltinInst(Inst);
        }

        getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
        // We stash otherValue in originalOtherValue in case we need to
        // perform a writeback.
        auto opLoc = getOpLocation(Inst->getLoc());

        assert(otherType.isAddress());

        // Perform a load_borrow and then copy that.
        SILValue otherValue =
            getBuilder().emitLoadBorrowOperation(opLoc, otherSrcAddr);

        auto *mvi = getBuilder().createExplicitCopyValue(opLoc, otherValue);

        getBuilder().emitStoreValueOperation(opLoc, mvi, otherResultAddr,
                                             StoreOwnershipQualifier::Init);
        // End the borrowed value.
        getBuilder().emitEndBorrowOperation(opLoc, otherValue);

        // We know that Inst returns a tuple value that isn't used by anything
        // else, so this /should/ be safe.
        return recordClonedInstruction(Inst, mvi);
      }
    }
  }

  return SILCloner<SILInlineCloner>::visitBuiltinInst(Inst);
}

//===----------------------------------------------------------------------===//
//                                 Cost Model
//===----------------------------------------------------------------------===//

static InlineCost getEnforcementCost(SILAccessEnforcement enforcement) {
  switch (enforcement) {
  case SILAccessEnforcement::Unknown:
    llvm_unreachable("evaluating cost of access with unknown enforcement?");
  case SILAccessEnforcement::Dynamic:
    return InlineCost::Expensive;
  case SILAccessEnforcement::Static:
  case SILAccessEnforcement::Unsafe:
  case SILAccessEnforcement::Signed:
    return InlineCost::Free;
  }
  llvm_unreachable("bad enforcement");
}

/// For now just assume that every SIL instruction is one to one with an LLVM
/// instruction. This is of course very much so not true.
InlineCost swift::instructionInlineCost(SILInstruction &I) {
  switch (I.getKind()) {
  case SILInstructionKind::IntegerLiteralInst:
  case SILInstructionKind::FloatLiteralInst:
  case SILInstructionKind::DebugValueInst:
  case SILInstructionKind::DebugStepInst:
  case SILInstructionKind::StringLiteralInst:
  case SILInstructionKind::FixLifetimeInst:
  case SILInstructionKind::EndBorrowInst:
  case SILInstructionKind::BeginBorrowInst:
  case SILInstructionKind::MarkDependenceInst:
  case SILInstructionKind::PreviousDynamicFunctionRefInst:
  case SILInstructionKind::DynamicFunctionRefInst:
  case SILInstructionKind::FunctionRefInst:
  case SILInstructionKind::AllocGlobalInst:
  case SILInstructionKind::GlobalAddrInst:
  case SILInstructionKind::BaseAddrForOffsetInst:
  case SILInstructionKind::EndLifetimeInst:
  case SILInstructionKind::UncheckedOwnershipConversionInst:
  case SILInstructionKind::BindMemoryInst:
  case SILInstructionKind::RebindMemoryInst:
  case SILInstructionKind::MoveValueInst:
  case SILInstructionKind::DropDeinitInst:
  case SILInstructionKind::MarkMustCheckInst:
  case SILInstructionKind::MarkUnresolvedReferenceBindingInst:
  case SILInstructionKind::CopyableToMoveOnlyWrapperValueInst:
  case SILInstructionKind::MoveOnlyWrapperToCopyableValueInst:
  case SILInstructionKind::TestSpecificationInst:
  case SILInstructionKind::MoveOnlyWrapperToCopyableAddrInst:
  case SILInstructionKind::CopyableToMoveOnlyWrapperAddrInst:
  case SILInstructionKind::MoveOnlyWrapperToCopyableBoxInst:
    return InlineCost::Free;

  // Typed GEPs are free.
  case SILInstructionKind::TupleElementAddrInst:
  case SILInstructionKind::StructElementAddrInst:
  case SILInstructionKind::ProjectBlockStorageInst:
    return InlineCost::Free;

  // tuple_pack_element_addr is just a GEP, but getting the offset
  // can require accessing metadata, so conservatively treat it as
  // expensive.
  case SILInstructionKind::TuplePackElementAddrInst:
    return InlineCost::Expensive;

  // pack_length is just a few adds, which is close enough to free.
  case SILInstructionKind::PackLengthInst:
    return InlineCost::Free;

  // dynamic_pack_index is free.  The other pack-indexing instructions
  // are just adds of values that should be trivially dynamically
  // available; that's cheap enough to still consider free under the
  // same principle as typed GEPs.
  case SILInstructionKind::DynamicPackIndexInst:
  case SILInstructionKind::PackPackIndexInst:
  case SILInstructionKind::ScalarPackIndexInst:
    return InlineCost::Free;

  // Pack element get/set are a GEP plus a load/store.
  // Cheap, but not free.
  case SILInstructionKind::PackElementGetInst:
  case SILInstructionKind::PackElementSetInst:
    return InlineCost::Expensive;

  // Aggregates are exploded at the IR level; these are effectively no-ops.
  case SILInstructionKind::TupleInst:
  case SILInstructionKind::StructInst:
  case SILInstructionKind::StructExtractInst:
  case SILInstructionKind::TupleExtractInst:
  case SILInstructionKind::DestructureStructInst:
  case SILInstructionKind::DestructureTupleInst:
    return InlineCost::Free;

  // Unchecked casts are free.
  case SILInstructionKind::AddressToPointerInst:
  case SILInstructionKind::PointerToAddressInst:

  case SILInstructionKind::UncheckedRefCastInst:
  case SILInstructionKind::UncheckedRefCastAddrInst:
  case SILInstructionKind::UncheckedAddrCastInst:
  case SILInstructionKind::UncheckedTrivialBitCastInst:
  case SILInstructionKind::UncheckedBitwiseCastInst:
  case SILInstructionKind::UncheckedValueCastInst:

  case SILInstructionKind::RawPointerToRefInst:
  case SILInstructionKind::RefToRawPointerInst:

  case SILInstructionKind::UpcastInst:

  case SILInstructionKind::ThinToThickFunctionInst:
  case SILInstructionKind::ConvertFunctionInst:
  case SILInstructionKind::ConvertEscapeToNoEscapeInst:

  case SILInstructionKind::BridgeObjectToWordInst:
    return InlineCost::Free;

  // Access instructions are free unless we're dynamically enforcing them.
  case SILInstructionKind::BeginAccessInst:
    return getEnforcementCost(cast<BeginAccessInst>(I).getEnforcement());
  case SILInstructionKind::EndAccessInst:
    return getEnforcementCost(cast<EndAccessInst>(I).getBeginAccess()
                                                   ->getEnforcement());
  case SILInstructionKind::BeginUnpairedAccessInst:
    return getEnforcementCost(cast<BeginUnpairedAccessInst>(I)
                                .getEnforcement());
  case SILInstructionKind::EndUnpairedAccessInst:
    return getEnforcementCost(cast<EndUnpairedAccessInst>(I)
                                .getEnforcement());

  // TODO: These are free if the metatype is for a Swift class.
  case SILInstructionKind::ThickToObjCMetatypeInst:
  case SILInstructionKind::ObjCToThickMetatypeInst:
    return InlineCost::Expensive;
    
  // TODO: Bridge object conversions imply a masking operation that should be
  // "hella cheap" but not really expensive.
  case SILInstructionKind::BridgeObjectToRefInst:
  case SILInstructionKind::RefToBridgeObjectInst:
  case SILInstructionKind::ClassifyBridgeObjectInst:
  case SILInstructionKind::ValueToBridgeObjectInst:
    return InlineCost::Expensive;

  case SILInstructionKind::MetatypeInst:
    // Thin metatypes are always free.
    if (cast<MetatypeInst>(I).getType().castTo<MetatypeType>()
          ->getRepresentation() == MetatypeRepresentation::Thin)
      return InlineCost::Free;
    // TODO: Thick metatypes are free if they don't require generic or lazy
    // instantiation.
    return InlineCost::Expensive;

  // Protocol descriptor references are free.
  case SILInstructionKind::ObjCProtocolInst:
    return InlineCost::Free;

  // Metatype-to-object conversions are free.
  case SILInstructionKind::ObjCExistentialMetatypeToObjectInst:
  case SILInstructionKind::ObjCMetatypeToObjectInst:
    return InlineCost::Free;

  // Return and unreachable are free.
  case SILInstructionKind::UnreachableInst:
  case SILInstructionKind::ReturnInst:
  case SILInstructionKind::ThrowInst:
  case SILInstructionKind::UnwindInst:
  case SILInstructionKind::YieldInst:
  case SILInstructionKind::EndCOWMutationInst:
    return InlineCost::Free;

  // Turning the task reference into a continuation should be basically free.
  // TODO(async): make sure this is true.
  case SILInstructionKind::GetAsyncContinuationAddrInst:
  case SILInstructionKind::GetAsyncContinuationInst:
    return InlineCost::Free;

  // Unconditional branch is free in empty blocks.
  case SILInstructionKind::BranchInst:
    return (I.getIterator() == I.getParent()->begin())
      ? InlineCost::Free : InlineCost::Expensive;

  case SILInstructionKind::AbortApplyInst:
  case SILInstructionKind::ApplyInst:
  case SILInstructionKind::TryApplyInst:
  case SILInstructionKind::AllocBoxInst:
  case SILInstructionKind::AllocExistentialBoxInst:
  case SILInstructionKind::AllocRefInst:
  case SILInstructionKind::AllocRefDynamicInst:
  case SILInstructionKind::AllocStackInst:
  case SILInstructionKind::AllocPackInst:
  case SILInstructionKind::AllocPackMetadataInst:
  case SILInstructionKind::BeginApplyInst:
  case SILInstructionKind::ValueMetatypeInst:
  case SILInstructionKind::WitnessMethodInst:
  case SILInstructionKind::AssignInst:
  case SILInstructionKind::AssignByWrapperInst:
  case SILInstructionKind::AssignOrInitInst:
  case SILInstructionKind::CheckedCastBranchInst:
  case SILInstructionKind::CheckedCastAddrBranchInst:
  case SILInstructionKind::ClassMethodInst:
  case SILInstructionKind::ObjCMethodInst:
  case SILInstructionKind::CondBranchInst:
  case SILInstructionKind::CondFailInst:
  case SILInstructionKind::CopyBlockInst:
  case SILInstructionKind::CopyBlockWithoutEscapingInst:
  case SILInstructionKind::CopyAddrInst:
  case SILInstructionKind::ExplicitCopyAddrInst:
  case SILInstructionKind::MarkUnresolvedMoveAddrInst:
  case SILInstructionKind::RetainValueInst:
  case SILInstructionKind::RetainValueAddrInst:
  case SILInstructionKind::UnmanagedRetainValueInst:
  case SILInstructionKind::CopyValueInst:
  case SILInstructionKind::ExplicitCopyValueInst:
  case SILInstructionKind::DeallocBoxInst:
  case SILInstructionKind::DeallocExistentialBoxInst:
  case SILInstructionKind::DeallocStackRefInst:
  case SILInstructionKind::DeallocRefInst:
  case SILInstructionKind::DeallocPartialRefInst:
  case SILInstructionKind::DeallocStackInst:
  case SILInstructionKind::DeallocPackInst:
  case SILInstructionKind::DeallocPackMetadataInst:
  case SILInstructionKind::DeinitExistentialAddrInst:
  case SILInstructionKind::DeinitExistentialValueInst:
  case SILInstructionKind::DestroyAddrInst:
  case SILInstructionKind::EndApplyInst:
  case SILInstructionKind::ProjectBoxInst:
  case SILInstructionKind::ProjectExistentialBoxInst:
  case SILInstructionKind::ReleaseValueInst:
  case SILInstructionKind::ReleaseValueAddrInst:
  case SILInstructionKind::UnmanagedReleaseValueInst:
  case SILInstructionKind::DestroyValueInst:
  case SILInstructionKind::AutoreleaseValueInst:
  case SILInstructionKind::UnmanagedAutoreleaseValueInst:
  case SILInstructionKind::DynamicMethodBranchInst:
  case SILInstructionKind::EnumInst:
  case SILInstructionKind::IndexAddrInst:
  case SILInstructionKind::TailAddrInst:
  case SILInstructionKind::IndexRawPointerInst:
  case SILInstructionKind::InitEnumDataAddrInst:
  case SILInstructionKind::InitExistentialAddrInst:
  case SILInstructionKind::InitExistentialValueInst:
  case SILInstructionKind::InitExistentialMetatypeInst:
  case SILInstructionKind::InitExistentialRefInst:
  case SILInstructionKind::InjectEnumAddrInst:
  case SILInstructionKind::LoadInst:
  case SILInstructionKind::LoadBorrowInst:
  case SILInstructionKind::OpenExistentialAddrInst:
  case SILInstructionKind::OpenExistentialBoxInst:
  case SILInstructionKind::OpenExistentialBoxValueInst:
  case SILInstructionKind::OpenExistentialMetatypeInst:
  case SILInstructionKind::OpenExistentialRefInst:
  case SILInstructionKind::OpenExistentialValueInst:
  case SILInstructionKind::OpenPackElementInst:
  case SILInstructionKind::PartialApplyInst:
  case SILInstructionKind::ExistentialMetatypeInst:
  case SILInstructionKind::RefElementAddrInst:
  case SILInstructionKind::RefTailAddrInst:
  case SILInstructionKind::StoreInst:
  case SILInstructionKind::StoreBorrowInst:
  case SILInstructionKind::StrongReleaseInst:
  case SILInstructionKind::SetDeallocatingInst:
  case SILInstructionKind::StrongRetainInst:
  case SILInstructionKind::SuperMethodInst:
  case SILInstructionKind::ObjCSuperMethodInst:
  case SILInstructionKind::SwitchEnumAddrInst:
  case SILInstructionKind::SwitchEnumInst:
  case SILInstructionKind::SwitchValueInst:
  case SILInstructionKind::UncheckedEnumDataInst:
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst:
  case SILInstructionKind::UnconditionalCheckedCastInst:
  case SILInstructionKind::UnconditionalCheckedCastAddrInst:
  case SILInstructionKind::IsEscapingClosureInst:
  case SILInstructionKind::IsUniqueInst:
  case SILInstructionKind::BeginCOWMutationInst:
  case SILInstructionKind::InitBlockStorageHeaderInst:
  case SILInstructionKind::SelectEnumAddrInst:
  case SILInstructionKind::SelectEnumInst:
  case SILInstructionKind::KeyPathInst:
  case SILInstructionKind::GlobalValueInst:
  case SILInstructionKind::DifferentiableFunctionInst:
  case SILInstructionKind::LinearFunctionInst:
  case SILInstructionKind::DifferentiableFunctionExtractInst:
  case SILInstructionKind::LinearFunctionExtractInst:
  case SILInstructionKind::DifferentiabilityWitnessFunctionInst:
  case SILInstructionKind::IncrementProfilerCounterInst:
  case SILInstructionKind::AwaitAsyncContinuationInst:
  case SILInstructionKind::HopToExecutorInst:
  case SILInstructionKind::ExtractExecutorInst:
  case SILInstructionKind::HasSymbolInst:
  case SILInstructionKind::UnownedCopyValueInst:
  case SILInstructionKind::WeakCopyValueInst:
#define COMMON_ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name)          \
  case SILInstructionKind::Name##ToRefInst:                                    \
  case SILInstructionKind::RefTo##Name##Inst:                                  \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                          \
  case SILInstructionKind::Load##Name##Inst:                                   \
  case SILInstructionKind::Store##Name##Inst:                                  \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                         \
  COMMON_ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name)                \
  case SILInstructionKind::Name##RetainInst:                                   \
  case SILInstructionKind::Name##ReleaseInst:                                  \
  case SILInstructionKind::StrongRetain##Name##Inst:
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                      \
  case SILInstructionKind::Load##Name##Inst:                                   \
  case SILInstructionKind::Store##Name##Inst:                                  \
    ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, "...")
#define UNCHECKED_REF_STORAGE(Name, ...) \
  COMMON_ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name)
#include "swift/AST/ReferenceStorage.def"
#undef COMMON_ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE
    return InlineCost::Expensive;

  case SILInstructionKind::BuiltinInst: {
    auto *BI = cast<BuiltinInst>(&I);
    // Expect intrinsics are 'free' instructions.
    if (BI->getIntrinsicInfo().ID == llvm::Intrinsic::expect)
      return InlineCost::Free;
    if (BI->getBuiltinInfo().ID == BuiltinValueKind::OnFastPath)
      return InlineCost::Free;

    return InlineCost::Expensive;
  }
  case SILInstructionKind::MarkFunctionEscapeInst:
  case SILInstructionKind::MarkUninitializedInst:
    llvm_unreachable("not valid in canonical sil");
  case SILInstructionKind::ObjectInst:
    llvm_unreachable("not valid in a function");
  }

  llvm_unreachable("Unhandled ValueKind in switch.");
}
