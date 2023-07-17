//===--- MoveOnlyChecker.cpp ----------------------------------------------===//
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

#define DEBUG_TYPE "sil-move-only-checker"

#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/Basic/STLExtras.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/FieldSensitivePrunedLiveness.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/NodeBits.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PostOrder.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILLocation.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/StackList.h"
#include "swift/SILOptimizer/Analysis/ClosureScope.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "swift/SILOptimizer/Utils/SILSSAUpdater.h"
#include "clang/AST/DeclTemplate.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntervalMap.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/RecyclingAllocator.h"

#include "MoveOnlyBorrowToDestructureUtils.h"
#include "MoveOnlyDiagnostics.h"
#include "MoveOnlyObjectCheckerUtils.h"

using namespace swift;
using namespace swift::siloptimizer;

//===----------------------------------------------------------------------===//
//                Mark Must Check Candidate Search for Objects
//===----------------------------------------------------------------------===//

bool swift::siloptimizer::searchForCandidateObjectMarkMustChecks(
    SILFunction *fn,
    SmallSetVector<MarkMustCheckInst *, 32> &moveIntroducersToProcess,
    DiagnosticEmitter &emitter) {
  bool localChanged = false;
  for (auto &block : *fn) {
    for (auto ii = block.begin(), ie = block.end(); ii != ie;) {
      auto *mmci = dyn_cast<MarkMustCheckInst>(&*ii);
      ++ii;

      if (!mmci || !mmci->hasMoveCheckerKind() || !mmci->getType().isObject())
        continue;

      // Handle guaranteed/owned move arguments and values.
      //
      // We are pattern matching against these patterns:
      //
      // bb0(%0 : @guaranteed $T):
      //   %1 = copy_value %0
      //   %2 = mark_must_check [no_consume_or_assign] %1
      // bb0(%0 : @owned $T):
      //   %1 = mark_must_check [no_consume_or_assign] %2
      //
      // This is forming a let or an argument.
      // bb0:
      //   %1 = move_value [lexical] %0
      //   %2 = mark_must_check [consumable_and_assignable] %1
      //
      // This occurs when SILGen materializes a temporary move only value?
      // bb0:
      //   %1 = begin_borrow [lexical] %0
      //   %2 = copy_value %1
      //   %3 = mark_must_check [no_consume_or_assign] %2
      if (mmci->getOperand()->getType().isMoveOnly() &&
          !mmci->getOperand()->getType().isMoveOnlyWrapped()) {
        if (auto *cvi = dyn_cast<CopyValueInst>(mmci->getOperand())) {
          if (auto *arg = dyn_cast<SILFunctionArgument>(cvi->getOperand())) {
            if (arg->getOwnershipKind() == OwnershipKind::Guaranteed) {
              moveIntroducersToProcess.insert(mmci);
              continue;
            }
          }

          // In the case we have a resilient argument, we may have the following pattern:
          //
          // bb0(%0 : $*Type): // in_guaranteed
          //   %1 = load_borrow %0
          //   %2 = copy_value
          //   %3 = mark_must_check [no_copy_or_assign]
          if (auto *lbi = dyn_cast<LoadBorrowInst>(cvi->getOperand())) {
            if (auto *arg = dyn_cast<SILFunctionArgument>(lbi->getOperand())) {
              if (arg->getKnownParameterInfo().isIndirectInGuaranteed()) {
                moveIntroducersToProcess.insert(mmci);
                continue;
              }
            }
          }

          if (auto *bbi = dyn_cast<BeginBorrowInst>(cvi->getOperand())) {
            if (bbi->isLexical()) {
              moveIntroducersToProcess.insert(mmci);
              continue;
            }
          }
        }

        // Any time we have a move_value, we can process it.
        if (auto *mvi = dyn_cast<MoveValueInst>(mmci->getOperand())) {
          moveIntroducersToProcess.insert(mmci);
          continue;
        }

        if (auto *arg = dyn_cast<SILFunctionArgument>(mmci->getOperand())) {
          if (arg->getOwnershipKind() == OwnershipKind::Owned) {
            moveIntroducersToProcess.insert(mmci);
            continue;
          }
        }
      }

      // Handle guaranteed arguments.
      //
      // We are pattern matching this pattern:
      //
      // bb0(%0 : @guaranteed $T):
      //   %1 = copyable_to_moveonlywrapper [guaranteed] %0
      //   %2 = copy_value %1
      //   %3 = mark_must_check [no_consume_or_assign] %2
      //
      // NOTE: Unlike with owned arguments, we do not need to insert a
      // begin_borrow lexical since the lexical value comes from the guaranteed
      // argument itself.
      //
      // NOTE: When we are done checking, we will eliminate the copy_value,
      // mark_must_check inst to leave the IR in a guaranteed state.
      if (auto *cvi = dyn_cast<CopyValueInst>(mmci->getOperand())) {
        if (auto *cvt = dyn_cast<CopyableToMoveOnlyWrapperValueInst>(
                cvi->getOperand())) {
          if (auto *arg = dyn_cast<SILFunctionArgument>(cvt->getOperand())) {
            if (arg->isNoImplicitCopy() &&
                arg->getOwnershipKind() == OwnershipKind::Guaranteed) {
              moveIntroducersToProcess.insert(mmci);
              continue;
            }
          }
        }
      }

      // Handle trivial arguments and values.
      //
      // In the instruction stream this looks like:
      //
      // bb0(%0 : $Trivial):
      //  %1 = copyable_to_moveonlywrapper [owned] %0
      //  %2 = move_value [lexical] %1
      //  %3 = mark_must_check [consumable_and_assignable] %2
      //
      // *OR*
      //
      // bb0(%0 : $Trivial):
      //  %1 = copyable_to_moveonlywrapper [owned] %0
      //  %2 = move_value [lexical] %1
      //  %3 = mark_must_check [no_consume_or_assign] %2
      //
      // We are relying on a structural SIL requirement that %0 has only one
      // use, %1. This is validated by the SIL verifier. In this case, we need
      // the move_value [lexical] to ensure that we get a lexical scope for the
      // non-trivial value.
      if (auto *mvi = dyn_cast<MoveValueInst>(mmci->getOperand())) {
        if (mvi->isLexical()) {
          if (auto *cvt = dyn_cast<CopyableToMoveOnlyWrapperValueInst>(
                  mvi->getOperand())) {
            if (cvt->getOperand()->getType().isTrivial(*fn)) {
              moveIntroducersToProcess.insert(mmci);
              continue;
            }
          }
        }
      }

      // Handle owned arguments.
      //
      // We are pattern matching this:
      //
      // bb0(%0 : @owned $T):
      //   %1 = copyable_to_moveonlywrapper [owned] %0
      //   %2 = move_value [lexical] %1
      //   %3 = mark_must_check [consumable_and_assignable_owned] %2
      if (auto *mvi = dyn_cast<MoveValueInst>(mmci->getOperand())) {
        if (mvi->isLexical()) {
          if (auto *cvt = dyn_cast<CopyableToMoveOnlyWrapperValueInst>(
                  mvi->getOperand())) {
            if (auto *arg = dyn_cast<SILFunctionArgument>(cvt->getOperand())) {
              if (arg->isNoImplicitCopy()) {
                moveIntroducersToProcess.insert(mmci);
                continue;
              }
            }
          }
        }
      }

      // Handle non-trivial values.
      //
      // We are looking for the following pattern:
      //
      //  %1 = begin_borrow [lexical] %0
      //  %2 = copy_value %1
      //  %3 = copyable_to_moveonlywrapper [owned] %2
      //  %4 = mark_must_check [consumable_and_assignable]
      //
      // Or for a move only type, we look for a move_value [lexical].
      if (auto *mvi = dyn_cast<CopyableToMoveOnlyWrapperValueInst>(
              mmci->getOperand())) {
        if (auto *cvi = dyn_cast<CopyValueInst>(mvi->getOperand())) {
          if (auto *bbi = dyn_cast<BeginBorrowInst>(cvi->getOperand())) {
            if (bbi->isLexical()) {
              moveIntroducersToProcess.insert(mmci);
              continue;
            }
          }
        }
      }

      // Handle trivial values.
      //
      // We pattern match:
      //
      // %1 = copyable_to_moveonlywrapper [owned] %0
      // %2 = move_value [lexical] %1
      // %3 = mark_must_check [consumable_and_assignable] %2
      if (auto *cvi = dyn_cast<ExplicitCopyValueInst>(mmci->getOperand())) {
        if (auto *bbi = dyn_cast<BeginBorrowInst>(cvi->getOperand())) {
          if (bbi->isLexical()) {
            moveIntroducersToProcess.insert(mmci);
            continue;
          }
        }
      }

      // Handle guaranteed parameters of a resilient type used by a resilient
      // function inside the module in which the resilient type is defined.
      if (auto *cvi = dyn_cast<CopyValueInst>(mmci->getOperand())) {
        if (auto *cmi = dyn_cast<CopyableToMoveOnlyWrapperValueInst>(cvi->getOperand())) {
          if (auto *lbi = dyn_cast<LoadBorrowInst>(cmi->getOperand())) {
            if (auto *arg = dyn_cast<SILFunctionArgument>(lbi->getOperand())) {
              if (arg->getKnownParameterInfo().isIndirectInGuaranteed()) {
                moveIntroducersToProcess.insert(mmci);
                continue;
              }
            }
          }
        }
      }


      // If we see a mark_must_check that is marked no implicit copy that we
      // don't understand, emit a diagnostic to fail the compilation. This
      // ensures that if someone marks something no implicit copy and we fail to
      // check it, we fail the compilation.
      //
      // We then RAUW the mark_must_check once we have emitted the error since
      // later passes expect that mark_must_check has been eliminated by
      // us. Since we are failing already, this is ok to do.
      emitter.emitCheckerDoesntUnderstandDiagnostic(mmci);
      mmci->replaceAllUsesWith(mmci->getOperand());
      mmci->eraseFromParent();
      localChanged = true;
    }
  }
  return localChanged;
}

//===----------------------------------------------------------------------===//
//                          MARK: OSSACanonicalizer
//===----------------------------------------------------------------------===//

void OSSACanonicalizer::computeBoundaryData(SILValue value) {
  // Now we have our liveness information. First compute the original boundary
  // (which ignores destroy_value).
  PrunedLivenessBoundary originalBoundary;
  canonicalizer.findOriginalBoundary(originalBoundary);

  // Then use that information to stash for our diagnostics the boundary
  // consuming/non-consuming users as well as enter the boundary consuming users
  // into the boundaryConsumignUserSet for quick set testing later.
  using IsInterestingUser = CanonicalizeOSSALifetime::IsInterestingUser;
  InstructionSet boundaryConsumingUserSet(value->getFunction());
  for (auto *lastUser : originalBoundary.lastUsers) {
    LLVM_DEBUG(llvm::dbgs() << "Looking at boundary use: " << *lastUser);
    switch (canonicalizer.isInterestingUser(lastUser)) {
    case IsInterestingUser::NonUser:
      llvm_unreachable("Last user of original boundary should be a user?!");
    case IsInterestingUser::NonLifetimeEndingUse:
      LLVM_DEBUG(llvm::dbgs() << "    NonLifetimeEndingUse!\n");
      nonConsumingBoundaryUsers.push_back(lastUser);
      continue;
    case IsInterestingUser::LifetimeEndingUse:
      LLVM_DEBUG(llvm::dbgs() << "    LifetimeEndingUse!\n");
      consumingBoundaryUsers.push_back(lastUser);
      boundaryConsumingUserSet.insert(lastUser);
      continue;
    }
  }

  // Then go through any of the consuming interesting uses found by our liveness
  // and any that are not on the boundary are ones that we must error for.
  for (auto *consumingUser : canonicalizer.getLifetimeEndingUsers()) {
    bool isConsumingUseOnBoundary =
        boundaryConsumingUserSet.contains(consumingUser);
    LLVM_DEBUG(llvm::dbgs() << "Is consuming user on boundary "
                            << (isConsumingUseOnBoundary ? "yes" : "no") << ": "
                            << *consumingUser);
    if (!isConsumingUseOnBoundary) {
      consumingUsesNeedingCopy.push_back(consumingUser);
    }
  }
}

bool OSSACanonicalizer::canonicalize() {
  // First compute liveness. If we fail, bail.
  if (!computeLiveness()) {
    return false;
  }

  computeBoundaryData(canonicalizer.getCurrentDef());

  // Finally, rewrite lifetimes.
  rewriteLifetimes();

  return true;
}

//===----------------------------------------------------------------------===//
//                         MARK: Forward Declaration
//===----------------------------------------------------------------------===//

namespace {

struct MoveOnlyObjectCheckerPImpl {
  SILFunction *fn;
  borrowtodestructure::IntervalMapAllocator &allocator;
  DiagnosticEmitter &diagnosticEmitter;

  /// A set of mark_must_check that we are actually going to process.
  llvm::SmallSetVector<MarkMustCheckInst *, 32> &moveIntroducersToProcess;

  bool changed = false;

  MoveOnlyObjectCheckerPImpl(
      SILFunction *fn, borrowtodestructure::IntervalMapAllocator &allocator,
      DiagnosticEmitter &diagnosticEmitter,
      llvm::SmallSetVector<MarkMustCheckInst *, 32> &moveIntroducersToProcess)
      : fn(fn), allocator(allocator), diagnosticEmitter(diagnosticEmitter),
        moveIntroducersToProcess(moveIntroducersToProcess) {}

  void check(DominanceInfo *domTree, PostOrderAnalysis *poa);

  bool convertBorrowExtractsToOwnedDestructures(MarkMustCheckInst *mmci,
                                                DominanceInfo *domTree,
                                                PostOrderAnalysis *poa);

  bool checkForSameInstMultipleUseErrors(MarkMustCheckInst *base);
};

} // namespace

bool MoveOnlyObjectCheckerPImpl::convertBorrowExtractsToOwnedDestructures(
    MarkMustCheckInst *mmci, DominanceInfo *domTree, PostOrderAnalysis *poa) {
  BorrowToDestructureTransform transform(allocator, mmci, mmci,
                                         diagnosticEmitter, poa);
  if (!transform.transform()) {
    LLVM_DEBUG(llvm::dbgs()
               << "Failed to perform borrow to destructure transform!\n");
    return false;
  }

  return true;
}

bool MoveOnlyObjectCheckerPImpl::checkForSameInstMultipleUseErrors(
    MarkMustCheckInst *mmci) {
  LLVM_DEBUG(llvm::dbgs() << "Checking for same inst multiple use error!\n");

  SmallFrozenMultiMap<SILInstruction *, Operand *, 8> instToOperandsMap;
  StackList<Operand *> worklist(mmci->getFunction());
  for (auto *use : mmci->getUses())
    worklist.push_back(use);

  while (!worklist.empty()) {
    auto *nextUse = worklist.pop_back_val();

    switch (nextUse->getOperandOwnership()) {
    case OperandOwnership::NonUse:
      continue;

    // Conservatively treat a conversion to an unowned value as a pointer
    // escape. If we see this in the SIL, fail and return false so we emit a
    // "compiler doesn't understand error".
    case OperandOwnership::ForwardingUnowned:
    case OperandOwnership::PointerEscape:
    case OperandOwnership::BitwiseEscape:
      LLVM_DEBUG(llvm::dbgs()
                 << "        Found forwarding unowned or escape!\n");
      return false;

    case OperandOwnership::TrivialUse:
    case OperandOwnership::InstantaneousUse:
    case OperandOwnership::UnownedInstantaneousUse:
      // Look through copy_value.
      if (auto *cvi = dyn_cast<CopyValueInst>(nextUse->getUser())) {
        for (auto *use : cvi->getUses())
          worklist.push_back(use);
        continue;
      }

      // Treat these as non-consuming uses that could have a consuming use as an
      // additional operand.
      LLVM_DEBUG(llvm::dbgs()
                 << "        Found non consuming use: " << *nextUse->getUser());
      instToOperandsMap.insert(nextUse->getUser(), nextUse);
      continue;
    case OperandOwnership::InteriorPointer:
      // We do not care about interior pointer uses since there aren't any
      // interior pointer using instructions that are also consuming uses.
      continue;

    case OperandOwnership::DestroyingConsume:
      if (isa<DestroyValueInst>(nextUse->getUser()))
        continue;
      [[fallthrough]];
    case OperandOwnership::ForwardingConsume:
      LLVM_DEBUG(llvm::dbgs()
                 << "        Found consuming use: " << *nextUse->getUser());

      instToOperandsMap.insert(nextUse->getUser(), nextUse);
      continue;

    case OperandOwnership::EndBorrow:
    case OperandOwnership::Reborrow:
    case OperandOwnership::GuaranteedForwarding:
      llvm_unreachable(
          "We do not process borrows recursively so should never see this.");

    case OperandOwnership::Borrow:
      // We don't care about borrows so we don't process them recursively
      continue;
    }
  }

  // Ok, we have our list of potential uses. Sort the multi-map and then search
  // for errors.
  instToOperandsMap.setFrozen();
  for (auto pair : instToOperandsMap.getRange()) {
    LLVM_DEBUG(llvm::dbgs()
               << "Checking inst for multiple uses: " << *pair.first);
    Operand *foundConsumingUse = nullptr;
    Operand *foundNonConsumingUse = nullptr;
    for (auto *use : pair.second) {
      LLVM_DEBUG(llvm::dbgs()
                 << "    Visiting use: " << use->getOperandNumber() << '\n');
      if (use->isConsuming()) {
        LLVM_DEBUG(llvm::dbgs() << "        Is consuming!\n");
        if (foundConsumingUse) {
          // Emit error.
          LLVM_DEBUG(
              llvm::dbgs()
              << "        Had previous consuming use! Emitting error!\n");
          diagnosticEmitter.emitObjectInstConsumesValueTwice(
              mmci, foundConsumingUse, use);
          continue;
        }

        if (foundNonConsumingUse) {
          LLVM_DEBUG(
              llvm::dbgs()
              << "        Had previous non consuming use! Emitting error!\n");
          // Emit error.
          diagnosticEmitter.emitObjectInstConsumesAndUsesValue(
              mmci, use, foundNonConsumingUse);
          continue;
        }

        if (!foundConsumingUse)
          foundConsumingUse = use;
        continue;
      }

      LLVM_DEBUG(llvm::dbgs() << "        Is non consuming!\n");
      if (foundConsumingUse) {
        // Emit error.
        LLVM_DEBUG(llvm::dbgs()
                   << "        Had previous consuming use! Emitting error!\n");
        diagnosticEmitter.emitObjectInstConsumesAndUsesValue(
            mmci, foundConsumingUse, use);
        continue;
      }

      if (!foundNonConsumingUse)
        foundNonConsumingUse = use;
    }
  }

  return true;
}

//===----------------------------------------------------------------------===//
//                          MARK: Main PImpl Routine
//===----------------------------------------------------------------------===//

void MoveOnlyObjectCheckerPImpl::check(DominanceInfo *domTree,
                                       PostOrderAnalysis *poa) {
  auto callbacks =
      InstModCallbacks().onDelete([&](SILInstruction *instToDelete) {
        if (auto *mvi = dyn_cast<MarkMustCheckInst>(instToDelete))
          moveIntroducersToProcess.remove(mvi);
        instToDelete->eraseFromParent();
      });
  InstructionDeleter deleter(std::move(callbacks));
  OSSACanonicalizer canonicalizer(fn, domTree, deleter);
  diagnosticEmitter.initCanonicalizer(&canonicalizer);

  unsigned initialDiagCount = diagnosticEmitter.getDiagnosticCount();

  auto moveIntroducers = llvm::makeArrayRef(moveIntroducersToProcess.begin(),
                                            moveIntroducersToProcess.end());
  while (!moveIntroducers.empty()) {
    MarkMustCheckInst *markedValue = moveIntroducers.front();

    OSSACanonicalizer::LivenessState livenessState(canonicalizer, markedValue);

    moveIntroducers = moveIntroducers.drop_front(1);
    LLVM_DEBUG(llvm::dbgs() << "Visiting: " << *markedValue);

    // Before we do anything, we need to look for borrowed extracted values and
    // convert them to destructure operations.
    unsigned diagCount = diagnosticEmitter.getDiagnosticCount();
    if (!convertBorrowExtractsToOwnedDestructures(markedValue, domTree, poa)) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Borrow extract to owned destructure transformation didn't "
                    "understand part of the SIL\n");
      diagnosticEmitter.emitCheckerDoesntUnderstandDiagnostic(markedValue);
      continue;
    }

    // If we emitted any non-exceptional diagnostics in
    // convertBorrowExtractsToOwnedDestructures, continue and process the next
    // instruction. The user can fix and re-compile. We want the OSSA
    // canonicalizer to be able to assume that all such borrow + struct_extract
    // uses were already handled.
    if (diagCount != diagnosticEmitter.getDiagnosticCount()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Emitting diagnostic in BorrowExtractToOwnedDestructure "
                    "transformation!\n");
      continue;
    }

    // First search for transitive consuming uses and prove that we do not have
    // any errors where a single instruction consumes the same value twice or
    // consumes and uses a value.
    if (!checkForSameInstMultipleUseErrors(markedValue)) {
      LLVM_DEBUG(llvm::dbgs() << "checkForSameInstMultipleUseError didn't "
                                 "understand part of the SIL\n");
      diagnosticEmitter.emitCheckerDoesntUnderstandDiagnostic(markedValue);
      continue;
    }

    if (diagCount != diagnosticEmitter.getDiagnosticCount()) {
      LLVM_DEBUG(llvm::dbgs() << "Found single inst multiple user error!\n");
      continue;
    }

    // Once that is complete, we then begin to canonicalize ownership, finding
    // our boundary and any uses that need a copy. We in this section only deal
    // with instructions due to our first step where we emitted errors for
    // instructions containing multiple operands.

    // Step 1. Compute liveness.
    if (!canonicalizer.computeLiveness()) {
      diagnosticEmitter.emitCheckerDoesntUnderstandDiagnostic(markedValue);
      LLVM_DEBUG(
          llvm::dbgs()
          << "Emitted checker doesnt understand diagnostic! Exiting early!\n");
      continue;
    } else {
      // Always set changed to true if we succeeded in canonicalizing since we
      // may have rewritten copies.
      changed = true;
    }

    // NOTE: In the following we only rewrite lifetimes once we have emitted
    // diagnostics. This ensures that we can emit diagnostics using the the
    // liveness information before rewrite lifetimes has enriched the liveness
    // info with maximized liveness information.

    // Step 2. Compute our boundary non consuming, consuming uses, and consuming
    // uses that need copies.
    canonicalizer.computeBoundaryData(markedValue);

    // If we are asked to perform guaranteed checking, emit an error if we have
    // /any/ consuming boundary uses or uses that need copies and then rewrite
    // lifetimes.
    if (markedValue->getCheckKind() ==
        MarkMustCheckInst::CheckKind::NoConsumeOrAssign) {
      if (canonicalizer.foundAnyConsumingUses()) {
        diagnosticEmitter.emitObjectGuaranteedDiagnostic(markedValue);
      }
      canonicalizer.rewriteLifetimes();
      continue;
    }

    if (!canonicalizer.foundConsumingUseRequiringCopy()) {
      // If we failed to understand how to perform the check or did not find
      // any targets... continue. In the former case we want to fail with a
      // checker did not understand diagnostic later and in the former, we
      // succeeded.
      canonicalizer.rewriteLifetimes();
      continue;
    }

    // Finally emit our object owned diagnostics and then rewrite lifetimes.
    diagnosticEmitter.emitObjectOwnedDiagnostic(markedValue);
    canonicalizer.rewriteLifetimes();
  }

  bool emittedDiagnostic =
      initialDiagCount != diagnosticEmitter.getDiagnosticCount();
  LLVM_DEBUG(llvm::dbgs() << "Emitting checker based diagnostic: "
                          << (emittedDiagnostic ? "yes" : "no") << '\n');

  // Ok, we have success. All of our marker instructions were proven as safe or
  // we emitted a diagnostic. Now we need to clean up the IR by eliminating our
  // marker instructions to signify that the checked SIL is correct. We also
  // perform some small cleanups for guaranteed values if we emitted a
  // diagnostic on them.
  //
  // NOTE: This is enforced in the verifier by only allowing MarkMustCheckInst
  // in Raw SIL. This ensures we do not miss any.
  //
  // NOTE: destroys is a separate array that we use to avoid iterator
  // invalidation when cleaning up destroy_value of guaranteed checked values.
  SmallVector<DestroyValueInst *, 8> destroys;
  while (!moveIntroducersToProcess.empty()) {
    auto *markedInst = moveIntroducersToProcess.pop_back_val();

    // If we didn't emit a diagnostic on a non-trivial guaranteed argument,
    // eliminate the copy_value, destroy_values, and the mark_must_check.
    if (!diagnosticEmitter.emittedDiagnosticForValue(markedInst)) {
      if (markedInst->getCheckKind() ==
          MarkMustCheckInst::CheckKind::NoConsumeOrAssign) {
        if (auto *cvi = dyn_cast<CopyValueInst>(markedInst->getOperand())) {
          SingleValueInstruction *i = cvi;
          if (auto *copyToMoveOnly =
                  dyn_cast<CopyableToMoveOnlyWrapperValueInst>(
                      cvi->getOperand())) {
            i = copyToMoveOnly;
          }

          // Handle:
          //
          // bb0(%0 : @guaranteed $Type):
          //   %1 = copy_value %0
          //   %2 = mark_must_check [no_consume_or_assign] %1
          if (auto *arg = dyn_cast<SILFunctionArgument>(i->getOperand(0))) {
            if (arg->getOwnershipKind() == OwnershipKind::Guaranteed) {
              for (auto *use : markedInst->getConsumingUses()) {
                destroys.push_back(cast<DestroyValueInst>(use->getUser()));
              }
              while (!destroys.empty())
                destroys.pop_back_val()->eraseFromParent();
              markedInst->replaceAllUsesWith(arg);
              markedInst->eraseFromParent();
              cvi->eraseFromParent();
              continue;
            }
          }

          // Handle:
          //
          // bb0(%0 : $*Type): // in_guaranteed
          //   %1 = load_borrow %0
          //   %2 = copy_value %1
          //   %3 = mark_must_check [no_consume_or_assign] %2
          if (auto *lbi = dyn_cast<LoadBorrowInst>(i->getOperand(0))) {
            if (auto *arg = dyn_cast<SILFunctionArgument>(lbi->getOperand())) {
              if (arg->getKnownParameterInfo().isIndirectInGuaranteed()) {
                for (auto *use : markedInst->getConsumingUses()) {
                  destroys.push_back(cast<DestroyValueInst>(use->getUser()));
                }
                while (!destroys.empty())
                  destroys.pop_back_val()->eraseFromParent();
                markedInst->replaceAllUsesWith(lbi);
                markedInst->eraseFromParent();
                cvi->eraseFromParent();
                continue;
              }
            }
          }
        }
      }
    }

    markedInst->replaceAllUsesWith(markedInst->getOperand());
    markedInst->eraseFromParent();
    changed = true;
  }
}

//===----------------------------------------------------------------------===//
//                            MARK: Driver Routine
//===----------------------------------------------------------------------===//

bool MoveOnlyObjectChecker::check(
    llvm::SmallSetVector<MarkMustCheckInst *, 32> &instsToCheck) {
  assert(instsToCheck.size() &&
         "Should only call this with actual insts to check?!");
  MoveOnlyObjectCheckerPImpl checker(instsToCheck[0]->getFunction(), allocator,
                                     diagnosticEmitter, instsToCheck);
  checker.check(domTree, poa);
  return checker.changed;
}
