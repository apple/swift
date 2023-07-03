//===--- ARCAnalysis.cpp - SIL ARC Analysis -------------------------------===//
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

#define DEBUG_TYPE "sil-arc-analysis"

#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "swift/SILOptimizer/Analysis/ValueTracking.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Debug.h"

using namespace swift;

using BasicBlockRetainValue = std::pair<SILBasicBlock *, SILValue>;

//===----------------------------------------------------------------------===//
//                             Utility Analysis
//===----------------------------------------------------------------------===//

bool swift::isRetainInstruction(SILInstruction *I) {
  switch (I->getKind()) {
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case SILInstructionKind::Name##RetainInst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::StrongRetainInst:
  case SILInstructionKind::RetainValueInst:
    return true;
  default:
    return false;
  }
}


bool swift::isReleaseInstruction(SILInstruction *I) {
  switch (I->getKind()) {
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case SILInstructionKind::Name##ReleaseInst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::StrongReleaseInst:
  case SILInstructionKind::ReleaseValueInst:
    return true;
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
//                             Decrement Analysis
//===----------------------------------------------------------------------===//

bool swift::mayDecrementRefCount(SILInstruction *User,
                                 SILValue Ptr, AliasAnalysis *AA) {
  // First do a basic check, mainly based on the type of instruction.
  // Reading the RC is as "bad" as releasing.
  if (!User->mayReleaseOrReadRefCount())
    return false;

  // Ok, this instruction may have ref counts. If it is an apply, attempt to
  // prove that the callee is unable to affect Ptr.
  if (auto *AI = dyn_cast<ApplyInst>(User))
    return AA->canApplyDecrementRefCount(AI, Ptr);
  if (auto *TAI = dyn_cast<TryApplyInst>(User))
    return AA->canApplyDecrementRefCount(TAI, Ptr);
  if (auto *BI = dyn_cast<BuiltinInst>(User))
    return AA->canBuiltinDecrementRefCount(BI, Ptr);

  // We cannot conservatively prove that this instruction cannot decrement the
  // ref count of Ptr. So assume that it does.
  return true;
}

//===----------------------------------------------------------------------===//
//                                Use Analysis
//===----------------------------------------------------------------------===//

/// Returns true if a builtin apply can use reference counted values.
///
/// The main case that this handles here are builtins that via read none imply
/// that they cannot read globals and at the same time do not take any
/// non-trivial types via the arguments. The reason why we care about taking
/// non-trivial types as arguments is that we want to be careful in the face of
/// intrinsics that may be equivalent to bitcast and inttoptr operations.
static bool canApplyOfBuiltinUseNonTrivialValues(BuiltinInst *BInst) {
  auto *F = BInst->getFunction();

  auto &II = BInst->getIntrinsicInfo();
  if (II.ID != llvm::Intrinsic::not_intrinsic) {
    auto attrs = II.getOrCreateAttributes(F->getASTContext());
    if (attrs.hasFnAttr(llvm::Attribute::ReadNone)) {
      for (auto &Op : BInst->getAllOperands()) {
        if (!Op.get()->getType().isTrivial(*F)) {
          return true;
        }
      }
      return false;
    }
    return true;
  }

  auto &BI = BInst->getBuiltinInfo();
  if (!BI.isReadNone())
    return true;

  for (auto &Op : BInst->getAllOperands()) {
    if (!Op.get()->getType().isTrivial(*F)) {
      return true;
    }
  }
  return false;
}

/// Returns true if \p Inst may access any indirect object either via an address
/// or reference.
///
/// If these instructions do have an address or reference type operand, then
/// they only operate on the value of the address itself, not the
/// memory. i.e. they don't dereference the address.
bool swift::canUseObject(SILInstruction *Inst) {
  switch (Inst->getKind()) {
  // These instructions do not use other values.
  case SILInstructionKind::FunctionRefInst:
  case SILInstructionKind::DynamicFunctionRefInst:
  case SILInstructionKind::PreviousDynamicFunctionRefInst:
  case SILInstructionKind::IntegerLiteralInst:
  case SILInstructionKind::FloatLiteralInst:
  case SILInstructionKind::StringLiteralInst:
  case SILInstructionKind::AllocStackInst:
  case SILInstructionKind::AllocRefInst:
  case SILInstructionKind::AllocRefDynamicInst:
  case SILInstructionKind::AllocBoxInst:
  case SILInstructionKind::MetatypeInst:
  case SILInstructionKind::WitnessMethodInst:
    return false;

  // DeallocStackInst do not use reference counted values.
  case SILInstructionKind::DeallocStackInst:
    return false;

  // Debug values do not use referenced counted values in a manner we care
  // about.
  case SILInstructionKind::DebugValueInst:
    return false;

  // Casts do not use pointers in a manner that we care about since we strip
  // them during our analysis. The reason for this is if the cast is not dead
  // then there must be some other use after the cast that we will protect if a
  // release is not in between the cast and the use.
  //
  // Note: UncheckedRefCastAddrInst moves a reference into a new object. While
  // the net reference count should be zero, there's no guarantee it won't
  // access the object.
  case SILInstructionKind::UpcastInst:
  case SILInstructionKind::AddressToPointerInst:
  case SILInstructionKind::PointerToAddressInst:
  case SILInstructionKind::UncheckedRefCastInst:
  case SILInstructionKind::UncheckedAddrCastInst:
  case SILInstructionKind::RefToRawPointerInst:
  case SILInstructionKind::RawPointerToRefInst:
  case SILInstructionKind::UnconditionalCheckedCastInst:
  case SILInstructionKind::UncheckedBitwiseCastInst:
    return false;

  // If we have a trivial bit cast between trivial types, it is not something
  // that can use ref count ops in a way we care about. We do need to be careful
  // with uses with ref count inputs. In such a case, we assume conservatively
  // that the bit cast could use it.
  //
  // The reason why this is different from the ref bitcast is b/c the use of a
  // ref bit cast is still a ref typed value implying that our ARC dataflow will
  // properly handle its users. A conversion of a reference count value to a
  // trivial value though could be used as a trivial value in ways that ARC
  // dataflow will not understand implying we need to treat it as a use to be
  // safe.
  case SILInstructionKind::UncheckedTrivialBitCastInst: {
    SILValue Op = cast<UncheckedTrivialBitCastInst>(Inst)->getOperand();
    return !Op->getType().isTrivial(*Inst->getFunction());
  }

  // Typed GEPs do not use pointers. The user of the typed GEP may but we will
  // catch that via the dataflow.
  case SILInstructionKind::StructExtractInst:
  case SILInstructionKind::TupleExtractInst:
  case SILInstructionKind::StructElementAddrInst:
  case SILInstructionKind::TupleElementAddrInst:
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst:
  case SILInstructionKind::RefElementAddrInst:
  case SILInstructionKind::RefTailAddrInst:
  case SILInstructionKind::UncheckedEnumDataInst:
  case SILInstructionKind::IndexAddrInst:
  case SILInstructionKind::IndexRawPointerInst:
    return false;

  // Aggregate formation by themselves do not create new uses since it is their
  // users that would create the appropriate uses.
  case SILInstructionKind::EnumInst:
  case SILInstructionKind::StructInst:
  case SILInstructionKind::TupleInst:
    return false;

  // Only uses non reference counted values.
  case SILInstructionKind::CondFailInst:
    return false;

  case SILInstructionKind::BuiltinInst: {
    auto *BI = cast<BuiltinInst>(Inst);

    // Certain builtin function refs we know can never use non-trivial values.
    return canApplyOfBuiltinUseNonTrivialValues(BI);
  }
  // We do not care about branch inst, since if the branch inst's argument is
  // dead, LLVM will clean it up.
  case SILInstructionKind::BranchInst:
  case SILInstructionKind::CondBranchInst:
    return false;
  default:
    return true;
  }
}

static bool doOperandsAlias(ArrayRef<Operand> Ops, SILValue Ptr,
                            AliasAnalysis *AA) {
  // If any are not no alias, we have a use.
  return std::any_of(Ops.begin(), Ops.end(),
                     [&AA, &Ptr](const Operand &Op) -> bool {
                       return !AA->isNoAlias(Ptr, Op.get());
                     });
}

static bool canTerminatorUseValue(TermInst *TI, SILValue Ptr,
                                  AliasAnalysis *AA) {
  if (auto *BI = dyn_cast<BranchInst>(TI)) {
    return doOperandsAlias(BI->getAllOperands(), Ptr, AA);
  }

  if (auto *CBI = dyn_cast<CondBranchInst>(TI)) {
    bool First = doOperandsAlias(CBI->getTrueOperands(), Ptr, AA);
    bool Second = doOperandsAlias(CBI->getFalseOperands(), Ptr, AA);
    return First || Second;
  }

  if (auto *SWEI = dyn_cast<SwitchEnumInst>(TI)) {
    return doOperandsAlias(SWEI->getAllOperands(), Ptr, AA);
  }

  if (auto *SWVI = dyn_cast<SwitchValueInst>(TI)) {
    return doOperandsAlias(SWVI->getAllOperands(), Ptr, AA);
  }

  auto *CCBI = dyn_cast<CheckedCastBranchInst>(TI);
  // If we don't have this last case, be conservative and assume that we can use
  // the value.
  if (!CCBI)
    return true;

  // Otherwise, look at the operands.
  return doOperandsAlias(CCBI->getAllOperands(), Ptr, AA);
}


bool swift::mayHaveSymmetricInterference(SILInstruction *User, SILValue Ptr, AliasAnalysis *AA) {
  // If Inst is an instruction that we know can never use values with reference
  // semantics, return true. Check this before AliasAnalysis because some memory
  // operations, like dealloc_stack, don't use ref counted values.
  if (!canUseObject(User))
    return false;

  if (auto *LI = dyn_cast<LoadInst>(User)) {
    return AA->isAddrVisibleFromObject(LI->getOperand(), Ptr);
  }
  if (auto *SI = dyn_cast<StoreInst>(User)) {
    return AA->isAddrVisibleFromObject(SI->getDest(), Ptr);
  }
  if (User->mayReadOrWriteMemory())
    return true;

  // If we have a terminator instruction, see if it can use ptr. This currently
  // means that we first show that TI cannot indirectly use Ptr and then use
  // alias analysis on the arguments.
  if (auto *TI = dyn_cast<TermInst>(User))
    return canTerminatorUseValue(TI, Ptr, AA);

  // TODO: If we add in alias analysis support here for apply inst, we will need
  // to check that the pointer does not escape.

  // Otherwise, assume that Inst can use Target.
  return true;
}

//===----------------------------------------------------------------------===//
//                             Must Use Analysis
//===----------------------------------------------------------------------===//

/// Returns true if User must use Ptr.
///
/// In terms of ARC this means that if we do not remove User, all releases post
/// dominated by User are known safe.
bool swift::mustUseValue(SILInstruction *User, SILValue Ptr,
                         AliasAnalysis *AA) {
  // Right now just pattern match applies.
  auto *AI = dyn_cast<ApplyInst>(User);
  if (!AI)
    return false;

  // If any of AI's arguments must alias Ptr, return true.
  for (SILValue Arg : AI->getArguments())
    if (AA->isMustAlias(Arg, Ptr))
      return true;
  return false;
}

/// Returns true if User must use Ptr in a guaranteed way.
///
/// This means that assuming that everything is conservative, we can ignore the
/// ref count effects of User on Ptr since we will only remove things over
/// guaranteed parameters if we are known safe in both directions.
bool swift::mustGuaranteedUseValue(SILInstruction *User, SILValue Ptr,
                                   AliasAnalysis *AA) {
  // Right now just pattern match applies.
  auto *AI = dyn_cast<ApplyInst>(User);
  if (!AI)
    return false;

  // For now just look for guaranteed self.
  //
  // TODO: Expand this to handle *any* guaranteed parameter.
  if (!AI->hasGuaranteedSelfArgument())
    return false;

  // Return true if Ptr alias's self.
  return AA->isMustAlias(AI->getSelfArgument(), Ptr);
}

//===----------------------------------------------------------------------===//
// Utility Methods for determining use, decrement of values in a contiguous
// instruction range in one BB.
//===----------------------------------------------------------------------===//

/// If \p Op has arc uses in the instruction range [Start, End), return the
/// first such instruction. Otherwise return None. We assume that
/// Start and End are both in the same basic block.
llvm::Optional<SILBasicBlock::iterator>
swift::valueHasARCUsesInInstructionRange(SILValue Op,
                                         SILBasicBlock::iterator Start,
                                         SILBasicBlock::iterator End,
                                         AliasAnalysis *AA) {
  assert(Start->getParent() == End->getParent() &&
         "Start and End should be in the same basic block");

  // If Start == End, then we have an empty range, return false.
  if (Start == End)
    return llvm::None;

  // Otherwise, until Start != End.
  while (Start != End) {
    // Check if Start can use Op in an ARC relevant way. If so, return true.
    if (mayHaveSymmetricInterference(&*Start, Op, AA))
      return Start;

    // Otherwise, increment our iterator.
    ++Start;
  }

  // If all such instructions cannot use Op, return false.
  return llvm::None;
}

/// If \p Op has arc uses in the instruction range (Start, End], return the
/// first such instruction. Otherwise return None. We assume that Start and End
/// are both in the same basic block.
llvm::Optional<SILBasicBlock::iterator>
swift::valueHasARCUsesInReverseInstructionRange(SILValue Op,
                                                SILBasicBlock::iterator Start,
                                                SILBasicBlock::iterator End,
                                                AliasAnalysis *AA) {
  assert(Start->getParent() == End->getParent() &&
         "Start and End should be in the same basic block");
  assert(End != End->getParent()->end() &&
         "End should be mapped to an actual instruction");

  // If Start == End, then we have an empty range, return false.
  if (Start == End)
    return llvm::None;

  // Otherwise, until End == Start.
  while (Start != End) {
    // Check if Start can use Op in an ARC relevant way. If so, return true.
    if (mayHaveSymmetricInterference(&*End, Op, AA))
      return End;

    // Otherwise, decrement our iterator.
    --End;
  }

  // If all such instructions cannot use Op, return false.
  return llvm::None;
}

/// If \p Op has instructions in the instruction range (Start, End] which may
/// decrement it, return the first such instruction. Returns None
/// if no such instruction exists. We assume that Start and End are both in the
/// same basic block.
llvm::Optional<SILBasicBlock::iterator>
swift::valueHasARCDecrementOrCheckInInstructionRange(
    SILValue Op, SILBasicBlock::iterator Start, SILBasicBlock::iterator End,
    AliasAnalysis *AA) {
  assert(Start->getParent() == End->getParent() &&
         "Start and End should be in the same basic block");

  // If Start == End, then we have an empty range, return nothing.
  if (Start == End)
    return llvm::None;

  // Otherwise, until Start != End.
  while (Start != End) {
    // Check if Start can decrement or check Op's ref count. If so, return
    // Start. Ref count checks do not have side effects, but are barriers for
    // retains.
    if (mayDecrementRefCount(&*Start, Op, AA) || mayCheckRefCount(&*Start))
      return Start;
    // Otherwise, increment our iterator.
    ++Start;
  }

  // If all such instructions cannot decrement Op, return nothing.
  return llvm::None;
}

bool
swift::
mayGuaranteedUseValue(SILInstruction *User, SILValue Ptr, AliasAnalysis *AA) {
  // Instructions that check the ref count are modeled as both a potential
  // decrement and a use.
  if (mayCheckRefCount(User)) {
    switch (User->getKind()) {
    case SILInstructionKind::IsUniqueInst:
      // This instruction takes the address of its referent, so there's no way
      // for the optimizer to reuse the reference across it (it appears to
      // mutate the reference itself). In fact it's operand's RC root would be
      // the parent object. This means we can ignore it as a direct RC user.
      return false;
    case SILInstructionKind::IsEscapingClosureInst:
      // FIXME: this is overly conservative. It should return true only of the
      // RC identity of the single operand matches Ptr.
      return true;
    case SILInstructionKind::BeginCOWMutationInst:
      // begin_cow_mutation takes the argument as owned and produces a new
      // owned result.
      return false;
    default:
      llvm_unreachable("Unexpected check-ref-count instruction.");
    }
  }

  // Only full apply sites can require a guaranteed lifetime. If we don't have
  // one, bail.
  if (!isa<FullApplySite>(User))
    return false;

  FullApplySite FAS(User);

  // Ok, we have a full apply site. Check if the callee is callee_guaranteed. In
  // such a case, if we can not prove no alias, we need to be conservative and
  // return true.
  CanSILFunctionType FType = FAS.getSubstCalleeType();
  if (FType->isCalleeGuaranteed() && !AA->isNoAlias(FAS.getCallee(), Ptr)) {
    return true;
  }

  // Ok, we have a full apply site and our callee is a normal use. Thus if the
  // apply does not have any normal arguments, we don't need to worry about any
  // guaranteed parameters and return early.
  if (!FAS.getNumArguments())
    return false;

  // Ok, we have an apply site with arguments. Look at the function type and
  // iterate through the function parameters. If any of the parameters are
  // guaranteed, attempt to prove that the passed in parameter cannot alias
  // Ptr. If we fail, return true.
  auto Params = FType->getParameters();
  for (unsigned i : indices(Params)) {    
    if (!Params[i].isGuaranteed())
      continue;
    SILValue Op = FAS.getArgumentsWithoutIndirectResults()[i];
    if (!AA->isNoAlias(Op, Ptr))
      return true;
  }

  // Ok, we were able to prove that all arguments to the apply that were
  // guaranteed do not alias Ptr. Return false.
  return false;
}

//===----------------------------------------------------------------------===//
//                          Owned Result Utilities
//===----------------------------------------------------------------------===//

ConsumedResultToEpilogueRetainMatcher::
ConsumedResultToEpilogueRetainMatcher(RCIdentityFunctionInfo *RCFI,
                                      AliasAnalysis *AA,
                                      SILFunction *F)
    : F(F), RCFI(RCFI), AA(AA) {
  recompute();
}

void ConsumedResultToEpilogueRetainMatcher::recompute() {
  EpilogueRetainInsts.clear();

  // Find the return BB of F. If we fail, then bail.
  SILFunction::iterator BB = F->findReturnBB();
  if (BB == F->end())
    return;
  findMatchingRetains(&*BB);
}

bool ConsumedResultToEpilogueRetainMatcher::isTransitiveSuccessorsRetainFree(
    const llvm::DenseSet<SILBasicBlock *> &BBs) {
  // For every block with retain, we need to check the transitive
  // closure of its successors are retain-free.
  for (auto &I : EpilogueRetainInsts) {
    for (auto &Succ : I->getParent()->getSuccessors()) {
      if (BBs.count(Succ))
        continue;
      return false;
    }
  }

  // FIXME: We are iterating over a DenseSet. That can lead to non-determinism
  // and is in general pretty inefficient since we are iterating over a hash
  // table.
  for (auto CBB : BBs) {
    for (auto &Succ : CBB->getSuccessors()) {
      if (BBs.count(Succ))
        continue;
      return false;
    }
  }
  return true;
}

ConsumedResultToEpilogueRetainMatcher::RetainKindValue
ConsumedResultToEpilogueRetainMatcher::
findMatchingRetainsInBasicBlock(SILBasicBlock *BB, SILValue V) {
  for (auto II = BB->rbegin(), IE = BB->rend(); II != IE; ++II) {
    // Handle self-recursion.
    if (auto *AI = dyn_cast<ApplyInst>(&*II))
      if (AI->getCalleeFunction() == BB->getParent()) 
        return std::make_pair(FindRetainKind::Recursion, AI);
    
    // If we do not have a retain_value or strong_retain...
    if (!isa<RetainValueInst>(*II) && !isa<StrongRetainInst>(*II)) {
      // we can ignore it if it can not decrement the reference count of the
      // return value.
      if (!mayDecrementRefCount(&*II, V, AA))
        continue;

      // Otherwise, we need to stop computing since we do not want to create
      // lifetime gap.
      return std::make_pair(FindRetainKind::Blocked, nullptr);
    }

    // Ok, we have a retain_value or strong_retain. Grab Target and find the
    // RC identity root of its operand.
    SILInstruction *Target = &*II;
    SILValue RetainValue = RCFI->getRCIdentityRoot(Target->getOperand(0));
    SILValue ReturnValue = RCFI->getRCIdentityRoot(V);

    // Is this the epilogue retain we are looking for ?.
    // We break here as we do not know whether this is a part of the epilogue
    // retain for the @own return value.
    if (RetainValue != ReturnValue)
      break;

    return std::make_pair(FindRetainKind::Found, &*II);
  }

  // Did not find retain in this block.
  return std::make_pair(FindRetainKind::None, nullptr);
} 

void
ConsumedResultToEpilogueRetainMatcher::
findMatchingRetains(SILBasicBlock *BB) {
  // Iterate over the instructions post-order and find retains associated with
  // return value.
  SILValue RV = SILValue();
  for (auto II = BB->rbegin(), IE = BB->rend(); II != IE; ++II) {
    if (auto *RI = dyn_cast<ReturnInst>(&*II)) {
      RV = RI->getOperand();
      break;
    }
  }

  // Somehow, we managed not to find a return value.
  if (!RV)
    return;

  // OK. we've found the return value, now iterate on the CFG to find all the
  // post-dominating retains.
  //
  // The ConsumedResultToEpilogueRetainMatcher finds the final releases
  // in the following way.
  //
  // 1. If an instruction, which is not releaseinst nor releasevalue, that
  // could decrement reference count is found. bail out.
  //
  // 2. If a release is found and the release that can not be mapped to any
  // @owned argument. bail as this release may well be the final release of
  // an @owned argument, but somehow rc-identity fails to prove that.
  //
  // 3. A release that is mapped to an argument which already has a release
  // that overlaps with this release. This release for sure is not the final
  // release.
  constexpr unsigned WorkListMaxSize = 4;

  llvm::DenseSet<SILBasicBlock *> RetainFrees;
  llvm::SmallVector<BasicBlockRetainValue, 4> WorkList;
  llvm::DenseSet<SILBasicBlock *> HandledBBs;
  WorkList.push_back(std::make_pair(BB, RV));
  HandledBBs.insert(BB);
  while (!WorkList.empty()) {
    // Too many blocks ?.
    if (WorkList.size() > WorkListMaxSize) {
      EpilogueRetainInsts.clear();
      return;
    }

    // Try to find a retain %value in this basic block.
    auto R = WorkList.pop_back_val();
    RetainKindValue Kind = findMatchingRetainsInBasicBlock(R.first, R.second);

    // We've found a retain on this path.
    if (Kind.first == FindRetainKind::Found) { 
      EpilogueRetainInsts.push_back(Kind.second);
      continue;
    }

    // There is a MayDecrement instruction.
    if (Kind.first == FindRetainKind::Blocked) {
      EpilogueRetainInsts.clear();
      return;
    }

    // There is a self-recursion. Use the apply instruction as the retain.
    if (Kind.first == FindRetainKind::Recursion) {
      EpilogueRetainInsts.push_back(Kind.second);
      continue;
    }
  
    // Did not find a retain in this block, try to go to its predecessors.
    if (Kind.first == FindRetainKind::None) {
      // We can not find a retain in a block with no predecessors.
      if (R.first->getPredecessorBlocks().begin() ==
          R.first->getPredecessorBlocks().end()) {
        EpilogueRetainInsts.clear();
        return;
      }

      // This block does not have a retain.
      RetainFrees.insert(R.first);

      // If this is a SILArgument of current basic block, we can split it up to
      // values in the predecessors.
      auto *SA = dyn_cast<SILPhiArgument>(R.second);
      if (SA && SA->getParent() != R.first)
        SA = nullptr;

      for (auto X : R.first->getPredecessorBlocks()) {
        if (HandledBBs.contains(X))
          continue;
        // Try to use the predecessor edge-value.
        if (SA && SA->getIncomingPhiValue(X)) {
          WorkList.push_back(std::make_pair(X, SA->getIncomingPhiValue(X)));
        } else
          WorkList.push_back(std::make_pair(X, R.second));
   
        HandledBBs.insert(X);
      }
    }
  }

  // Lastly, check whether all the successor blocks are retain-free.
  if (!isTransitiveSuccessorsRetainFree(RetainFrees))
    EpilogueRetainInsts.clear();

  // At this point, we've either failed to find any epilogue retains or
  // all the post-dominating epilogue retains.
}

//===----------------------------------------------------------------------===//
//                          Owned Argument Utilities
//===----------------------------------------------------------------------===//

ConsumedArgToEpilogueReleaseMatcher::ConsumedArgToEpilogueReleaseMatcher(
    RCIdentityFunctionInfo *RCFI,
    SILFunction *F,
    ArrayRef<SILArgumentConvention> ArgumentConventions,
    ExitKind Kind)
    : F(F), RCFI(RCFI), Kind(Kind), ArgumentConventions(ArgumentConventions),
      ProcessedBlock(nullptr) {
  recompute();
}

void ConsumedArgToEpilogueReleaseMatcher::recompute() {
  ArgInstMap.clear();

  // Find the return BB of F. If we fail, then bail.
  SILFunction::iterator BB;
  switch (Kind) {
  case ExitKind::Return:
    BB = F->findReturnBB();
    break;
  case ExitKind::Throw:
    BB = F->findThrowBB();
    break;
  }

  if (BB == F->end()) {
    ProcessedBlock = nullptr;
    return;
  }
  ProcessedBlock = &*BB;
  findMatchingReleases(&*BB);
}

bool ConsumedArgToEpilogueReleaseMatcher::isRedundantRelease(
    ArrayRef<SILInstruction *> Insts, SILValue Base, SILValue Derived) {
  // We use projection path to analyze the relation.
  auto POp = ProjectionPath::getProjectionPath(Base, Derived);
  // We can not build a projection path from the base to the derived, bail out.
  // and return true so that we can stop the epilogue walking sequence.
  if (!POp.has_value())
    return true;

  for (auto &R : Insts) {
    SILValue ROp = R->getOperand(0);
    auto PROp = ProjectionPath::getProjectionPath(Base, ROp); 
    if (!PROp.has_value())
      return true;
    // If Op is a part of ROp or Rop is a part of Op. then we have seen
    // a redundant release.
    if (!PROp.value().hasNonEmptySymmetricDifference(POp.value()))
      return true;
  }
  return false;
}

bool ConsumedArgToEpilogueReleaseMatcher::releaseArgument(
    ArrayRef<SILInstruction *> Insts, SILValue Arg) {
  // Reason about whether all parts are released.
  auto *F = (*Insts.begin())->getFunction();

  // These are the list of SILValues that are actually released.
  ProjectionPathSet Paths;
  for (auto &I : Insts) {
    auto PP = ProjectionPath::getProjectionPath(Arg, I->getOperand(0));
    if (!PP)
      return false;
    Paths.insert(PP.value());
  } 

  // Is there an uncovered non-trivial type.
  return !ProjectionPath::hasUncoveredNonTrivials(Arg->getType(), *F, Paths);
}

void
ConsumedArgToEpilogueReleaseMatcher::
processMatchingReleases() {
  // If we can not find a release for all parts with reference semantics
  // that means we did not find all releases for the base.
  for (auto &pair : ArgInstMap) {
    // We do not know if we have a fully post dominating release set
    // so all release sets should be considered partially post
    // dominated.
    auto releaseSet = pair.second.getPartiallyPostDomReleases();
    if (!releaseSet)
      continue;

    // If an argument has a single release and it is rc-identical to the
    // SILArgument. Then we do not need to use projection to check for whether
    // all non-trivial fields are covered.
    if (releaseSet->size() == 1) {
      SILInstruction *inst = *releaseSet->begin();
      SILValue rv = inst->getOperand(0);
      if (pair.first == RCFI->getRCIdentityRoot(rv)) {
        pair.second.setHasJointPostDominatingReleaseSet();
        continue;
      }
    }

    // OK. we have multiple epilogue releases for this argument, check whether
    // it has covered all fields with reference semantic in the argument.
    if (!releaseArgument(*releaseSet, pair.first))
      continue;

    // OK. At this point we know that we found a joint post dominating
    // set of releases. Mark our argument as such.
    pair.second.setHasJointPostDominatingReleaseSet();
  }
}

/// Check if a given argument convention is in the list
/// of possible argument conventions.
static bool
isOneOfConventions(SILArgumentConvention Convention,
                   ArrayRef<SILArgumentConvention> ArgumentConventions) {
  for (auto ArgumentConvention : ArgumentConventions) {
    if (Convention == ArgumentConvention)
      return true;
  }
  return false;
}

void ConsumedArgToEpilogueReleaseMatcher::collectMatchingDestroyAddresses(
    SILBasicBlock *block) {
  // Check if we can find destroy_addr for each @in argument.
  SILFunction::iterator anotherEpilogueBB =
      (Kind == ExitKind::Return) ? F->findThrowBB() : F->findReturnBB();

  for (auto *arg : F->begin()->getSILFunctionArguments()) {
    if (arg->isIndirectResult())
      continue;
    if (arg->getArgumentConvention() != SILArgumentConvention::Indirect_In)
      continue;
    bool hasDestroyAddrOutsideEpilogueBB = false;
    // This is an @in argument. Check if there are any destroy_addr
    // instructions for it.
    for (Operand *op : getNonDebugUses(arg)) {
      auto *user = op->getUser();
      if (!isa<DestroyAddrInst>(user))
        continue;
      // Do not take into account any uses in the other
      // epilogue BB.
      if (anotherEpilogueBB != F->end() &&
          user->getParent() == &*anotherEpilogueBB)
        continue;
      if (user->getParent() != block)
        hasDestroyAddrOutsideEpilogueBB = true;

      // Since ArgumentState uses a TinyPtrVector, creating
      // temporaries containing one element is cheap.
      auto iter = ArgInstMap.insert({arg, ArgumentState(user)});
      // We inserted the value.
      if (iter.second)
        continue;
      // Otherwise, add this release to the set.
      iter.first->second.addRelease(user);
    }

    // Don't know how to handle destroy_addr outside of the epilogue.
    if (hasDestroyAddrOutsideEpilogueBB)
      ArgInstMap.erase(arg);
  }
}

void ConsumedArgToEpilogueReleaseMatcher::collectMatchingReleases(
    SILBasicBlock *block) {
  // Iterate over the instructions post-order and find final releases
  // associated with each arguments.
  //
  // The ConsumedArgToEpilogueReleaseMatcher finds the final releases
  // in the following way. 
  //
  // 1. If an instruction, which is not releaseinst nor releasevalue, that
  // could decrement reference count is found. bail out.
  //
  // 2. If a release is found and the release that can not be mapped to any
  // @owned argument. bail as this release may well be the final release of
  // an @owned argument, but somehow rc-identity fails to prove that.
  //
  // 3. A release that is mapped to an argument which already has a release
  // that overlaps with this release. This release for sure is not the final
  // release.
  bool isTrackingInArgs = isOneOfConventions(SILArgumentConvention::Indirect_In,
                                             ArgumentConventions);
  for (auto &inst : llvm::reverse(*block)) {
    if (isTrackingInArgs && isa<DestroyAddrInst>(inst)) {
      // It is probably a destroy addr for an @in argument.
      continue;
    }
    // If we do not have a release_value or strong_release. We can continue
    if (!isa<ReleaseValueInst>(inst) && !isa<StrongReleaseInst>(inst)) {
      // We cannot match a final release if it is followed by a dealloc_ref.
      if (isa<DeallocRefInst>(inst) || isa<DeallocPartialRefInst>(inst))
        break;

      // We do not know what this instruction is, do a simple check to make sure
      // that it does not decrement the reference count of any of its operand. 
      //
      // TODO: we could make the logic here more complicated to handle each type
      // of instructions in a more precise manner.
      if (!inst.mayRelease())
        continue;
      // This instruction may release something, bail out conservatively.
      break;
    }

    // Ok, we have a release_value or strong_release. Grab Target and find the
    // RC identity root of its operand.
    SILValue origOp = inst.getOperand(0);
    SILValue op = RCFI->getRCIdentityRoot(origOp);

    // Check whether this is a SILArgument or a part of a SILArgument. This is
    // possible after we expand release instructions in SILLowerAgg pass.
    auto *arg = dyn_cast<SILFunctionArgument>(stripValueProjections(op));
    if (!arg)
      break;

    // If Op is not a consumed argument, we must break since this is not an Op
    // that is a part of a return sequence. We are being conservative here since
    // we could make this more general by allowing for intervening non-arg
    // releases in the sense that we do not allow for race conditions in between
    // destructors.
    if (!isOneOfConventions(arg->getArgumentConvention(), ArgumentConventions))
      break;

    // Ok, we have a release on a SILArgument that has a consuming convention.
    // Attempt to put it into our arc opts map. If we already have it, we have
    // exited the return value sequence so break. Otherwise, continue looking
    // for more arc operations.
    auto iter = ArgInstMap.find(arg);
    if (iter == ArgInstMap.end()) {
      ArgInstMap.insert({arg, {&inst}});
      continue;
    }

    // We've already seen at least part of this base. Check to see whether we
    // are seeing a redundant release.
    //
    // If we are seeing a redundant release we have exited the return value
    // sequence, so break.
    if (!isa<DestroyAddrInst>(inst)) {
      // We do not know if we have a fully post dominating release
      // set, so we use the partial post dom entry point.
      if (auto partialReleases = iter->second.getPartiallyPostDomReleases()) {
        if (isRedundantRelease(*partialReleases, arg, origOp)) {
          break;
        }
      }
    }

    // We've seen part of this base, but this is a part we've have not seen.
    // Record it.
    iter->second.addRelease(&inst);
  }

  if (isTrackingInArgs) {
    // Find destroy_addr for each @in argument.
    collectMatchingDestroyAddresses(block);
  }
}

void
ConsumedArgToEpilogueReleaseMatcher::
findMatchingReleases(SILBasicBlock *BB) {
  // Walk the given basic block to find all the epilogue releases.
  collectMatchingReleases(BB);
  // We've exited the epilogue sequence, try to find out which parameter we
  // have all the epilogue releases for and which one we did not.
  processMatchingReleases();
}

//===----------------------------------------------------------------------===//
//                            Leaking BB Analysis
//===----------------------------------------------------------------------===//

static bool ignorableApplyInstInUnreachableBlock(const ApplyInst *AI) {
  auto applySite = FullApplySite(const_cast<ApplyInst *>(AI));
  return applySite.isCalleeKnownProgramTerminationPoint();
}

static bool ignorableBuiltinInstInUnreachableBlock(const BuiltinInst *BI) {
  const BuiltinInfo &BInfo = BI->getBuiltinInfo();
  if (BInfo.ID == BuiltinValueKind::CondUnreachable)
    return true;

  const IntrinsicInfo &IInfo = BI->getIntrinsicInfo();
  if (IInfo.ID == llvm::Intrinsic::trap)
    return true;

  return false;
}

/// Match a call to a trap BB with no ARC relevant side effects.
bool swift::isARCInertTrapBB(const SILBasicBlock *BB) {
  // Do a quick check at the beginning to make sure that our terminator is
  // actually an unreachable. This ensures that in many cases this function will
  // exit early and quickly.
  auto II = BB->rbegin();
  if (!isa<UnreachableInst>(*II))
    return false;

  auto IE = BB->rend();
  while (II != IE) {
    // Ignore any instructions without side effects.
    if (!II->mayHaveSideEffects()) {
      ++II;
      continue;
    }

    // Ignore cond fail.
    if (isa<CondFailInst>(*II)) {
      ++II;
      continue;
    }

    // Check for apply insts that we can ignore.
    if (auto *AI = dyn_cast<ApplyInst>(&*II)) {
      if (ignorableApplyInstInUnreachableBlock(AI)) {
        ++II;
        continue;
      }
    }

    // Check for builtins that we can ignore.
    if (auto *BI = dyn_cast<BuiltinInst>(&*II)) {
      if (ignorableBuiltinInstInUnreachableBlock(BI)) {
        ++II;
        continue;
      }
    }

    // If we can't ignore the instruction, return false.
    return false;
  }

  // Otherwise, we have an unreachable and every instruction is inert from an
  // ARC perspective in an unreachable BB.
  return true;
}
