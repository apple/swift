//===--- MoveOnlyDiagnostics.cpp ------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-move-only-checker"

#include "MoveOnlyDiagnostics.h"

#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Stmt.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/FieldSensitivePrunedLiveness.h"
#include "swift/SIL/SILArgument.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using namespace swift::siloptimizer;

static llvm::cl::opt<bool> SilentlyEmitDiagnostics(
    "move-only-diagnostics-silently-emit-diagnostics",
    llvm::cl::desc(
        "For testing purposes, emit the diagnostic silently so we can "
        "filecheck the result of emitting an error from the move checkers"),
    llvm::cl::init(false));

//===----------------------------------------------------------------------===//
//                              MARK: Utilities
//===----------------------------------------------------------------------===//

template <typename... T, typename... U>
static void diagnose(ASTContext &context, SILInstruction *inst, Diag<T...> diag,
                     U &&...args) {
  // See if the consuming use is an owned moveonly_to_copyable whose only
  // user is a return. In that case, use the return loc instead. We do this
  // b/c it is illegal to put a return value location on a non-return value
  // instruction... so we have to hack around this slightly.
  auto loc = inst->getLoc();
  if (auto *mtc = dyn_cast<MoveOnlyWrapperToCopyableValueInst>(inst)) {
    if (auto *ri = mtc->getSingleUserOfType<ReturnInst>()) {
      loc = ri->getLoc();
    }
  }

  // If for testing reasons we want to return that we emitted an error but not
  // emit the actual error itself, return early.
  if (SilentlyEmitDiagnostics)
    return;

  context.Diags.diagnose(loc.getSourceLoc(), diag, std::forward<U>(args)...);
}

template <typename... T, typename... U>
static void diagnose(ASTContext &context, SourceLoc loc, Diag<T...> diag,
                     U &&...args) {
  // If for testing reasons we want to return that we emitted an error but not
  // emit the actual error itself, return early.
  if (SilentlyEmitDiagnostics)
    return;

  context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

/// Helper function that actually implements getVariableNameForValue. Do not
/// call it directly! Call the unary variants instead.
static void getVariableNameForValue(SILValue value2,
                                    SILValue searchValue,
                                    SmallString<64> &resultingString) {
  // Before we do anything, lets see if we have an exact debug_value on our
  // mmci. In such a case, we can end early and are done.
  if (auto *use = getAnyDebugUse(value2)) {
    if (auto debugVar = DebugVarCarryingInst(use->getUser())) {
      assert(debugVar.getKind() == DebugVarCarryingInst::Kind::DebugValue);
      resultingString += debugVar.getName();
      return;
    }
  }

  // Otherwise, we need to look at our mark_must_check's operand.
  StackList<llvm::PointerUnion<SILInstruction *, SILValue>> variableNamePath(
      value2->getFunction());
  while (true) {
    if (auto *allocInst = dyn_cast<AllocationInst>(searchValue)) {
      variableNamePath.push_back(allocInst);
      break;
    }

    if (auto *globalAddrInst = dyn_cast<GlobalAddrInst>(searchValue)) {
      variableNamePath.push_back(globalAddrInst);
      break;
    }

    if (auto *rei = dyn_cast<RefElementAddrInst>(searchValue)) {
      variableNamePath.push_back(rei);
      searchValue = rei->getOperand();
      continue;
    }

    if (auto *fArg = dyn_cast<SILFunctionArgument>(searchValue)) {
      variableNamePath.push_back({fArg});
      break;
    }

    // If we do not do an exact match, see if we can find a debug_var inst. If
    // we do, we always break since we have a root value.
    if (auto *use = getAnyDebugUse(searchValue)) {
      if (auto debugVar = DebugVarCarryingInst(use->getUser())) {
        assert(debugVar.getKind() == DebugVarCarryingInst::Kind::DebugValue);
        variableNamePath.push_back(use->getUser());
        break;
      }
    }

    // Otherwise, try to see if we have a single value instruction we can look
    // through.
    if (isa<BeginBorrowInst>(searchValue) || isa<LoadInst>(searchValue) ||
        isa<LoadBorrowInst>(searchValue) || isa<BeginAccessInst>(searchValue) ||
        isa<MarkMustCheckInst>(searchValue) ||
        isa<ProjectBoxInst>(searchValue) || isa<CopyValueInst>(searchValue)) {
      searchValue = cast<SingleValueInstruction>(searchValue)->getOperand(0);
      continue;
    }

    // If we do not pattern match successfully, just set resulting string to
    // unknown and return early.
    resultingString += "unknown";
    return;
  }

  // Walk backwards, constructing our string.
  while (true) {
    auto next = variableNamePath.pop_back_val();

    if (auto *inst = next.dyn_cast<SILInstruction *>()) {
      if (auto i = DebugVarCarryingInst(inst)) {
        resultingString += i.getName();
      } else if (auto i = VarDeclCarryingInst(inst)) {
        resultingString += i.getName();
      }
    } else {
      auto value = next.get<SILValue>();
      if (auto *fArg = dyn_cast<SILFunctionArgument>(value))
        resultingString += fArg->getDecl()->getBaseName().userFacingName();
    }

    if (variableNamePath.empty())
      return;

    resultingString += '.';
  }
}

static void getVariableNameForValue(MarkMustCheckInst *mmci,
                                    SmallString<64> &resultingString) {
  return getVariableNameForValue(mmci, mmci->getOperand(), resultingString);
}

//===----------------------------------------------------------------------===//
//                           MARK: Misc Diagnostics
//===----------------------------------------------------------------------===//

void DiagnosticEmitter::emitCheckerDoesntUnderstandDiagnostic(
    MarkMustCheckInst *markedValue) {
  // If we failed to canonicalize ownership, there was something in the SIL
  // that copy propagation did not understand. Emit a we did not understand
  // error.
  if (markedValue->getType().isMoveOnlyWrapped()) {
    diagnose(
        fn->getASTContext(), markedValue,
        diag::sil_movechecking_not_understand_consumable_and_assignable);
  } else {
    diagnose(fn->getASTContext(), markedValue,
             diag::sil_movechecking_not_understand_moveonly);
  }
  registerDiagnosticEmitted(markedValue);
  emittedCheckerDoesntUnderstandDiagnostic = true;
}

void DiagnosticEmitter::emitCheckedMissedCopyError(SILInstruction *copyInst) {
  diagnose(copyInst->getFunction()->getASTContext(), copyInst,
           diag::sil_movechecking_bug_missed_copy);
}

void DiagnosticEmitter::emitReinitAfterDiscardError(SILInstruction *badReinit,
                                                    SILInstruction *discard) {
  assert(isa<DropDeinitInst>(discard));
  assert(badReinit->getLoc() && "missing loc!");
  assert(discard->getLoc() && "missing loc!");

  diagnose(badReinit->getFunction()->getASTContext(),
           badReinit,
           diag::sil_movechecking_reinit_after_discard);

  diagnose(discard->getFunction()->getASTContext(), discard,
           diag::sil_movechecking_discard_self_here);
}

void DiagnosticEmitter::emitMissingConsumeInDiscardingContext(
    SILInstruction *leftoverDestroy,
    SILInstruction *discard) {
  assert(isa<DropDeinitInst>(discard));

  // A good location is one that has some connection with the original source
  // and corresponds to an exit of the function.
  auto hasGoodLocation = [](SILInstruction *si) -> bool {
    if (!si)
      return false;

    SILLocation loc = si->getLoc();
    if (loc.isNull())
      return false;

    switch (loc.getKind()) {
    case SILLocation::ReturnKind:
    case SILLocation::ImplicitReturnKind:
      return true;

    case SILLocation::RegularKind: {
      Stmt *stmt = loc.getAsASTNode<Stmt>();
      if (!stmt)
        return true; // For non-statements, assume it is exiting the func.

      // Prefer statements that can possibly lead to an exit of the function.
      // This is determined by whether the statement causes an exit of a
      // lexical scope; so a 'break' counts but not a 'continue'.
      switch (stmt->getKind()) {
      case StmtKind::Throw:
      case StmtKind::Return:
      case StmtKind::Yield:
      case StmtKind::Break:
      case StmtKind::Fail:
      case StmtKind::PoundAssert:
        return true;

      case StmtKind::Continue:
      case StmtKind::Brace:
      case StmtKind::Defer:
      case StmtKind::If:
      case StmtKind::Guard:
      case StmtKind::While:
      case StmtKind::Do:
      case StmtKind::DoCatch:
      case StmtKind::RepeatWhile:
      case StmtKind::ForEach:
      case StmtKind::Switch:
      case StmtKind::Case:
      case StmtKind::Fallthrough:
      case StmtKind::Discard:
        return false;
      };
    }

    case SILLocation::InlinedKind:
    case SILLocation::MandatoryInlinedKind:
    case SILLocation::CleanupKind:
    case SILLocation::ArtificialUnreachableKind:
      return false;
    };
  };

  // An instruction corresponding to the logical place where the value is
  // destroyed. Ideally an exit point of the function reachable from here or
  // some relevant statement.
  SILInstruction *destroyPoint = leftoverDestroy;
  if (!hasGoodLocation(destroyPoint)) {
    // Search for a nearby function exit reachable from this destroy. We do this
    // because the move checker may have injected or hoisted an existing
    // destroy from leaf blocks to some earlier point. For example, if 'd'
    // represents a destroy of self, then we may have this CFG:
    //
    //        before:      after:
    //           .          d
    //          / \        / \
    //         d   d      .   .
    //
    BasicBlockSet visited(destroyPoint->getFunction());
    std::deque<SILBasicBlock *> bfsWorklist = {destroyPoint->getParent()};
    while (auto *bb = bfsWorklist.front()) {
      visited.insert(bb);
      bfsWorklist.pop_front();

      TermInst *term = bb->getTerminator();

      // Looking for a block that exits the function or terminates the program.
      if (term->isFunctionExiting() || term->isProgramTerminating()) {
        SILInstruction *candidate = term;

        // Walk backwards until we find an instruction with any source location.
        // Sometimes a terminator like 'unreachable' may not have one, but one
        // of the preceding instructions will.
        while (candidate && candidate->getLoc().isNull())
          candidate = candidate->getPreviousInstruction();

        if (candidate && candidate->getLoc()) {
          destroyPoint = candidate;
          break;
        }
      }

      for (auto *nextBB : term->getSuccessorBlocks())
        if (!visited.contains(nextBB))
          bfsWorklist.push_back(nextBB);
    }
  }

  assert(destroyPoint->getLoc() && "missing loc!");
  assert(discard->getLoc() && "missing loc!");

  diagnose(leftoverDestroy->getFunction()->getASTContext(),
           destroyPoint,
           diag::sil_movechecking_discard_missing_consume_self);

  diagnose(discard->getFunction()->getASTContext(), discard,
            diag::sil_movechecking_discard_self_here);
}

//===----------------------------------------------------------------------===//
//                          MARK: Object Diagnostics
//===----------------------------------------------------------------------===//

void DiagnosticEmitter::emitObjectGuaranteedDiagnostic(
    MarkMustCheckInst *markedValue) {
  auto &astContext = fn->getASTContext();
  SmallString<64> varName;
  getVariableNameForValue(markedValue, varName);

  // See if we have any closure capture uses and emit a better diagnostic.
  if (getCanonicalizer().hasPartialApplyConsumingUse()) {
    diagnose(astContext, markedValue,
             diag::sil_movechecking_borrowed_parameter_captured_by_closure,
             varName);
    emitObjectDiagnosticsForPartialApplyUses(varName);
    registerDiagnosticEmitted(markedValue);
  }

  // If we do not have any non-partial apply consuming uses... just exit early.
  if (!getCanonicalizer().hasNonPartialApplyConsumingUse())
    return;

  registerDiagnosticEmitted(markedValue);

  // Check if this value is closure captured. In such a case, emit a special
  // error.
  if (auto *fArg = dyn_cast<SILFunctionArgument>(
          lookThroughCopyValueInsts(markedValue->getOperand()))) {
    if (fArg->isClosureCapture()) {
      diagnose(astContext, markedValue,
               diag::sil_movechecking_capture_consumed,
               varName);
      emitObjectDiagnosticsForGuaranteedUses(
          true /*ignore partial apply uses*/);
      registerDiagnosticEmitted(markedValue);
      return;
    }
  }

  diagnose(astContext, markedValue,
           diag::sil_movechecking_guaranteed_value_consumed, varName);

  emitObjectDiagnosticsForGuaranteedUses(true /*ignore partial apply uses*/);
}

void DiagnosticEmitter::emitObjectOwnedDiagnostic(
    MarkMustCheckInst *markedValue) {
  registerDiagnosticEmitted(markedValue);

  auto &astContext = fn->getASTContext();
  SmallString<64> varName;
  getVariableNameForValue(markedValue, varName);

  // Ok we know that we are going to emit an error. Lets use a little more
  // compile time to emit a nice error.
  InstructionSet consumingUserSet(markedValue->getFunction());
  InstructionSet nonConsumingUserSet(markedValue->getFunction());
  llvm::SmallDenseMap<SILBasicBlock *, SILInstruction *, 8>
      consumingBlockToUserMap;
  llvm::SmallDenseMap<SILBasicBlock *, SILInstruction *, 8>
      nonConsumingBlockToUserMap;

  // NOTE: We use all lifetime ending and non-lifetime ending users to ensure
  // that we properly identify cases where the actual boundary use is in a loop
  // further down the loop nest from our original use. In such a case, it will
  // not be identified as part of the boundary and instead we will identify a
  // boundary edge which does not provide us with something that we want to
  // error upon.
  for (auto *user : getCanonicalizer().canonicalizer.getLifetimeEndingUsers()) {
    consumingUserSet.insert(user);
    consumingBlockToUserMap.try_emplace(user->getParent(), user);
  }
  for (auto *user :
       getCanonicalizer().canonicalizer.getNonLifetimeEndingUsers()) {
    nonConsumingUserSet.insert(user);
    nonConsumingBlockToUserMap.try_emplace(user->getParent(), user);
  }

  // Now for each consuming use that needs a copy...
  for (auto *user : getCanonicalizer().consumingUsesNeedingCopy) {
    // First search from user to the end of the block for one of our boundary
    // uses and if it is in the block, emit an error and continue.
    bool foundSingleBlockError = false;
    for (auto ii = std::next(user->getIterator()),
              ie = user->getParent()->end();
         ii != ie; ++ii) {
      if (consumingUserSet.contains(&*ii)) {
        foundSingleBlockError = true;
        diagnose(astContext, markedValue,
                 diag::sil_movechecking_owned_value_consumed_more_than_once,
                 varName);
        diagnose(astContext, user,
                 diag::sil_movechecking_consuming_use_here);
        diagnose(astContext, &*ii,
                 diag::sil_movechecking_consumed_again_here);
        break;
      }

      if (nonConsumingUserSet.contains(&*ii)) {
        foundSingleBlockError = true;
        diagnose(astContext, markedValue,
                 diag::sil_movechecking_value_used_after_consume, varName);
        diagnose(astContext, user,
                 diag::sil_movechecking_consuming_use_here);
        diagnose(astContext, &*ii,
                 diag::sil_movechecking_nonconsuming_use_here);
        break;
      }
    }

    // If we found a single block error for this user, continue.
    if (foundSingleBlockError)
      continue;

    // Otherwise, the reason why the consuming use needs to be copied is in a
    // successor block. Lets go look for that user.
    BasicBlockWorklist worklist(markedValue->getFunction());
    for (auto *succBlock : user->getParent()->getSuccessorBlocks())
      worklist.push(succBlock);
    while (auto *nextBlock = worklist.pop()) {
      // First, check if we are visiting the same block as our user block. In
      // such a case, we found a consuming use within a loop.
      if (nextBlock == user->getParent()) {
        diagnose(astContext, markedValue,
                 diag::sil_movechecking_value_consumed_in_a_loop, varName);
        auto d =
            diag::sil_movechecking_consumed_in_loop_here;
        diagnose(astContext, user, d);
        break;
      }

      {
        auto iter = consumingBlockToUserMap.find(nextBlock);
        if (iter != consumingBlockToUserMap.end()) {
          // We found it... emit the error and break.
          diagnose(
              astContext, markedValue,
              diag::sil_movechecking_owned_value_consumed_more_than_once,
              varName);
          diagnose(astContext, user,
                   diag::sil_movechecking_consuming_use_here);
          diagnose(astContext, iter->second,
                   diag::sil_movechecking_consumed_again_here);
          break;
        }
      }

      {
        auto iter = nonConsumingBlockToUserMap.find(nextBlock);
        if (iter != nonConsumingBlockToUserMap.end()) {
          // We found it... emit the error and break.
          diagnose(astContext, markedValue,
                   diag::sil_movechecking_value_used_after_consume, varName);
          diagnose(astContext, user,
                   diag::sil_movechecking_consuming_use_here);
          diagnose(astContext, iter->second,
                   diag::sil_movechecking_nonconsuming_use_here);
          break;
        }
      }

      // If we didn't break, keep walking successors we haven't seen yet.
      for (auto *succBlock : nextBlock->getSuccessorBlocks()) {
        worklist.pushIfNotVisited(succBlock);
      }
    }
  }
}

void DiagnosticEmitter::emitObjectDiagnosticsForGuaranteedUses(
    bool ignorePartialApplyUses) const {
  auto &astContext = fn->getASTContext();

  for (auto *consumingUser : getCanonicalizer().consumingUsesNeedingCopy) {
    if (ignorePartialApplyUses &&
        OSSACanonicalizer::isPartialApplyUser(consumingUser))
      continue;
    diagnose(astContext, consumingUser,
             diag::sil_movechecking_consuming_use_here);
  }

  for (auto *user : getCanonicalizer().consumingBoundaryUsers) {
    if (ignorePartialApplyUses && OSSACanonicalizer::isPartialApplyUser(user))
      continue;

    diagnose(astContext, user, diag::sil_movechecking_consuming_use_here);
  }
}

void DiagnosticEmitter::emitObjectDiagnosticsForPartialApplyUses(
    StringRef capturedVarName) const {
  auto &astContext = fn->getASTContext();

  for (auto *user : getCanonicalizer().consumingUsesNeedingCopy) {
    if (!OSSACanonicalizer::isPartialApplyUser(user))
      continue;
    diagnose(astContext,
             user,
             diag::sil_movechecking_consuming_closure_use_here,
             capturedVarName);
  }

  for (auto *user : getCanonicalizer().consumingBoundaryUsers) {
    if (!OSSACanonicalizer::isPartialApplyUser(user))
      continue;

    diagnose(astContext,
             user,
             diag::sil_movechecking_consuming_closure_use_here,
             capturedVarName);
  }
}

//===----------------------------------------------------------------------===//
//                         MARK: Address Diagnostics
//===----------------------------------------------------------------------===//

void DiagnosticEmitter::emitAddressExclusivityHazardDiagnostic(
    MarkMustCheckInst *markedValue, SILInstruction *consumingUser) {
  if (!useWithDiagnostic.insert(consumingUser).second)
    return;
  registerDiagnosticEmitted(markedValue);

  auto &astContext = markedValue->getFunction()->getASTContext();
  SmallString<64> varName;
  getVariableNameForValue(markedValue, varName);

  LLVM_DEBUG(llvm::dbgs() << "Emitting error for exclusivity!\n");
  LLVM_DEBUG(llvm::dbgs() << "    Mark: " << *markedValue);
  LLVM_DEBUG(llvm::dbgs() << "    Consuming use: " << *consumingUser);

  diagnose(astContext, markedValue,
           diag::sil_movechecking_bug_exclusivity_violation, varName);
  diagnose(astContext, consumingUser,
           diag::sil_movechecking_consuming_use_here);
}

void DiagnosticEmitter::emitAddressDiagnostic(MarkMustCheckInst *markedValue,
                                              SILInstruction *lastLiveUser,
                                              SILInstruction *violatingUser,
                                              bool isUseConsuming,
                                              bool isInOutEndOfFunction) {
  if (!useWithDiagnostic.insert(violatingUser).second)
    return;
  registerDiagnosticEmitted(markedValue);

  auto &astContext = markedValue->getFunction()->getASTContext();
  SmallString<64> varName;
  getVariableNameForValue(markedValue, varName);

  LLVM_DEBUG(llvm::dbgs() << "Emitting error!\n");
  LLVM_DEBUG(llvm::dbgs() << "    Mark: " << *markedValue);
  LLVM_DEBUG(llvm::dbgs() << "    Last Live Use: " << *lastLiveUser);
  LLVM_DEBUG(llvm::dbgs() << "    Last Live Use Is Consuming? "
                          << (isUseConsuming ? "yes" : "no") << '\n');
  LLVM_DEBUG(llvm::dbgs() << "    Violating Use: " << *violatingUser);

  // If our liveness use is the same as our violating use, then we know that we
  // had a loop. Give a better diagnostic.
  if (lastLiveUser == violatingUser) {
    diagnose(astContext, markedValue,
             diag::sil_movechecking_value_consumed_in_a_loop, varName);
    diagnose(astContext, violatingUser,
             diag::sil_movechecking_consuming_use_here);
    return;
  }

  if (isInOutEndOfFunction) {
    diagnose(
        astContext, markedValue,
        diag::
            sil_movechecking_inout_not_reinitialized_before_end_of_function,
        varName);
    diagnose(astContext, violatingUser,
             diag::sil_movechecking_consuming_use_here);
    return;
  }

  // First if we are consuming emit an error for no implicit copy semantics.
  if (isUseConsuming) {
    diagnose(astContext, markedValue,
             diag::sil_movechecking_owned_value_consumed_more_than_once,
             varName);
    diagnose(astContext, violatingUser,
             diag::sil_movechecking_consuming_use_here);
    diagnose(astContext, lastLiveUser,
             diag::sil_movechecking_consumed_again_here);
    return;
  }

  // Otherwise, use the "used after consuming use" error.
  diagnose(astContext, markedValue,
           diag::sil_movechecking_value_used_after_consume, varName);
  diagnose(astContext, violatingUser,
           diag::sil_movechecking_consuming_use_here);
  diagnose(astContext, lastLiveUser,
           diag::sil_movechecking_nonconsuming_use_here);
}

void DiagnosticEmitter::emitInOutEndOfFunctionDiagnostic(
    MarkMustCheckInst *markedValue, SILInstruction *violatingUser) {
  if (!useWithDiagnostic.insert(violatingUser).second)
    return;
  registerDiagnosticEmitted(markedValue);

  assert(cast<SILFunctionArgument>(markedValue->getOperand())
             ->getArgumentConvention()
             .isInoutConvention() &&
         "Expected markedValue to be on an inout");

  auto &astContext = markedValue->getFunction()->getASTContext();
  SmallString<64> varName;
  getVariableNameForValue(markedValue, varName);

  LLVM_DEBUG(llvm::dbgs() << "Emitting inout error error!\n");
  LLVM_DEBUG(llvm::dbgs() << "    Mark: " << *markedValue);
  LLVM_DEBUG(llvm::dbgs() << "    Violating Use: " << *violatingUser);

  // Otherwise, we need to do no implicit copy semantics. If our last use was
  // consuming message:
  diagnose(
      astContext, markedValue,
      diag::sil_movechecking_inout_not_reinitialized_before_end_of_function,
      varName);
  diagnose(astContext, violatingUser,
           diag::sil_movechecking_consuming_use_here);
}

void DiagnosticEmitter::emitAddressDiagnosticNoCopy(
    MarkMustCheckInst *markedValue, SILInstruction *consumingUser) {
  if (!useWithDiagnostic.insert(consumingUser).second)
    return;

  auto &astContext = markedValue->getFunction()->getASTContext();
  SmallString<64> varName;
  getVariableNameForValue(markedValue, varName);

  LLVM_DEBUG(llvm::dbgs() << "Emitting no copy error!\n");
  LLVM_DEBUG(llvm::dbgs() << "    Mark: " << *markedValue);
  LLVM_DEBUG(llvm::dbgs() << "    Consuming Use: " << *consumingUser);

  // Otherwise, we need to do no implicit copy semantics. If our last use was
  // consuming message:
  diagnose(astContext, markedValue,
           diag::sil_movechecking_guaranteed_value_consumed, varName);
  diagnose(astContext, consumingUser,
           diag::sil_movechecking_consuming_use_here);
  registerDiagnosticEmitted(markedValue);
}

void DiagnosticEmitter::emitObjectDestructureNeededWithinBorrowBoundary(
    MarkMustCheckInst *markedValue, SILInstruction *destructureNeedingUser,
    TypeTreeLeafTypeRange destructureSpan,
    FieldSensitivePrunedLivenessBoundary &boundary) {
  if (!useWithDiagnostic.insert(destructureNeedingUser).second)
    return;

  auto &astContext = markedValue->getFunction()->getASTContext();
  SmallString<64> varName;
  getVariableNameForValue(markedValue, varName);

  LLVM_DEBUG(llvm::dbgs() << "Emitting destructure can't be created error!\n");
  LLVM_DEBUG(llvm::dbgs() << "    Mark: " << *markedValue);
  LLVM_DEBUG(llvm::dbgs() << "    Destructure Needing Use: "
                          << *destructureNeedingUser);

  diagnose(astContext, markedValue,
           diag::sil_movechecking_use_after_partial_consume, varName);
  diagnose(astContext, destructureNeedingUser,
           diag::sil_movechecking_partial_consume_here);

  // Only emit errors for last users that overlap with our needed destructure
  // bits.
  for (auto pair : boundary.getLastUsers()) {
    if (llvm::any_of(destructureSpan.getRange(),
                     [&](unsigned index) { return pair.second.test(index); })) {
      LLVM_DEBUG(llvm::dbgs()
                 << "    Destructure Boundary Use: " << *pair.first);
      diagnose(astContext, pair.first, diag::sil_movechecking_nonconsuming_use_here);
    }
  }
  registerDiagnosticEmitted(markedValue);
}

void DiagnosticEmitter::emitObjectInstConsumesValueTwice(
    MarkMustCheckInst *markedValue, Operand *firstUse, Operand *secondUse) {
  assert(firstUse->getUser() == secondUse->getUser());
  assert(firstUse->isConsuming());
  assert(secondUse->isConsuming());

  LLVM_DEBUG(llvm::dbgs() << "Emitting object consumes value twice error!\n");
  LLVM_DEBUG(llvm::dbgs() << "    Mark: " << *markedValue);
  LLVM_DEBUG(llvm::dbgs() << "    User: " << *firstUse->getUser());
  LLVM_DEBUG(llvm::dbgs() << "    First Conflicting Operand: "
                          << firstUse->getOperandNumber() << '\n');
  LLVM_DEBUG(llvm::dbgs() << "    Second Conflicting Operand: "
                          << secondUse->getOperandNumber() << '\n');

  auto &astContext = markedValue->getModule().getASTContext();
  SmallString<64> varName;
  getVariableNameForValue(markedValue, varName);
  diagnose(astContext, markedValue,
           diag::sil_movechecking_owned_value_consumed_more_than_once,
           varName);
  diagnose(astContext, firstUse->getUser(),
           diag::sil_movechecking_two_consuming_uses_here);
  registerDiagnosticEmitted(markedValue);
}

void DiagnosticEmitter::emitObjectInstConsumesAndUsesValue(
    MarkMustCheckInst *markedValue, Operand *consumingUse,
    Operand *nonConsumingUse) {
  assert(consumingUse->getUser() == nonConsumingUse->getUser());
  assert(consumingUse->isConsuming());
  assert(!nonConsumingUse->isConsuming());

  LLVM_DEBUG(llvm::dbgs() << "Emitting object consumeed and used error!\n");
  LLVM_DEBUG(llvm::dbgs() << "    Mark: " << *markedValue);
  LLVM_DEBUG(llvm::dbgs() << "    User: " << *consumingUse->getUser());
  LLVM_DEBUG(llvm::dbgs() << "    Consuming Operand: "
                          << consumingUse->getOperandNumber() << '\n');
  LLVM_DEBUG(llvm::dbgs() << "    Non Consuming Operand: "
                          << nonConsumingUse->getOperandNumber() << '\n');

  auto &astContext = markedValue->getModule().getASTContext();
  SmallString<64> varName;
  getVariableNameForValue(markedValue, varName);
  diagnose(astContext, markedValue,
           diag::sil_movechecking_owned_value_consumed_and_used_at_same_time,
           varName);
  diagnose(astContext, consumingUse->getUser(),
           diag::sil_movechecking_consuming_and_non_consuming_uses_here);
  registerDiagnosticEmitted(markedValue);
}

void DiagnosticEmitter::emitAddressEscapingClosureCaptureLoadedAndConsumed(
    MarkMustCheckInst *markedValue) {
  SmallString<64> varName;
  getVariableNameForValue(markedValue, varName);

  SILValue operand = stripAccessMarkers(markedValue->getOperand());

  // is it a class?
  if (isa<RefElementAddrInst>(operand)) {
    diagnose(markedValue->getModule().getASTContext(),
             markedValue,
             diag::sil_movechecking_notconsumable_but_assignable_was_consumed,
             varName, /*isGlobal=*/false);
    registerDiagnosticEmitted(markedValue);
    return;
  }

  // is it a global?
  if (isa<GlobalAddrInst>(operand)) {
    diagnose(markedValue->getModule().getASTContext(),
             markedValue,
             diag::sil_movechecking_notconsumable_but_assignable_was_consumed,
             varName, /*isGlobal=*/true);
    registerDiagnosticEmitted(markedValue);
    return;
  }

  // remaining cases must be a closure capture.
  diagnose(markedValue->getModule().getASTContext(),
           markedValue,
           diag::sil_movechecking_capture_consumed,
           varName);
  registerDiagnosticEmitted(markedValue);
}

void DiagnosticEmitter::emitPromotedBoxArgumentError(
    MarkMustCheckInst *markedValue, SILFunctionArgument *arg) {
  auto &astContext = fn->getASTContext();
  SmallString<64> varName;
  getVariableNameForValue(markedValue, varName);

  registerDiagnosticEmitted(markedValue);

  // diagnose consume of capture within a closure
  diagnose(astContext,
           arg->getDecl()->getLoc(),
           diag::sil_movechecking_capture_consumed,
           varName);

  // Now for each consuming use that needs a copy...
  for (auto *user : getCanonicalizer().consumingUsesNeedingCopy) {
    diagnose(astContext, user, diag::sil_movechecking_consuming_use_here);
  }

  for (auto *user : getCanonicalizer().consumingBoundaryUsers) {
    diagnose(astContext, user, diag::sil_movechecking_consuming_use_here);
  }
}

void DiagnosticEmitter::emitCannotDestructureDeinitNominalError(
    MarkMustCheckInst *markedValue, StringRef pathString,
    NominalTypeDecl *deinitedNominal, SILInstruction *consumingUser) {
  auto &astContext = fn->getASTContext();

  SmallString<64> varName;
  getVariableNameForValue(markedValue, varName);

  if (!pathString.empty())
    varName.append(pathString);

  diagnose(
      astContext, consumingUser,
      diag::sil_movechecking_cannot_destructure_has_deinit,
      varName);
  registerDiagnosticEmitted(markedValue);

  // point to the deinit if we know where it is.
  if (auto deinitLoc =
      deinitedNominal->getValueTypeDestructor()->getLoc(/*SerializedOK=*/false))
    astContext.Diags.diagnose(deinitLoc, diag::sil_movechecking_deinit_here);
}
