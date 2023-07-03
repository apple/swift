//===--- RedundantOverflowCheckRemoval.cpp --------------------------------===//
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
// Remove overflow checks that are guarded by control flow or other
// overflow checks.
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "remove-redundant-overflow-checks"

#include "swift/SIL/Dominance.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"

using namespace swift;

STATISTIC(NumCondFailRemoved, "Number of cond_fail instructions removed");

namespace {

class RedundantOverflowCheckRemovalPass : public SILFunctionTransform {
public:
  RedundantOverflowCheckRemovalPass() {}

  /// This enum represents a relationship between two operands.
  /// The relationship represented by arithmetic operators represents the
  /// information that the operation did not trap.
  ///
  /// The following code translate (with the correct signedness prefix):
  ///
  /// if (x > 2) { x }           -> LT(2, x)
  /// if (x > 2) {} else { x }   -> LE(x, 2)
  /// x - 2                      -> Sub(x, 2)
  /// 2 - x                      -> Sub(2, x)
  /// 2 * x                      -> Mul(2, x)
  /// x + y                      -> Add(x, y)
  enum class ValueRelation {EQ, ULT, ULE, UAdd, USub, UMul,
                                SLT, SLE, SAdd, SSub, SMul};

  /// This struct represents a constraint on the range of some values in some
  /// basic blocks in the program.
  /// For example, it can represent the constraint "X < 2" for some blocks in
  /// the function.
  struct Constraint {
    Constraint(SILBasicBlock *BB, SILValue L, SILValue R, ValueRelation Rel) :
      DominatingBlock(BB), Left(L), Right(R), Relationship(Rel) {
      LLVM_DEBUG(dump());
    }

    /// The constraint is valid in blocks dominated by this block.
    SILBasicBlock *DominatingBlock;
    /// The first operand.
    SILValue Left;
    /// The second operand.
    SILValue Right;
    /// Describes the relationship between the operands.
    ValueRelation Relationship;

    /// Print the content of the constraint.
    void dump() {
      llvm::dbgs()<<"Constraint [" << DominatingBlock <<"]\n";
      llvm::dbgs()<<"  Relationship:";
      switch (Relationship) {
        case ValueRelation::EQ:   llvm::dbgs()<<"Equal\n"; break;
        case ValueRelation::SLT:  llvm::dbgs()<<"SLT\n"; break;
        case ValueRelation::ULT:  llvm::dbgs()<<"ULT\n"; break;
        case ValueRelation::SLE:  llvm::dbgs()<<"SLE\n"; break;
        case ValueRelation::ULE:  llvm::dbgs()<<"ULE\n"; break;
        case ValueRelation::SMul: llvm::dbgs()<<"SMul\n"; break;
        case ValueRelation::SSub: llvm::dbgs()<<"SSub\n"; break;
        case ValueRelation::SAdd: llvm::dbgs()<<"SAdd\n"; break;
        case ValueRelation::UMul: llvm::dbgs()<<"UMul\n"; break;
        case ValueRelation::USub: llvm::dbgs()<<"USub\n"; break;
        case ValueRelation::UAdd: llvm::dbgs()<<"UAdd\n"; break;
      }
      llvm::dbgs()<<"  Left:"; Left->dump();
      llvm::dbgs()<<"  Right:"; Right->dump();
    }
  };

  typedef SmallVector<Constraint, 16>  ConstraintList;
  typedef SmallVector<CondFailInst*, 16>  CondFailList;

  /// A list of constraints that represent the value relationships.
  ConstraintList Constraints;

  /// A list of cond_fail instructions to remove.
  CondFailList ToRemove;

  // Dominators info.
  DominanceInfo *DT;

  /// Remove the instructions that were marked as redundant
  /// and return True if and instructions were removed.
  bool removeCollectedRedundantInstructions() {
    if (ToRemove.size()) {
      LLVM_DEBUG(llvm::dbgs() << "Removing " << ToRemove.size()
                              << " condfails in "
                              << getFunction()->getName() << "\n");

      for (auto *CF : ToRemove) {
        CF->eraseFromParent();
        ++NumCondFailRemoved;
      }
      ToRemove.clear();
      return true;
    }
    return false;
  }

  void run() override {
    DT = PM->getAnalysis<DominanceAnalysis>()->get(getFunction());
    auto *PO = getAnalysis<PostOrderAnalysis>()->get(getFunction());
    Constraints.clear();
    ToRemove.clear();

    auto ReversePostOrder = PO->getReversePostOrder();

    // Perform a forward scan and use control flow and previously detected
    // overflow checks to remove the overflow checks.

    // For each block in a Reverse Post Order scan:
    for (auto &BB : ReversePostOrder) {
      // For each instruction:
      for (auto Inst = BB->begin(), End = BB->end(); Inst != End; ++Inst) {
        // Use branch information for eliminating condfails.
        if (auto *CBI = dyn_cast<CondBranchInst>(Inst))
          registerBranchFormula(CBI);

        // Handle cond_fail instructions.
        if (auto *CFI = dyn_cast<CondFailInst>(Inst)) {
          if (tryToRemoveCondFail(CFI)) {
            ToRemove.push_back(CFI);
            continue;
          }

          // We were not able to remove the condfail. Try to use this
          // information to remove other cond_fails.
          registerCondFailFormula(CFI);
        }
      }
    }

    // If we've collected redundant cond_fails then remove them now.
    bool Changed = removeCollectedRedundantInstructions();

    // Perform another check, this time in reverse and use future overflow
    // checks that must be executed to eliminate earlier overflow checks.
    // Notice that this scan is only block local because at this point we
    // don't use post-dominators.

    for (auto &BB : ReversePostOrder) {
      // Clear the list of constraint on every block.
      Constraints.clear();

      // Notice: we scan the basic block in reverse.
      for (auto Inst = --BB->end(), End = BB->begin(); Inst != End; --Inst) {
        if (auto *CFI = dyn_cast<CondFailInst>(Inst)) {
          // Try to remove the cond_fail based on previous overflow checks.
          if (tryToRemoveCondFail(CFI)) {
            ToRemove.push_back(CFI);
            continue;
          }

          // Record the overflow check and try to optimize other checks.
          registerCondFailFormula(CFI);
          continue;
        }

        // We do not optimize overflow checks across instructions with side
        // effects because we don't want to delay the trap past user-visible
        // changes.
        if (Inst->mayHaveSideEffects()) {
          Constraints.clear();
          continue;
        }

      }
    }

    // If we've collected more redundant cond_fails then remove them now.
    Changed |= removeCollectedRedundantInstructions();

    if (Changed)
      PM->invalidateAnalysis(getFunction(),
                             SILAnalysis::InvalidationKind::Instructions);
  }

  /// Return True if the relationship \p Rel describes a known relation
  /// between A and B.
  static bool knownRelation(SILValue A, SILValue B, ValueRelation Rel) {
    // Identical values are known to be equal, or less than or equal.
    if ((A == B) && (Rel == ValueRelation::EQ   ||
                     Rel == ValueRelation::SLE  ||
                     Rel == ValueRelation::ULE))
      return true;

    // Evaluate literal integers.
    auto *AI = dyn_cast<IntegerLiteralInst>(A);
    auto *BI = dyn_cast<IntegerLiteralInst>(B);
    if (AI && BI) {
      APInt Ap = AI->getValue();
      APInt Bp = BI->getValue();

      if (Ap.getBitWidth() != Bp.getBitWidth())
        return false;

      switch (Rel) {
        case ValueRelation::EQ:  return Ap.eq(Bp);
        case ValueRelation::SLE: return Ap.sle(Bp);
        case ValueRelation::ULE: return Ap.ule(Bp);
        case ValueRelation::SLT: return Ap.slt(Bp);
        case ValueRelation::ULT: return Ap.ult(Bp);
        default: llvm_unreachable("Invalid value relation");
      }
    }
    return false;
  }

  /// Return True if we can deduct that \p N is always positive (N > 0).
  static bool isKnownPositive(SILValue N) {
    if (auto *NI = dyn_cast<IntegerLiteralInst>(N))
      return NI->getValue().isStrictlyPositive();
    return false;
  }

  /// Return true if the absolute value of \p A is smaller than the
  /// absolute value of \p B. In other words, check if \p A known to be closer
  /// to zero.
  static bool isKnownAbsLess(SILValue A, SILValue B) {
    auto *AI = dyn_cast<IntegerLiteralInst>(A);
    auto *BI = dyn_cast<IntegerLiteralInst>(B);

    if (AI && BI)
      return AI->getValue().abs().ult(BI->getValue().abs());

    return false;
  }

  /// Return true if the constraint \p F can prove that the overflow check
  /// for \p BI is not needed.
  static bool isOverflowCheckRemovedByConstraint(Constraint &F,
                                                 BuiltinInst *BI) {
    // L and R are the righthand and lefthand sides of the constraint.
    SILValue L = F.Left;
    SILValue R = F.Right;
    assert(L->getType() == R->getType() && "Invalid constraint type");

    // Make sure that the types of the constraints match the types of the
    // arithmetic operation.
    switch (BI->getBuiltinInfo().ID) {
      default: return false;
      case BuiltinValueKind::SAddOver:
      case BuiltinValueKind::UAddOver:
      case BuiltinValueKind::SMulOver:
      case BuiltinValueKind::UMulOver:
      case BuiltinValueKind::USubOver:
      case BuiltinValueKind::SSubOver:
        if (L->getType() != BI->getOperand(0)->getType())
          return false;
    }

    switch (BI->getBuiltinInfo().ID) {
      default: return false;
      case BuiltinValueKind::SAddOver:
        //  A + B traps unless:
        if (F.Relationship == ValueRelation::SAdd) {
          // L + R already known to not trap at this point in the program.
          // And the following applies:
          // L >= A and R >= B or (commutatively) R >= A and L >= B.
          SILValue A = BI->getOperand(0);
          SILValue B = BI->getOperand(1);
          if (knownRelation(A, L,  ValueRelation::SLE) &&
              knownRelation(B, R, ValueRelation::SLE))
            return true;
          if (knownRelation(B, L,  ValueRelation::SLE) &&
              knownRelation(A, R, ValueRelation::SLE))
            return true;
        }

        // A + 1 does not trap if A is smaller than anything.
        if (F.Relationship == ValueRelation::SLT) {
          SILValue A = BI->getOperand(0);
          SILValue B = BI->getOperand(1);
          auto *AI = dyn_cast<IntegerLiteralInst>(A);
          auto *BI = dyn_cast<IntegerLiteralInst>(B);
          if (L == A && BI && BI->getValue().getSExtValue() == 1)
            return true;
          if (L == B && AI && AI->getValue().getSExtValue() == 1)
            return true;
        }

        return false;
      case BuiltinValueKind::UAddOver:
        //  A + B traps unless:
        if (F.Relationship == ValueRelation::UAdd) {
          // L + R already known to not trap at this point in the program.
          // And the following applies:
          // L >= A and R >= B or (commutatively) R >= A and L >= B.
          SILValue A = BI->getOperand(0);
          SILValue B = BI->getOperand(1);
          if (knownRelation(A, L,  ValueRelation::ULE) &&
              knownRelation(B, R, ValueRelation::ULE))
            return true;
          if (knownRelation(B, L,  ValueRelation::ULE) &&
              knownRelation(A, R, ValueRelation::ULE))
            return true;
        }

        // A + 1 does not trap if A is smaller than anything.
        if (F.Relationship == ValueRelation::ULT) {
          SILValue A = BI->getOperand(0);
          SILValue B = BI->getOperand(1);
          auto *AI = dyn_cast<IntegerLiteralInst>(A);
          auto *BI = dyn_cast<IntegerLiteralInst>(B);
          if (L == A && BI && BI->getValue().getZExtValue() == 1)
            return true;
          if (L == B && AI && AI->getValue().getZExtValue() == 1)
            return true;
        }
        return false;

      case BuiltinValueKind::SMulOver:
        //  A * B traps unless:
        if (F.Relationship == ValueRelation::SMul) {
          // L * R already known to not trap at this point in the program and
          // the following rules apply:
          //
          // A is closer zero than L and B == R,
          // or A == L, and B is closer to zero than R.
          //
          // We do not allow removing the overflow checks when one of the
          // multipliers just switches the sign (abs(L) == abs(A)) because
          // there are more negative numbers and (-MIN_INT * -1 overflows).
          // In other words X * -1 does not does not guard X * 1.
          SILValue A = BI->getOperand(0);
          SILValue B = BI->getOperand(1);

          if (isKnownAbsLess(A, L) &&
              knownRelation(B, R, ValueRelation::EQ))
            return true;
          if (knownRelation(A, L, ValueRelation::EQ) &&
              isKnownAbsLess(B, R))
            return true;

          // And commutatively, swapping A and B.
          if (isKnownAbsLess(B, L) &&
              knownRelation(A, R, ValueRelation::EQ))
            return true;
          if (knownRelation(B, L, ValueRelation::EQ) &&
              isKnownAbsLess(A, R))
            return true;
        }
        return false;
      case BuiltinValueKind::UMulOver:
        //  A * B traps unless:
        if (F.Relationship == ValueRelation::UMul) {
          // L * R already known to not trap at this point in the program.
          // And the following applies:
          // L >= A and R >= B or (commutatively) R >= A and L >= B.
          SILValue A = BI->getOperand(0);
          SILValue B = BI->getOperand(1);
          if (knownRelation(A, L,  ValueRelation::ULE) &&
              knownRelation(B, R, ValueRelation::ULE))
            return true;
          if (knownRelation(B, L,  ValueRelation::ULE) &&
              knownRelation(A, R, ValueRelation::ULE))
            return true;
        }
        return false;

      case BuiltinValueKind::USubOver:
        //  A - B traps unless:
        if (F.Relationship == ValueRelation::ULE ||
            F.Relationship == ValueRelation::ULT) {
          //  A >= B.
          // Given the constraint L < R check if:
          // 1. R == A
          // 2. B <= L (subtracting less than L is okay)
          //
          // Example: Given 2<X we know that X-2 can't trap.
          SILValue A = BI->getOperand(0);
          SILValue B = BI->getOperand(1);
          if (knownRelation(R, A, ValueRelation::EQ) &&
              knownRelation(B, L, ValueRelation::ULE)) {
            return true;
          }
        }

        if (F.Relationship == ValueRelation::EQ) {
          SILValue A = BI->getOperand(0);
          SILValue B = BI->getOperand(1);
          // A - B, L == R and known that L >= A and R == B.
          if (knownRelation(R, B, ValueRelation::EQ) &&
              knownRelation(A, L, ValueRelation::ULE)) {
            return true;
          }
          // Swap L and R because equality is commutative.
          if (knownRelation(L, B, ValueRelation::EQ) &&
              knownRelation(A, R, ValueRelation::ULE)) {
            return true;
          }
        }

        if (F.Relationship == ValueRelation::USub) {
          // L - R already known to not trap at this point in the program.
          // And the following applies:
          // L <= A and B <= R.
          SILValue A = BI->getOperand(0);
          SILValue B = BI->getOperand(1);
          if (knownRelation(L, A, ValueRelation::ULE) &&
              knownRelation(B, R,  ValueRelation::ULE))
            return true;
        }

        // A - 1 does not trap if A is greater than some other number.
        if (F.Relationship == ValueRelation::ULT) {
          SILValue A = BI->getOperand(0);
          SILValue B = BI->getOperand(1);
          auto *BI = dyn_cast<IntegerLiteralInst>(B);
          if (R == A && BI && BI->getValue().getZExtValue() == 1)
            return true;
        }

        return false;
      case BuiltinValueKind::SSubOver:
        //  A - B traps unless:
        if (F.Relationship == ValueRelation::SLE ||
            F.Relationship == ValueRelation::SLT) {
          //  A >= B and B is positive.
          // Notice that we need to handle underflow and overflow.

          // Given the constraint L < R check if:
          // 1. L is positive (because double negative can overflow)
          // 2. R == A
          // 3. B <= L (subtracting less than L is okay)
          //
          // Example: Given 2<X we know that X-2 can't trap.
          SILValue A = BI->getOperand(0);
          SILValue B = BI->getOperand(1);
          if (isKnownPositive(L) &&
              knownRelation(R, A, ValueRelation::EQ) &&
              knownRelation(B, L, ValueRelation::SLE)) {
            return true;
          }
        }

        if (F.Relationship == ValueRelation::SSub) {
          // L - R already known to not trap at this point in the program.
          // And the following applies:
          // L <= A and B <= R.
          SILValue A = BI->getOperand(0);
          SILValue B = BI->getOperand(1);
          if (knownRelation(L, A, ValueRelation::SLE) &&
              knownRelation(B, R,  ValueRelation::SLE))
            return true;
        }

        if (F.Relationship == ValueRelation::EQ) {
          SILValue A = BI->getOperand(0);
          SILValue B = BI->getOperand(1);
          // A - B, L == R and known that L >= A and R == B.
          if (knownRelation(R, B, ValueRelation::EQ) &&
              knownRelation(A, L, ValueRelation::SLE)) {
            return true;
          }
          // Swap L and R because equality is commutative.
          if (knownRelation(L, B, ValueRelation::EQ) &&
              knownRelation(A, R, ValueRelation::SLE)) {
            return true;
          }
        }

        // A - 1 does not trap if A is greater than some other number.
        if (F.Relationship == ValueRelation::SLT) {
          SILValue A = BI->getOperand(0);
          SILValue B = BI->getOperand(1);
          auto *BI = dyn_cast<IntegerLiteralInst>(B);
          if (R == A && BI && BI->getValue().getSExtValue() == 1)
            return true;
        }
        return false;
    }
  }

  bool tryToRemoveCondFail(CondFailInst *CFI) {
    // Extract the arithmetic operation from the condfail.
    auto *TEI = dyn_cast<TupleExtractInst>(CFI->getOperand());
    if (!TEI) return false;
    auto *BI = dyn_cast<BuiltinInst>(TEI->getOperand());
    if (!BI) return false;

    for (auto &F : Constraints) {
      // If we are dominated by a constraint:
      if (DT->dominates(F.DominatingBlock, CFI->getParent())) {
        // Try to use the constraint to remove the overflow check.
        if (isOverflowCheckRemovedByConstraint(F, BI)) {
          return true;
        }
      }
    }

    // Was not able to remove this branch.
    return false;
  }

  llvm::Optional<ValueRelation> getArithOpRelation(BuiltinInst *BI) {
    ValueRelation Rel;
    switch (BI->getBuiltinInfo().ID) {
    default:
      return llvm::None;
    case BuiltinValueKind::SAddOver:
      Rel = ValueRelation::SAdd;
      break;
    case BuiltinValueKind::UAddOver:
      Rel = ValueRelation::UAdd;
      break;
    case BuiltinValueKind::SSubOver:
      Rel = ValueRelation::SSub;
      break;
    case BuiltinValueKind::USubOver:
      Rel = ValueRelation::USub;
      break;
    case BuiltinValueKind::SMulOver:
      Rel = ValueRelation::SMul;
      break;
    case BuiltinValueKind::UMulOver:
      Rel = ValueRelation::UMul;
      break;
    }
    return Rel;
  }

  void addComparisonRelation(BuiltinInst *CMP, SILBasicBlock *TrueBB,
                             SILBasicBlock *FalseBB) {
    // The relationship expressed in the builtin.
    ValueRelation TrueRel;
    ValueRelation FalseRel;
    bool Swap = false;

    switch (CMP->getBuiltinInfo().ID) {
    default:
      return;
    case BuiltinValueKind::ICMP_NE: {
      SILValue Left = CMP->getOperand(0);
      SILValue Right = CMP->getOperand(1);
      if (FalseBB)
        Constraints.push_back(
            Constraint(FalseBB, Left, Right, ValueRelation::EQ));
      return;
    }
    case BuiltinValueKind::ICMP_EQ: {
      SILValue Left = CMP->getOperand(0);
      SILValue Right = CMP->getOperand(1);
      if (TrueBB)
        Constraints.push_back(
            Constraint(TrueBB, Left, Right, ValueRelation::EQ));
      return;
    }
    case BuiltinValueKind::ICMP_SLE:
      TrueRel = ValueRelation::SLE;
      FalseRel = ValueRelation::SLT;
      break;
    case BuiltinValueKind::ICMP_SLT:
      TrueRel = ValueRelation::SLT;
      FalseRel = ValueRelation::SLE;
      break;
    case BuiltinValueKind::ICMP_SGE:
      TrueRel = ValueRelation::SLT;
      FalseRel = ValueRelation::SLE;
      Swap = true;
      break;
    case BuiltinValueKind::ICMP_SGT:
      TrueRel = ValueRelation::SLE;
      FalseRel = ValueRelation::SLT;
      Swap = true;
      break;
    case BuiltinValueKind::ICMP_ULE:
      TrueRel = ValueRelation::ULE;
      FalseRel = ValueRelation::ULT;
      break;
    case BuiltinValueKind::ICMP_ULT:
      TrueRel = ValueRelation::ULT;
      FalseRel = ValueRelation::ULE;
      break;
    case BuiltinValueKind::ICMP_UGT:
      TrueRel = ValueRelation::ULE;
      FalseRel = ValueRelation::ULT;
      Swap = true;
      break;
    case BuiltinValueKind::ICMP_UGE:
      TrueRel = ValueRelation::ULT;
      FalseRel = ValueRelation::ULE;
      Swap = true;
      break;
    }

    SILValue Left = CMP->getOperand(0);
    SILValue Right = CMP->getOperand(1);

    if (Swap)
      std::swap(Left, Right);

    // Set the constraints for both side of the conditional branch, if
    // that the condition is dominating the dest block (see comment above).
    if (TrueBB)  Constraints.push_back(Constraint(TrueBB,  Left, Right, TrueRel));
    if (FalseBB) Constraints.push_back(Constraint(FalseBB, Right, Left, FalseRel));
  }

  void registerCondFailFormula(CondFailInst *CFI) {
    // Extract the arithmetic operation from the condfail.
    if (auto *TEI = dyn_cast<TupleExtractInst>(CFI->getOperand())) {
      auto *BI = dyn_cast<BuiltinInst>(TEI->getOperand());
      if (!BI)
        return;

      // The relationship expressed in the builtin.
      llvm::Optional<ValueRelation> Rel = getArithOpRelation(BI);
      if (!Rel.has_value())
        return;

      // Construct and register the constraint.
      SILBasicBlock *Dom = CFI->getParent();
      SILValue Left = BI->getOperand(0);
      SILValue Right = BI->getOperand(1);
      Constraint F = Constraint(Dom, Left, Right, *Rel);
      Constraints.push_back(F);
    }

    //  Handle patterns like this:
    //  %cmp_result = builtin "cmp_ult_Int64"
    //    (%x : $Builtin.Int64, %y : $Builtin.Int64) : $Builtin.Int1
    //  This cond_fail formula should be registered!
    //  cond_fail %cmp_result : $Builtin.Int1
    //  %check_underflow = integer_literal $Builtin.Int1, -1
    //  At this point we know that x >= y
    //  %usub_result = builtin "usub_with_overflow_Int64"
    //    (%x : $Builtin.Int64, %y : $Builtin.Int64, %check_underflow : $Builtin.Int1):
    //       $(Builtin.Int64, Builtin.Int1)
    //  %usub_val = tuple_extract %usub_result : $(Builtin.Int64, Builtin.Int1), 0
    //  We can figure out that x - y will not underflow because of x >= y
    //  %usub_underflow = tuple_extract %usub_result : $(Builtin.Int64, Builtin.Int1), 1
    //  cond_fail %usub_underflow : $Builtin.Int1
    if (auto *CMP = dyn_cast<BuiltinInst>(CFI->getOperand())) {
      SILBasicBlock *TrueBB = nullptr;
      SILBasicBlock *FalseBB = CMP->getParent();
      addComparisonRelation(CMP, TrueBB, FalseBB);
    }
  }


  void registerBranchFormula(CondBranchInst *BI) {
    // Extract the arithmetic operation from the Branch.
    auto *CMP = dyn_cast<BuiltinInst>(BI->getCondition());
    if (!CMP) return;

    SILBasicBlock *TrueBB = BI->getTrueBB();
    SILBasicBlock *FalseBB = BI->getFalseBB();

    // Notice that we need to handle control-flow programs such as the one
    // below. The rule here is that only blocks with a single predecessor
    // and blocks that are dominated by them can rely on branch information.
    // The reason is that if there is not a single predecessor then the code
    // that is dominated by the block can be reachable from other blocks.
    //
    //        [ x > 2 ]
    //         /   |
    //        /    |
    //       /     |
    //    [ .. ]   |
    //       \     |
    //        \    |
    //         \   |
    //          \  v
    //         [use(x)]
    if (!TrueBB->getSinglePredecessorBlock())
      TrueBB = nullptr;
    if (!FalseBB->getSinglePredecessorBlock())
      FalseBB = nullptr;

    addComparisonRelation(CMP, TrueBB, FalseBB );
  }

};
} // end anonymous namespace

SILTransform *swift::createRedundantOverflowCheckRemoval() {
  return new RedundantOverflowCheckRemovalPass();
}
