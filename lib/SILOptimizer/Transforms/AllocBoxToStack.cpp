//===--- AllocBoxToStack.cpp - Promote alloc_box to alloc_stack -----------===//
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

#define DEBUG_TYPE "allocbox-to-stack"

#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/Basic/BlotMapVector.h"
#include "swift/Basic/GraphNodeWorklist.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/NodeDatastructures.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/SpecializationMangler.h"
#include "swift/SILOptimizer/Utils/StackNesting.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
using namespace swift;

STATISTIC(NumStackPromoted, "Number of alloc_box's promoted to the stack");

// MaxLocalApplyRecurDepth limits the recursive analysis depth while
// checking if a box can be promoted to stack. This is currently set to 4, a
// limit assumed to be sufficient to handle typical call chain of local
// functions through which a box can be passed.
static llvm::cl::opt<unsigned> MaxLocalApplyRecurDepth(
    "max-local-apply-recur-depth", llvm::cl::init(4),
    llvm::cl::desc("Max recursive depth for analyzing local functions"));

static llvm::cl::opt<bool> AllocBoxToStackAnalyzeApply(
    "allocbox-to-stack-analyze-apply", llvm::cl::init(true),
    llvm::cl::desc("Analyze functions into while alloc_box is passed"));

//===-----------------------------------------------------------------------===//
//                 SIL Utilities for alloc_box Promotion
//===----------------------------------------------------------------------===//

static SILValue stripOffCopyAndBorrow(SILValue V) {
  while (isa<CopyValueInst>(V) || isa<BeginBorrowInst>(V)) {
    V = cast<SingleValueInstruction>(V)->getOperand(0);
  }
  return V;
}

/// Returns True if the operand or one of its users is captured.
static bool useCaptured(Operand *UI) {
  auto *User = UI->getUser();

  // These instructions do not cause the address to escape.
  if (isa<DebugValueInst>(User)
      || isa<StrongReleaseInst>(User) || isa<StrongRetainInst>(User)
      || isa<DestroyValueInst>(User)
      || isa<EndBorrowInst>(User))
    return false;

  if (auto *Store = dyn_cast<StoreInst>(User)) {
    if (Store->getDest() == UI->get())
      return false;
  } else if (auto *Assign = dyn_cast<AssignInst>(User)) {
    if (Assign->getDest() == UI->get())
      return false;
  }

  return true;
}

//===----------------------------------------------------------------------===//
//                 Liveness for alloc_box Promotion
//===----------------------------------------------------------------------===//

// Is any successor of BB in the LiveIn set?
static bool successorHasLiveIn(SILBasicBlock *BB,
                               BasicBlockSetVector &LiveIn) {
  for (auto &Succ : BB->getSuccessors())
    if (LiveIn.contains(Succ))
      return true;

  return false;
}

// Propagate liveness backwards from an initial set of blocks in our
// LiveIn set.
static void propagateLiveness(BasicBlockSetVector &LiveIn,
                              SILBasicBlock *DefBB) {

  // First populate a worklist of predecessors.
  SmallVector<SILBasicBlock *, 64> Worklist;
  for (auto *BB : LiveIn)
    for (auto Pred : BB->getPredecessorBlocks())
      Worklist.push_back(Pred);

  // Now propagate liveness backwards until we hit the alloc_box.
  while (!Worklist.empty()) {
    auto *BB = Worklist.pop_back_val();

    // If it's already in the set, then we've already queued and/or
    // processed the predecessors.
    if (BB == DefBB || !LiveIn.insert(BB))
      continue;

    for (auto Pred : BB->getPredecessorBlocks())
      Worklist.push_back(Pred);
  }
}

// Walk backwards in BB looking for strong_release, destroy_value, or
// dealloc_box of the given value, and add it to releases.
static bool addLastRelease(SILValue V, SILBasicBlock *BB,
                           SmallVectorImpl<SILInstruction *> &Releases) {
  for (auto I = BB->rbegin(); I != BB->rend(); ++I) {
    if (isa<StrongReleaseInst>(*I) || isa<DeallocBoxInst>(*I) ||
        isa<DestroyValueInst>(*I)) {
      if (stripOffCopyAndBorrow(I->getOperand(0)) != V)
        continue;

      Releases.push_back(&*I);
      return true;
    }
  }

  return false;
}

// Find the final releases of the alloc_box along any given path.
// These can include paths from a release back to the alloc_box in a
// loop.
static bool getFinalReleases(SILValue Box,
                             SmallVectorImpl<SILInstruction *> &Releases) {
  SILFunction *function = Box->getFunction();
  BasicBlockSetVector LiveIn(function);
  BasicBlockSetVector UseBlocks(function);

  auto *DefBB = Box->getParentBlock();

  auto seenRelease = false;
  SILInstruction *OneRelease = nullptr;

  // We'll treat this like a liveness problem where the alloc_box is
  // the def. Each block that has a use of the owning pointer has the
  // value live-in unless it is the block with the alloc_box.
  SmallVector<Operand *, 32> Worklist(Box->use_begin(), Box->use_end());
  while (!Worklist.empty()) {
    auto *Op = Worklist.pop_back_val();
    auto *User = Op->getUser();
    auto *BB = User->getParent();

    if (isa<ProjectBoxInst>(User))
      continue;

    if (BB != DefBB)
      LiveIn.insert(BB);

    // Also keep track of the blocks with uses.
    UseBlocks.insert(BB);

    // If we have a copy value or a mark_uninitialized, add its uses to the work
    // list and continue.
    if (isa<MarkUninitializedInst>(User) || isa<CopyValueInst>(User) ||
        isa<BeginBorrowInst>(User)) {
      llvm::copy(cast<SingleValueInstruction>(User)->getUses(),
                 std::back_inserter(Worklist));
      continue;
    }

    // Try to speed up the trivial case of single release/dealloc.
    if (isa<StrongReleaseInst>(User) || isa<DeallocBoxInst>(User) ||
        isa<DestroyValueInst>(User)) {
      if (!seenRelease)
        OneRelease = User;
      else
        OneRelease = nullptr;

      seenRelease = true;
    }
  }

  // Only a single release/dealloc? We're done!
  if (OneRelease) {
    Releases.push_back(OneRelease);
    return true;
  }

  propagateLiveness(LiveIn, DefBB);

  // Now examine each block we saw a use in. If it has no successors
  // that are in LiveIn, then the last use in the block is the final
  // release/dealloc.
  for (auto *BB : UseBlocks)
    if (!successorHasLiveIn(BB, LiveIn))
      if (!addLastRelease(Box, BB, Releases))
        return false;

  return true;
}

//===----------------------------------------------------------------------===//
//                      alloc_box Escape Analysis
//===----------------------------------------------------------------------===//

/// This is a list we use to store a set of indices. We create the set by
/// sorting, uniquing at the appropriate time. The reason why it makes sense to
/// just use a sorted vector with std::count is because generally functions do
/// not have that many arguments and even fewer promoted arguments.
using ArgIndexList = SmallVector<unsigned, 8>;

static bool partialApplyEscapes(SILValue V, bool examineApply);

/// Could this operand to an apply escape that function by being
/// stored or returned?
static bool applyArgumentEscapes(FullApplySite Apply, Operand *O) {
  SILFunction *F = Apply.getReferencedFunctionOrNull();
  // If we cannot examine the function body, assume the worst.
  if (!F || F->empty())
    return true;

  // Check the uses of the operand, but do not recurse down into other
  // apply instructions.
  auto calleeArg = F->getArgument(Apply.getCalleeArgIndex(*O));
  return partialApplyEscapes(calleeArg, /* examineApply = */ false);
}

static bool partialApplyEscapes(SILValue V, bool examineApply) {
  SILModuleConventions ModConv(*V->getModule());
  SmallVector<Operand *, 32> Worklist(V->use_begin(), V->use_end());
  while (!Worklist.empty()) {
    Operand *Op = Worklist.pop_back_val();

    // These instructions do not cause the address to escape.
    if (!useCaptured(Op))
      continue;

    auto *User = Op->getUser();

    // If we have a copy_value, begin_borrow, or move_value, that instruction
    // does not cause an escape, but its uses might do so... so add the
    // its uses to the worklist and continue.
    if (isa<CopyValueInst>(User) || isa<BeginBorrowInst>(User) ||
        isa<MoveValueInst>(User)) {
      llvm::copy(cast<SingleValueInstruction>(User)->getUses(),
                 std::back_inserter(Worklist));
      continue;
    }

    if (auto Apply = FullApplySite::isa(User)) {
      // Applying a function does not cause the function to escape.
      if (!Apply.isArgumentOperand(*Op))
        continue;

      // apply instructions do not capture the pointer when it is passed
      // indirectly
      if (Apply.getArgumentConvention(*Op).isIndirectConvention())
        continue;

      // Optionally drill down into an apply to see if the operand is
      // captured in or returned from the apply.
      if (examineApply && !applyArgumentEscapes(Apply, Op))
        continue;
    }

    // partial_apply instructions do not allow the pointer to escape
    // when it is passed indirectly, unless the partial_apply itself
    // escapes
    if (auto *PartialApply = dyn_cast<PartialApplyInst>(User)) {
      auto Args = PartialApply->getArguments();
      auto Params = PartialApply->getSubstCalleeType()->getParameters();
      Params = Params.slice(Params.size() - Args.size(), Args.size());
      if (ModConv.isSILIndirect(Params[Op->getOperandNumber() - 1])) {
        if (partialApplyEscapes(PartialApply, /*examineApply = */ true))
          return true;
        continue;
      }
    }

    return true;
  }

  return false;
}

static SILInstruction *recursivelyFindBoxOperandsPromotableToAddress(
    SILValue Box, bool inAppliedFunction, SmallVectorImpl<Operand *> &,
    SmallPtrSetImpl<SILFunction *> &, unsigned CurrentRecurDepth);

/// checkLocalApplyBody - Check the body of an apply's callee to see
/// if the box pointer argument passed to it has uses that would
/// disqualify it from being promoted to a stack location.  Return
/// true if this apply will not block our promoting the box.
static bool checkLocalApplyBody(Operand *O,
                                SmallVectorImpl<Operand *> &PromotedOperands,
                                SmallPtrSetImpl<SILFunction *> &VisitedCallees,
                                unsigned CurrentRecurDepth) {
  SILFunction *F = ApplySite(O->getUser()).getReferencedFunctionOrNull();
  // If we cannot examine the function body, assume the worst.
  if (!F || F->empty())
    return false;

  // Since this function can be called recursively while analyzing the same box,
  // mark the callee as visited, so that we don't end up in a recursive cycle.
  auto iter = VisitedCallees.insert(F);
  if (!iter.second)
    return false;

  auto calleeArg = F->getArgument(ApplySite(O->getUser()).getCalleeArgIndex(*O));
  auto res = !recursivelyFindBoxOperandsPromotableToAddress(
      calleeArg,
      /* inAppliedFunction = */ true, PromotedOperands, VisitedCallees,
      CurrentRecurDepth + 1);
  return res;
}

// Returns true if a callee is eligible to be cloned and rewritten for
// AllocBoxToStack opt. We don't want to increase code size, so this is
// restricted only for private local functions currently.
static bool isOptimizableApplySite(ApplySite Apply) {
  if (!AllocBoxToStackAnalyzeApply) {
    // turned off explicitly
    return false;
  }
  auto callee = Apply.getReferencedFunctionOrNull();
  if (!callee) {
    return false;
  }

  // Callee should be optimizable.
  if (!callee->shouldOptimize())
    return false;

  // External function definitions.
  if (!callee->isDefinition())
    return false;

  // Do not optimize always_inlinable functions.
  if (callee->getInlineStrategy() == Inline_t::AlwaysInline)
    return false;

  if (callee->getLinkage() != SILLinkage::Private)
    return false;

  return true;
}

/// Validate that the uses of a pointer to a box do not eliminate it from
/// consideration for promotion to a stack element. Return the instruction with
/// the unexpected use if we find one.
/// If a box has ApplySite users, we recursively examine the callees to check
/// for unexpected use of the box argument. If all the callees through which the
/// box is passed don't have any unexpected uses, `PromotedOperands` will be
/// populated with the box arguments in DFS order.
static SILInstruction *recursivelyFindBoxOperandsPromotableToAddress(
    SILValue Box, bool inAppliedFunction,
    SmallVectorImpl<Operand *> &PromotedOperands,
    SmallPtrSetImpl<SILFunction *> &VisitedCallees,
    unsigned CurrentRecurDepth = 0) {
  assert((Box->getType().is<SILBoxType>()
          || Box->getType()
                 == SILType::getNativeObjectType(Box->getType().getASTContext()))
         && "Expected an object pointer!");

  SmallVector<Operand *, 4> LocalPromotedOperands;

  // Scan all of the uses of the retain count value, collecting all
  // the releases and validating that we don't have an unexpected
  // user.
  SmallVector<Operand *, 32> Worklist(Box->use_begin(), Box->use_end());
  while (!Worklist.empty()) {
    auto *Op = Worklist.pop_back_val();
    auto *User = Op->getUser();

    // Retains and releases are fine. Deallocs are fine if we're not
    // examining a function that the alloc_box was passed into.
    // Projections are fine as well.
    if (isa<StrongRetainInst>(User) || isa<StrongReleaseInst>(User) ||
        isa<ProjectBoxInst>(User) || isa<DestroyValueInst>(User) ||
        (!inAppliedFunction && isa<DeallocBoxInst>(User)) ||
        isa<EndBorrowInst>(User))
      continue;

    // If our user instruction is a copy_value or a mark_uninitialized, visit
    // the users recursively.
    if (isa<MarkUninitializedInst>(User) || isa<CopyValueInst>(User) ||
        isa<BeginBorrowInst>(User)) {
      llvm::copy(cast<SingleValueInstruction>(User)->getUses(),
                 std::back_inserter(Worklist));
      continue;
    }

    if (auto Apply = ApplySite::isa(User)) {
      if (CurrentRecurDepth > MaxLocalApplyRecurDepth) {
        return User;
      }
      switch (Apply.getKind()) {
      case ApplySiteKind::PartialApplyInst: {
        if (checkLocalApplyBody(Op, LocalPromotedOperands, VisitedCallees,
                                CurrentRecurDepth) &&
            !partialApplyEscapes(cast<PartialApplyInst>(User),
                                 /* examineApply = */ true)) {
          LocalPromotedOperands.push_back(Op);
          continue;
        }
        break;
      }
      case ApplySiteKind::ApplyInst:
      case ApplySiteKind::BeginApplyInst:
      case ApplySiteKind::TryApplyInst:
        if (isOptimizableApplySite(Apply) &&
            checkLocalApplyBody(Op, LocalPromotedOperands, VisitedCallees,
                                CurrentRecurDepth)) {
          LocalPromotedOperands.push_back(Op);
          continue;
        }
      }
    }

    return User;
  }

  PromotedOperands.append(LocalPromotedOperands.begin(),
                          LocalPromotedOperands.end());
  return nullptr;
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnose(ASTContext &Context, SourceLoc loc,
                                   Diag<T...> diag, U &&... args) {
  return Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

/// canPromoteAllocBox - Can we promote this alloc_box to an alloc_stack?
static bool canPromoteAllocBox(AllocBoxInst *ABI,
                               SmallVectorImpl<Operand *> &PromotedOperands) {
  SmallPtrSet<SILFunction *, 8> VisitedCallees;
  // Scan all of the uses of the address of the box to see if any
  // disqualifies the box from being promoted to the stack.
  if (auto *User = recursivelyFindBoxOperandsPromotableToAddress(
          ABI,
          /* inAppliedFunction = */ false, PromotedOperands, VisitedCallees,
          /* CurrentRecurDepth = */ 0)) {
    (void)User;
    // Otherwise, we have an unexpected use.
    LLVM_DEBUG(llvm::dbgs() << "*** Failed to promote alloc_box in @"
               << ABI->getFunction()->getName() << ": " << *ABI
               << "    Due to user: " << *User << "\n");

    // Check if the vardecl has a "boxtostack.mustbeonstack" attribute. If so,
    // emit a diagnostic.
    if (auto *decl = ABI->getDecl()) {
      if (decl->hasSemanticsAttr("boxtostack.mustbeonstack")) {
        auto allocDiag =
            diag::box_to_stack_cannot_promote_box_to_stack_due_to_escape_alloc;
        diagnose(ABI->getModule().getASTContext(), ABI->getLoc().getSourceLoc(),
                 allocDiag);
        auto escapeNote = diag::
            box_to_stack_cannot_promote_box_to_stack_due_to_escape_location;
        diagnose(ABI->getModule().getASTContext(),
                 User->getLoc().getSourceLoc(), escapeNote);
      }
    }

    return false;
  }

  // Okay, it looks like this value doesn't escape.
  return true;
}

//===----------------------------------------------------------------------===//
//                           alloc_box Promotion
//===----------------------------------------------------------------------===//

namespace {
// Pass context and per-function analysis results.
struct AllocBoxToStackState {
  SILFunctionTransform *T;
  bool CFGChanged = false;

  SmallVector<AllocBoxInst *, 8> Promotable;
  SmallVector<Operand *, 8> PromotedOperands;

  AllocBoxToStackState(SILFunctionTransform *T) : T(T) {}
};
} // anonymous namespace

static void replaceProjectBoxUsers(SILValue heapBox, SILValue stackBox) {
  StackList<Operand *> worklist(heapBox->getFunction());
  for (auto *use : heapBox->getUses())
    worklist.push_back(use);
  while (!worklist.empty()) {
    auto *nextUse = worklist.pop_back_val();
    if (auto *pbi = dyn_cast<ProjectBoxInst>(nextUse->getUser())) {
      // This may result in an alloc_stack being used by begin_access [dynamic].
      pbi->replaceAllUsesWith(stackBox);
      pbi->eraseFromParent();
      continue;
    }

    auto *user = nextUse->getUser();
    if (isa<MarkUninitializedInst>(user) || isa<CopyValueInst>(user) ||
        isa<BeginBorrowInst>(user)) {
      for (auto *use : cast<SingleValueInstruction>(user)->getUses()) {
        worklist.push_back(use);
      }
    }
  }
}

static void replaceAllNonDebugUsesWith(SILValue value,
                                       SILValue with) {
  auto useI = value->use_begin();
  while (useI != value->use_end()) {
    Operand *op = *useI;
    ++useI;
    // Leave debug instructions on the original value.
    if (op->getUser()->isDebugInstruction()) {
      continue;
    }
    
    // Rewrite all other uses.
    op->set(with);
  }
}

static void hoistMarkMustCheckInsts(SILValue stackBox,
                                    MarkMustCheckInst::CheckKind checkKind) {
  StackList<Operand *> worklist(stackBox->getFunction());

  for (auto *use : stackBox->getUses()) {
    worklist.push_back(use);
  }

  StackList<MarkMustCheckInst *> targets(stackBox->getFunction());
  while (!worklist.empty()) {
    auto *nextUse = worklist.pop_back_val();
    auto *nextUser = nextUse->getUser();

    if (isa<BeginBorrowInst>(nextUser) || isa<BeginAccessInst>(nextUser) ||
        isa<CopyValueInst>(nextUser) || isa<MarkUninitializedInst>(nextUser) ||
        isa<MarkMustCheckInst>(nextUser)) {
      for (auto result : nextUser->getResults()) {
        for (auto *use : result->getUses())
          worklist.push_back(use);
      }
    }

    if (auto *mmci = dyn_cast<MarkMustCheckInst>(nextUser)) {
      targets.push_back(mmci);
    }
  }

  if (targets.empty())
    return;

  while (!targets.empty()) {
    auto *mmci = targets.pop_back_val();
    mmci->replaceAllUsesWith(mmci->getOperand());
    mmci->eraseFromParent();
  }

  auto *next = stackBox->getNextInstruction();
  auto loc = next->getLoc();
  if (isa<TermInst>(next))
    loc = RegularLocation::getDiagnosticsOnlyLocation(loc, next->getModule());
  SILBuilderWithScope builder(next);

  auto *undef = SILUndef::get(stackBox->getType(), *stackBox->getModule());

  auto *mmci = builder.createMarkMustCheckInst(loc, undef, checkKind);
  // Leave debug uses on the to-be-promoted box, but hoist all other uses to the
  // new mark_must_check.
  replaceAllNonDebugUsesWith(stackBox, mmci);
  mmci->setOperand(stackBox);
}

/// rewriteAllocBoxAsAllocStack - Replace uses of the alloc_box with a
/// new alloc_stack, but do not delete the alloc_box yet.
static bool rewriteAllocBoxAsAllocStack(AllocBoxInst *ABI) {
  LLVM_DEBUG(llvm::dbgs() << "*** Promoting alloc_box to stack: " << *ABI);

  SILValue HeapBox = ABI;
  llvm::Optional<MarkUninitializedInst::Kind> Kind;
  if (HeapBox->hasOneUse()) {
    auto *User = HeapBox->getSingleUse()->getUser();
    if (auto *MUI = dyn_cast<MarkUninitializedInst>(User)) {
      HeapBox = MUI;
      Kind = MUI->getMarkUninitializedKind();
    }
  }

  SmallVector<SILInstruction *, 4> FinalReleases;
  if (!getFinalReleases(HeapBox, FinalReleases))
    return false;

  // Promote this alloc_box to an alloc_stack. Insert the alloc_stack
  // at the beginning of the function.
  SILBuilderWithScope Builder(ABI);
  assert(ABI->getBoxType()->getLayout()->getFields().size() == 1
         && "rewriting multi-field box not implemented");
  auto ty = getSILBoxFieldType(TypeExpansionContext(*ABI->getFunction()),
                               ABI->getBoxType(), ABI->getModule().Types, 0);
  auto isLexical = [&]() -> bool {
    auto &mod = ABI->getFunction()->getModule();
    bool lexicalLifetimesEnabled =
        mod.getASTContext().SILOpts.supportsLexicalLifetimes(mod);
    if (!lexicalLifetimesEnabled)
      return false;
    // Look for lexical borrows of the alloc_box.
    GraphNodeWorklist<Operand *, 4> worklist;
    worklist.initializeRange(ABI->getUses());
    while (auto *use = worklist.pop()) {
      // See through mark_uninitialized and non-lexical begin_borrow
      // instructions.  It's verified that lexical begin_borrows of SILBoxType
      // values originate either from AllocBoxInsts or SILFunctionArguments.
      if (auto *mui = dyn_cast<MarkUninitializedInst>(use->getUser())) {
        for (auto *use : mui->getUses())
          worklist.insert(use);
      } else if (auto *bbi = dyn_cast<BeginBorrowInst>(use->getUser())) {
        if (bbi->isLexical())
          return true;
        for (auto *use : bbi->getUses())
          worklist.insert(use);
      }
    }
    return false;
  };
  auto *ASI =
      Builder.createAllocStack(ABI->getLoc(), ty, ABI->getVarInfo(),
                               ABI->hasDynamicLifetime(), isLexical(), false
#ifndef NDEBUG
                               ,
                               true
#endif
      );

  // Transfer a mark_uninitialized if we have one.
  SingleValueInstruction *StackBox = ASI;
  if (Kind) {
    StackBox =
        Builder.createMarkUninitialized(ASI->getLoc(), ASI, Kind.value());
  }

  // Replace all uses of the address of the box's contained value with
  // the address of the stack location.
  replaceProjectBoxUsers(HeapBox, StackBox);

  // Then hoist any mark_must_check [assignable_but_not_consumable] to the
  // alloc_stack and convert them to [consumable_but_not_assignable]. This is
  // because we are semantically converting from escaping semantics to
  // non-escaping semantics.
  hoistMarkMustCheckInsts(
      StackBox, MarkMustCheckInst::CheckKind::ConsumableAndAssignable);

  assert(ABI->getBoxType()->getLayout()->getFields().size() == 1
         && "promoting multi-field box not implemented");
  auto &Lowering = ABI->getFunction()->getTypeLowering(
      getSILBoxFieldType(TypeExpansionContext(*ABI->getFunction()),
                         ABI->getBoxType(), ABI->getModule().Types, 0));
  auto Loc = CleanupLocation(ABI->getLoc());

  for (auto LastRelease : FinalReleases) {
    SILBuilderWithScope Builder(LastRelease);
    if (!isa<DeallocBoxInst>(LastRelease)&& !Lowering.isTrivial()) {
      // If we have a mark_must_check use of our stack box, we want to destroy
      // that.
      SILValue valueToDestroy = StackBox;
      if (auto *mmci = StackBox->getSingleUserOfType<MarkMustCheckInst>()) {
        valueToDestroy = mmci;
      }

      // For non-trivial types, insert destroys for each final release-like
      // instruction we found that isn't an explicit dealloc_box.
      Builder.emitDestroyAddrAndFold(Loc, valueToDestroy);
    }
    Builder.createDeallocStack(Loc, ASI);
  }

  // Remove any retain and release instructions.  Since all uses of project_box
  // are gone, this only walks through uses of the box itself (the retain count
  // pointer).
  SmallVector<SILInstruction *, 8> Worklist;
  std::transform(ABI->use_begin(), ABI->use_end(), std::back_inserter(Worklist),
                 [](Operand *Op) -> SILInstruction * { return Op->getUser(); });
  while (!Worklist.empty()) {
    auto *User = Worklist.pop_back_val();

    // Look through any mark_uninitialized, copy_values, begin_borrow.
    if (isa<MarkUninitializedInst>(User) || isa<CopyValueInst>(User) ||
        isa<BeginBorrowInst>(User)) {
      auto Inst = cast<SingleValueInstruction>(User);
      llvm::transform(Inst->getUses(), std::back_inserter(Worklist),
                      [](Operand *Op) -> SILInstruction * {
        return Op->getUser();
      });
      Inst->replaceAllUsesWithUndef();
      Inst->eraseFromParent();
      continue;
    }

    assert(isa<StrongReleaseInst>(User) || isa<StrongRetainInst>(User) ||
           isa<DeallocBoxInst>(User) || isa<ProjectBoxInst>(User) ||
           isa<DestroyValueInst>(User) || isa<EndBorrowInst>(User));

    User->eraseFromParent();
  }

  return true;
}

namespace {

/// A SILCloner subclass which clones a closure function while
/// promoting some of its box parameters to stack addresses.
class PromotedParamCloner : public SILClonerWithScopes<PromotedParamCloner> {
  friend class SILInstructionVisitor<PromotedParamCloner>;
  friend class SILCloner<PromotedParamCloner>;

  SILFunction *Orig;
  ArgIndexList &PromotedArgIndices;
  SmallVector<SILValue, 4> NewPromotedArgs;

  // The values in the original function that are promoted to stack
  // references.
  SmallPtrSet<SILValue, 4> OrigPromotedParameters;

public:
  PromotedParamCloner(SILOptFunctionBuilder &FuncBuilder, SILFunction *Orig,
                      IsSerialized_t Serialized,
                      ArgIndexList &PromotedArgIndices, StringRef ClonedName);

  void populateCloned();

  SILFunction *getCloned() { return &getBuilder().getFunction(); }

private:
  static SILFunction *initCloned(SILOptFunctionBuilder &FuncBuilder,
                                 SILFunction *Orig, IsSerialized_t Serialized,
                                 ArgIndexList &PromotedArgIndices,
                                 StringRef ClonedName);

  void visitStrongReleaseInst(StrongReleaseInst *Inst);
  void visitDestroyValueInst(DestroyValueInst *Inst);
  void visitStrongRetainInst(StrongRetainInst *Inst);
  void visitCopyValueInst(CopyValueInst *Inst);
  void visitProjectBoxInst(ProjectBoxInst *Inst);
  void checkNoPromotedBoxInApply(ApplySite Apply);
  void visitApplyInst(ApplyInst *Inst);
  void visitBeginApplyInst(BeginApplyInst *Inst);
  void visitPartialApplyInst(PartialApplyInst *Inst);
  void visitTryApplyInst(TryApplyInst *Inst);
};
} // end anonymous namespace

PromotedParamCloner::PromotedParamCloner(SILOptFunctionBuilder &FuncBuilder,
                                         SILFunction *Orig,
                                         IsSerialized_t Serialized,
                                         ArgIndexList &PromotedArgIndices,
                                         StringRef ClonedName)
    : SILClonerWithScopes<PromotedParamCloner>(*initCloned(
          FuncBuilder, Orig, Serialized, PromotedArgIndices, ClonedName)),
      Orig(Orig), PromotedArgIndices(PromotedArgIndices) {
  NewPromotedArgs.reserve(PromotedArgIndices.size());
  assert(Orig->getDebugScope()->getParentFunction() !=
         getCloned()->getDebugScope()->getParentFunction());
}

static std::string getClonedName(SILFunction *F, IsSerialized_t Serialized,
                                 ArgIndexList &PromotedArgIndices) {
  auto P = Demangle::SpecializationPass::AllocBoxToStack;
  Mangle::FunctionSignatureSpecializationMangler Mangler(P, Serialized, F);
  for (unsigned i : PromotedArgIndices) {
    Mangler.setArgumentBoxToStack(i);
  }
  return Mangler.mangle();
}

/// Create the function corresponding to the clone of the
/// original closure with the signature modified to reflect promoted
/// parameters (which are specified by PromotedArgIndices).
SILFunction *PromotedParamCloner::initCloned(SILOptFunctionBuilder &FuncBuilder,
                                             SILFunction *Orig,
                                             IsSerialized_t Serialized,
                                             ArgIndexList &PromotedArgIndices,
                                             StringRef ClonedName) {
  SILModule &M = Orig->getModule();

  SmallVector<SILParameterInfo, 4> ClonedInterfaceArgTys;

  // Generate a new parameter list with deleted parameters removed.
  SILFunctionType *OrigFTI = Orig->getLoweredFunctionType();
  unsigned Index = Orig->getConventions().getSILArgIndexOfFirstParam();
  for (auto &param : OrigFTI->getParameters()) {
    if (count(PromotedArgIndices, Index)) {
      auto boxTy = param.getSILStorageInterfaceType().castTo<SILBoxType>();
      assert(boxTy->getLayout()->getFields().size() == 1
             && "promoting compound box not implemented");
      SILType paramTy;
      {
        auto &TC = Orig->getModule().Types;
        paramTy = getSILBoxFieldType(TypeExpansionContext(*Orig), boxTy, TC, 0);
      }
      auto promotedParam = SILParameterInfo(paramTy.getASTType(),
                                  ParameterConvention::Indirect_InoutAliasable);
      ClonedInterfaceArgTys.push_back(promotedParam);
    } else {
      ClonedInterfaceArgTys.push_back(param);
    }
    ++Index;
  }

  // Create the new function type for the cloned function with some of
  // the parameters promoted.
  auto ClonedTy = SILFunctionType::get(
      OrigFTI->getInvocationGenericSignature(), OrigFTI->getExtInfo(),
      OrigFTI->getCoroutineKind(), OrigFTI->getCalleeConvention(),
      ClonedInterfaceArgTys, OrigFTI->getYields(), OrigFTI->getResults(),
      OrigFTI->getOptionalErrorResult(), OrigFTI->getPatternSubstitutions(),
      OrigFTI->getInvocationSubstitutions(), M.getASTContext(),
      OrigFTI->getWitnessMethodConformanceOrInvalid());

  assert((Orig->isTransparent() || Orig->isBare() || Orig->getLocation())
         && "SILFunction missing location");
  assert((Orig->isTransparent() || Orig->isBare() || Orig->getDebugScope())
         && "SILFunction missing DebugScope");
  assert(!Orig->isGlobalInit() && "Global initializer cannot be cloned");
  auto *Fn = FuncBuilder.createFunction(
      swift::getSpecializedLinkage(Orig, Orig->getLinkage()), ClonedName,
      ClonedTy, Orig->getGenericEnvironment(), Orig->getLocation(),
      Orig->isBare(), Orig->isTransparent(), Serialized, IsNotDynamic,
      IsNotDistributed, IsNotRuntimeAccessible, Orig->getEntryCount(),
      Orig->isThunk(), Orig->getClassSubclassScope(), Orig->getInlineStrategy(),
      Orig->getEffectsKind(), Orig, Orig->getDebugScope());
  for (auto &Attr : Orig->getSemanticsAttrs()) {
    Fn->addSemanticsAttr(Attr);
  }
  if (!Orig->hasOwnership()) {
    Fn->setOwnershipEliminated();
  }
  return Fn;
}

/// Populate the body of the cloned closure, modifying instructions as
/// necessary to take into consideration the removed parameters.
void
PromotedParamCloner::populateCloned() {
  SILFunction *Cloned = getCloned();

  // Create arguments for the entry block
  SILBasicBlock *OrigEntryBB = &*Orig->begin();
  SILBasicBlock *ClonedEntryBB = Cloned->createBasicBlock();

  SmallVector<SILValue, 4> entryArgs;
  entryArgs.reserve(OrigEntryBB->getArguments().size());

  // Initialize all NewPromotedArgs slots to an invalid value.
  NewPromotedArgs.resize(OrigEntryBB->getArguments().size());

  unsigned ArgNo = 0;
  auto I = OrigEntryBB->args_begin(), E = OrigEntryBB->args_end();
  while (I != E) {
    if (count(PromotedArgIndices, ArgNo)) {
      // Create a new argument with the promoted type.
      auto boxTy = (*I)->getType().castTo<SILBoxType>();
      assert(boxTy->getLayout()->getFields().size() == 1
             && "promoting multi-field boxes not implemented yet");
      auto promotedTy = getSILBoxFieldType(TypeExpansionContext(*Cloned), boxTy,
                                           Cloned->getModule().Types, 0);
      auto *promotedArg =
          ClonedEntryBB->createFunctionArgument(promotedTy, (*I)->getDecl());
      promotedArg->copyFlags(cast<SILFunctionArgument>(*I));
      OrigPromotedParameters.insert(*I);

      NewPromotedArgs[ArgNo] = promotedArg;
      // We only promote boxes used in apply or projections or copy/destroy
      // value operations.
      // We should never see an apply user of the box, because we rewrite the
      // applies and specialize the callees in dfs order.
      // Projection users are folded when visited and copy/destroy operations
      // are ignored.
      entryArgs.push_back(SILValue());
    } else {
      // Create a new argument which copies the original argument.
      auto *newArg = ClonedEntryBB->createFunctionArgument((*I)->getType(),
                                                           (*I)->getDecl());
      newArg->copyFlags(cast<SILFunctionArgument>(*I));
      entryArgs.push_back(newArg);
    }
    ++ArgNo;
    ++I;
  }

  // Visit original BBs in depth-first preorder, starting with the
  // entry block, cloning all instructions and terminators.
  cloneFunctionBody(Orig, ClonedEntryBB, entryArgs);
}

/// Handle a strong_release instruction during cloning of a closure; if
/// it is a strong release of a promoted box argument, then it is replaced with
/// a ReleaseValue of the new object type argument, otherwise it is handled
/// normally.
void
PromotedParamCloner::visitStrongReleaseInst(StrongReleaseInst *Inst) {
  // If it's a release of a promoted parameter, just drop the instruction.
  if (OrigPromotedParameters.count(Inst->getOperand()))
    return;

  SILCloner<PromotedParamCloner>::visitStrongReleaseInst(Inst);
}

/// Handle a strong_release instruction during cloning of a closure; if
/// it is a strong release of a promoted box argument, then it is replaced with
/// a ReleaseValue of the new object type argument, otherwise it is handled
/// normally.
void PromotedParamCloner::visitDestroyValueInst(DestroyValueInst *Inst) {
  // If we are a destroy of a promoted parameter, just drop the instruction. We
  // look through copy_value to preserve current behavior.
  SILInstruction *Tmp = Inst;
  while (auto *CopyOp = dyn_cast<CopyValueInst>(Tmp->getOperand(0))) {
    Tmp = CopyOp;
  }

  if (OrigPromotedParameters.count(Tmp->getOperand(0)))
    return;

  SILCloner<PromotedParamCloner>::visitDestroyValueInst(Inst);
}

void
PromotedParamCloner::visitStrongRetainInst(StrongRetainInst *Inst) {
  // If it's a retain of a promoted parameter, just drop the instruction.
  if (OrigPromotedParameters.count(Inst->getOperand()))
    return;

  SILCloner<PromotedParamCloner>::visitStrongRetainInst(Inst);
}

void PromotedParamCloner::visitCopyValueInst(CopyValueInst *cvi) {
  // If it's a copy of a promoted parameter, just drop the instruction.
  auto *tmp = cvi;
  while (auto *copyOp = dyn_cast<CopyValueInst>(tmp->getOperand())) {
    tmp = copyOp;
  }
  if (OrigPromotedParameters.count(tmp->getOperand()))
    return;

  SILCloner<PromotedParamCloner>::visitCopyValueInst(cvi);
}

void PromotedParamCloner::visitProjectBoxInst(ProjectBoxInst *pbi) {
  // If it's a projection of a promoted parameter (or a copy_value of a promoted
  // parameter), drop the instruction.  Its uses will be replaced by the
  // promoted address.
  SILValue box = pbi->getOperand();
  while (auto *copyOp = dyn_cast<CopyValueInst>(box)) {
    box = copyOp->getOperand();
  }
  if (OrigPromotedParameters.count(box)) {
    auto *origArg = cast<SILFunctionArgument>(box);
    recordFoldedValue(pbi, NewPromotedArgs[origArg->getIndex()]);
    return;
  }

  SILCloner<PromotedParamCloner>::visitProjectBoxInst(pbi);
}

// While cloning during specialization, make sure apply instructions do not have
// box arguments that need to be promoted.
// This is an assertion in debug builds only. The reason why this should never
// be true is that we have cloned our callees in DFS order meaning that any of
// our callees that had a promotable box will have already have been promoted
// away by the time this runs.
void PromotedParamCloner::checkNoPromotedBoxInApply(ApplySite Apply) {
#ifndef NDEBUG
  for (auto &O : Apply.getArgumentOperands()) {
    assert(OrigPromotedParameters.count(O.get()) == 0);
  }
#endif
}
void PromotedParamCloner::visitApplyInst(ApplyInst *Inst) {
  checkNoPromotedBoxInApply(Inst);
  SILCloner<PromotedParamCloner>::visitApplyInst(Inst);
}
void PromotedParamCloner::visitBeginApplyInst(BeginApplyInst *Inst) {
  checkNoPromotedBoxInApply(Inst);
  SILCloner<PromotedParamCloner>::visitBeginApplyInst(Inst);
}
void PromotedParamCloner::visitPartialApplyInst(PartialApplyInst *Inst) {
  checkNoPromotedBoxInApply(Inst);
  SILCloner<PromotedParamCloner>::visitPartialApplyInst(Inst);
}
void PromotedParamCloner::visitTryApplyInst(TryApplyInst *Inst) {
  checkNoPromotedBoxInApply(Inst);
  SILCloner<PromotedParamCloner>::visitTryApplyInst(Inst);
}

/// Specialize ApplySite by promoting the parameters indicated by
/// indices. We expect these parameters to be replaced by stack address
/// references.
static SILInstruction *
specializeApplySite(SILOptFunctionBuilder &FuncBuilder, ApplySite Apply,
                    ArgIndexList &PromotedCalleeArgIndices,
                    AllocBoxToStackState &pass) {
  auto *FRI = cast<FunctionRefInst>(Apply.getCallee());
  assert(FRI && "Expected a direct ApplySite");
  auto *F = FRI->getReferencedFunction();
  assert(F && "Expected a referenced function!");

  IsSerialized_t Serialized = IsNotSerialized;
  if (Apply.getFunction()->isSerialized())
    Serialized = IsSerialized;

  std::string ClonedName =
    getClonedName(F, Serialized, PromotedCalleeArgIndices);

  auto &M = Apply.getModule();

  SILFunction *ClonedFn;
  if (auto *PrevFn = M.lookUpFunction(ClonedName)) {
    assert(PrevFn->isSerialized() == Serialized);
    ClonedFn = PrevFn;
  } else {
    // Clone the function the existing ApplySite references.
    PromotedParamCloner Cloner(FuncBuilder, F, Serialized,
                               PromotedCalleeArgIndices,
                               ClonedName);
    Cloner.populateCloned();
    ClonedFn = Cloner.getCloned();
    pass.T->addFunctionToPassManagerWorklist(ClonedFn, F);

    // Set the moveonly delete-if-unused flag so we do not emit an error on the
    // original once we promote all its current uses.
    F->addSemanticsAttr(semantics::MOVEONLY_DELETE_IF_UNUSED);

    // If any of our promoted callee arg indices were originally noncopyable let
    // boxes, convert them from having escaping to having non-escaping
    // semantics.
    for (unsigned index : PromotedCalleeArgIndices) {
      if (F->getArgument(index)->getType().isBoxedNonCopyableType(*F)) {
        auto boxType = F->getArgument(index)->getType().castTo<SILBoxType>();
        bool isMutable = boxType->getLayout()->getFields()[0].isMutable();
        auto checkKind =
            isMutable ? MarkMustCheckInst::CheckKind::ConsumableAndAssignable
                      : MarkMustCheckInst::CheckKind::NoConsumeOrAssign;
        hoistMarkMustCheckInsts(ClonedFn->getArgument(index), checkKind);
      }
    }
  }

  // Now create the new ApplySite using the cloned function.
  SmallVector<SILValue, 16> Args;

  ValueLifetimeAnalysis::Frontier PAFrontier;

  // Promote the arguments that need promotion.
  for (auto &O : Apply.getArgumentOperands()) {
    auto CalleeArgIndex = ApplySite(O.getUser()).getCalleeArgIndex(O);
    if (!count(PromotedCalleeArgIndices, CalleeArgIndex)) {
      Args.push_back(O.get());
      continue;
    }

    SILValue Box = O.get();
    assert((isa<SingleValueInstruction>(Box) && isa<AllocBoxInst>(Box) ||
            isa<CopyValueInst>(Box) ||
            isa<MarkUninitializedInst>(Box) ||
            isa<BeginBorrowInst>(Box) ||
            isa<SILFunctionArgument>(Box)) &&
           "Expected either an alloc box or a copy of an alloc box or a "
           "function argument");
    SILBuilderWithScope::insertAfter(Box, [&](SILBuilder &B) {
      Args.push_back(B.createProjectBox(Box.getLoc(), Box, 0));
    });

    // For a partial_apply, if this argument is promoted, it is a box that we're
    // turning into an address because we've proven we can keep this value on
    // the stack. The partial_apply had ownership of this box so we must now
    // release it explicitly when the partial_apply is released.
    if (Apply.getKind() == ApplySiteKind::PartialApplyInst) {
      auto *PAI = cast<PartialApplyInst>(Apply);
      // If it's already been stack promoted, then the stack closure only
      // borrows its captures, and we don't need to adjust capture lifetimes.
      if (!PAI->isOnStack()) {
        if (PAFrontier.empty()) {
          SmallVector<SILInstruction *, 8> users;
          InstructionWorklist worklist(PAI->getFunction());
          worklist.push(PAI);
          while (auto *inst = worklist.pop()) {
            auto *svi = cast<SingleValueInstruction>(inst);
            for (auto *use : svi->getUses()) {
              auto *user = use->getUser();
              SingleValueInstruction *svi;
              // A copy_value produces a value with a new lifetime on which the
              // captured alloc_box's lifetime depends.  If the transformation
              // were only to create a destroy_value of the alloc_box (and to
              // rewrite the closure not to consume it), the alloc_box would be
              // kept alive by the copy_value.  The transformation does more,
              // however: it rewrites the alloc_box as an alloc_stack, creating
              // the alloc_stack/dealloc_stack instructions where the alloc_box/
              // destroy_value instructions are respectively.  The copy_value
              // can't keep the alloc_stack alive.
              if ((svi = dyn_cast<CopyValueInst>(user)) ||
                  (svi = dyn_cast<MoveValueInst>(user))) {
                worklist.push(svi);
              }
              users.push_back(user);
            }
          }
          ValueLifetimeAnalysis VLA(PAI, users);
          pass.CFGChanged |= !VLA.computeFrontier(
              PAFrontier, ValueLifetimeAnalysis::AllowToModifyCFG);
          assert(!PAFrontier.empty() &&
                 "partial_apply must have at least one use "
                 "to release the returned function");
        }

        // Insert destroys of the box at each point where the partial_apply
        // becomes dead.
        for (SILInstruction *FrontierInst : PAFrontier) {
          SILBuilderWithScope Builder(FrontierInst);
          Builder.emitDestroyValueOperation(Apply.getLoc(), Box);
        }
      }
    }
  }

  auto ApplyInst = Apply.getInstruction();
  SILBuilderWithScope Builder(ApplyInst);

  // Build the function_ref and ApplySite.
  SILValue FunctionRef = Builder.createFunctionRef(Apply.getLoc(), ClonedFn);
  switch (Apply.getKind()) {
  case ApplySiteKind::PartialApplyInst: {
    auto *PAI = cast<PartialApplyInst>(ApplyInst);
    return Builder.createPartialApply(
        Apply.getLoc(), FunctionRef, Apply.getSubstitutionMap(), Args,
        PAI->getType().getAs<SILFunctionType>()->getCalleeConvention(),
        PAI->isOnStack(),
        GenericSpecializationInformation::create(ApplyInst, Builder));
  }
  case ApplySiteKind::ApplyInst:
    return Builder.createApply(
        Apply.getLoc(), FunctionRef, Apply.getSubstitutionMap(), Args,
        Apply.getApplyOptions(),
        GenericSpecializationInformation::create(ApplyInst, Builder));
  case ApplySiteKind::BeginApplyInst:
    return Builder.createBeginApply(
        Apply.getLoc(), FunctionRef, Apply.getSubstitutionMap(), Args,
        Apply.getApplyOptions(),
        GenericSpecializationInformation::create(ApplyInst, Builder));
  case ApplySiteKind::TryApplyInst: {
    auto TAI = cast<TryApplyInst>(Apply);
    return Builder.createTryApply(
        Apply.getLoc(), FunctionRef, Apply.getSubstitutionMap(), Args,
        TAI->getNormalBB(), TAI->getErrorBB(),
        TAI->getApplyOptions(),
        GenericSpecializationInformation::create(ApplyInst, Builder));
  }
  }
  llvm_unreachable("unhandled apply inst kind!");
}

static void rewriteApplySites(AllocBoxToStackState &pass) {
  swift::SmallBlotMapVector<ApplySite, ArgIndexList, 8> AppliesToSpecialize;
  ArgIndexList Indices;

  // Build a map from the ApplySite to the indices of the operands
  // that will be promoted in our rewritten version.
  for (auto *O : pass.PromotedOperands) {
    auto User = O->getUser();
    auto Apply = ApplySite(User);

    auto CalleeArgIndexNumber = Apply.getCalleeArgIndex(*O);

    Indices.clear();
    Indices.push_back(CalleeArgIndexNumber);

    // AllocBoxStack opt promotes boxes passed to a chain of applies when it is
    // safe to do so. All such applies have to be specialized to take pointer
    // arguments instead of box arguments. This has to be done in dfs order.

    // PromotedOperands is already populated in dfs order by
    // `recursivelyFindBoxOperandsPromotableToAddress` w.r.t a single alloc_box.
    // AppliesToSpecialize is then populated in the order of PromotedOperands.
    // If multiple alloc_boxes are passed to the same apply instruction, then
    // the apply instruction can appear multiple times in AppliesToSpecialize.
    // Only its last appearance is maintained and previous appearances are
    // blotted.
    auto iterAndSuccess =
        AppliesToSpecialize.insert(std::make_pair(Apply, Indices));
    if (!iterAndSuccess.second) {
      // Blot the previously inserted apply and insert at the end with updated
      // indices
      auto OldIndices = iterAndSuccess.first->value().second;
      OldIndices.push_back(CalleeArgIndexNumber);
      AppliesToSpecialize.erase(iterAndSuccess.first);
      AppliesToSpecialize.insert(std::make_pair(Apply, OldIndices));
    }
  }

  // Clone the referenced function of each ApplySite, removing the
  // operands that we will not need, and remove the existing
  // ApplySite.
  SILOptFunctionBuilder FuncBuilder(*pass.T);
  for (auto &It : AppliesToSpecialize) {
    if (!It.has_value()) {
      continue;
    }
    auto Apply = It.value().first;
    auto Indices = It.value().second;
    // Sort the indices and unique them.
    sortUnique(Indices);

    auto *Replacement = specializeApplySite(FuncBuilder, Apply, Indices, pass);
    assert(Apply.getKind() == ApplySite(Replacement).getKind());
    Apply.getInstruction()->replaceAllUsesPairwiseWith(Replacement);

    auto *FRI = cast<FunctionRefInst>(Apply.getCallee());
    Apply.getInstruction()->eraseFromParent();

    // TODO: Erase from module if there are no more uses.
    if (FRI->use_empty())
      FRI->eraseFromParent();
  }
}

/// Clone closure bodies and rewrite partial applies. Returns the number of
/// alloc_box allocations promoted.
static unsigned rewritePromotedBoxes(AllocBoxToStackState &pass) {
  // First we'll rewrite any ApplySite that we can to remove
  // the box container pointer from the operands.
  rewriteApplySites(pass);

  unsigned Count = 0;
  auto rend = pass.Promotable.rend();
  for (auto I = pass.Promotable.rbegin(); I != rend; ++I) {
    auto *ABI = *I;
    if (rewriteAllocBoxAsAllocStack(ABI)) {
      ++Count;
      ABI->eraseFromParent();
    }
  }
  return Count;
}

namespace {
class AllocBoxToStack : public SILFunctionTransform {
  /// The entry point to the transformation.
  void run() override {
    // Don't rerun on deserialized functions. Nothing should have changed.
    if (getFunction()->wasDeserializedCanonical())
      return;

    AllocBoxToStackState pass(this);
    for (auto &BB : *getFunction()) {
      for (auto &I : BB)
        if (auto *ABI = dyn_cast<AllocBoxInst>(&I))
          if (canPromoteAllocBox(ABI, pass.PromotedOperands))
            pass.Promotable.push_back(ABI);
    }

    if (!pass.Promotable.empty()) {
      auto Count = rewritePromotedBoxes(pass);
      NumStackPromoted += Count;
      if (Count) {
        if (StackNesting::fixNesting(getFunction()) == StackNesting::Changes::CFG)
          pass.CFGChanged = true;
      }

      invalidateAnalysis(
          pass.CFGChanged
              ? SILAnalysis::InvalidationKind::FunctionBody
              : SILAnalysis::InvalidationKind::CallsAndInstructions);
    }
  }
};
} // end anonymous namespace

SILTransform *swift::createAllocBoxToStack() {
  return new AllocBoxToStack();
}
