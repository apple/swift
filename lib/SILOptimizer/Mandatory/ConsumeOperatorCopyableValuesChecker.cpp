//===--- ConsumeOperatorCopyableValuesChecker.cpp -------------------------===//
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

#define DEBUG_TYPE "sil-consume-operator-copyable-values-checker"

#include "swift/AST/DiagnosticsSIL.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/Analysis/ClosureScope.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/LoopAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"

using namespace swift;

static llvm::cl::opt<bool> DisableUnhandledMoveDiagnostic(
    "sil-consume-operator-disable-unknown-move-diagnostic");

//===----------------------------------------------------------------------===//
//                            Diagnostic Utilities
//===----------------------------------------------------------------------===//

template <typename... T, typename... U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
                     U &&...args) {
  Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

//===----------------------------------------------------------------------===//
//                             Canonical Liveness
//===----------------------------------------------------------------------===//

namespace {

struct CheckerLivenessInfo {
  GraphNodeWorklist<SILValue, 8> defUseWorklist;
  SmallSetVector<Operand *, 8> consumingUse;
  SmallSetVector<SILInstruction *, 8> nonLifetimeEndingUsesInLiveOut;
  SmallVector<Operand *, 8> interiorPointerTransitiveUses;
  BitfieldRef<DiagnosticPrunedLiveness> liveness;

  CheckerLivenessInfo() : nonLifetimeEndingUsesInLiveOut() {}

  void initDef(SILValue def) {
    liveness->initializeDef(def);
    defUseWorklist.insert(def);
  }

  /// Compute the liveness for any value currently in the defUseWorklist.
  ///
  /// Returns false if we found any escapes. Returns true if no escape uses were
  /// found. NOTE: Even if we return false, we still visit all uses and compute
  /// liveness normally. We may just be missing uses through the escaping use.
  bool compute();

  void clear() {
    defUseWorklist.clear();
    consumingUse.clear();
    interiorPointerTransitiveUses.clear();
    nonLifetimeEndingUsesInLiveOut.clear();
  }
};

} // end anonymous namespace

bool CheckerLivenessInfo::compute() {
  LLVM_DEBUG(llvm::dbgs() << "LivenessVisitor Begin!\n");
  while (SILValue value = defUseWorklist.pop()) {
    LLVM_DEBUG(llvm::dbgs() << "New Value: " << value);
    SWIFT_DEFER { LLVM_DEBUG(llvm::dbgs() << "Finished Value: " << value); };

    for (Operand *use : value->getUses()) {
      auto *user = use->getUser();
      LLVM_DEBUG(llvm::dbgs() << "    User: " << *user);
      // Recurse through copies.
      if (auto *copy = dyn_cast<CopyValueInst>(user)) {
        LLVM_DEBUG(llvm::dbgs() << "    Copy Value. Looking through it\n");
        defUseWorklist.insert(copy);
        continue;
      }

      LLVM_DEBUG(llvm::dbgs() << "    OperandOwnership: "
                              << use->getOperandOwnership() << '\n');
      switch (use->getOperandOwnership()) {
      case OperandOwnership::NonUse:
        break;
      case OperandOwnership::TrivialUse:
        llvm_unreachable("this operand cannot handle ownership");

      // Conservatively treat a conversion to an unowned value as a pointer
      // escape. Is it legal to canonicalize ForwardingUnowned?
      case OperandOwnership::ForwardingUnowned:
      case OperandOwnership::PointerEscape:
        // This is an escape but it is up to the user to handle this, move
        // checking stops here.
        break;
      case OperandOwnership::InstantaneousUse:
      case OperandOwnership::UnownedInstantaneousUse:
      case OperandOwnership::BitwiseEscape:
        liveness->updateForUse(user, /*lifetimeEnding*/ false);
        break;
      case OperandOwnership::ForwardingConsume:
        consumingUse.insert(use);
        liveness->updateForUse(user, /*lifetimeEnding*/ true);
        break;
      case OperandOwnership::DestroyingConsume:
        // destroy_value does not force pruned liveness (but store etc. does).
        if (!isa<DestroyValueInst>(user)) {
          liveness->updateForUse(user, /*lifetimeEnding*/ true);
        }
        consumingUse.insert(use);
        break;
      case OperandOwnership::Borrow: {
        if (auto *bbi = dyn_cast<BeginBorrowInst>(user)) {
          // If we have a lexical begin_borrow, we are going to check its uses
          // separately and emit diagnostics for it. So we just need to add the
          // liveness of the begin_borrow.
          //
          // NOTE: We know that semantically the use lexical lifetime must have
          // a separate lifetime from the base lexical lifetime that we are
          // processing. We do not want to include those uses as transitive uses
          // of our base lexical lifetime. We just want to treat the formation
          // of the new variable as a use. Thus we only include the begin_borrow
          // itself as the use.
          if (bbi->isLexical()) {
            liveness->updateForUse(bbi, false /*lifetime ending*/);
          } else {
            // Otherwise, try to update liveness for a borrowing operand
            // use. This will make it so that we add the end_borrows of the
            // liveness use. If we have a reborrow here, we will bail.
            if (liveness->updateForBorrowingOperand(use) !=
                InnerBorrowKind::Contained) {
              return false;
            }
          }
        }
        break;
      }
      case OperandOwnership::GuaranteedForwarding:
        // A forwarding borrow is validated as part of its parent borrow. So
        // just mark it as extending liveness and look through it.
        liveness->updateForUse(user, /*lifetimeEnding*/ false);
        ForwardingOperand(use).visitForwardedValues([&](SILValue result) {
          if (SILArgument::isTerminatorResult(result)) {
            return true;
          }
          if (result->getOwnershipKind() == OwnershipKind::Guaranteed)
            defUseWorklist.insert(result);
          return true;
        });
        break;
      case OperandOwnership::InteriorPointer: {
        // An interior pointer user extends liveness until the end of the
        // interior pointer section.
        //
        // TODO: We really should have all OperandOwnership::InteriorPointer
        // instructions be valid to pass to InteriorPointerOperand. Some
        // builtins do not do it today and it is probably a misuse of the
        // system. That being said, lets do our best here.
        if (auto operand = InteriorPointerOperand(use)) {
          auto addrUseKind =
              operand.findTransitiveUses(&interiorPointerTransitiveUses);
          (void)addrUseKind;
          while (!interiorPointerTransitiveUses.empty()) {
            auto *addrUse = interiorPointerTransitiveUses.pop_back_val();
            liveness->updateForUse(addrUse->getUser(),
                                   /*lifetimeEnding*/ false);
          }
        }
        break;
      }
      case OperandOwnership::EndBorrow:
        // Don't care about this use.
        break;
      case OperandOwnership::Reborrow:
        // Reborrows do not occur this early in the pipeline.
        llvm_unreachable(
            "Reborrows do not occur until we optimize later in the pipeline");
      }
    }
  }

  // We succeeded if we reached this point since we handled all uses.
  return true;
}

//===----------------------------------------------------------------------===//
//                                 Main Pass
//===----------------------------------------------------------------------===//

namespace {

struct ConsumeOperatorCopyableValuesChecker {
  SILFunction *fn;
  CheckerLivenessInfo livenessInfo;
  DominanceInfo *dominanceToUpdate;
  SILLoopInfo *loopInfoToUpdate;

  ConsumeOperatorCopyableValuesChecker(SILFunction *fn)
      : fn(fn), dominanceToUpdate(nullptr), loopInfoToUpdate(nullptr) {}

  void setDominanceToUpdate(DominanceInfo *newDFI) {
    dominanceToUpdate = newDFI;
  }

  void setLoopInfoToUpdate(SILLoopInfo *newLFI) { loopInfoToUpdate = newLFI; }

  bool check();

  void emitDiagnosticForMove(SILValue borrowedValue,
                             StringRef borrowedValueName, MoveValueInst *mvi);
};

} // namespace

static SourceLoc getSourceLocFromValue(SILValue value) {
  if (auto *defInst = value->getDefiningInstruction())
    return defInst->getLoc().getSourceLoc();
  if (auto *arg = dyn_cast<SILFunctionArgument>(value))
    return arg->getDecl()->getLoc();
  llvm_unreachable("Do not know how to get source loc for value?!");
}

void ConsumeOperatorCopyableValuesChecker::emitDiagnosticForMove(
    SILValue borrowedValue, StringRef borrowedValueName, MoveValueInst *mvi) {
  auto &astContext = fn->getASTContext();

  // First we emit the main error and then the note on where the move was.
  diagnose(astContext, getSourceLocFromValue(borrowedValue),
           diag::sil_movechecking_value_used_after_consume,
           borrowedValueName);
  diagnose(astContext, mvi->getLoc().getSourceLoc(),
           diag::sil_movechecking_consuming_use_here);

  // Then we do a bit of work to figure out where /all/ of the later uses than
  // mvi are so we can emit notes to the user telling them this is a problem
  // use. We can do a little more work here since we already know that we are
  // going to be emitting a diagnostic and thus later parts of the compiler are
  // not going to run. First we look for uses in the same block as our move.
  auto *mviBlock = mvi->getParent();
  auto mviBlockLiveness = livenessInfo.liveness->getBlockLiveness(mviBlock);
  switch (mviBlockLiveness) {
  case PrunedLiveBlocks::Dead:
    llvm_unreachable("We should never see this");
  case PrunedLiveBlocks::LiveWithin: {
    // The boundary was within our block. We need to search for uses later than
    // us and emit a diagnostic upon them and then return. We leave the rest of
    // the function for the implementation of the LiveOutCase.
    //
    // NOTE: This does mean that once the user fixes this use, they will get
    // additional errors that we did not diagnose before. We do this to simplify
    // the implementation noting that the program in either case will not
    // compile meaning correctness will be maintained despite this
    // implementation choice.
    for (SILInstruction &inst :
         make_range(std::next(mvi->getIterator()), mviBlock->end())) {
      switch (livenessInfo.liveness->isInterestingUser(&inst)) {
      case PrunedLiveness::NonUser:
        break;
      case PrunedLiveness::NonLifetimeEndingUse:
      case PrunedLiveness::LifetimeEndingUse:
        LLVM_DEBUG(llvm::dbgs() << "Emitting note for in block use: " << inst);
        diagnose(astContext, inst.getLoc().getSourceLoc(),
                 diag::sil_movechecking_nonconsuming_use_here);
        break;
      }
    }
    return;
  }
  case PrunedLiveBlocks::LiveOut: {
    // The boundary was later than us, we need to do a full on CFG search, which
    // we do below.
    break;
  }
  }

  // Just to check for dumb mistakes, assert we are LiveOut here.
  assert(mviBlockLiveness == PrunedLiveBlocks::LiveOut &&
         "We are handling only the live out case here. The rest of the cases "
         "were handled in the switch above and return early upon success");

  // Ok, our boundary was later, so we need to search the CFG along successor
  // edges starting at the successors's of our move function block
  BasicBlockWorklist worklist(mvi->getFunction());
  for (auto *succBlock : mvi->getParent()->getSuccessorBlocks()) {
    worklist.pushIfNotVisited(succBlock);
  }

  // In order to make sure that we do not miss uses that are within loops, we
  // maintain a list of all user sets.
  //
  // DISCUSSION: The issue is that a block at a deeper loop level than our def,
  // even if it contained the use that triggered the issue will be LiveOut. So
  // when we see a live out block, we perform this extra check and emit a
  // diagnostic if needed.
  BasicBlockSet usesToCheckForInLiveOutBlocks(mvi->getFunction());
  for (auto *user : livenessInfo.nonLifetimeEndingUsesInLiveOut)
    usesToCheckForInLiveOutBlocks.insert(user->getParent());
  for (auto *consumingUse : livenessInfo.consumingUse) {
    // We ignore consuming uses that are destroy_value since in our model they
    // do not provide liveness.
    if (!isa<DestroyValueInst>(consumingUse->getUser()))
      usesToCheckForInLiveOutBlocks.insert(consumingUse->getParentBlock());
  }

  while (auto *block = worklist.pop()) {
    // First do a quick check if we are not a live out block. If so, the
    // boundary was within the block. We need to search for interesting uses in
    // the block and then emit diagnostics upon them. We then continue without
    // adding successors since we do not need to look further than the pruned
    // liveness boundary for uses.
    if (PrunedLiveBlocks::LiveOut !=
        livenessInfo.liveness->getBlockLiveness(block)) {
      for (SILInstruction &inst : *block) {
        switch (livenessInfo.liveness->isInterestingUser(&inst)) {
        case PrunedLiveness::NonUser:
          break;
        case PrunedLiveness::NonLifetimeEndingUse:
        case PrunedLiveness::LifetimeEndingUse:
          LLVM_DEBUG(llvm::dbgs()
                     << "(3) Emitting diagnostic for user: " << inst);
          diagnose(astContext, inst.getLoc().getSourceLoc(),
                   diag::sil_movechecking_nonconsuming_use_here);
          break;
        }
      }
      continue;
    }

    // Otherwise, we have a live out block. First before we do anything, add the
    // successors of this block to the worklist.
    for (auto *succBlock : block->getSuccessorBlocks())
      worklist.pushIfNotVisited(succBlock);

    // Then check if we have any of those deeper loop nest uses. If not, we are
    // done with this block and continue...
    if (!usesToCheckForInLiveOutBlocks.contains(block))
      continue;

    // Ok! This is a live out block with a use we need to emit an error for . We
    // know it is reachable from the move since we are walking successors from
    // the move block. Of course, if we do not have any such uses... just
    // continue.
    for (SILInstruction &inst : *block) {
      if (livenessInfo.nonLifetimeEndingUsesInLiveOut.contains(&inst)) {
        LLVM_DEBUG(llvm::dbgs()
                   << "(1) Emitting diagnostic for user: " << inst);
        diagnose(astContext, inst.getLoc().getSourceLoc(),
                 diag::sil_movechecking_nonconsuming_use_here);
        continue;
      }

      for (auto &op : inst.getAllOperands()) {
        if (livenessInfo.consumingUse.contains(&op)) {
          // If one of our in loop moves is ourselves, then we know that our
          // original value is outside of the loop and thus we have a loop
          // carry dataflow violation.
          if (mvi == &inst) {
            diagnose(astContext, inst.getLoc().getSourceLoc(),
                     diag::sil_movechecking_consumed_in_loop_here);
            continue;
          }
          // We ignore consuming uses that are destroy_value since in our model
          // they do not provide liveness.
          if (isa<DestroyValueInst>(inst))
            continue;

          LLVM_DEBUG(llvm::dbgs()
                     << "(2) Emitting diagnostic for user: " << inst);
          diagnose(astContext, inst.getLoc().getSourceLoc(),
                   diag::sil_movechecking_nonconsuming_use_here);
        }
      }
    }
  }
}

bool ConsumeOperatorCopyableValuesChecker::check() {
  SmallSetVector<SILValue, 32> valuesToCheck;

  for (auto *arg : fn->getEntryBlock()->getSILFunctionArguments()) {
    if (arg->getOwnershipKind() == OwnershipKind::Owned &&
        !arg->getType().isMoveOnly()) {
      LLVM_DEBUG(llvm::dbgs() << "Found owned arg to check: " << *arg);
      valuesToCheck.insert(arg);
    }
  }

  for (auto &block : *fn) {
    for (auto &ii : block) {
      if (auto *bbi = dyn_cast<BeginBorrowInst>(&ii)) {
        if (bbi->isLexical() && !bbi->getType().isMoveOnly()) {
          LLVM_DEBUG(llvm::dbgs()
                     << "Found lexical lifetime to check: " << *bbi);
          valuesToCheck.insert(bbi);
        }
        continue;
      }
    }
  }

  if (valuesToCheck.empty()) {
    LLVM_DEBUG(llvm::dbgs() << "No values to check! Exiting early!\n");
    return false;
  }

  LLVM_DEBUG(llvm::dbgs()
             << "Found at least one value to check, performing checking.\n");
  auto valuesToProcess =
      llvm::makeArrayRef(valuesToCheck.begin(), valuesToCheck.end());
  auto &mod = fn->getModule();

  // If we do not emit any diagnostics, we need to put in a break after each dbg
  // info carrying inst for a lexical value that we find a move on. This ensures
  // that we avoid a behavior today in SelectionDAG that causes dbg info addr to
  // be always sunk to the end of a block.
  //
  // TODO: We should add llvm.dbg.addr support for fastisel and also teach
  // CodeGen how to handle llvm.dbg.addr better.
  while (!valuesToProcess.empty()) {
    BitfieldRef<DiagnosticPrunedLiveness>::StackState livenessBitfieldContainer(
        livenessInfo.liveness, fn, nullptr,
        &livenessInfo.nonLifetimeEndingUsesInLiveOut);

    auto lexicalValue = valuesToProcess.front();
    valuesToProcess = valuesToProcess.drop_front(1);
    LLVM_DEBUG(llvm::dbgs() << "Visiting: " << *lexicalValue);

    // Then compute liveness.
    SWIFT_DEFER { livenessInfo.clear(); };
    livenessInfo.initDef(lexicalValue);

    // We only fail to optimize if for some reason we hit reborrows. This is
    // temporary since we really should just ban reborrows in Raw SIL.
    bool canOptimize = livenessInfo.compute();
    if (!canOptimize)
      continue;

    // Then look at all of our found consuming uses. See if any of these are
    // _move that are within the boundary.
    bool foundMove = false;
    auto dbgVarInst = DebugVarCarryingInst::getFromValue(lexicalValue);
    StringRef varName = DebugVarCarryingInst::getName(dbgVarInst);
    for (auto *use : livenessInfo.consumingUse) {
      if (auto *mvi = dyn_cast<MoveValueInst>(use->getUser())) {
        // Only emit diagnostics if our move value allows us to.
        if (!mvi->getAllowDiagnostics())
          continue;

        // Now that we know we may emit diagnostics for this, unset allows
        // diagnostics so that we skip these when we search at the end for
        // unvisited move_value [allows_diagnostics].
        mvi->setAllowsDiagnostics(false);

        LLVM_DEBUG(llvm::dbgs() << "Move Value: " << *mvi);
        if (livenessInfo.liveness->isWithinBoundary(mvi)) {
          LLVM_DEBUG(llvm::dbgs() << "    WithinBoundary: Yes!\n");
          emitDiagnosticForMove(lexicalValue, varName, mvi);
        } else {
          LLVM_DEBUG(llvm::dbgs() << "    WithinBoundary: No!\n");
          if (auto varInfo = dbgVarInst.getVarInfo()) {
            auto *next = mvi->getNextInstruction();
            SILBuilderWithScope builder(next);
            // We need to make sure any undefs we put in are the same loc/debug
            // scope as our original so that the backend treats them as
            // referring to the same "debug entity".
            builder.setCurrentDebugScope(dbgVarInst->getDebugScope());
            builder.createDebugValue(
                dbgVarInst->getLoc(),
                SILUndef::get(mvi->getOperand()->getType(), mod), *varInfo,
                false /*poison*/, true /*moved*/);
          }
        }
        foundMove = true;
      }
    }

    // If we found a move, mark our debug var inst as having a moved value. This
    // ensures we emit llvm.dbg.addr instead of llvm.dbg.declare in IRGen.
    if (foundMove) {
      dbgVarInst.markAsMoved();
    }
  }

  return false;
}

//===----------------------------------------------------------------------===//
//                        Unsupported Use Case Errors
//===----------------------------------------------------------------------===//

static void emitUnsupportedUseCaseError(MoveValueInst *mvi) {
  auto &astContext = mvi->getModule().getASTContext();
  auto diag = diag::sil_movekillscopyablevalue_move_applied_to_unsupported_move;
  diagnose(astContext, mvi->getLoc().getSourceLoc(), diag);
  mvi->setAllowsDiagnostics(false);
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class ConsumeOperatorCopyableValuesCheckerPass : public SILFunctionTransform {
  void run() override {
    auto *fn = getFunction();

    // Only run this pass if the move only language feature is enabled.
    if (!fn->getASTContext().supportsMoveOnlyTypes())
      return;

    // Don't rerun diagnostics on deserialized functions.
    if (fn->wasDeserializedCanonical())
      return;

    assert(fn->getModule().getStage() == SILStage::Raw &&
           "Should only run on Raw SIL");

    LLVM_DEBUG(llvm::dbgs() << "*** Checking moved values in fn: "
                            << getFunction()->getName() << '\n');

    ConsumeOperatorCopyableValuesChecker checker(getFunction());

    // If we already had dominance or loop info generated, update them when
    // splitting blocks.
    auto *dominanceAnalysis = getAnalysis<DominanceAnalysis>();
    if (dominanceAnalysis->hasFunctionInfo(fn))
      checker.setDominanceToUpdate(dominanceAnalysis->get(fn));
    auto *loopAnalysis = getAnalysis<SILLoopAnalysis>();
    if (loopAnalysis->hasFunctionInfo(fn))
      checker.setLoopInfoToUpdate(loopAnalysis->get(fn));

    if (checker.check()) {
      AnalysisPreserver preserveDominance(dominanceAnalysis);
      AnalysisPreserver preserveLoop(loopAnalysis);
      invalidateAnalysis(
          SILAnalysis::InvalidationKind::BranchesAndInstructions);
    }

    // Now search through our function one last time and:
    //
    // 1. Given any move_value on a move only type, just unset the allows
    //    diagnostics flag. The move checker will have emitted any errors caused
    //    by our move [allows_diagnostic] earlier in the compilation pipeline.
    //
    // 2. Any move_value [allows_diagnostics] that remain that are not on a move
    //    only type are ones that we did not know how to check so emit a
    //    diagnostic so the user doesn't assume that they have guarantees.
    //
    // TODO: Emit specific diagnostics here (e.x.: _move of global).
    for (auto &block : *fn) {
      for (auto &inst : block) {
        if (auto *mvi = dyn_cast<MoveValueInst>(&inst)) {
          if (mvi->getAllowDiagnostics()) {
            if (mvi->getType().isMoveOnly()) {
              mvi->setAllowsDiagnostics(false);
              continue;
            }

            if (!DisableUnhandledMoveDiagnostic)
              emitUnsupportedUseCaseError(mvi);
          }
        }
      }
    }
  }
};

} // anonymous namespace

SILTransform *swift::createConsumeOperatorCopyableValuesChecker() {
  return new ConsumeOperatorCopyableValuesCheckerPass();
}
