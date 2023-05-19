//===--- ArrayBoundsCheckOpts.cpp - Bounds check elim ---------------------===//
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

#define DEBUG_TYPE "sil-abcopts"

#include "swift/AST/Builtins.h"
#include "swift/Basic/STLExtras.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/Analysis/ArraySemantic.h"
#include "swift/SILOptimizer/Analysis/DestructorAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/IVAnalysis.h"
#include "swift/SILOptimizer/Analysis/LoopAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/SILSSAUpdater.h"

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Debug.h"

#include <algorithm>

#include "llvm/Support/CommandLine.h"

using namespace swift;
using namespace PatternMatch;

static llvm::cl::opt<bool> ShouldReportBoundsChecks("sil-abcopts-report",
                                              llvm::cl::init(false));

static llvm::cl::opt<bool> EnableABCOpts("enable-abcopts",
                                         llvm::cl::init(true));

static llvm::cl::opt<bool> EnableABCHoisting("enable-abc-hoisting",
                                             llvm::cl::init(true));


using ArraySet = llvm::SmallPtrSet<SILValue, 16>;
// A pair of the array pointer and the array check kind (kCheckIndex or
// kCheckSubscript).
using ArrayAccessDesc = llvm::PointerIntPair<ValueBase *, 1, bool>;
using IndexedArraySet = llvm::DenseSet<std::pair<ValueBase *, ArrayAccessDesc>>;
using InstructionSet = llvm::SmallPtrSet<SILInstruction *, 16>;

/// The effect an instruction can have on array bounds.
enum class ArrayBoundsEffect {
  kNone = 0,
  kMayChangeArg, // Can only change the array argument.
  kMayChangeAny  // Might change any array.
};

static SILValue getArrayStructPointer(ArrayCallKind K, SILValue Array) {
  assert(K != ArrayCallKind::kNone);

  if (K < ArrayCallKind::kMakeMutable) {
    auto LI = dyn_cast<LoadInst>(lookThroughCopyValueInsts(Array));
    if (!LI) {
      return Array;
    }
    return LI->getOperand();
  }
  return Array;
}

static bool isReleaseSafeArrayReference(SILValue Ref,
                                        ArraySet &ReleaseSafeArrayReferences,
                                        RCIdentityFunctionInfo *RCIA) {
  auto RefRoot = RCIA->getRCIdentityRoot(Ref);
  if (ReleaseSafeArrayReferences.count(RefRoot))
    return true;
  RefRoot = getArrayStructPointer(ArrayCallKind::kCheckIndex, RefRoot);
  return ReleaseSafeArrayReferences.count(RefRoot);
}

/// Determines the kind of array bounds effect the instruction can have.
static ArrayBoundsEffect
mayChangeArraySize(SILInstruction *I, ArrayCallKind &Kind, SILValue &Array,
                   ArraySet &ReleaseSafeArrayReferences,
                   RCIdentityFunctionInfo *RCIA) {
  Array = SILValue();
  Kind = ArrayCallKind::kNone;

  // TODO: What else.
  if (isa<StrongRetainInst>(I) || isa<RetainValueInst>(I) ||
      isa<CopyValueInst>(I) || isa<CondFailInst>(I) ||
      isa<DeallocStackInst>(I) || isa<AllocationInst>(I))
    return ArrayBoundsEffect::kNone;

  // A release on an arbitrary class can have sideeffects because of the deinit
  // function.
  if (isa<StrongReleaseInst>(I) || isa<ReleaseValueInst>(I) ||
      isa<DestroyValueInst>(I))
    return isReleaseSafeArrayReference(I->getOperand(0),
                                       ReleaseSafeArrayReferences, RCIA)
               ? ArrayBoundsEffect::kNone
               : ArrayBoundsEffect::kMayChangeAny;

  // Check array bounds semantic.
  ArraySemanticsCall ArrayCall(I);
  Kind = ArrayCall.getKind();
  if (Kind != ArrayCallKind::kNone) {
    if (Kind < ArrayCallKind::kMutateUnknown) {
      // These methods are not mutating and pass the array owned. Therefore we
      // will potentially see a load of the array struct if there are mutating
      // functions in the loop on the same array.
      Array = getArrayStructPointer(Kind, ArrayCall.getSelf());
      return ArrayBoundsEffect::kNone;
    } else if (Kind >= ArrayCallKind::kArrayInit)
      return ArrayBoundsEffect::kMayChangeAny;

    Array = ArrayCall.getSelf();
    return ArrayBoundsEffect::kMayChangeArg;
  }

  if (!I->mayHaveSideEffects())
    return ArrayBoundsEffect::kNone;

  // A store to an alloc_stack can't possibly store to the array size which is
  // stored in a runtime allocated object sub field of an alloca.
  if (auto *SI = dyn_cast<StoreInst>(I)) {
    if (SI->getOwnershipQualifier() == StoreOwnershipQualifier::Assign) {
      // store [assign] can call a destructor with unintended effects
      return ArrayBoundsEffect::kMayChangeAny;
    }
    auto Ptr = SI->getDest();
    return isa<AllocStackInst>(Ptr) || isAddressOfArrayElement(SI->getDest())
               ? ArrayBoundsEffect::kNone
               : ArrayBoundsEffect::kMayChangeAny;
  }

  if (isa<LoadInst>(I))
    return ArrayBoundsEffect::kNone;

  if (isa<BeginBorrowInst>(I) || isa<EndBorrowInst>(I))
    return ArrayBoundsEffect::kNone;

  return ArrayBoundsEffect::kMayChangeAny;
}

/// Two allocations of a mutable array struct cannot reference the same
/// storage after modification. So we can treat them as not aliasing for the
/// purpose of bound checking. The change would only be tracked through one of
/// the allocations.
static bool isIdentifiedUnderlyingArrayObject(SILValue V) {
  // Allocations are safe.
  if (isa<AllocationInst>(V))
    return true;

  // Function arguments are safe.
  if (isa<SILFunctionArgument>(V))
    return true;

  return false;
}

/// Array bounds check analysis finds array bounds checks that are safe to
/// eliminate if there exists an earlier bounds check that covers the same
/// index.
///
/// We analyze a region of code for instructions that mayModify the size of an
/// array whenever we encounter an instruction that mayModify a specific array
/// or all arrays we clear the safe arrays (either a specific array or all of
/// them).
///
/// We classify instructions wrt to their effect on arrays. We are conservative,
/// any instruction that may write the size of an array (ie. an unidentified
/// store) is classified as mayModify.
///
/// Arrays are identified by their 'underlying' pointer to the array structure
/// which must either be an alloc_stack or a function argument.
///
/// Because size modifying instructions would create a copy of the storage this
/// is sufficient for the purpose of eliminating potential aliasing.
///
class ABCAnalysis {

  // List of arrays in memory which are unsafe.
  ArraySet UnsafeArrays;

  // If true, all arrays in memory are considered to be unsafe. In this case the
  // list in UnsafeArrays is not relevant.
  bool allArraysInMemoryAreUnsafe;

  ArraySet &ReleaseSafeArrayReferences;
  RCIdentityFunctionInfo *RCIA;
  bool LoopMode;

public:
  ABCAnalysis(bool loopMode, ArraySet &ReleaseSafe,
              RCIdentityFunctionInfo *rcia)
      : allArraysInMemoryAreUnsafe(false),
        ReleaseSafeArrayReferences(ReleaseSafe), RCIA(rcia),
        LoopMode(loopMode) {}

  ABCAnalysis(const ABCAnalysis &) = delete;
  ABCAnalysis &operator=(const ABCAnalysis &) = delete;

  /// Find safe array bounds check in a loop. A bounds_check is safe if no size
  /// modifying instruction to the same array has been seen so far.
  ///
  /// The code relies on isIdentifiedUnderlyingArrayObject' to make sure that a
  /// 'safe arrays' is not aliased.
  /// If an instruction is encountered that might modify any array this method
  /// stops further analysis and returns false. Otherwise, true is returned and
  /// the safe arrays can be queried.
  void analyzeBlock(SILBasicBlock *BB) {
    for (auto &Inst : *BB)
      analyzeInstruction(&Inst);
  }

  /// Returns false if the instruction may change the size of any array. All
  /// redundant safe array accesses seen up to the instruction can be removed.
  void analyze(SILInstruction *I) {
    assert(!LoopMode &&
           "This function can only be used in on cfg without loops");
    (void)LoopMode;

    analyzeInstruction(I);
  }

  /// Returns true if the Array is unsafe.
  bool isUnsafe(SILValue Array) const {
    return allArraysInMemoryAreUnsafe || UnsafeArrays.count(Array) != 0;
  }

  /// Returns true if all arrays in memory are considered to be unsafe and
  /// clears this flag.
  bool clearArraysUnsafeFlag() {
    bool arraysUnsafe = allArraysInMemoryAreUnsafe;
    allArraysInMemoryAreUnsafe = false;
    return arraysUnsafe;
  }

private:
  /// Analyze one instruction wrt. the instructions we have seen so far.
  void analyzeInstruction(SILInstruction *Inst) {
    SILValue Array;
    ArrayCallKind K;
    auto BoundsEffect =
        mayChangeArraySize(Inst, K, Array, ReleaseSafeArrayReferences, RCIA);

    if (BoundsEffect == ArrayBoundsEffect::kMayChangeAny) {
      LLVM_DEBUG(llvm::dbgs() << " not safe because kMayChangeAny " << *Inst);
      allArraysInMemoryAreUnsafe = true;
      // No need to store specific arrays in this case.
      UnsafeArrays.clear();
      return;
    }

    assert(Array ||
           K == ArrayCallKind::kNone &&
               "Need to have an array for array semantic functions");

    // We need to make sure that the array container is not aliased in ways
    // that we don't understand.
    if (Array && !isIdentifiedUnderlyingArrayObject(Array)) {
      LLVM_DEBUG(llvm::dbgs()
                 << " not safe because of not identified underlying object "
                 << *Array << " in " << *Inst);
      allArraysInMemoryAreUnsafe = true;
      // No need to store specific arrays in this case.
      UnsafeArrays.clear();
      return;
    }

    if (BoundsEffect == ArrayBoundsEffect::kMayChangeArg) {
      UnsafeArrays.insert(Array);
      return;
    }
    assert(BoundsEffect == ArrayBoundsEffect::kNone);
  }
};

// Get the pair of array and index. Because we want to disambiguate between the
// two types of check bounds checks merge in the type into the lower bit of one
// of the addresses' index.
static std::pair<ValueBase *, ArrayAccessDesc>
getArrayIndexPair(SILValue Array, SILValue ArrayIndex, ArrayCallKind K) {
  assert((K == ArrayCallKind::kCheckIndex ||
          K == ArrayCallKind::kCheckSubscript) &&
         "Must be a bounds check call");
  return std::make_pair(
      Array,
      ArrayAccessDesc(ArrayIndex, K == ArrayCallKind::kCheckIndex));
}


static CondFailInst *hasCondFailUse(SILValue V) {
    for (auto *Op : V->getUses())
      if (auto C = dyn_cast<CondFailInst>(Op->getUser()))
        return C;
    return nullptr;
}

/// Checks whether the apply instruction is checked for overflow by looking for
/// a cond_fail on the second result.
static CondFailInst *isOverflowChecked(BuiltinInst *AI) {
  for (auto *Op : AI->getUses()) {
    if (!match(Op->getUser(), m_TupleExtractOperation(m_ValueBase(), 1)))
      continue;

    TupleExtractInst *TEI = cast<TupleExtractInst>(Op->getUser());
    if (CondFailInst *C = hasCondFailUse(TEI))
      return C;
  }
  return nullptr;
}

/// Look for checks that guarantee that start is less than or equal to end.
static bool isSignedLessEqual(SILValue Start, SILValue End, SILBasicBlock &BB) {

  // If we have an inclusive range "low...up" the loop exit count will be
  // "up + 1" but the overflow check is on "up".
  SILValue PreInclusiveEnd;
  if (!match(End, m_TupleExtractOperation(
                      m_ApplyInst(BuiltinValueKind::SAddOver,
                                  m_SILValue(PreInclusiveEnd), m_One()),
                      0)))
    PreInclusiveEnd = SILValue();

  bool IsPreInclusiveEndLEQ = false;
  bool IsPreInclusiveEndGTEnd = false;
  for (auto &Inst : BB)
    if (auto CF = dyn_cast<CondFailInst>(&Inst)) {
      // Try to match a cond_fail on "XOR , (SLE Start, End), 1".
      if (match(CF->getOperand(),
                m_ApplyInst(BuiltinValueKind::Xor,
                            m_ApplyInst(BuiltinValueKind::ICMP_SLE,
                                        m_Specific(Start),
                                        m_Specific(End)),
                            m_One())))
        return true;
      // Inclusive ranges will have a check on the upper value (before adding
      // one).
      if (PreInclusiveEnd) {
        if (match(CF->getOperand(),
                  m_ApplyInst(BuiltinValueKind::Xor,
                              m_ApplyInst(BuiltinValueKind::ICMP_SLE,
                                          m_Specific(Start),
                                          m_Specific(PreInclusiveEnd)),
                              m_One())))
          IsPreInclusiveEndLEQ = true;
        if (match(CF->getOperand(),
                  m_ApplyInst(BuiltinValueKind::Xor,
                              m_ApplyInst(BuiltinValueKind::ICMP_SGT,
                                          m_Specific(End),
                                          m_Specific(PreInclusiveEnd)),
                              m_One())))
          IsPreInclusiveEndGTEnd = true;
        if (IsPreInclusiveEndLEQ && IsPreInclusiveEndGTEnd)
          return true;
      }
    }

  return false;
}

static bool isLessThan(SILValue Start, SILValue End) {
  auto S = dyn_cast<IntegerLiteralInst>(Start);
  if (!S)
    return false;
  auto E = dyn_cast<IntegerLiteralInst>(End);
  if (!E)
    return false;
  return S->getValue().slt(E->getValue());
}

static BuiltinValueKind swapCmpID(BuiltinValueKind ID) {
  switch (ID) {
    case BuiltinValueKind::ICMP_EQ: return BuiltinValueKind::ICMP_EQ;
    case BuiltinValueKind::ICMP_NE: return BuiltinValueKind::ICMP_NE;
    case BuiltinValueKind::ICMP_SLE: return BuiltinValueKind::ICMP_SGE;
    case BuiltinValueKind::ICMP_SLT: return BuiltinValueKind::ICMP_SGT;
    case BuiltinValueKind::ICMP_SGE: return BuiltinValueKind::ICMP_SLE;
    case BuiltinValueKind::ICMP_SGT: return BuiltinValueKind::ICMP_SLT;
    case BuiltinValueKind::ICMP_ULE: return BuiltinValueKind::ICMP_UGE;
    case BuiltinValueKind::ICMP_ULT: return BuiltinValueKind::ICMP_UGT;
    case BuiltinValueKind::ICMP_UGE: return BuiltinValueKind::ICMP_ULE;
    case BuiltinValueKind::ICMP_UGT: return BuiltinValueKind::ICMP_ULT;
    default:
      return ID;
  }
}

static BuiltinValueKind invertCmpID(BuiltinValueKind ID) {
  switch (ID) {
    case BuiltinValueKind::ICMP_EQ: return BuiltinValueKind::ICMP_NE;
    case BuiltinValueKind::ICMP_NE: return BuiltinValueKind::ICMP_EQ;
    case BuiltinValueKind::ICMP_SLE: return BuiltinValueKind::ICMP_SGT;
    case BuiltinValueKind::ICMP_SLT: return BuiltinValueKind::ICMP_SGE;
    case BuiltinValueKind::ICMP_SGE: return BuiltinValueKind::ICMP_SLT;
    case BuiltinValueKind::ICMP_SGT: return BuiltinValueKind::ICMP_SLE;
    case BuiltinValueKind::ICMP_ULE: return BuiltinValueKind::ICMP_UGT;
    case BuiltinValueKind::ICMP_ULT: return BuiltinValueKind::ICMP_UGE;
    case BuiltinValueKind::ICMP_UGE: return BuiltinValueKind::ICMP_ULT;
    case BuiltinValueKind::ICMP_UGT: return BuiltinValueKind::ICMP_ULE;
    default:
      return ID;
  }
}

/// Checks if Start to End is the range of 0 to the count of an array.
/// Returns the array if this is the case.
static SILValue getZeroToCountArray(SILValue Start, SILValue End) {
  auto *IL = dyn_cast<IntegerLiteralInst>(Start);
  if (!IL || IL->getValue() != 0)
    return SILValue();
    
  auto *SEI = dyn_cast<StructExtractInst>(End);
  if (!SEI)
    return SILValue();
    
  ArraySemanticsCall SemCall(SEI->getOperand());
  if (SemCall.getKind() != ArrayCallKind::kGetCount)
    return SILValue();
  return SemCall.getSelf();
}

/// Checks whether the cond_br in the preheader's predecessor ensures that the
/// loop is only executed if "Start < End".
static bool isLessThanCheck(SILValue Start, SILValue End,
                            CondBranchInst *CondBr, SILBasicBlock *Preheader) {
  auto *BI = dyn_cast<BuiltinInst>(CondBr->getCondition());
  if (!BI)
    return false;

  BuiltinValueKind Id = BI->getBuiltinInfo().ID;
  if (BI->getNumOperands() != 2)
    return false;

  SILValue LeftArg = BI->getOperand(0);
  SILValue RightArg = BI->getOperand(1);
  
  if (RightArg == Start) {
    std::swap(LeftArg, RightArg);
    Id = swapCmpID(Id);
  }
  if (LeftArg != Start || RightArg != End)
    return false;
  
  if (CondBr->getTrueBB() != Preheader) {
    assert(CondBr->getFalseBB() == Preheader);
    Id = invertCmpID(Id);
  }
  
  switch (Id) {
    case BuiltinValueKind::ICMP_SLT:
    case BuiltinValueKind::ICMP_ULT:
      return true;
    case BuiltinValueKind::ICMP_NE:
      // Special case: if it is a 0-to-count loop, we know that the count cannot
      // be negative. In this case the 'Start < End' check can also be done with
      // 'count != 0'.
      return getZeroToCountArray(Start, End);
    default:
      return false;
  }
}

/// Checks whether there are checks in the preheader's predecessor that ensure
/// that "Start < End".
static bool isRangeChecked(SILValue Start, SILValue End,
                           SILBasicBlock *Preheader, DominanceInfo *DT) {
  // Check two constants.
  if (isLessThan(Start, End))
    return true;

  // Look for a branch on EQ around the Preheader.
  auto *PreheaderPred = Preheader->getSinglePredecessorBlock();
  if (!PreheaderPred)
    return false;
  auto *CondBr = dyn_cast<CondBranchInst>(PreheaderPred->getTerminator());
  if (!CondBr)
    return false;

  if (isLessThanCheck(Start, End, CondBr, Preheader))
    return true;

  // Walk up the dominator tree looking for a range check ("SLE Start, End").
  DominanceInfoNode *CurDTNode = DT->getNode(PreheaderPred);
  while (CurDTNode) {
    if (isSignedLessEqual(Start, End, *CurDTNode->getBlock()))
      return true;
    CurDTNode = CurDTNode->getIDom();
  }
  return false;
}

static bool dominates(DominanceInfo *DT, SILValue V, SILBasicBlock *B) {
  if (auto ValueBB = V->getParentBlock())
    return DT->dominates(ValueBB, B);
  return false;
}

/// Subtract a constant from a builtin integer value.
static SILValue getSub(SILLocation Loc, SILValue Val, unsigned SubVal,
                       SILBuilder &B) {
  SmallVector<SILValue, 4> Args(1, Val);
  Args.push_back(B.createIntegerLiteral(Loc, Val->getType(), SubVal));
  Args.push_back(B.createIntegerLiteral(
      Loc, SILType::getBuiltinIntegerType(1, B.getASTContext()), -1));

  auto *AI = B.createBuiltinBinaryFunctionWithOverflow(
      Loc, "ssub_with_overflow", Args);
  return B.createTupleExtract(Loc, AI, 0);
}

static SILValue getAdd(SILLocation Loc, SILValue Val, unsigned AddVal,
                       SILBuilder &B) {
  SmallVector<SILValue, 4> Args(1, Val);
  Args.push_back(B.createIntegerLiteral(Loc, Val->getType(), AddVal));
  Args.push_back(B.createIntegerLiteral(
      Loc, SILType::getBuiltinIntegerType(1, B.getASTContext()), -1));

  auto *AI = B.createBuiltinBinaryFunctionWithOverflow(
      Loc, "sadd_with_overflow", Args);
  return B.createTupleExtract(Loc, AI, 0);
}

/// A canonical induction variable incremented by one from Start to End-1.
struct InductionInfo {
  SILArgument *HeaderVal;
  BuiltinInst *Inc;
  SILValue Start;
  SILValue End;
  BuiltinValueKind Cmp;
  bool IsOverflowCheckInserted;

  InductionInfo()
      : Cmp(BuiltinValueKind::None), IsOverflowCheckInserted(false) {}

  InductionInfo(SILArgument *HV, BuiltinInst *I, SILValue S, SILValue E,
                BuiltinValueKind C, bool IsOverflowChecked = false)
      : HeaderVal(HV), Inc(I), Start(S), End(E), Cmp(C),
        IsOverflowCheckInserted(IsOverflowChecked) {}

  bool isValid() { return Start && End; }
  operator bool() { return isValid(); }

  SILInstruction *getInstruction() { return Inc; }

  SILValue getFirstValue(SILLocation &Loc, SILBuilder &B, unsigned AddVal) {
    return AddVal != 0 ? getAdd(Loc, Start, AddVal, B) : Start;
  }

  SILValue getLastValue(SILLocation &Loc, SILBuilder &B, unsigned SubVal) {
    return SubVal != 0 ? getSub(Loc, End, SubVal, B) : End;
  }

  /// If necessary insert an overflow for this induction variable.
  /// If we compare for equality we need to make sure that the range does wrap.
  /// We would have trapped either when overflowing or when accessing an array
  /// out of bounds in the original loop.
  /// Returns true if an overflow check was inserted.
  bool checkOverflow(SILBuilder &Builder) {
    if (IsOverflowCheckInserted || Cmp != BuiltinValueKind::ICMP_EQ)
      return false;

    auto Loc = Inc->getLoc();
    auto ResultTy = SILType::getBuiltinIntegerType(1, Builder.getASTContext());
    auto *CmpSGE = Builder.createBuiltinBinaryFunction(
        Loc, "cmp_sge", Start->getType(), ResultTy, {Start, End});
    Builder.createCondFail(Loc, CmpSGE, "loop induction variable overflowed");
    IsOverflowCheckInserted = true;

    // We can now remove the cond fail on the increment the above comparison
    // guarantees that the addition won't overflow.
    auto *CondFail = isOverflowChecked(cast<BuiltinInst>(Inc));
    if (CondFail)
      CondFail->eraseFromParent();
    return true;
  }
};

/// Analyze canonical induction variables in a loop to find their start and end
/// values.
/// At the moment we only handle very simple induction variables that increment
/// by one and use equality comparison.
class InductionAnalysis {
  using InductionInfoMap = llvm::DenseMap<SILArgument *, InductionInfo *>;

  DominanceInfo *DT;
  SILBasicBlock *Preheader;
  SILBasicBlock *Header;
  SILBasicBlock *ExitingBlk;
  SILBasicBlock *ExitBlk;
  IVInfo &IVs;
  InductionInfoMap Map;
  llvm::SpecificBumpPtrAllocator<InductionInfo> Allocator;

public:
  InductionAnalysis(DominanceInfo *D, IVInfo &IVs, SILBasicBlock *Preheader,
                    SILBasicBlock *Header, SILBasicBlock *ExitingBlk,
                    SILBasicBlock *ExitBlk)
      : DT(D), Preheader(Preheader), Header(Header), ExitingBlk(ExitingBlk),
        ExitBlk(ExitBlk), IVs(IVs) {}

  InductionAnalysis(const InductionAnalysis &) = delete;
  InductionAnalysis &operator=(const InductionAnalysis &) = delete;

  bool analyze() {
    bool FoundIndVar = false;
    for (auto *Arg : Header->getArguments()) {
      // Look for induction variables.
      IVInfo::IVDesc IV;
      if (!(IV = IVs.getInductionDesc(Arg))) {
        LLVM_DEBUG(llvm::dbgs() << " not an induction variable: " << *Arg);
        continue;
      }

      InductionInfo *Info;
      if (!(Info = analyzeIndVar(Arg, IV.Inc, IV.IncVal))) {
        LLVM_DEBUG(llvm::dbgs() << " could not analyze the induction on: "
                                << *Arg);
        continue;
      }

      LLVM_DEBUG(llvm::dbgs() << " found an induction variable: " << *Arg);
      FoundIndVar = true;
      Map[Arg] = Info;
    }
    return FoundIndVar;
  }

  InductionInfo *operator[](SILArgument *A) {
    InductionInfoMap::iterator It =  Map.find(A);
    if (It == Map.end())
      return nullptr;
    return It->second;
  }

private:

  /// Analyze one potential induction variable starting at Arg.
  InductionInfo *analyzeIndVar(SILArgument *HeaderVal, BuiltinInst *Inc,
                               IntegerLiteralInst *IncVal) {
    if (IncVal->getValue() != 1)
      return nullptr;

    // Find the start value.
    auto *PreheaderTerm = dyn_cast<BranchInst>(Preheader->getTerminator());
    if (!PreheaderTerm)
      return nullptr;
    auto Start = PreheaderTerm->getArg(HeaderVal->getIndex());

    // Find the exit condition.
    auto CondBr = dyn_cast<CondBranchInst>(ExitingBlk->getTerminator());
    if (!CondBr)
      return nullptr;

    if (ExitBlk == CondBr->getFalseBB())
      return nullptr;
    assert(ExitBlk == CondBr->getTrueBB() &&
           "The loop's exiting blocks terminator must exit");

    auto Cond = CondBr->getCondition();
    SILValue End;

    // Look for a compare of induction variable + 1.
    // TODO: obviously we need to handle many more patterns.
    if (!match(Cond, m_ApplyInst(BuiltinValueKind::ICMP_EQ,
                                 m_TupleExtractOperation(m_Specific(Inc), 0),
                                 m_SILValue(End))) &&
        !match(Cond,
               m_ApplyInst(BuiltinValueKind::ICMP_EQ, m_SILValue(End),
                           m_TupleExtractOperation(m_Specific(Inc), 0)))) {
      LLVM_DEBUG(llvm::dbgs() << " found no exit condition\n");
      return nullptr;
    }

    // Make sure our end value is loop invariant.
    if (!dominates(DT, End, Preheader))
      return nullptr;

    LLVM_DEBUG(llvm::dbgs() << " found an induction variable (ICMP_EQ): "
                            << *HeaderVal << "  start: " << *Start
                            << "  end: " << *End);

    // Check whether the addition is overflow checked by a cond_fail or whether
    // code in the preheader's predecessor ensures that we won't overflow.
    bool IsRangeChecked = false;
    if (!isOverflowChecked(Inc)) {
      IsRangeChecked = isRangeChecked(Start, End, Preheader, DT);
      if (!IsRangeChecked)
        return nullptr;
    }
    return new (Allocator.Allocate()) InductionInfo(
        HeaderVal, Inc, Start, End, BuiltinValueKind::ICMP_EQ, IsRangeChecked);
  }
};

/// A block in the loop is guaranteed to be executed if it dominates the single
/// exiting block.
static bool isGuaranteedToBeExecuted(DominanceInfo *DT, SILBasicBlock *Block,
                                     SILBasicBlock *SingleExitingBlk) {
  // If there are multiple exiting blocks then no block in the loop is
  // guaranteed to be executed in _all_ iterations until the upper bound of the
  // induction variable is reached.
  if (!SingleExitingBlk)
    return false;
  return DT->dominates(Block, SingleExitingBlk);
}

/// Describes the access function "a[f(i)]" that is based on a canonical
/// induction variable.
class AccessFunction {
  InductionInfo *Ind;
  bool preIncrement;

  AccessFunction(InductionInfo *I, bool isPreIncrement = false)
      : Ind(I), preIncrement(isPreIncrement) {}

public:

  operator bool() { return Ind != nullptr; }

  static AccessFunction getLinearFunction(SILValue Idx,
                                          InductionAnalysis &IndVars) {
    // Match the actual induction variable buried in the integer struct.
    // bb(%ivar)
    // %2 = struct $Int(%ivar : $Builtin.Word)
    //    = apply %check_bounds(%array, %2) :
    // or
    // bb(%ivar1)
    // %ivar2 = builtin "sadd_with_overflow_Int64"(%ivar1,...)
    // %t = tuple_extract %ivar2
    // %s = struct $Int(%t : $Builtin.Word)
    //    = apply %check_bounds(%array, %s) :

    bool preIncrement = false;

    auto ArrayIndexStruct = dyn_cast<StructInst>(Idx);
    if (!ArrayIndexStruct)
      return nullptr;

    auto AsArg =
        dyn_cast<SILArgument>(ArrayIndexStruct->getElements()[0]);

    if (!AsArg) {
      auto *TupleExtract =
          dyn_cast<TupleExtractInst>(ArrayIndexStruct->getElements()[0]);

      if (!TupleExtract) {
        return nullptr;
      }

      auto *Builtin = dyn_cast<BuiltinInst>(TupleExtract->getOperand());
      if (!Builtin || Builtin->getBuiltinKind() != BuiltinValueKind::SAddOver) {
        return nullptr;
      }

      // We don't check if the second argument to the builtin is loop invariant
      // here, because only induction variables with a +1 incremenent are
      // considered for bounds check optimization.
      AsArg = dyn_cast<SILArgument>(Builtin->getArguments()[0]);
      if (!AsArg) {
        return nullptr;
      }
      preIncrement = true;
    }

    if (auto *Ind = IndVars[AsArg])
      return AccessFunction(Ind, preIncrement);

    return nullptr;
  }

  /// Returns true if the loop iterates from 0 until count of \p ArrayVal.
  bool isZeroToCount(SILValue ArrayVal) {
    return getZeroToCountArray(Ind->Start, Ind->End) == ArrayVal;
  }

  /// Hoists the necessary check for beginning and end of the induction
  /// encapsulated by this access function to the header.
  void hoistCheckToPreheader(ArraySemanticsCall CheckToHoist,
                             SILBasicBlock *Preheader,
                             DominanceInfo *DT) {
    ApplyInst *AI = CheckToHoist;
    SILLocation Loc = AI->getLoc();
    SILBuilderWithScope Builder(Preheader->getTerminator(), AI);

    // Get the first induction value.
    auto FirstVal = Ind->getFirstValue(Loc, Builder, preIncrement ? 1 : 0);
    // Clone the struct for the start index.
    auto Start = cast<SingleValueInstruction>(CheckToHoist.getIndex())
                     ->clone(Preheader->getTerminator());
    // Set the new start index to the first value of the induction.
    Start->setOperand(0, FirstVal);

      // Clone and fixup the load, retain sequence to the header.
    auto NewCheck = CheckToHoist.copyTo(Preheader->getTerminator(), DT);
    NewCheck->setOperand(1, Start);

    // Get the last induction value.
    auto LastVal = Ind->getLastValue(Loc, Builder, preIncrement ? 0 : 1);
    // Clone the struct for the end index.
    auto End = cast<SingleValueInstruction>(CheckToHoist.getIndex())
                   ->clone(Preheader->getTerminator());
    // Set the new end index to the last value of the induction.
    End->setOperand(0, LastVal);

    NewCheck = CheckToHoist.copyTo(Preheader->getTerminator(), DT);
    NewCheck->setOperand(1, End);
  }
};

/// A dominating cond_fail on the same value ensures that this value is false.
static bool isValueKnownFalseAt(SILValue Val, SILInstruction *At,
                                DominanceInfo *DT) {
  auto *Inst = Val->getDefiningInstruction();
  if (!Inst ||
      std::next(SILBasicBlock::iterator(Inst)) == Inst->getParent()->end())
    return false;
  auto *CF = dyn_cast<CondFailInst>(std::next(SILBasicBlock::iterator(Inst)));
  return CF && DT->properlyDominates(CF, At);
}

/// Based on the induction variable information this comparison is known to be
/// true.
static bool isComparisonKnownTrue(BuiltinInst *Builtin, InductionInfo &IndVar) {
  if (!IndVar.IsOverflowCheckInserted ||
      IndVar.Cmp != BuiltinValueKind::ICMP_EQ)
    return false;
  return match(Builtin,
               m_ApplyInst(BuiltinValueKind::ICMP_SLE, m_Specific(IndVar.Start),
                           m_Specific(IndVar.HeaderVal))) ||
         match(Builtin, m_ApplyInst(BuiltinValueKind::ICMP_SLT,
                                    m_Specific(IndVar.HeaderVal),
                                    m_Specific(IndVar.End)));
}

/// Based on the induction variable information this comparison is known to be
/// false.
static bool isComparisonKnownFalse(BuiltinInst *Builtin,
                                   InductionInfo &IndVar) {
  if (!IndVar.IsOverflowCheckInserted ||
      IndVar.Cmp != BuiltinValueKind::ICMP_EQ)
    return false;

  // Pattern match a false condition patterns that we can detect and optimize:
  // Iteration count < 0 (start)
  // Iteration count + 1 <= 0 (start)
  // Iteration count + 1 < 0 (start)
  // Iteration count + 1 == 0 (start)
  auto MatchIndVarHeader = m_Specific(IndVar.HeaderVal);
  auto MatchIncrementIndVar = m_TupleExtractOperation(
      m_ApplyInst(BuiltinValueKind::SAddOver, MatchIndVarHeader, m_One()), 0);
  auto MatchIndVarStart = m_Specific(IndVar.Start);

  if (match(Builtin,
            m_ApplyInst(BuiltinValueKind::ICMP_SLT,
                        m_CombineOr(MatchIndVarHeader, MatchIncrementIndVar),
                        MatchIndVarStart)) ||
      match(Builtin, m_ApplyInst(BuiltinValueKind::ICMP_EQ,
                                 MatchIncrementIndVar, MatchIndVarStart)) ||
      match(Builtin, m_ApplyInst(BuiltinValueKind::ICMP_SLE,
                                 MatchIncrementIndVar, MatchIndVarStart))) {
    return true;
  }

  return false;
}

#ifndef NDEBUG
static void reportBoundsChecks(SILFunction *F) {
  unsigned NumBCs = 0;

  F->dump();
  for (auto &BB : *F) {
    for (auto &Inst : BB) {
      ArraySemanticsCall ArrayCall(&Inst);
      auto Kind = ArrayCall.getKind();
      if (Kind != ArrayCallKind::kCheckSubscript)
        continue;
      auto Array = ArrayCall.getSelf();
      ++NumBCs;
      llvm::dbgs() << " # CheckBounds: " << Inst
                   << "     with array arg: " << *Array
                   << "     and index: " << Inst.getOperand(1);
    }
  }
  llvm::dbgs() << " ### " << NumBCs << " bounds checks in " << F->getName()
               << "\n";
}
#endif

namespace {

// Should be more than enough to cover "usual" functions.
static constexpr int maxRecursionDepth = 500;

/// Remove redundant checks in basic blocks and hoist redundant checks out of
/// loops.
class ABCOpt : public SILFunctionTransform {
private:
  SILLoopInfo *LI;
  DominanceInfo *DT;
  IVInfo *IVs;
  RCIdentityFunctionInfo *RCIA;
  DestructorAnalysis *DestAnalysis;
  // Arrays with element type that does not call a deinit function.
  ArraySet ReleaseSafeArrays;

  /// Remove redundant checks in a basic block. This function will reset the
  /// state after an instruction that may modify any array allowing removal of
  /// redundant checks up to that point and after that point.
  bool removeRedundantChecksInBlock(SILBasicBlock &BB);
  /// Hoist or remove redundant bound checks in \p Loop
  bool processLoop(SILLoop *Loop);
  /// Walk down the dominator tree inside the loop, removing redundant checks.
  bool removeRedundantChecksInLoop(DominanceInfoNode *CurBB, ABCAnalysis &ABC,
                                   IndexedArraySet &DominatingSafeChecks,
                                   SILLoop *Loop,
                                   int recursionDepth);
  /// Analyze the loop for arrays that are not modified and perform dominator
  /// tree based redundant bounds check removal.
  bool hoistChecksInLoop(DominanceInfoNode *DTNode, ABCAnalysis &ABC,
                         InductionAnalysis &IndVars, SILBasicBlock *Preheader,
                         SILBasicBlock *Header,
                         SILBasicBlock *SingleExitingBlk,
                         int recursionDepth);

public:
  void run() override {
    if (!EnableABCOpts)
      return;

    SILFunction *F = getFunction();
    LI = PM->getAnalysis<SILLoopAnalysis>()->get(F);
    DT = PM->getAnalysis<DominanceAnalysis>()->get(F);
    IVs = PM->getAnalysis<IVAnalysis>()->get(F);
    RCIA = PM->getAnalysis<RCIdentityAnalysis>()->get(F);
    DestAnalysis = PM->getAnalysis<DestructorAnalysis>();

#ifndef NDEBUG
    if (ShouldReportBoundsChecks) {
      reportBoundsChecks(F);
    }
#endif
    LLVM_DEBUG(llvm::dbgs()
               << "ArrayBoundsCheckOpts on function: " << F->getName() << "\n");
    // Collect all arrays in this function. A release is only 'safe' if we know
    // its deinitializer does not have sideeffects that could cause memory
    // safety issues. A deinit could deallocate array or put a different array
    // in its location.
    for (auto &BB : *F) {
      for (auto &Inst : BB) {
        ArraySemanticsCall Call(&Inst);
        if (Call && Call.hasSelf()) {
          LLVM_DEBUG(llvm::dbgs() << "Gathering " << *(ApplyInst *)Call);
          auto rcRoot = RCIA->getRCIdentityRoot(Call.getSelf());
          // Check the type of the array. We need to have an array element type
          // that is not calling a deinit function.
          if (DestAnalysis->mayStoreToMemoryOnDestruction(rcRoot->getType()))
            continue;
          LLVM_DEBUG(llvm::dbgs() << "ReleaseSafeArray: " << rcRoot << "\n");
          ReleaseSafeArrays.insert(rcRoot);
          ReleaseSafeArrays.insert(
              getArrayStructPointer(ArrayCallKind::kCheckIndex, rcRoot));
        }
      }
    }
    // Remove redundant checks on a per basic block basis.
    bool Changed = false;
    for (auto &BB : *F)
      Changed |= removeRedundantChecksInBlock(BB);

#ifndef NDEBUG
    if (ShouldReportBoundsChecks) {
      reportBoundsChecks(F);
    }
#endif

    if (LI->empty()) {
      LLVM_DEBUG(llvm::dbgs() << "No loops in " << F->getName() << "\n");
      if (Changed) {
        PM->invalidateAnalysis(
            F, SILAnalysis::InvalidationKind::CallsAndInstructions);
      }
      return;
    }
    // Remove redundant checks along the dominator tree in a loop and hoist
    // checks.
    for (auto *LoopIt : *LI) {
      // Process loops recursively bottom-up in the loop tree.
      SmallVector<SILLoop *, 8> Worklist;
      Worklist.push_back(LoopIt);
      for (unsigned i = 0; i < Worklist.size(); ++i) {
        auto *L = Worklist[i];
        for (auto *SubLoop : *L)
          Worklist.push_back(SubLoop);
      }

      while (!Worklist.empty()) {
        Changed |= processLoop(Worklist.pop_back_val());
      }
    }

#ifndef NDEBUG
    if (ShouldReportBoundsChecks) {
      reportBoundsChecks(F);
    }
#endif

    if (Changed) {
      PM->invalidateAnalysis(
          F, SILAnalysis::InvalidationKind::CallsAndInstructions);
    }
  }
};

bool ABCOpt::removeRedundantChecksInBlock(SILBasicBlock &BB) {
  ABCAnalysis ABC(false, ReleaseSafeArrays, RCIA);
  IndexedArraySet RedundantChecks;
  bool Changed = false;

  LLVM_DEBUG(llvm::dbgs() << "Removing in BB" << BB.getDebugID() << "\n");

  // Process all instructions in the current block.
  for (auto Iter = BB.begin(); Iter != BB.end();) {
    auto Inst = &*Iter;
    ++Iter;

    ABC.analyze(Inst);

    if (ABC.clearArraysUnsafeFlag()) {
      // Any array may be modified -> forget everything. This is just a
      // shortcut to the isUnsafe test for a specific array below.
      RedundantChecks.clear();
      continue;
    }

    // Is this a check_bounds.
    ArraySemanticsCall ArrayCall(Inst);
    auto Kind = ArrayCall.getKind();
    if (Kind != ArrayCallKind::kCheckSubscript &&
        Kind != ArrayCallKind::kCheckIndex) {
      LLVM_DEBUG(llvm::dbgs() << " not a check_bounds call " << *Inst);
      continue;
    }
    auto Array = ArrayCall.getSelf();

    // Get the underlying array pointer.
    Array = getArrayStructPointer(Kind, Array);

    // Is this an unsafe array whose size could have been changed?
    if (ABC.isUnsafe(Array)) {
      LLVM_DEBUG(llvm::dbgs() << " not a safe array argument " << *Array);
      continue;
    }

    // Get the array index.
    auto ArrayIndex = ArrayCall.getIndex();
    if (!ArrayIndex)
      continue;

    auto IndexedArray = getArrayIndexPair(Array, ArrayIndex, Kind);
    LLVM_DEBUG(llvm::dbgs()
               << " IndexedArray: " << *Array << " and " << *ArrayIndex);

    // Saw a check for the first time.
    if (!RedundantChecks.count(IndexedArray)) {
      LLVM_DEBUG(llvm::dbgs() << " first time: " << *Inst
                              << "  with array argument: " << *Array);
      RedundantChecks.insert(IndexedArray);
      continue;
    }

    // Remove the bounds check.
    ArrayCall.removeCall();
    Changed = true;
  }
  return Changed;
}

bool ABCOpt::removeRedundantChecksInLoop(DominanceInfoNode *CurBB,
                                         ABCAnalysis &ABC,
                                         IndexedArraySet &DominatingSafeChecks,
                                         SILLoop *Loop,
                                         int recursionDepth) {
  auto *BB = CurBB->getBlock();
  if (!Loop->contains(BB))
    return false;

  // Avoid a stack overflow for very deep dominator trees.
  if (recursionDepth >= maxRecursionDepth)
    return false;

  bool Changed = false;

  // When we come back from the dominator tree recursion we need to remove
  // checks that we have seen for the first time.
  SmallVector<std::pair<ValueBase *, ArrayAccessDesc>, 8> SafeChecksToPop;

  // Process all instructions in the current block.
  for (auto Iter = BB->begin(); Iter != BB->end();) {
    auto Inst = &*Iter;
    ++Iter;

    // Is this a check_bounds.
    ArraySemanticsCall ArrayCall(Inst);
    auto Kind = ArrayCall.getKind();
    if (Kind != ArrayCallKind::kCheckSubscript &&
        Kind != ArrayCallKind::kCheckIndex) {
      LLVM_DEBUG(llvm::dbgs() << " not a check_bounds call " << *Inst);
      continue;
    }
    auto Array = ArrayCall.getSelf();

    // Get the underlying array pointer.
    Array = getArrayStructPointer(Kind, Array);

    // Is this an unsafe array whose size could have been changed?
    if (ABC.isUnsafe(Array)) {
      LLVM_DEBUG(llvm::dbgs() << " not a safe array argument " << *Array);
      continue;
    }

    // Get the array index.
    auto ArrayIndex = ArrayCall.getIndex();
    if (!ArrayIndex)
      continue;
    auto IndexedArray = getArrayIndexPair(Array, ArrayIndex, Kind);

    // Saw a check for the first time.
    if (!DominatingSafeChecks.count(IndexedArray)) {
      LLVM_DEBUG(llvm::dbgs()
                 << " first time: " << *Inst << "  with array arg: " << *Array);
      DominatingSafeChecks.insert(IndexedArray);
      SafeChecksToPop.push_back(IndexedArray);
      continue;
    }

    // Remove the bounds check.
    ArrayCall.removeCall();
    Changed = true;
  }

  // Traverse the children in the dominator tree inside the loop.
  for (auto Child : *CurBB)
    Changed |=
        removeRedundantChecksInLoop(Child, ABC, DominatingSafeChecks, Loop,
        recursionDepth + 1);

  // Remove checks we have seen for the first time.
  std::for_each(SafeChecksToPop.begin(), SafeChecksToPop.end(),
                [&](std::pair<ValueBase *, ArrayAccessDesc> &V) {
                  DominatingSafeChecks.erase(V);
                });

  return Changed;
}

bool ABCOpt::processLoop(SILLoop *Loop) {
  auto *Header = Loop->getHeader();
  if (!Header)
    return false;

  auto *Preheader = Loop->getLoopPreheader();
  if (!Preheader) {
    // TODO: create one if necessary.
    return false;
  }

  // Only handle innermost loops for now.
  if (!Loop->getSubLoops().empty())
    return false;

  LLVM_DEBUG(llvm::dbgs() << "Attempting to remove redundant checks in "
                          << *Loop);
  LLVM_DEBUG(Header->getParent()->dump());

  // Collect safe arrays. Arrays are safe if there is no function call that
  // could mutate their size in the loop.
  ABCAnalysis ABC(true, ReleaseSafeArrays, RCIA);
  for (auto *BB : Loop->getBlocks()) {
    ABC.analyzeBlock(BB);
  }

  // Remove redundant checks down the dominator tree inside the loop,
  // starting at the header.
  // We may not go to dominated blocks outside the loop, because we didn't
  // check for safety outside the loop (with ABCAnalysis).
  IndexedArraySet DominatingSafeChecks;
  bool Changed = removeRedundantChecksInLoop(DT->getNode(Header), ABC,
                                             DominatingSafeChecks, Loop,
                                             /*recursionDepth*/ 0);

  if (!EnableABCHoisting)
    return Changed;

  LLVM_DEBUG(llvm::dbgs() << "Attempting to hoist checks in " << *Loop);

  // Find an exiting block.
  SILBasicBlock *SingleExitingBlk = Loop->getExitingBlock();
  SILBasicBlock *ExitingBlk = SingleExitingBlk;
  SILBasicBlock *ExitBlk = Loop->getExitBlock();
  SILBasicBlock *Latch = Loop->getLoopLatch();
  if (!ExitingBlk || !Latch || !ExitBlk) {
    LLVM_DEBUG(llvm::dbgs() << "No single exiting block or latch found\n");
    if (!Latch)
      return Changed;

    // Look back a split edge.
    if (!Loop->isLoopExiting(Latch) && Latch->getSinglePredecessorBlock() &&
        Loop->isLoopExiting(Latch->getSinglePredecessorBlock()))
      Latch = Latch->getSinglePredecessorBlock();
    if (Loop->isLoopExiting(Latch) && Latch->getSuccessors().size() == 2) {
      ExitingBlk = Latch;
      ExitBlk = Loop->contains(Latch->getSuccessors()[0])
                    ? Latch->getSuccessors()[1]
                    : Latch->getSuccessors()[0];
      LLVM_DEBUG(llvm::dbgs() << "Found a latch ...\n");
    } else return Changed;
  }

  LLVM_DEBUG(Preheader->getParent()->dump());

  // Find canonical induction variables.
  InductionAnalysis IndVars(DT, *IVs, Preheader, Header, ExitingBlk, ExitBlk);
  bool IVarsFound = IndVars.analyze();
  if (!IVarsFound) {
    LLVM_DEBUG(llvm::dbgs() << "No induction variables found\n");
  }

  // Hoist the overflow check of induction variables out of the loop. This also
  // needs to happen for memory safety. Also remove superfluous range checks.
  if (IVarsFound) {
    SILValue TrueVal;
    SILValue FalseVal;
    for (auto *Arg : Header->getArguments()) {
      if (auto *IV = IndVars[Arg]) {
        SILBuilderWithScope B(Preheader->getTerminator(), IV->getInstruction());

        // Only if the loop has a single exiting block (which contains the
        // induction variable check) we may hoist the overflow check.
        if (SingleExitingBlk)
          Changed |= IV->checkOverflow(B);

        if (!IV->IsOverflowCheckInserted)
          continue;
        for (auto *BB : Loop->getBlocks())
          for (auto &Inst : *BB) {
            auto *Builtin = dyn_cast<BuiltinInst>(&Inst);
            if (!Builtin)
              continue;
            if (isComparisonKnownTrue(Builtin, *IV)) {
              if (!TrueVal)
                TrueVal = SILValue(B.createIntegerLiteral(
                    Builtin->getLoc(), Builtin->getType(), -1));
              Builtin->replaceAllUsesWith(TrueVal);
              Changed = true;
              continue;
            }
            if (isComparisonKnownFalse(Builtin, *IV)) {
              if (!FalseVal) {
                FalseVal = SILValue(B.createIntegerLiteral(
                    Builtin->getLoc(), Builtin->getType(), 0));
              }
              Builtin->replaceAllUsesWith(FalseVal);
              Changed = true;
              continue;
            }
            // Check whether a dominating check of the condition let's us
            // replace
            // the condition by false.
            SILValue Left, Right;
            if (match(Builtin, m_Or(m_SILValue(Left), m_SILValue(Right)))) {
              if (isValueKnownFalseAt(Left, Builtin, DT)) {
                if (!FalseVal)
                  FalseVal = SILValue(B.createIntegerLiteral(
                      Builtin->getLoc(), Builtin->getType(), 0));
                Builtin->setOperand(0, FalseVal);
                Changed = true;
              }
              if (isValueKnownFalseAt(Right, Builtin, DT)) {
                if (!FalseVal)
                  FalseVal = SILValue(B.createIntegerLiteral(
                      Builtin->getLoc(), Builtin->getType(), 0));
                Builtin->setOperand(1, FalseVal);
                Changed = true;
              }
            }
          }
      }
    }
  }

  LLVM_DEBUG(Preheader->getParent()->dump());

  // Hoist bounds checks.
  Changed |= hoistChecksInLoop(DT->getNode(Header), ABC, IndVars, Preheader,
                               Header, SingleExitingBlk, /*recursionDepth*/ 0);
  if (Changed) {
    Preheader->getParent()->verify(getPassManager());
  }
  return Changed;
}

bool ABCOpt::hoistChecksInLoop(DominanceInfoNode *DTNode, ABCAnalysis &ABC,
                               InductionAnalysis &IndVars,
                               SILBasicBlock *Preheader, SILBasicBlock *Header,
                               SILBasicBlock *SingleExitingBlk,
                               int recursionDepth) {
  // Avoid a stack overflow for very deep dominator trees.
  if (recursionDepth >= maxRecursionDepth)
    return false;

  bool Changed = false;
  auto *CurBB = DTNode->getBlock();
  bool blockAlwaysExecutes =
      isGuaranteedToBeExecuted(DT, CurBB, SingleExitingBlk);

  for (auto Iter = CurBB->begin(); Iter != CurBB->end();) {
    auto Inst = &*Iter;
    ++Iter;

    ArraySemanticsCall ArrayCall(Inst);
    auto Kind = ArrayCall.getKind();
    if (Kind != ArrayCallKind::kCheckSubscript &&
        Kind != ArrayCallKind::kCheckIndex) {
      LLVM_DEBUG(llvm::dbgs() << " not a check_bounds call " << *Inst);
      continue;
    }
    auto ArrayVal = ArrayCall.getSelf();

    // Get the underlying array pointer.
    SILValue Array = getArrayStructPointer(Kind, ArrayVal);

    // The array must strictly dominate the header.
    if (!dominates(DT, Array, Preheader)) {
      LLVM_DEBUG(llvm::dbgs() << " does not dominated header" << *Array);
      continue;
    }

    // Is this a safe array whose size could not have changed?
    // This is either a SILValue which is defined outside the loop or it is an
    // array, which loaded from memory and the memory is not changed in the
    // loop.
    if (!dominates(DT, ArrayVal, Preheader) && ABC.isUnsafe(Array)) {
      LLVM_DEBUG(llvm::dbgs() << " not a safe array argument " << *Array);
      continue;
    }

    // Get the array index.
    auto ArrayIndex = ArrayCall.getIndex();
    if (!ArrayIndex)
      continue;

    // Make sure we know how-to hoist the array call.
    if (!ArrayCall.canHoist(Preheader->getTerminator(), DT))
      continue;

    // Invariant check.
    if (blockAlwaysExecutes && dominates(DT, ArrayIndex, Preheader)) {
      assert(ArrayCall.canHoist(Preheader->getTerminator(), DT) &&
             "Must be able to hoist the instruction.");
      Changed = true;
      ArrayCall.hoist(Preheader->getTerminator(), DT);
      LLVM_DEBUG(llvm::dbgs()
                 << " could hoist invariant bounds check: " << *Inst);
      continue;
    }

    // Get the access function "a[f(i)]". At the moment this handles only the
    // identity function.
    auto F = AccessFunction::getLinearFunction(ArrayIndex, IndVars);
    if (!F) {
      LLVM_DEBUG(llvm::dbgs() << " not a linear function " << *Inst);
      continue;
    }

    // Check if the loop iterates from 0 to the count of this array.
    if (F.isZeroToCount(ArrayVal) &&
        // This works only for Arrays but not e.g. for ArraySlice.
        ArrayVal->getType().getASTType()->isArray()) {
      // We can remove the check. This is even possible if the block does not
      // dominate the loop exit block.
      Changed = true;
      ArrayCall.removeCall();
      LLVM_DEBUG(llvm::dbgs() << "  Bounds check removed\n");
      continue;
    }

    // For hoisting bounds checks the block must dominate the exit block.
    if (!blockAlwaysExecutes)
      continue;

    // Hoist the access function and the check to the preheader for start and
    // end of the induction.
    assert(ArrayCall.canHoist(Preheader->getTerminator(), DT) &&
           "Must be able to hoist the call");

    F.hoistCheckToPreheader(ArrayCall, Preheader, DT);

    // Remove the old check in the loop and the match the retain with a release.
    ArrayCall.removeCall();

    LLVM_DEBUG(llvm::dbgs() << "  Bounds check hoisted\n");
    Changed = true;
  }

  LLVM_DEBUG(Preheader->getParent()->dump());
  // Traverse the children in the dominator tree.
  for (auto Child : *DTNode)
    Changed |= hoistChecksInLoop(Child, ABC, IndVars, Preheader, Header,
                                 SingleExitingBlk, recursionDepth + 1);

  return Changed;
}

} // end anonymous namespace

SILTransform *swift::createABCOpt() {
  return new ABCOpt();
}
