//===------ YieldOnceCheck.cpp - Check usage of yields in accessors  ------===//
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

// This pass statically verifies that yield-once coroutines, such as the
// generalized accessors `read` and `modify`, yield exactly once in every
// invocation, and diagnoses any violation of this property. This pass uses a
// linear-time, data-flow analysis to check that every path in the control-flow
// graph of the coroutine has a yield instruction before a return instruction.

#define DEBUG_TYPE "yield-once-check"

#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Stmt.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/CFG.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/TerminatorUtils.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockData.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "llvm/ADT/BreadthFirstIterator.h"
#include "llvm/ADT/DenseSet.h"

using namespace swift;

namespace {

class YieldOnceCheck : public SILFunctionTransform {

  template <typename... T, typename... U>
  static InFlightDiagnostic diagnose(ASTContext &Context, SourceLoc loc,
                                     Diag<T...> diag, U &&... args) {
    return Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
  }

  /// Data-flow analysis state that is associated with basic blocks.
  struct BBState {

    /// Indicates whether a basic block is encountered before seeing a yield
    /// (BeforeYield) or after seeing a yield (AfterYield), or in both states
    /// (Conflict). This enum is a semi-lattice where Conflict is the top and
    /// the merge of BeforeYield and AfterYield states is Conflict.
    enum YieldState { BeforeYield, AfterYield, Conflict } yieldState = Conflict;

  private:
    friend class BasicBlockData<BBState>;
  
    // The following states are maintained for emitting diagnostics.

    /// For AfterYield and Conflict states, this field records the yield
    /// instruction that was seen while propagating the state.
    SILInstruction *yieldInst = nullptr;

    /// For Conflict state, these fields record the basic blocks that
    /// propagated the 'AfterYield' and 'BeforeYield' states which resulted
    /// in the Conflict.
    SILBasicBlock *yieldingPred = nullptr;
    SILBasicBlock *nonYieldingPred = nullptr;
    
    bool visited = false;

    BBState() {}

    BBState(YieldState yState, SILInstruction *yieldI, SILBasicBlock *yieldPred,
            SILBasicBlock *noYieldPred)
        : yieldState(yState), yieldInst(yieldI), yieldingPred(yieldPred),
          nonYieldingPred(noYieldPred), visited(true) {}

  public:
    static BBState getInitialState() {
      return BBState(BeforeYield, nullptr, nullptr, nullptr);
    }

    static BBState getAfterYieldState(SILInstruction *yieldI) {
      assert(yieldI);
      return BBState(AfterYield, yieldI, nullptr, nullptr);
    }

    static BBState getConflictState(SILInstruction *yieldI,
                                    SILBasicBlock *yieldPred,
                                    SILBasicBlock *noYieldPred) {
      assert(yieldI && yieldPred && noYieldPred);
      return BBState(Conflict, yieldI, yieldPred, noYieldPred);
    }

    SILInstruction *getYieldInstruction() const {
      assert(yieldState == AfterYield || yieldState == Conflict);
      return yieldInst;
    }

    SILBasicBlock *getYieldingPred() {
      assert(yieldState == Conflict);
      return yieldingPred;
    }

    SILBasicBlock *getNonYieldingPred() {
      assert(yieldState == Conflict);
      return nonYieldingPred;
    }
    
    bool isVisited() const { return visited; }
  };

  /// A structure that records an error found during the analysis along with
  /// some context information that will be used by diagnostics.
  struct YieldError {
    /// The kind of error.
    enum Kind { MultipleYield, ReturnBeforeYield, ReturnOnConflict } errorKind;
    /// The termination instruction where the error should be reported.
    SILInstruction *termInst;
    /// The input state when the error is encountered.
    BBState inState;

  private:
    YieldError(Kind kind, SILInstruction *term, BBState state)
        : errorKind(kind), termInst(term), inState(state) {}

  public:
    static YieldError getMultipleYieldError(YieldInst *yield, BBState state) {
      assert(state.yieldState != BBState::BeforeYield);
      return YieldError(MultipleYield, yield, state);
    }

    static YieldError getReturnBeforeYieldError(ReturnInst *returnI,
                                                BBState state) {
      assert(state.yieldState == BBState::BeforeYield);
      return YieldError(ReturnBeforeYield, returnI, state);
    }

    static YieldError getReturnOnConflict(ReturnInst *returnI, BBState state) {
      assert(state.yieldState == BBState::Conflict);
      return YieldError(ReturnOnConflict, returnI, state);
    }
  };

  /// Transfer function of the data-flow analysis.
  ///
  /// \param bb Basic block that should be processed
  /// \param inState BBState at the start of the basic block
  /// \param error out parameter that will contain information about
  /// an error that is detected.
  /// \return the state at the exit of the basic block if it can be computed
  /// and None otherwise.
  static std::optional<BBState>
  transferStateThroughBasicBlock(SILBasicBlock *bb, BBState inState,
                                 std::optional<YieldError> &error) {
    error = std::nullopt;
    auto *term = bb->getTerminator();

    if (auto *returnInst = dyn_cast<ReturnInst>(term)) {
      if (inState.yieldState == BBState::BeforeYield) {
        error = YieldError::getReturnBeforeYieldError(returnInst, inState);
        return std::nullopt;
      }

      if (inState.yieldState == BBState::Conflict) {
        error = YieldError::getReturnOnConflict(returnInst, inState);
        return std::nullopt;
      }
      return inState;
    }

    if (auto *yieldInst = dyn_cast<YieldInst>(term)) {
      if (inState.yieldState != BBState::BeforeYield) {
        error = YieldError::getMultipleYieldError(yieldInst, inState);
        return std::nullopt;
      }

      // If the current state is BeforeYield and if the basic block ends in a
      // yield the new state is AfterYield.
      return inState.getAfterYieldState(term);
    }

    // We cannot have throws within generalized accessors.
    assert(!isa<ThrowInst>(term));

    return inState;
  }

  /// Merge operation of the data-flow analysis.
  ///
  /// \param mergeBlock the basic block that is reached with two states
  /// \param oldState the previous state at the entry of the basic block
  /// \param newState the current state at the entry of the basic block
  /// \param newStatePred the predecessor of 'mergeBlock' that has propagated
  ///        the 'newState'.
  /// \param bbToStateMap a map from the basic blocks visited by the analysis
  ///        to the BBStates in which they were seen. This is used to identify
  ///        blocks that propagate conflicting states when the merge results
  ///        in a conflict.
  /// \return the new state obtained by merging the oldState with the newState
  static BBState merge(SILBasicBlock *mergeBlock, BBState oldState,
                       BBState newState, SILBasicBlock *newStatePred,
                       BasicBlockData<BBState> &bbToStateMap) {
    auto oldYieldState = oldState.yieldState;
    auto newYieldState = newState.yieldState;

    if (oldYieldState == BBState::Conflict) {
      return oldState;
    }

    if (newYieldState == BBState::Conflict) {
      return newState;
    }

    if (oldYieldState == newYieldState) {
      return oldState;
    }

    // Here, one state is AfterYield and the other one is BeforeYield.
    // Merging them will result in Conflict.
    assert((newYieldState == BBState::AfterYield &&
            oldYieldState == BBState::BeforeYield) ||
           (newYieldState == BBState::BeforeYield &&
            oldYieldState == BBState::AfterYield));

    // For diagnostics, find another predecessor of 'mergeBlock' that was
    // previously seen by the analysis. This predecessor would have
    // propagated the 'oldState'.
    SILBasicBlock *oldStatePred = nullptr;
    for (auto predBB : mergeBlock->getPredecessorBlocks()) {
      if (predBB != newStatePred && bbToStateMap[predBB].isVisited()) {
        oldStatePred = predBB;
        break;
      }
    }
    assert(oldStatePred);

    if (oldState.yieldState == BBState::BeforeYield) {
      return BBState::getConflictState(newState.getYieldInstruction(),
                                       /* yieldPred */ newStatePred,
                                       /* noYieldPred */ oldStatePred);
    } else {
      return BBState::getConflictState(oldState.getYieldInstruction(),
                                       /* yieldPred */ oldStatePred,
                                       /* noYieldPred */ newStatePred);
    }
  }

  /// Perform a data-flow analysis to check whether there is exactly one
  /// yield before a return in every path in the control-flow graph.
  /// Diagnostics are not reported for nodes unreachable from the entry of
  /// the control-flow graph.
  void diagnoseYieldOnceUsage(SILFunction &fun) {
    BasicBlockData<BBState> bbToStateMap(&fun);

    SmallVector<SILBasicBlock *, 16> worklist;

    auto *entryBB = fun.getEntryBlock();
    bbToStateMap[entryBB] = BBState::getInitialState();
    worklist.push_back(entryBB);

    // ReturnBeforeYield errors, which denote that no paths yield before
    // returning, are not diagnosed until the analysis completes, in order to
    // distinguish them from ReturnOnConflict errors, which happen when some
    // paths yield and some don't.
    std::optional<YieldError> returnBeforeYieldError = std::nullopt;

    // The algorithm uses a worklist to propagate the state through basic
    // blocks until a fix point. Since the state lattice has height one, each
    // basic block will be visited at most twice, and at most once if there are
    // no conflicts (which are errors). The basic blocks are added to the
    // worklist in a breadth-first fashion. The order of visiting basic blocks
    // is not important for correctness, but it could change the errors
    // diagnosed when there are multiple errors. Breadth-first order diagnoses
    // errors along shorter paths to return.
    while (!worklist.empty()) {
      SILBasicBlock *bb = worklist.pop_back_val();
      const BBState &state = bbToStateMap[bb];
      assert(state.isVisited());

      std::optional<YieldError> errorResult = std::nullopt;
      auto resultState = transferStateThroughBasicBlock(bb, state, errorResult);

      if (!resultState.has_value()) {
        auto error = errorResult.value();

        // ReturnBeforeYield errors will not be reported until the analysis
        // completes. So record it and continue.
        if (error.errorKind == YieldError::ReturnBeforeYield) {
          if (!returnBeforeYieldError.has_value()) {
            returnBeforeYieldError = error;
          }
          continue;
        }

        emitDiagnostics(error, fun, bbToStateMap);
        return;
      }

      auto nextState = resultState.value();

      for (auto *succBB : bb->getSuccessorBlocks()) {
        BBState &succState = bbToStateMap[succBB];
        if (!succState.isVisited()) {
          // We are seeing the successor for the first time.
          // Add the successor to the worklist.
          succState = nextState;
          worklist.insert(worklist.begin(), succBB);
          continue;
        }

        // Here the successor already has a state. Merge the current and
        // previous states and propagate it if it is different from the
        // old state.
        auto mergedState = merge(succBB, succState, nextState, bb, bbToStateMap);

        if (mergedState.yieldState == succState.yieldState)
          continue;

        // Even though at this point there has to be an error since there is an
        // inconsistency between states coming along two different paths,
        // continue propagation of this conflict state to determine
        // whether this results in multiple-yields error or return-on-conflict
        // error.
        succState = mergedState;
        worklist.insert(worklist.begin(), succBB);
      }
    }

    if (returnBeforeYieldError.has_value()) {
      emitDiagnostics(returnBeforeYieldError.value(), fun, bbToStateMap);
    }
  }

  void emitDiagnostics(YieldError &error, SILFunction &fun,
                       BasicBlockData<BBState> &visitedBBs) {
    ASTContext &astCtx = fun.getModule().getASTContext();

    switch (error.errorKind) {
    case YieldError::ReturnBeforeYield: {
      diagnose(astCtx, error.termInst->getLoc().getSourceLoc(),
               diag::return_before_yield);
      return;
    }
    case YieldError::MultipleYield: {
      diagnose(astCtx, error.termInst->getLoc().getSourceLoc(),
               diag::multiple_yields);

      // Add a note that points to the previous yield.
      diagnose(astCtx,
               error.inState.getYieldInstruction()->getLoc().getSourceLoc(),
               diag::previous_yield);
      return;
    }
    case YieldError::ReturnOnConflict: {
      // Emit an error on the return statement.
      diagnose(astCtx, error.termInst->getLoc().getSourceLoc(),
               diag::possible_return_before_yield);

      // Here, the yield state of 'error' is Conflict.
      auto &conflictState = error.inState;

      // Emit a note that pin points the branch construct that resulted in
      // this conflict. Note that a conflict state is created at a merge block
      // when along one incoming edge the analysis sees a BeforeYield state
      // and along another it sees an AfterYield state.
      // Also note that, by the definition of the merge operation,
      // 'error.yieldingPred()' is the immediate predecessor of the merge block
      // that propagates AfterYield state, and 'error.nonYieldingPred()' is
      // the immediate predecessor of the merge block that propagates a
      // BeforeYield state.
      auto yieldPred = conflictState.getYieldingPred();
      auto noYieldPred = conflictState.getNonYieldingPred();

      // Step 1: find a branching SIL instruction where one branch has
      // 'yieldPred' and another branch has 'noYieldPred'. For instance,
      // in the following example, cond_br is the instruction to find.
      //              cond_br bb1, bb2
      //              bb1:
      //                  yield resume yieldPred, unwind err
      //              bb2:
      //                  br noYieldPred
      //              yieldPred:
      //                  br mergePoint
      //              noYieldPred:
      //                  br mergePoint
      //              mergePoint:
      //                  ...
      // Intuitively, the conflicting branch is the instruction where
      // 'yieldPred' and 'noYieldPred' meet when traversing the CFG in the
      // reverse order. More formally, the branching instruction is a
      // "lowest common ancestor" of 'yieldPred' and 'noYieldPred' in the
      // the DAG obtained from the CFG by ignoring back edges of loops.
      //
      // Note that the lowest common ancestor may not be unique in a DAG.
      // But, any such ancestor could be considered as the conflicting branch as
      // 'yieldPred' and 'noYieldPred' will belong to two different branches of
      // every such ancestor.

      // Find all transitive predecessors of 'yieldPred' that were visited
      // during the analysis
      BasicBlockSet predecessorsOfYieldPred(&fun);
      for (auto *predBB :
           llvm::breadth_first<llvm::Inverse<SILBasicBlock *>>(yieldPred)) {
        if (visitedBBs[predBB].isVisited()) {
          predecessorsOfYieldPred.insert(predBB);
        }
      }

      // Find the first predecessor of 'noYieldPred' that is also a predecessor
      // of 'yieldPred', in the breadth-first search order of the reversed CFG.
      SILBasicBlock *lowestCommonAncestorBB = nullptr;
      BasicBlockSet predecessorsOfNoYieldPred(&fun);
      for (auto *pred :
           llvm::breadth_first<llvm::Inverse<SILBasicBlock *>>(noYieldPred)) {
        if (!visitedBBs[pred].isVisited()) {
          continue;
        }
        if (predecessorsOfYieldPred.contains(pred)) {
          lowestCommonAncestorBB = pred;
          break;
        }
        predecessorsOfNoYieldPred.insert(pred);
      }
      assert(lowestCommonAncestorBB);

      auto *conflictingBranch = lowestCommonAncestorBB->getTerminator();

      // Step 2: Find the target basic block of the 'conflictingBranch' that
      // doesn't yield.
      SILBasicBlock *noYieldTarget = nullptr;
      for (auto *succ : lowestCommonAncestorBB->getSuccessorBlocks()) {
        if (predecessorsOfNoYieldPred.contains(succ)) {
          noYieldTarget = succ;
          break;
        }
      }
      assert(noYieldTarget);

      // Step 3: Report specialized diagnostics for each kind of conflicting
      // branch.

      // For conditional-branch instructions, which correspond to the
      // conditions of 'if', 'where' or 'guard' statements, report for what
      // truth value the branch doesn't yield.
      if (auto *condbr = dyn_cast<CondBranchInst>(conflictingBranch)) {
        diagnose(astCtx, condbr->getLoc().getSourceLoc(),
                 diag::branch_doesnt_yield,
                 /*non-yielding branch*/ condbr->getTrueBB() == noYieldTarget);
        return;
      }

      // For switch_enum instructions, report the case that doesn't yield.
      enum SwitchCaseKind { Default, OptionNil, OptionSome };

      if (auto switchEnum = SwitchEnumTermInst(conflictingBranch)) {
        auto enumCaseLoc = noYieldTarget->begin()->getLoc().getSourceLoc();

        if (switchEnum.hasDefault() &&
            switchEnum.getDefaultBB() == noYieldTarget) {
          diagnose(astCtx, enumCaseLoc, diag::case_doesnt_yield, Default);
          return;
        }

        // Find the case identifier that doesn't yield.
        NullablePtr<EnumElementDecl> enumElemDecl =
            switchEnum.getUniqueCaseForDestination(noYieldTarget);
        assert(enumElemDecl.isNonNull());

        // Specialize diagnostics for cases of an optional.
        if (enumElemDecl.get() == astCtx.getOptionalNoneDecl()) {
          diagnose(astCtx, enumCaseLoc, diag::case_doesnt_yield, OptionNil);
        } else if (enumElemDecl.get() == astCtx.getOptionalSomeDecl()) {
          diagnose(astCtx, enumCaseLoc, diag::case_doesnt_yield, OptionSome);
        } else {
          diagnose(astCtx, enumCaseLoc, diag::named_case_doesnt_yield,
                   enumElemDecl.get()->getBaseIdentifier());
        }
        return;
      }

      // For switch_value instructions, report the case number that doesn't
      // yield.
      if (auto *switchValue = dyn_cast<SwitchValueInst>(conflictingBranch)) {
        auto enumCaseLoc = noYieldTarget->begin()->getLoc().getSourceLoc();

        if (switchValue->hasDefault() &&
            switchValue->getDefaultBB() == noYieldTarget) {
          diagnose(astCtx, enumCaseLoc, diag::case_doesnt_yield, Default);
          return;
        }
        // Find the case that doesn't yield.
        std::optional<unsigned> caseNumberOpt =
            switchValue->getUniqueCaseForDestination(noYieldTarget);
        assert(caseNumberOpt.has_value());

        auto caseNumber = caseNumberOpt.value() + 1;
        diagnose(
            astCtx, enumCaseLoc, diag::switch_value_case_doesnt_yield,
            (Twine(caseNumber) + llvm::getOrdinalSuffix(caseNumber)).str());
        return;
      }

      // For try-apply instructions, report whether throwing or non-throwing
      // case doesn't yield.
      if (auto *tryApply = dyn_cast<TryApplyInst>(conflictingBranch)) {
        diagnose(astCtx, tryApply->getLoc().getSourceLoc(),
                 diag::try_branch_doesnt_yield,
                 /*does error case not yield?*/ tryApply->getErrorBB() ==
                     noYieldTarget);
        return;
      }

      llvm_unreachable("unexpected branch resulting in conflicting yield "
                       "states found in generalized accessor");
    }
    }
  }

  /// The entry point to the transformation.
  void run() override {
    auto *fun = getFunction();

    if (fun->getLoweredFunctionType()->getCoroutineKind() !=
        SILCoroutineKind::YieldOnce)
      return;

    diagnoseYieldOnceUsage(*fun);
  }
};

} // end anonymous namespace

SILTransform *swift::createYieldOnceCheck() {
  return new YieldOnceCheck();
}
