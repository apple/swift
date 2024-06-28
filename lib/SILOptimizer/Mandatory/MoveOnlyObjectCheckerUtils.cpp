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
#include "swift/Basic/Assertions.h"
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

bool swift::siloptimizer::
    searchForCandidateObjectMarkUnresolvedNonCopyableValueInsts(
        SILFunction *fn,
        llvm::SmallSetVector<MarkUnresolvedNonCopyableValueInst *, 32>
            &moveIntroducersToProcess,
        DiagnosticEmitter &emitter) {
  bool localChanged = false;
  for (auto &block : *fn) {
    for (auto ii = block.begin(), ie = block.end(); ii != ie;) {
      auto *mmci = dyn_cast<MarkUnresolvedNonCopyableValueInst>(&*ii);
      ++ii;

      if (!mmci || !mmci->hasMoveCheckerKind() || !mmci->getType().isObject())
        continue;

      moveIntroducersToProcess.insert(mmci);
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

  /// A set of mark_unresolved_non_copyable_value that we are actually going to
  /// process.
  llvm::SmallSetVector<MarkUnresolvedNonCopyableValueInst *, 32>
      &moveIntroducersToProcess;

  bool changed = false;

  MoveOnlyObjectCheckerPImpl(
      SILFunction *fn, borrowtodestructure::IntervalMapAllocator &allocator,
      DiagnosticEmitter &diagnosticEmitter,
      llvm::SmallSetVector<MarkUnresolvedNonCopyableValueInst *, 32>
          &moveIntroducersToProcess)
      : fn(fn), allocator(allocator), diagnosticEmitter(diagnosticEmitter),
        moveIntroducersToProcess(moveIntroducersToProcess) {}

  void check(DominanceInfo *domTree, PostOrderAnalysis *poa);

  bool convertBorrowExtractsToOwnedDestructures(
      MarkUnresolvedNonCopyableValueInst *mmci, DominanceInfo *domTree,
      PostOrderAnalysis *poa);

  bool
  checkForSameInstMultipleUseErrors(MarkUnresolvedNonCopyableValueInst *base);
};

} // namespace

bool MoveOnlyObjectCheckerPImpl::convertBorrowExtractsToOwnedDestructures(
    MarkUnresolvedNonCopyableValueInst *mmci, DominanceInfo *domTree,
    PostOrderAnalysis *poa) {
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
    MarkUnresolvedNonCopyableValueInst *mmci) {
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
        if (auto *mvi =
                dyn_cast<MarkUnresolvedNonCopyableValueInst>(instToDelete))
          moveIntroducersToProcess.remove(mvi);
        instToDelete->eraseFromParent();
      });
  InstructionDeleter deleter(std::move(callbacks));
  OSSACanonicalizer canonicalizer(fn, domTree, deleter);
  diagnosticEmitter.initCanonicalizer(&canonicalizer);

  unsigned initialDiagCount = diagnosticEmitter.getDiagnosticCount();

  auto moveIntroducers = llvm::ArrayRef(moveIntroducersToProcess.begin(),
                                        moveIntroducersToProcess.end());
  while (!moveIntroducers.empty()) {
    MarkUnresolvedNonCopyableValueInst *markedValue = moveIntroducers.front();

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
        MarkUnresolvedNonCopyableValueInst::CheckKind::NoConsumeOrAssign) {
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
  // NOTE: This is enforced in the verifier by only allowing
  // MarkUnresolvedNonCopyableValueInst in Raw SIL. This ensures we do not
  // miss any.
  //
  // NOTE: destroys is a separate array that we use to avoid iterator
  // invalidation when cleaning up destroy_value of guaranteed checked values.
  SmallVector<DestroyValueInst *, 8> destroys;
  while (!moveIntroducersToProcess.empty()) {
    auto *markedInst = moveIntroducersToProcess.pop_back_val();

    // If we didn't emit a diagnostic on a non-trivial guaranteed argument,
    // eliminate the copy_value, destroy_values, and the
    // mark_unresolved_non_copyable_value.
    if (!diagnosticEmitter.emittedDiagnosticForValue(markedInst)) {
      if (markedInst->getCheckKind() ==
          MarkUnresolvedNonCopyableValueInst::CheckKind::NoConsumeOrAssign) {
        if (auto *cvi = dyn_cast<CopyValueInst>(markedInst->getOperand())) {
          SingleValueInstruction *i = cvi;
          if (auto *copyToMoveOnly =
                  dyn_cast<CopyableToMoveOnlyWrapperValueInst>(
                      cvi->getOperand())) {
            i = copyToMoveOnly;
          }

          // TODO: Instead of pattern matching specific code generation patterns,
          // we should be able to eliminate any `copy_value` whose canonical
          // lifetime fits within the borrow scope of the borrowed value being
          // copied from.

          // Handle:
          //
          // bb(%arg : @guaranteed $Type):
          //   %copy = copy_value %arg
          //   %mark = mark_unresolved_non_copyable_value [no_consume_or_assign] %copy
          if (auto *arg = dyn_cast<SILArgument>(i->getOperand(0))) {
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
          //   %1 = load_borrow %0
          //   %2 = copy_value %1
          //   %3 = mark_unresolved_non_copyable_value [no_consume_or_assign] %2
          if (auto *lbi = dyn_cast<LoadBorrowInst>(i->getOperand(0))) {
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
          
          // Handle:
          //   (%yield, ..., %handle) = begin_apply
          //   %copy = copy_value %yield
          //   %mark = mark_unresolved_noncopyable_value [no_consume_or_assign] %copy
          if (auto bai = dyn_cast_or_null<BeginApplyInst>(i->getOperand(0)->getDefiningInstruction())) {
            if (i->getOperand(0)->getOwnershipKind() == OwnershipKind::Guaranteed) {
              for (auto *use : markedInst->getConsumingUses()) {
                destroys.push_back(cast<DestroyValueInst>(use->getUser()));
              }
              while (!destroys.empty())
                destroys.pop_back_val()->eraseFromParent();
              markedInst->replaceAllUsesWith(i->getOperand(0));
              markedInst->eraseFromParent();
              cvi->eraseFromParent();
              continue;
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
    llvm::SmallSetVector<MarkUnresolvedNonCopyableValueInst *, 32>
        &instsToCheck) {
  assert(instsToCheck.size() &&
         "Should only call this with actual insts to check?!");
  MoveOnlyObjectCheckerPImpl checker(instsToCheck[0]->getFunction(), allocator,
                                     diagnosticEmitter, instsToCheck);
  checker.check(domTree, poa);
  return checker.changed;
}
