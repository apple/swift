//===--- SILInstruction.cpp - Instructions for SIL code -------------------===//
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
//
// This file defines the high-level SILInstruction classes used for SIL code.
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/SILInstruction.h"
#include "swift/Basic/AssertImplements.h"
#include "swift/Basic/Unicode.h"
#include "swift/Basic/type_traits.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SIL/Test.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ErrorHandling.h"
using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//
// Instruction-specific properties on SILValue
//===----------------------------------------------------------------------===//

void SILInstruction::setDebugScope(const SILDebugScope *DS) {
  if (getDebugScope() && getDebugScope()->InlinedCallSite)
    assert(DS->InlinedCallSite && "throwing away inlined scope info");

  assert(DS->getParentFunction() == getFunction() &&
         "scope belongs to different function");

  debugScope = DS;
}

//===----------------------------------------------------------------------===//
// ilist_traits<SILInstruction> Implementation
//===----------------------------------------------------------------------===//

// The trait object is embedded into a basic block.  Use dirty hacks to
// reconstruct the BB from the 'self' pointer of the trait.
SILBasicBlock *llvm::ilist_traits<SILInstruction>::getContainingBlock() {
  size_t Offset(
      size_t(&((SILBasicBlock *)nullptr->*SILBasicBlock::getSublistAccess())));
  iplist<SILInstruction> *Anchor(static_cast<iplist<SILInstruction> *>(this));
  return reinterpret_cast<SILBasicBlock *>(reinterpret_cast<char *>(Anchor) -
                                           Offset);
}


void llvm::ilist_traits<SILInstruction>::addNodeToList(SILInstruction *I) {
  I->ParentBB = getContainingBlock();
}

void llvm::ilist_traits<SILInstruction>::
transferNodesFromList(llvm::ilist_traits<SILInstruction> &L2,
                      instr_iterator first, instr_iterator last) {
  // If transferring instructions within the same basic block, no reason to
  // update their parent pointers.
  SILBasicBlock *ThisParent = getContainingBlock();
  SILBasicBlock *l2Block = L2.getContainingBlock();
  if (ThisParent == l2Block) return;
  
  bool differentFunctions = (ThisParent->getFunction() != l2Block->getFunction());

  // Update the parent fields in the instructions.
  for (; first != last; ++first) {
    SWIFT_FUNC_STAT_NAMED("sil");
    first->ParentBB = ThisParent;
    if (differentFunctions) {
      for (SILValue result : first->getResults()) {
        result->resetBitfields();
      }
      first->asSILNode()->resetBitfields();
    }
  }
}

//===----------------------------------------------------------------------===//
// SILInstruction Implementation
//===----------------------------------------------------------------------===//

// Assert that all subclasses of ValueBase implement classof.
#define NODE(CLASS, PARENT) \
  ASSERT_IMPLEMENTS_STATIC(CLASS, PARENT, classof, bool(SILNodePointer));
#include "swift/SIL/SILNodes.def"

/// eraseFromParent - This method unlinks 'self' from the containing basic
/// block and deletes it.
///
void SILInstruction::eraseFromParent() {
#ifndef NDEBUG
  for (auto result : getResults()) {
    assert(result->use_empty() && "Uses of SILInstruction remain at deletion.");
  }
#endif
  getParent()->erase(this);
}

void SILInstruction::moveFront(SILBasicBlock *Block) {
  Block->moveInstructionToFront(this);
}

/// Unlink this instruction from its current basic block and insert it into
/// the basic block that Later lives in, right before Later.
void SILInstruction::moveBefore(SILInstruction *Later) {
  SILBasicBlock::moveInstruction(this, Later);
}

namespace swift::test {
FunctionTest MoveBeforeTest("instruction-move-before",
                            [](auto &function, auto &arguments, auto &test) {
                              auto *inst = arguments.takeInstruction();
                              auto *other = arguments.takeInstruction();
                              inst->moveBefore(other);
                            });
} // end namespace swift::test

/// Unlink this instruction from its current basic block and insert it into
/// the basic block that Earlier lives in, right after Earlier.
void SILInstruction::moveAfter(SILInstruction *Earlier) {
  // Since MovePos is an instruction, we know that there is always a valid
  // iterator after it.
  auto Later = std::next(SILBasicBlock::iterator(Earlier));
  moveBefore(&*Later);
}

void SILInstruction::dropAllReferences() {
  MutableArrayRef<Operand> PossiblyDeadOps = getAllOperands();
  for (auto OpI = PossiblyDeadOps.begin(),
            OpE = PossiblyDeadOps.end(); OpI != OpE; ++OpI) {
    OpI->drop();
  }
  dropNonOperandReferences();
}

void SILInstruction::dropNonOperandReferences() {
  if (auto *termInst = dyn_cast<TermInst>(this)) {
    for (SILSuccessor &succ : termInst->getSuccessors()) {
      succ = nullptr;
    }
  }

  // If we have a function ref inst, we need to especially drop its function
  // argument so that it gets a proper ref decrement.
  if (auto *FRI = dyn_cast<FunctionRefBaseInst>(this)) {
    if (!FRI->getInitiallyReferencedFunction())
      return;
    FRI->dropReferencedFunction();
    return;
  }

  // If we have a KeyPathInst, drop its pattern reference so that we can
  // decrement refcounts on referenced functions.
  if (auto *KPI = dyn_cast<KeyPathInst>(this)) {
    if (!KPI->hasPattern())
      return;
    
    KPI->dropReferencedPattern();
    return;
  }
}

namespace {

class AllResultsAccessor
    : public SILInstructionVisitor<AllResultsAccessor,
                                   SILInstructionResultArray> {
public:
// Make sure we hit a linker error if we ever miss an instruction.
#define INST(ID, NAME) SILInstructionResultArray visit##ID(ID *I);
#define NON_VALUE_INST(ID, NAME, PARENT, MEMBEHAVIOR, MAYRELEASE)              \
  SILInstructionResultArray visit##ID(ID *I) {                                 \
    return SILInstructionResultArray();                                        \
  }
#define SINGLE_VALUE_INST(ID, TEXTUALNAME, PARENT, MEMBEHAVIOR, MAYRELEASE)    \
  SILInstructionResultArray visit##ID(ID *I) {                                 \
    return SILInstructionResultArray(                                          \
        static_cast<SingleValueInstruction *>(I));                             \
  }
#define MULTIPLE_VALUE_INST(ID, TEXTUALNAME, PARENT, MEMBEHAVIOR, MAYRELEASE)  \
  SILInstructionResultArray visit##ID(ID *I) {                                 \
    return SILInstructionResultArray(I->getAllResults());                      \
  }
#include "swift/SIL/SILNodes.def"
};

} // end anonymous namespace

SILInstructionResultArray SILInstruction::getResultsImpl() const {
  return AllResultsAccessor().visit(const_cast<SILInstruction *>(this));
}

// Initialize the static members of SILInstruction.
int SILInstruction::NumCreatedInstructions = 0;
int SILInstruction::NumDeletedInstructions = 0;

/// Map a SILInstruction name to its SILInstructionKind.
SILInstructionKind swift::getSILInstructionKind(StringRef name) {
#define FULL_INST(ID, NAME, PARENT, MEMBEHAVIOR, MAYRELEASE)  \
  if (name == #NAME)                                          \
    return SILInstructionKind::ID;
#include "swift/SIL/SILNodes.def"

#ifdef NDEBUG
  llvm::errs() << "Unknown SIL instruction name\n";
  abort();
#endif
  llvm_unreachable("Unknown SIL instruction name");
}

/// Map SILInstructionKind to a corresponding SILInstruction name.
StringRef swift::getSILInstructionName(SILInstructionKind kind) {
  switch (kind) {
#define FULL_INST(ID, NAME, PARENT, MEMBEHAVIOR, MAYRELEASE)  \
  case SILInstructionKind::ID:                                \
    return #NAME;
#include "swift/SIL/SILNodes.def"
  }
  llvm_unreachable("bad kind");
}

void SILInstruction::replaceAllUsesOfAllResultsWithUndef() {
  for (auto result : getResults()) {
    result->replaceAllUsesWithUndef();
  }
}

void SILInstruction::replaceAllUsesPairwiseWith(SILInstruction *other) {
  auto results = getResults();

  // If we don't have any results, fast-path out without asking the other
  // instruction for its results.
  if (results.empty()) {
    assert(other->getResults().empty());
    return;
  }

  // Replace values with the corresponding values of the other instruction.
  auto otherResults = other->getResults();
  assert(results.size() == otherResults.size());
  for (auto i : indices(results)) {
    results[i]->replaceAllUsesWith(otherResults[i]);
  }
}

void SILInstruction::replaceAllUsesPairwiseWith(
    const llvm::SmallVectorImpl<SILValue> &NewValues) {
  auto Results = getResults();

  // If we don't have any results, fast-path out without asking the other
  // instruction for its results.
  if (Results.empty()) {
    assert(NewValues.empty());
    return;
  }

  // Replace values with the corresponding values of the list. Make sure they
  // are all the same type.
  assert(Results.size() == NewValues.size());
  for (unsigned i : indices(Results)) {
    assert(Results[i]->getType() == NewValues[i]->getType() &&
           "Can only replace results with new values of the same type");
    Results[i]->replaceAllUsesWith(NewValues[i]);
  }
}

Operand *BeginBorrowInst::getSingleNonEndingUse() const {
  Operand *singleUse = nullptr;
  for (auto *use : getUses()) {
    if (isa<EndBorrowInst>(use->getUser()))
      continue;

    if (singleUse)
      return nullptr;

    singleUse = use;
  }
  return singleUse;
}

namespace {
class InstructionDestroyer
    : public SILInstructionVisitor<InstructionDestroyer> {
public:
#define INST(CLASS, PARENT) \
  void visit##CLASS(CLASS *I) { I->~CLASS(); }
#include "swift/SIL/SILNodes.def"
  };
} // end anonymous namespace

void SILInstruction::destroy(SILInstruction *I) {
  InstructionDestroyer().visit(I);
}

namespace {
  /// Given a pair of instructions that are already known to have the same kind,
  /// type, and operands check any special state in the two instructions that
  /// could disrupt equality.
  class InstructionIdentityComparer :
    public SILInstructionVisitor<InstructionIdentityComparer, bool> {
  public:

    InstructionIdentityComparer(const SILInstruction *L) : LHS(L) { }

    /// Make sure we only process instructions we know how to process.
    bool visitSILInstruction(const SILInstruction *RHS) {
      return false;
    }

    bool visitInjectEnumAddrInst(const InjectEnumAddrInst *RHS) {
        auto *X = cast<InjectEnumAddrInst>(LHS);
        return X->getElement() == RHS->getElement();
    }

    bool visitDestroyAddrInst(const DestroyAddrInst *RHS) {
      return true;
    }

    bool visitReleaseValueInst(const ReleaseValueInst *RHS) {
      return true;
    }

    bool visitReleaseValueAddrInst(const ReleaseValueAddrInst *RHS) {
      return true;
    }

    bool visitRetainValueInst(const RetainValueInst *RHS) {
      return true;
    }

    bool visitRetainValueAddrInst(const RetainValueAddrInst *RHS) {
      return true;
    }

    bool visitDeallocStackInst(const DeallocStackInst *RHS) {
      return true;
    }

    bool visitDeallocStackRefInst(const DeallocStackRefInst *RHS) {
      return true;
    }

    bool visitAllocStackInst(const AllocStackInst *RHS) {
      return true;
    }

    bool visitDeallocBoxInst(const DeallocBoxInst *RHS) {
      return true;
    }

    bool visitAllocBoxInst(const AllocBoxInst *RHS) {
      return true;
    }

    bool visitDeallocRefInst(const DeallocRefInst *RHS) {
      return true;
    }

    bool visitDeallocPartialRefInst(const DeallocPartialRefInst *RHS) {
      return true;
    }

    bool visitDestructureStructInst(const DestructureStructInst *RHS) {
      return true;
    }

    bool visitDestructureTupleInst(const DestructureTupleInst *RHS) {
      return true;
    }

    bool visitAllocRefInst(const AllocRefInst *RHS) {
      auto *LHSInst = cast<AllocRefInst>(LHS);
      auto LHSTypes = LHSInst->getTailAllocatedTypes();
      auto RHSTypes = RHS->getTailAllocatedTypes();
      unsigned NumTypes = LHSTypes.size();
      assert(NumTypes == RHSTypes.size());
      for (unsigned Idx = 0; Idx < NumTypes; ++Idx) {
        if (LHSTypes[Idx] != RHSTypes[Idx])
          return false;
      }
      return true;
    }
    
    bool visitDestroyValueInst(const DestroyValueInst *RHS) {
      auto *left = cast<DestroyValueInst>(LHS);
      return left->poisonRefs() == RHS->poisonRefs();
    }

    bool visitDebugValue(const DebugValueInst *RHS) {
      auto *left = cast<DebugValueInst>(LHS);
      return left->poisonRefs() == RHS->poisonRefs();
    }

    bool visitBeginCOWMutationInst(const BeginCOWMutationInst *RHS) {
      auto *left = cast<BeginCOWMutationInst>(LHS);
      return left->isNative() == RHS->isNative();
    }

    bool visitEndCOWMutationInst(const EndCOWMutationInst *RHS) {
      auto *left = cast<EndCOWMutationInst>(LHS);
      return left->doKeepUnique() == RHS->doKeepUnique();
    }

    bool visitAllocRefDynamicInst(const AllocRefDynamicInst *RHS) {
      return true;
    }

    bool visitProjectBoxInst(const ProjectBoxInst *RHS) {
      return true;
    }

    bool visitProjectExistentialBoxInst(const ProjectExistentialBoxInst *RHS) {
      return true;
    }

    bool visitBeginAccessInst(const BeginAccessInst *right) {
      auto left = cast<BeginAccessInst>(LHS);
      return left->getAccessKind() == right->getAccessKind()
          && left->getEnforcement() == right->getEnforcement()
          && left->hasNoNestedConflict() == right->hasNoNestedConflict()
          && left->isFromBuiltin() == right->isFromBuiltin();
    }

    bool visitEndAccessInst(const EndAccessInst *right) {
      auto left = cast<EndAccessInst>(LHS);
      return left->isAborting() == right->isAborting();
    }

    bool visitBeginUnpairedAccessInst(const BeginUnpairedAccessInst *right) {
      auto left = cast<BeginUnpairedAccessInst>(LHS);
      return left->getAccessKind() == right->getAccessKind()
          && left->getEnforcement() == right->getEnforcement()
          && left->hasNoNestedConflict() == right->hasNoNestedConflict()
          && left->isFromBuiltin() == right->isFromBuiltin();
    }

    bool visitEndUnpairedAccessInst(const EndUnpairedAccessInst *right) {
      auto left = cast<EndUnpairedAccessInst>(LHS);
      return left->getEnforcement() == right->getEnforcement()
             && left->isAborting() == right->isAborting()
             && left->isFromBuiltin() == right->isFromBuiltin();
    }

    bool visitStrongReleaseInst(const StrongReleaseInst *RHS) {
      return true;
    }

    bool visitStrongRetainInst(const StrongRetainInst *RHS) {
      return true;
    }

    bool visitLoadInst(const LoadInst *RHS) {
      auto LHSQualifier = cast<LoadInst>(LHS)->getOwnershipQualifier();
      return LHSQualifier == RHS->getOwnershipQualifier();
    }

    bool visitLoadBorrowInst(const LoadBorrowInst *RHS) { return true; }

    bool visitEndBorrowInst(const EndBorrowInst *RHS) { return true; }
    bool visitBeginBorrowInst(const BeginBorrowInst *BBI) { return true; }

    bool visitStoreBorrowInst(const StoreBorrowInst *RHS) {
      auto *X = cast<StoreBorrowInst>(LHS);
      return X->getSrc() == RHS->getSrc() && X->getDest() == RHS->getDest();
    }

    bool visitStoreInst(const StoreInst *RHS) {
      auto *X = cast<StoreInst>(LHS);
      return X->getSrc() == RHS->getSrc() && X->getDest() == RHS->getDest() &&
        X->getOwnershipQualifier() == RHS->getOwnershipQualifier();
    }

    bool visitBindMemoryInst(const BindMemoryInst *RHS) {
      auto *X = cast<BindMemoryInst>(LHS);
      return X->getBoundType() == RHS->getBoundType();
    }

    bool visitFunctionRefInst(const FunctionRefInst *RHS) {
      auto *X = cast<FunctionRefInst>(LHS);
      return X->getReferencedFunction() == RHS->getReferencedFunction();
    }
    bool visitDynamicFunctionRefInst(const DynamicFunctionRefInst *RHS) {
      auto *X = cast<DynamicFunctionRefInst>(LHS);
      return X->getInitiallyReferencedFunction() ==
             RHS->getInitiallyReferencedFunction();
    }
    bool visitPreviousDynamicFunctionRefInst(
        const PreviousDynamicFunctionRefInst *RHS) {
      auto *X = cast<PreviousDynamicFunctionRefInst>(LHS);
      return X->getInitiallyReferencedFunction() ==
             RHS->getInitiallyReferencedFunction();
    }

    bool visitAllocGlobalInst(const AllocGlobalInst *RHS) {
      auto *X = cast<AllocGlobalInst>(LHS);
      return X->getReferencedGlobal() == RHS->getReferencedGlobal();
    }

    bool visitGlobalAddrInst(const GlobalAddrInst *RHS) {
      auto *X = cast<GlobalAddrInst>(LHS);
      return X->getReferencedGlobal() == RHS->getReferencedGlobal();
    }

    bool visitIntegerLiteralInst(const IntegerLiteralInst *RHS) {
      APInt X = cast<IntegerLiteralInst>(LHS)->getValue();
      APInt Y = RHS->getValue();
      return X.getBitWidth() == Y.getBitWidth() &&
        X == Y;
    }

    bool visitFloatLiteralInst(const FloatLiteralInst *RHS) {
      // Avoid floating point comparison issues by doing a bitwise comparison.
      APInt X = cast<FloatLiteralInst>(LHS)->getBits();
      APInt Y = RHS->getBits();
      return X.getBitWidth() == Y.getBitWidth() &&
        X == Y;
    }

    bool visitStringLiteralInst(const StringLiteralInst *RHS) {
      auto LHS_ = cast<StringLiteralInst>(LHS);
      return LHS_->getEncoding() == RHS->getEncoding()
        && LHS_->getValue().equals(RHS->getValue());
    }

    bool visitStructInst(const StructInst *RHS) {
      // We have already checked the operands. Make sure that the StructDecls
      // match up.
      StructDecl *S1 = cast<StructInst>(LHS)->getStructDecl();
      return S1 == RHS->getStructDecl();
    }

    bool visitStructExtractInst(const StructExtractInst *RHS) {
      // We have already checked that the operands of our struct_extracts
      // match. Thus we need to check the field/struct decl which are not
      // operands.
      auto *X = cast<StructExtractInst>(LHS);
      if (X->getStructDecl() != RHS->getStructDecl())
        return false;
      if (X->getField() != RHS->getField())
        return false;
      return true;
    }

    bool visitRefElementAddrInst(RefElementAddrInst *RHS) {
      auto *X = cast<RefElementAddrInst>(LHS);
      if (X->getField() != RHS->getField())
        return false;
      return true;
    }

    bool visitRefTailAddrInst(RefTailAddrInst *RHS) {
      auto *X = cast<RefTailAddrInst>(LHS);
      return X->getTailType() == RHS->getTailType();
    }

    bool visitStructElementAddrInst(const StructElementAddrInst *RHS) {
      // We have already checked that the operands of our struct_element_addrs
      // match. Thus we only need to check the field/struct decl which are not
      // operands.
      auto *X = cast<StructElementAddrInst>(LHS);
      if (X->getStructDecl() != RHS->getStructDecl())
        return false;
      if (X->getField() != RHS->getField())
        return false;
      return true;
    }

    bool visitTupleInst(const TupleInst *RHS) {
      // We have already checked the operands. Make sure that the tuple types
      // match up.
      TupleType *TT1 = cast<TupleInst>(LHS)->getTupleType();
      return TT1 == RHS->getTupleType();
    }

    bool visitTupleExtractInst(const TupleExtractInst *RHS) {
      // We have already checked that the operands match. Thus we only need to
      // check the field no and tuple type which are not represented as operands.
      auto *X = cast<TupleExtractInst>(LHS);
      if (X->getTupleType() != RHS->getTupleType())
        return false;
      if (X->getFieldIndex() != RHS->getFieldIndex())
        return false;
      return true;
    }

    bool visitTupleElementAddrInst(const TupleElementAddrInst *RHS) {
      // We have already checked that the operands match. Thus we only need to
      // check the field no and tuple type which are not represented as operands.
      auto *X = cast<TupleElementAddrInst>(LHS);
      if (X->getTupleType() != RHS->getTupleType())
        return false;
      if (X->getFieldIndex() != RHS->getFieldIndex())
        return false;
      return true;
    }

    bool visitMetatypeInst(const MetatypeInst *RHS) {
      // We have already compared the operands/types, so we should have equality
      // at this point.
      return true;
    }

    bool visitValueMetatypeInst(const ValueMetatypeInst *RHS) {
      // We have already compared the operands/types, so we should have equality
      // at this point.
      return true;
    }

    bool visitExistentialMetatypeInst(const ExistentialMetatypeInst *RHS) {
      // We have already compared the operands/types, so we should have equality
      // at this point.
      return true;
    }

    bool visitIndexRawPointerInst(IndexRawPointerInst *RHS) {
      // We have already compared the operands/types, so we should have equality
      // at this point.
      return true;
    }

    bool visitIndexAddrInst(IndexAddrInst *RHS) {
      auto *lhs = cast<IndexAddrInst>(LHS);
      return lhs->needsStackProtection() == RHS->needsStackProtection();
    }

    bool visitTailAddrInst(TailAddrInst *RHS) {
      auto *X = cast<TailAddrInst>(LHS);
      return X->getTailType() == RHS->getTailType();
    }

    bool visitCondFailInst(CondFailInst *RHS) {
      // We have already compared the operands/types, so we should have equality
      // at this point.
      return true;
    }

    bool visitApplyInst(ApplyInst *RHS) {
      auto *X = cast<ApplyInst>(LHS);
      return X->getSubstitutionMap() == RHS->getSubstitutionMap();
    }

    bool visitBuiltinInst(BuiltinInst *RHS) {
      auto *X = cast<BuiltinInst>(LHS);
      if (X->getName() != RHS->getName())
        return false;
      return X->getSubstitutions() == RHS->getSubstitutions();
    }

    bool visitEnumInst(EnumInst *RHS) {
      // We already checked operands and types. Only thing we need to check is
      // that the element is the same.
      auto *X = cast<EnumInst>(LHS);
      return X->getElement() == RHS->getElement();
    }

    bool visitUncheckedEnumDataInst(UncheckedEnumDataInst *RHS) {
      // We already checked operands and types. Only thing we need to check is
      // that the element is the same.
      auto *X = cast<UncheckedEnumDataInst>(LHS);
      return X->getElement() == RHS->getElement();
    }

    bool visitSelectEnumOperation(SelectEnumOperation RHS) {
      // Check that the instructions match cases in the same order.
      auto X = SelectEnumOperation(LHS);

      if (X.getNumCases() != RHS.getNumCases())
        return false;
      if (X.hasDefault() != RHS.hasDefault())
        return false;

      for (unsigned i = 0, e = X.getNumCases(); i < e; ++i) {
        if (X.getCase(i).first != RHS.getCase(i).first)
          return false;
      }

      return true;
    }

    bool visitSelectEnumInst(const SelectEnumInst *RHS) {
      return visitSelectEnumOperation(RHS);
    }
    bool visitSelectEnumAddrInst(const SelectEnumAddrInst *RHS) {
      return visitSelectEnumOperation(RHS);
    }

    // Conversion instructions.
    // All of these just return true as they have already had their
    // operands and types checked
    bool visitUncheckedRefCastInst(UncheckedRefCastInst *RHS) {
      return true;
    }

    bool visitUncheckedAddrCastInst(UncheckedAddrCastInst *RHS) {
      return true;
    }

    bool visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *RHS) {
      return true;
    }

    bool visitUncheckedBitwiseCastInst(UncheckedBitwiseCastInst *RHS) {
      return true;
    }

    bool visitUpcastInst(UpcastInst *RHS) {
      return true;
    }

    bool visitAddressToPointerInst(AddressToPointerInst *RHS) {
      auto *lhs = cast<AddressToPointerInst>(LHS);
      return lhs->needsStackProtection() == RHS->needsStackProtection();
    }

    bool visitPointerToAddressInst(PointerToAddressInst *RHS) {
      return cast<PointerToAddressInst>(LHS)->isStrict() == RHS->isStrict();
    }

    bool visitRefToRawPointerInst(RefToRawPointerInst *RHS) {
      return true;
    }

    bool visitRawPointerToRefInst(RawPointerToRefInst *RHS) {
      return true;
    }

#define LOADABLE_REF_STORAGE_HELPER(Name)                                      \
  bool visit##Name##ToRefInst(Name##ToRefInst *RHS) { return true; }           \
  bool visitRefTo##Name##Inst(RefTo##Name##Inst *RHS) { return true; }         \
  bool visitStrongCopy##Name##ValueInst(StrongCopy##Name##ValueInst *RHS) {    \
    return true;                                                               \
  }
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
    LOADABLE_REF_STORAGE_HELPER(Name) \
    bool visitStrongRetain##Name##Inst(const StrongRetain##Name##Inst *RHS) { \
      return true; \
    }
#define UNCHECKED_REF_STORAGE(Name, ...) \
    LOADABLE_REF_STORAGE_HELPER(Name)
#include "swift/AST/ReferenceStorage.def"
#undef LOADABLE_REF_STORAGE_HELPER

    bool visitThinToThickFunctionInst(ThinToThickFunctionInst *RHS) {
      return true;
    }

    bool visitThickToObjCMetatypeInst(ThickToObjCMetatypeInst *RHS) {
      return true;
    }

    bool visitObjCToThickMetatypeInst(ObjCToThickMetatypeInst *RHS) {
      return true;
    }

    bool visitConvertFunctionInst(ConvertFunctionInst *RHS) {
      return true;
    }

    bool visitConvertEscapeToNoEscapeInst(ConvertEscapeToNoEscapeInst *RHS) {
      return true;
    }

    bool visitObjCMetatypeToObjectInst(ObjCMetatypeToObjectInst *RHS) {
      return true;
    }

    bool visitObjCExistentialMetatypeToObjectInst(ObjCExistentialMetatypeToObjectInst *RHS) {
      return true;
    }

    bool visitProjectBlockStorageInst(ProjectBlockStorageInst *RHS) {
      return true;
    }

    bool visitBridgeObjectToRefInst(BridgeObjectToRefInst *X) {
      return true;
    }

    bool visitValueToBridgeObjectInst(ValueToBridgeObjectInst *i) {
      return true;
    }

    bool visitBridgeObjectToWordInst(BridgeObjectToWordInst *X) {
      return true;
    }
      
    bool visitRefToBridgeObjectInst(RefToBridgeObjectInst *X) {
      return true;
    }
    bool visitClassifyBridgeObjectInst(ClassifyBridgeObjectInst *X) {
      return true;
    }

    bool visitObjCProtocolInst(ObjCProtocolInst *RHS) {
      auto *X = cast<ObjCProtocolInst>(LHS);
      return X->getProtocol() == RHS->getProtocol();
    }

    bool visitClassMethodInst(ClassMethodInst *RHS) {
      auto *X = cast<ClassMethodInst>(LHS);
      return X->getMember()  == RHS->getMember() &&
             X->getOperand() == RHS->getOperand() &&
             X->getType()    == RHS->getType();
    }

    bool visitSuperMethodInst(SuperMethodInst *RHS) {
      auto *X = cast<SuperMethodInst>(LHS);
      return X->getMember()  == RHS->getMember() &&
             X->getOperand() == RHS->getOperand() &&
             X->getType()    == RHS->getType();
    }

    bool visitObjCMethodInst(ObjCMethodInst *RHS) {
      auto *X = cast<ObjCMethodInst>(LHS);
      return X->getMember()  == RHS->getMember() &&
             X->getOperand() == RHS->getOperand() &&
             X->getType()    == RHS->getType();
    }

    bool visitObjCSuperMethodInst(ObjCSuperMethodInst *RHS) {
      auto *X = cast<ObjCSuperMethodInst>(LHS);
      return X->getMember()  == RHS->getMember() &&
             X->getOperand() == RHS->getOperand() &&
             X->getType()    == RHS->getType();
    }

    bool visitWitnessMethodInst(const WitnessMethodInst *RHS) {
      auto *X = cast<WitnessMethodInst>(LHS);
      if (X->getMember() != RHS->getMember())
        return false;
      if (X->getLookupType() != RHS->getLookupType())
        return false;
      if (X->getConformance() != RHS->getConformance())
        return false;
      return true;
    }

    bool visitMarkDependenceInst(const MarkDependenceInst *RHS) {
       return true;
    }

    bool visitOpenExistentialRefInst(const OpenExistentialRefInst *RHS) {
      return true;
    }

    bool
    visitInitExistentialMetatypeInst(const InitExistentialMetatypeInst *RHS) {
      auto *X = cast<InitExistentialMetatypeInst>(LHS);
      ArrayRef<ProtocolConformanceRef> lhsConformances = X->getConformances();
      ArrayRef<ProtocolConformanceRef> rhsConformances = RHS->getConformances();
      if (lhsConformances.size() != rhsConformances.size())
        return false;

      for (unsigned i : indices(lhsConformances)) {
        if (lhsConformances[i] != rhsConformances[i])
          return false;
      }
      return true;
    }

    bool visitScalarPackIndexInst(const ScalarPackIndexInst *RHS) {
      auto *X = cast<ScalarPackIndexInst>(LHS);
      return (X->getIndexedPackType() == RHS->getIndexedPackType() &&
              X->getComponentIndex() == RHS->getComponentIndex());
    }

    bool visitDynamicPackIndexInst(const DynamicPackIndexInst *RHS) {
      auto *X = cast<DynamicPackIndexInst>(LHS);
      return X->getIndexedPackType() == RHS->getIndexedPackType();
    }

    bool visitTuplePackElementAddrInst(const TuplePackElementAddrInst *RHS) {
      auto *X = cast<TuplePackElementAddrInst>(LHS);
      return X->getElementType() == RHS->getElementType();
    }

  private:
    const SILInstruction *LHS;
  };
} // end anonymous namespace

bool SILInstruction::hasIdenticalState(const SILInstruction *RHS) const {
  SILInstruction *UnconstRHS = const_cast<SILInstruction *>(RHS);
  return InstructionIdentityComparer(this).visit(UnconstRHS);
}

namespace {
  class AllOperandsAccessor : public SILInstructionVisitor<AllOperandsAccessor,
                                                           ArrayRef<Operand> > {
  public:
#define INST(CLASS, PARENT)                                                    \
  ArrayRef<Operand> visit##CLASS(const CLASS *I) {                             \
    ASSERT_IMPLEMENTS(CLASS, SILInstruction, getAllOperands,                   \
                      ArrayRef<Operand>() const);                              \
    return I->getAllOperands();                                                \
  }
#include "swift/SIL/SILNodes.def"
  };

  class AllOperandsMutableAccessor
    : public SILInstructionVisitor<AllOperandsMutableAccessor,
                                   MutableArrayRef<Operand> > {
  public:
#define INST(CLASS, PARENT)                                                    \
  MutableArrayRef<Operand> visit##CLASS(CLASS *I) {                            \
    ASSERT_IMPLEMENTS(CLASS, SILInstruction, getAllOperands,                   \
                      MutableArrayRef<Operand>());                             \
    return I->getAllOperands();                                                \
  }
#include "swift/SIL/SILNodes.def"
  };

#define IMPLEMENTS_METHOD(DerivedClass, BaseClass, MemberName, ExpectedType)  \
  (!::std::is_same<BaseClass, GET_IMPLEMENTING_CLASS(DerivedClass, MemberName,\
                                                     ExpectedType)>::value)

  class TypeDependentOperandsAccessor
      : public SILInstructionVisitor<TypeDependentOperandsAccessor,
                                     ArrayRef<Operand>> {
  public:
#define INST(CLASS, PARENT)                                                    \
  ArrayRef<Operand> visit##CLASS(const CLASS *I) {                             \
    if (!IMPLEMENTS_METHOD(CLASS, SILInstruction, getTypeDependentOperands,    \
                           ArrayRef<Operand>() const))                         \
      return {};                                                               \
    return I->getTypeDependentOperands();                                      \
  }
#include "swift/SIL/SILNodes.def"
  };

  class TypeDependentOperandsMutableAccessor
    : public SILInstructionVisitor<TypeDependentOperandsMutableAccessor,
                                   MutableArrayRef<Operand> > {
  public:
#define INST(CLASS, PARENT)                                                    \
  MutableArrayRef<Operand> visit##CLASS(CLASS *I) {                            \
    if (!IMPLEMENTS_METHOD(CLASS, SILInstruction, getTypeDependentOperands,    \
                           MutableArrayRef<Operand>()))                        \
      return {};                                                               \
    return I->getTypeDependentOperands();                                      \
  }
#include "swift/SIL/SILNodes.def"
  };
} // end anonymous namespace

ArrayRef<Operand> SILInstruction::getAllOperands() const {
  return AllOperandsAccessor().visit(const_cast<SILInstruction *>(this));
}

MutableArrayRef<Operand> SILInstruction::getAllOperands() {
  return AllOperandsMutableAccessor().visit(this);
}

ArrayRef<Operand> SILInstruction::getTypeDependentOperands() const {
  return TypeDependentOperandsAccessor().visit(
      const_cast<SILInstruction *>(this));
}

MutableArrayRef<Operand> SILInstruction::getTypeDependentOperands() {
  return TypeDependentOperandsMutableAccessor().visit(this);
}

/// getOperandNumber - Return which operand this is in the operand list of the
/// using instruction.
unsigned Operand::getOperandNumber() const {
  return this - &cast<SILInstruction>(getUser())->getAllOperands()[0];
}

MemoryBehavior SILInstruction::getMemoryBehavior() const {

  if (auto *BI = dyn_cast<BuiltinInst>(this)) {
    // Handle Swift builtin functions.
    const BuiltinInfo &BInfo = BI->getBuiltinInfo();
    if (BInfo.ID == BuiltinValueKind::ZeroInitializer) {
      // The address form of `zeroInitializer` writes to its argument to
      // initialize it. The value form has no side effects.
      return BI->getArguments().size() > 0
        ? MemoryBehavior::MayWrite
        : MemoryBehavior::None;
    }
    if (BInfo.ID != BuiltinValueKind::None)
      return BInfo.isReadNone() ? MemoryBehavior::None
                                : MemoryBehavior::MayHaveSideEffects;

    // Handle LLVM intrinsic functions.
    const IntrinsicInfo &IInfo = BI->getIntrinsicInfo();
    if (IInfo.ID != llvm::Intrinsic::not_intrinsic) {
      auto IAttrs = IInfo.getOrCreateAttributes(getModule().getASTContext());
      // Read-only.
      if (IAttrs.hasFnAttr(llvm::Attribute::ReadOnly) &&
          IAttrs.hasFnAttr(llvm::Attribute::NoUnwind))
        return MemoryBehavior::MayRead;
      // Read-none?
      return IAttrs.hasFnAttr(llvm::Attribute::ReadNone) &&
                     IAttrs.hasFnAttr(llvm::Attribute::NoUnwind)
                 ? MemoryBehavior::None
                 : MemoryBehavior::MayHaveSideEffects;
    }
  }

  // Handle full apply sites that have a resolvable callee function with an
  // effects attribute.
  if (isa<FullApplySite>(this)) {
    FullApplySite Site(const_cast<SILInstruction *>(this));
    if (auto *F = Site.getCalleeFunction()) {
      return F->getEffectsKind() == EffectsKind::ReadNone
                 ? MemoryBehavior::None
                 : MemoryBehavior::MayHaveSideEffects;
    }
  }

  if (auto *ga = dyn_cast<GlobalAddrInst>(this)) {
    // Global variables with resilient types might be allocated into a buffer
    // and not statically in the data segment.
    // In this case, the global_addr depends on alloc_global being executed
    // first. We model this by letting global_addr have a side effect.
    // It prevents e.g. LICM to move a global_addr out of a loop while keeping
    // the alloc_global inside the loop.
    SILModule &M = ga->getFunction()->getModule();
    auto expansion = TypeExpansionContext::maximal(M.getAssociatedContext(),
                                                   M.isWholeModule());
    const TypeLowering &tl =
      M.Types.getTypeLowering(ga->getType().getObjectType(), expansion);
    return tl.isFixedABI() ? MemoryBehavior::None :
                             MemoryBehavior::MayHaveSideEffects;
  }

  if (auto *li = dyn_cast<LoadInst>(this)) {
    switch (li->getOwnershipQualifier()) {
    case LoadOwnershipQualifier::Unqualified:
    case LoadOwnershipQualifier::Trivial:
      return MemoryBehavior::MayRead;
    case LoadOwnershipQualifier::Take:
      // Take deinitializes the underlying memory. Until we separate notions of
      // memory writing from deinitialization (since a take doesn't actually
      // write to the memory), lets be conservative and treat it as may read
      // write.
      return MemoryBehavior::MayReadWrite;
    case LoadOwnershipQualifier::Copy:
      return MemoryBehavior::MayHaveSideEffects;
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  if (auto *si = dyn_cast<StoreInst>(this)) {
    switch (si->getOwnershipQualifier()) {
    case StoreOwnershipQualifier::Unqualified:
    case StoreOwnershipQualifier::Trivial:
    case StoreOwnershipQualifier::Init:
      return MemoryBehavior::MayWrite;
    case StoreOwnershipQualifier::Assign:
      // For the release.
      return MemoryBehavior::MayHaveSideEffects;
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  switch (getKind()) {
#define FULL_INST(CLASS, TEXTUALNAME, PARENT, MEMBEHAVIOR, RELEASINGBEHAVIOR)  \
  case SILInstructionKind::CLASS:                                              \
    return MemoryBehavior::MEMBEHAVIOR;
#include "swift/SIL/SILNodes.def"
  }
  llvm_unreachable("We've just exhausted the switch.");
}

SILInstruction::ReleasingBehavior SILInstruction::getReleasingBehavior() const {
  switch (getKind()) {
#define FULL_INST(CLASS, TEXTUALNAME, PARENT, MEMBEHAVIOR, RELEASINGBEHAVIOR)  \
  case SILInstructionKind::CLASS:                                              \
    return ReleasingBehavior::RELEASINGBEHAVIOR;
#include "swift/SIL/SILNodes.def"
  }
  llvm_unreachable("We've just exhausted the switch.");
}

bool SILInstruction::mayHaveSideEffects() const {
  // If this instruction traps then it must have side effects.
  if (mayTrap())
    return true;

  return mayWriteToMemory();
}

bool SILInstruction::mayRelease() const {
  // Overrule a "DoesNotRelease" of dynamic casts. If a dynamic cast is not
  // RC identity preserving it can release it's source (in some cases - we are
  // conservative here).
  auto dynCast = SILDynamicCastInst::getAs(const_cast<SILInstruction *>(this));
  if (dynCast && !dynCast.isRCIdentityPreserving())
    return true;

  if (getReleasingBehavior() ==
      SILInstruction::ReleasingBehavior::DoesNotRelease)
    return false;

  switch (getKind()) {
  default:
    llvm_unreachable("Unhandled releasing instruction!");

  case SILInstructionKind::EndLifetimeInst:
  case SILInstructionKind::GetAsyncContinuationInst:
  case SILInstructionKind::GetAsyncContinuationAddrInst:
  case SILInstructionKind::AwaitAsyncContinuationInst:
    return false;

  case SILInstructionKind::ApplyInst:
  case SILInstructionKind::TryApplyInst:
  case SILInstructionKind::BeginApplyInst:
  case SILInstructionKind::AbortApplyInst:
  case SILInstructionKind::EndApplyInst:
  case SILInstructionKind::YieldInst:
  case SILInstructionKind::DestroyAddrInst:
  case SILInstructionKind::StrongReleaseInst:
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  case SILInstructionKind::Name##ReleaseValueInst:
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case SILInstructionKind::Name##ReleaseInst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::ReleaseValueInst:
  case SILInstructionKind::ReleaseValueAddrInst:
    return true;

  case SILInstructionKind::DestroyValueInst:
  case SILInstructionKind::HopToExecutorInst:
    return true;

  case SILInstructionKind::UnconditionalCheckedCastAddrInst:
  case SILInstructionKind::UncheckedOwnershipConversionInst:
    return true;

  case SILInstructionKind::CheckedCastAddrBranchInst: {
    // Failing casts with take_always can release.
    auto *Cast = cast<CheckedCastAddrBranchInst>(this);
    return Cast->getConsumptionKind() == CastConsumptionKind::TakeAlways;
  }

  case SILInstructionKind::CopyAddrInst: {
    auto *CopyAddr = cast<CopyAddrInst>(this);
    // copy_addr without initialization can cause a release.
    return CopyAddr->isInitializationOfDest() ==
           IsInitialization_t::IsNotInitialization;
  }
  // mark_unresolved_move_addr is equivalent to a copy_addr [init], so a release
  // does not occur.
  case SILInstructionKind::MarkUnresolvedMoveAddrInst:
    return false;

  case SILInstructionKind::BuiltinInst: {
    auto *BI = cast<BuiltinInst>(this);
    // Builtins without side effects also do not release.
    if (!BI->mayHaveSideEffects())
      return false;
    // If this is a builtin which might have side effect, but its side
    // effects do not cause reference counts to be decremented, return false.
    if (auto Kind = BI->getBuiltinKind()) {
      switch (Kind.value()) {
        case BuiltinValueKind::CopyArray:
          return false;
        default:
          break;
      }
    }
    if (auto ID = BI->getIntrinsicID()) {
      switch (ID.value()) {
        case llvm::Intrinsic::memcpy:
        case llvm::Intrinsic::memmove:
        case llvm::Intrinsic::memset:
          return false;
        default:
          break;
      }
    }
    return true;
  }
  case SILInstructionKind::StoreInst:
    switch (cast<StoreInst>(this)->getOwnershipQualifier()) {
    case StoreOwnershipQualifier::Unqualified:
    case StoreOwnershipQualifier::Init:
    case StoreOwnershipQualifier::Trivial:
      return false;
    case StoreOwnershipQualifier::Assign:
      // Assign destroys the old value that was in the memory location before we
      // write the new value into the location.
      return true;
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }
}

bool SILInstruction::mayReleaseOrReadRefCount() const {
  switch (getKind()) {
  case SILInstructionKind::IsUniqueInst:
  case SILInstructionKind::BeginCOWMutationInst:
  case SILInstructionKind::IsEscapingClosureInst:
    return true;
  default:
    return mayRelease();
  }
}

namespace {
  class TrivialCloner : public SILCloner<TrivialCloner> {
    friend class SILCloner<TrivialCloner>;
    friend class SILInstructionVisitor<TrivialCloner>;
    SILInstruction *Result = nullptr;
    TrivialCloner(SILFunction *F, SILInstruction *InsertPt) : SILCloner(*F) {
      Builder.setInsertionPoint(InsertPt);
    }

  public:

    static SILInstruction *doIt(SILInstruction *I, SILInstruction *InsertPt) {
      TrivialCloner TC(I->getFunction(), InsertPt);
      TC.visit(I);
      return TC.Result;
    }

    void postProcess(SILInstruction *Orig, SILInstruction *Cloned) {
      assert(Orig->getFunction() == &getBuilder().getFunction() &&
             "cloning between functions is not supported");

      Result = Cloned;
      SILCloner<TrivialCloner>::postProcess(Orig, Cloned);
    }
    SILValue getMappedValue(SILValue Value) {
      return Value;
    }
    SILBasicBlock *remapBasicBlock(SILBasicBlock *BB) { return BB; }
  };
} // end anonymous namespace

bool SILInstruction::isAllocatingStack() const {
  if (isa<AllocStackInst>(this) ||
      isa<AllocPackInst>(this) ||
      isa<AllocPackMetadataInst>(this))
    return true;

  if (auto *ARI = dyn_cast<AllocRefInstBase>(this)) {
    if (ARI->canAllocOnStack())
      return true;
  }

  // In OSSA, PartialApply is modeled as a value which borrows its operands
  // and whose lifetime is ended by a `destroy_value`.
  //
  // After OSSA, we make the memory allocation and dependencies explicit again,
  // with a `dealloc_stack` ending the closure's lifetime.
  if (auto *PA = dyn_cast<PartialApplyInst>(this)) {
    return PA->isOnStack()
      && !PA->getFunction()->hasOwnership();
  }

  if (auto *BI = dyn_cast<BuiltinInst>(this)) {
    if (BI->getBuiltinKind() == BuiltinValueKind::StackAlloc ||
        BI->getBuiltinKind() == BuiltinValueKind::UnprotectedStackAlloc) {
      return true;
    }
  }

  return false;
}

bool SILInstruction::isDeallocatingStack() const {
  if (isa<DeallocStackInst>(this) ||
      isa<DeallocStackRefInst>(this) ||
      isa<DeallocPackInst>(this) ||
      isa<DeallocPackMetadataInst>(this))
    return true;

  if (auto *BI = dyn_cast<BuiltinInst>(this)) {
    if (BI->getBuiltinKind() == BuiltinValueKind::StackDealloc) {
      return true;
    }
  }

  return false;
}

bool SILInstruction::mayRequirePackMetadata() const {
  switch (getKind()) {
  case SILInstructionKind::AllocPackInst:
  case SILInstructionKind::TuplePackElementAddrInst:
  case SILInstructionKind::OpenPackElementInst:
    return true;
  case SILInstructionKind::PartialApplyInst:
  case SILInstructionKind::ApplyInst:
  case SILInstructionKind::BeginApplyInst:
  case SILInstructionKind::TryApplyInst: {
    // Check the function type for packs.
    auto apply = ApplySite::isa(const_cast<SILInstruction *>(this));
    if (apply.getCallee()->getType().hasAnyPack())
      return true;
    // Check the substituted types for packs.
    for (auto ty : apply.getSubstitutionMap().getReplacementTypes()) {
      if (ty->hasAnyPack())
        return true;
    }
    return false;
  }
  case SILInstructionKind::ClassMethodInst:
  case SILInstructionKind::DebugValueInst: 
  case SILInstructionKind::DestroyAddrInst:
  case SILInstructionKind::DestroyValueInst:
  // Unary instructions.
  {
    return getOperand(0)->getType().hasAnyPack();
  }
  case SILInstructionKind::AllocStackInst: {
    auto *asi = cast<AllocStackInst>(this);
    return asi->getType().hasAnyPack();
  }
  case SILInstructionKind::MetatypeInst: {
    auto *mi = cast<MetatypeInst>(this);
    return mi->getType().hasAnyPack();
  }
  case SILInstructionKind::WitnessMethodInst: {
    auto *wmi = cast<WitnessMethodInst>(this);
    auto ty = wmi->getLookupType();
    return ty->hasAnyPack();
  }
  default:
    // Instructions that deallocate stack must not result in pack metadata
    // materialization.  If they did there would be no way to create the pack
    // metadata on stack.
    if (isDeallocatingStack())
      return false;

    // Terminators that exit the function must not result in pack metadata
    // materialization.
    auto *ti = dyn_cast<TermInst>(this);
    if (ti && ti->isFunctionExiting())
      return false;

    // Check results and operands for packs.  If a pack appears, lowering the
    // instruction might result in pack metadata emission.
    for (auto result : getResults()) {
      if (result->getType().hasAnyPack())
        return true;
    }
    for (auto operandTy : getOperandTypes()) {
      if (operandTy.hasAnyPack())
        return true;
    }
    for (auto &tdo : getTypeDependentOperands()) {
      if (tdo.get()->getType().hasAnyPack())
        return true;
    }

    return false;
  }
}

/// Create a new copy of this instruction, which retains all of the operands
/// and other information of this one.  If an insertion point is specified,
/// then the new instruction is inserted before the specified point, otherwise
/// the new instruction is returned without a parent.
SILInstruction *SILInstruction::clone(SILInstruction *InsertPt) {
  SILInstruction *NewInst = TrivialCloner::doIt(this, InsertPt);
  return NewInst;
}

/// Returns true if the instruction can be duplicated without any special
/// additional handling. It is important to know this information when
/// you perform such optimizations like e.g. jump-threading.
bool SILInstruction::isTriviallyDuplicatable() const {
  if (isAllocatingStack())
    return false;

  if (isa<OpenExistentialAddrInst>(this) || isa<OpenExistentialRefInst>(this) ||
      isa<OpenExistentialMetatypeInst>(this) ||
      isa<OpenExistentialValueInst>(this) || isa<OpenExistentialBoxInst>(this) ||
      isa<OpenExistentialBoxValueInst>(this)) {
    // Don't know how to duplicate these properly yet. Inst.clone() per
    // instruction does not work. Because the follow-up instructions need to
    // reuse the same archetype uuid which would only work if we used a
    // cloner.
    return false;
  }

  if (auto *MI = dyn_cast<MethodInst>(this)) {
    // We can't build SSA for method values that lower to objc methods.
    if (MI->getMember().isForeign)
      return false;
  }
  if (isa<ThrowInst>(this))
    return false;

  // BeginAccess defines the access scope entry point. All associated EndAccess
  // instructions must directly operate on the BeginAccess.
  if (isa<BeginAccessInst>(this))
    return false;

  // begin_apply creates a token that has to be directly used by the
  // corresponding end_apply and abort_apply.
  if (isa<BeginApplyInst>(this))
    return false;

  // dynamic_method_br is not duplicatable because IRGen does not support phi
  // nodes of objc_method type.
  if (isa<DynamicMethodBranchInst>(this))
    return false;

  // Can't duplicate get/await_async_continuation.
  if (isa<AwaitAsyncContinuationInst>(this) ||
      isa<GetAsyncContinuationAddrInst>(this) ||
      isa<GetAsyncContinuationInst>(this))
    return false;

  // If you add more cases here, you should also update SILLoop:canDuplicate.

  return true;
}

bool SILInstruction::mayTrap() const {
  switch(getKind()) {
  case SILInstructionKind::CondFailInst:
  case SILInstructionKind::UnconditionalCheckedCastInst:
  case SILInstructionKind::UnconditionalCheckedCastAddrInst:
    return true;
  default:
    return false;
  }
}

bool SILInstruction::isMetaInstruction() const {
  // Every instruction that implements getVarInfo() should be in this list.
  switch (getKind()) {
  case SILInstructionKind::AllocBoxInst:
  case SILInstructionKind::AllocStackInst:
  case SILInstructionKind::DebugValueInst:
    return true;
  default:
    return false;
  }
  llvm_unreachable("Instruction not handled in isMetaInstruction()!");
}

unsigned SILInstruction::getCachedFieldIndex(NominalTypeDecl *decl,
                                             VarDecl *property) {
  return getModule().getFieldIndex(decl, property);
}

unsigned SILInstruction::getCachedCaseIndex(EnumElementDecl *enumElement) {
  return getModule().getCaseIndex(enumElement);
}

//===----------------------------------------------------------------------===//
//                                 Utilities
//===----------------------------------------------------------------------===//

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &OS,
                                     MemoryBehavior B) {
  switch (B) {
    case MemoryBehavior::None:
      return OS << "None";
    case MemoryBehavior::MayRead:
      return OS << "MayRead";
    case MemoryBehavior::MayWrite:
      return OS << "MayWrite";
    case MemoryBehavior::MayReadWrite:
      return OS << "MayReadWrite";
    case MemoryBehavior::MayHaveSideEffects:
      return OS << "MayHaveSideEffects";
  }

  llvm_unreachable("Unhandled MemoryBehavior in switch.");
}

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &OS,
                                     SILInstruction::ReleasingBehavior B) {
  switch (B) {
  case SILInstruction::ReleasingBehavior::DoesNotRelease:
    return OS << "DoesNotRelease";
  case SILInstruction::ReleasingBehavior::MayRelease:
    return OS << "MayRelease";
  }

  llvm_unreachable("Unhandled ReleasingBehavior in switch.");
}

//===----------------------------------------------------------------------===//
//                         SILInstructionResultArray
//===----------------------------------------------------------------------===//

SILInstructionResultArray::SILInstructionResultArray(
    const SingleValueInstruction *SVI)
    : Pointer(), Size(1) {
  // Make sure that even though we are munging things, we are able to get back
  // the original value, types, and operands.
  SILValue originalValue(SVI);
  SILType originalType = SVI->getType();
  (void)originalValue;
  (void)originalType;

  // *PLEASE READ BEFORE CHANGING*
  //
  // Since SingleValueInstruction is both a ValueBase and a SILInstruction, but
  // SILInstruction is the first parent, we need to ensure that our ValueBase *
  // pointer is properly offset. by first static casting to ValueBase and then
  // going back to a uint8_t *.
  auto *Value = static_cast<const ValueBase *>(SVI);
  assert(uintptr_t(Value) != uintptr_t(SVI) &&
         "Expected value to be offset from SVI since it is not the first "
         "multi-inheritance parent");
  Pointer = reinterpret_cast<const uint8_t *>(Value);

#ifndef NDEBUG
  assert(originalValue == (*this)[0] &&
         "Wrong value returned for single result");
  assert(originalType == (*this)[0]->getType());

  auto ValueRange = getValues();
  assert(1 == std::distance(ValueRange.begin(), ValueRange.end()));
  assert(originalValue == *ValueRange.begin());

  auto TypedRange = getTypes();
  assert(1 == std::distance(TypedRange.begin(), TypedRange.end()));
  assert(originalType == *TypedRange.begin());

  SILInstructionResultArray Copy = *this;
  assert(Copy.hasSameTypes(*this));
  assert(Copy == *this);
#endif
}

SILInstructionResultArray::SILInstructionResultArray(
    ArrayRef<MultipleValueInstructionResult> MVResults)
    : Pointer(nullptr), Size(MVResults.size()) {
  // We are assuming here that MultipleValueInstructionResult when static_cast
  // is not offset.
  if (Size)
    Pointer = reinterpret_cast<const uint8_t *>(&MVResults[0]);

#ifndef NDEBUG
  // Verify our invariants.
  assert(size() == MVResults.size());
  auto ValueRange = getValues();
  auto VRangeBegin = ValueRange.begin();
  auto VRangeIter = VRangeBegin;
  auto VRangeEnd = ValueRange.end();
  assert(MVResults.size() == unsigned(std::distance(VRangeBegin, VRangeEnd)));

  auto TypedRange = getTypes();
  auto TRangeBegin = TypedRange.begin();
  auto TRangeIter = TRangeBegin;
  auto TRangeEnd = TypedRange.end();
  assert(MVResults.size() == unsigned(std::distance(TRangeBegin, TRangeEnd)));
  for (unsigned i : indices(MVResults)) {
    assert(SILValue(&MVResults[i]) == (*this)[i]);
    assert(SILValue(&MVResults[i])->getType() == (*this)[i]->getType());
    assert(SILValue(&MVResults[i]) == (*VRangeIter));
    assert(SILValue(&MVResults[i])->getType() == (*VRangeIter)->getType());
    assert(SILValue(&MVResults[i])->getType() == *TRangeIter);
    ++VRangeIter;
    ++TRangeIter;
  }

  SILInstructionResultArray Copy = *this;
  assert(Copy.hasSameTypes(*this));
  assert(Copy == *this);
#endif
}

SILValue SILInstructionResultArray::operator[](size_t Index) const {
  assert(Index < Size && "Index out of bounds");
  // *NOTE* In the case where we have a single instruction, Index will always
  // necessarily be 0 implying that it is safe for us to just multiple Index by
  // sizeof(MultipleValueInstructionResult).
  size_t Offset = sizeof(MultipleValueInstructionResult) * Index;
  return SILValue(reinterpret_cast<const ValueBase *>(&Pointer[Offset]));
}

bool SILInstructionResultArray::hasSameTypes(
    const SILInstructionResultArray &rhs) {
  auto &lhs = *this;
  if (lhs.size() != rhs.size())
    return false;
  for (unsigned i : indices(lhs)) {
    if (lhs[i]->getType() != rhs[i]->getType())
      return false;
  }
  return true;
}

bool SILInstructionResultArray::
operator==(const SILInstructionResultArray &other) {
  if (size() != other.size())
    return false;
  for (auto i : indices(*this))
    if ((*this)[i] != other[i])
      return false;
  return true;
}

SILInstructionResultArray::type_range
SILInstructionResultArray::getTypes() const {
  SILType (*F)(SILValue) = [](SILValue V) -> SILType {
    return V->getType();
  };
  return {llvm::map_iterator(begin(), F), llvm::map_iterator(end(), F)};
}

const ValueBase *SILInstructionResultArray::front() const {
  assert(size() && "Can not access front of an empty result array");
  return *begin();
}

const ValueBase *SILInstructionResultArray::back() const {
  assert(size() && "Can not access back of an empty result array");
  if (std::next(begin()) == end()) {
    return *begin();
  }
  return *std::prev(end());
}

//===----------------------------------------------------------------------===//
//                          Defined opened archetypes
//===----------------------------------------------------------------------===//

bool SILInstruction::definesLocalArchetypes() const {
  bool definesAny = false;
  forEachDefinedLocalArchetype([&](CanLocalArchetypeType type,
                                   SILValue dependency) {
    definesAny = true;
  });
  return definesAny;
}

void SILInstruction::forEachDefinedLocalArchetype(
      llvm::function_ref<void(CanLocalArchetypeType, SILValue)> fn) const {
  switch (getKind()) {
#define SINGLE_VALUE_SINGLE_OPEN(TYPE)                                    \
  case SILInstructionKind::TYPE: {                                        \
    auto I = cast<TYPE>(this);                                            \
    auto archetype = I->getDefinedOpenedArchetype();                      \
    assert(archetype);                                                    \
    return fn(archetype, I);                                              \
  }
  SINGLE_VALUE_SINGLE_OPEN(OpenExistentialAddrInst)
  SINGLE_VALUE_SINGLE_OPEN(OpenExistentialRefInst)
  SINGLE_VALUE_SINGLE_OPEN(OpenExistentialBoxInst)
  SINGLE_VALUE_SINGLE_OPEN(OpenExistentialBoxValueInst)
  SINGLE_VALUE_SINGLE_OPEN(OpenExistentialMetatypeInst)
  SINGLE_VALUE_SINGLE_OPEN(OpenExistentialValueInst)
#undef SINGLE_VALUE_SINGLE_OPEN
  case SILInstructionKind::OpenPackElementInst:
    return cast<OpenPackElementInst>(this)->forEachDefinedLocalArchetype(fn);
  default:
    return;
  }
}

void OpenPackElementInst::forEachDefinedLocalArchetype(
      llvm::function_ref<void(CanLocalArchetypeType, SILValue)> fn) const {
  getOpenedGenericEnvironment()->forEachPackElementBinding(
                                  [&](ElementArchetypeType *elementType,
                                      PackType *packSubstitution) {
    fn(CanElementArchetypeType(elementType), this);
  });
}

//===----------------------------------------------------------------------===//
//                         Multiple Value Instruction
//===----------------------------------------------------------------------===//

llvm::Optional<unsigned>
MultipleValueInstruction::getIndexOfResult(SILValue Target) const {
  // First make sure we actually have one of our instruction results.
  auto *MVIR = dyn_cast<MultipleValueInstructionResult>(Target);
  if (!MVIR || MVIR->getParent() != this)
    return llvm::None;
  return MVIR->getIndex();
}

MultipleValueInstructionResult::MultipleValueInstructionResult(
    unsigned index, SILType type, ValueOwnershipKind ownershipKind)
    : ValueBase(ValueKind::MultipleValueInstructionResult, type) {
  setOwnershipKind(ownershipKind);
  setIndex(index);
}

void MultipleValueInstructionResult::setOwnershipKind(
    ValueOwnershipKind NewKind) {
  sharedUInt8().MultipleValueInstructionResult.valueOwnershipKind = uint8_t(NewKind);
}

void MultipleValueInstructionResult::setIndex(unsigned NewIndex) {
  // We currently use 32 bits to store the Index. A previous comment wrote
  // that "500k fields is probably enough".
  sharedUInt32().MultipleValueInstructionResult.index = NewIndex;
}

ValueOwnershipKind MultipleValueInstructionResult::getOwnershipKind() const {
  return ValueOwnershipKind(sharedUInt8().MultipleValueInstructionResult.valueOwnershipKind);
}

MultipleValueInstruction *MultipleValueInstructionResult::getParentImpl() const {
  char *Ptr = reinterpret_cast<char *>(
      const_cast<MultipleValueInstructionResult *>(this));

  // We know that we are in a trailing objects array with an extra prefix
  // element that contains the pointer to our parent SILNode. So grab the
  // address of the beginning of the array.
  Ptr -= getIndex() * sizeof(MultipleValueInstructionResult);

  // We may have some bytes of padding depending on our platform. Move past
  // those bytes if we need to.
  static_assert(alignof(MultipleValueInstructionResult) >=
                    alignof(MultipleValueInstruction *),
                "We assume this relationship in between the alignments");
  Ptr -= alignof(MultipleValueInstructionResult) -
         alignof(MultipleValueInstruction *);

  // Then subtract the size of MultipleValueInstruction.
  Ptr -= sizeof(MultipleValueInstruction *);

  // Now that we have the correct address of our parent instruction, grab it and
  // return it avoiding type punning.
  uintptr_t value;
  memcpy(&value, Ptr, sizeof(value));
  return reinterpret_cast<MultipleValueInstruction *>(value);
}

/// Returns true if evaluation of this node may cause suspension of an
/// async task.
bool SILInstruction::maySuspend() const {
  // await_async_continuation always suspends the current task.
  if (isa<AwaitAsyncContinuationInst>(this))
    return true;

  // hop_to_executor also may cause a suspension
  if (isa<HopToExecutorInst>(this))
    return true;
  
  // Fully applying an async function may suspend the caller.
  if (auto applySite = FullApplySite::isa(const_cast<SILInstruction*>(this))) {
    return applySite.getOrigCalleeType()->isAsync();
  }
  
  return false;
}

static bool visitRecursivelyLifetimeEndingUses(
  SILValue i,
  bool &noUsers,
  llvm::function_ref<bool(Operand *)> func)
{
  for (Operand *use : i->getConsumingUses()) {
    noUsers = false;
    if (isa<DestroyValueInst>(use->getUser())) {
      if (!func(use)) {
        return false;
      }
      continue;
    }
    
    // There shouldn't be any dead-end consumptions of a nonescaping
    // partial_apply that don't forward it along, aside from destroy_value.
    assert(use->getUser()->hasResults()
           && use->getUser()->getNumResults() == 1);
    if (!visitRecursivelyLifetimeEndingUses(use->getUser()->getResult(0),
                                            noUsers, func)) {
      return false;
    }
  }
  return true;
}

bool
PartialApplyInst::visitOnStackLifetimeEnds(
                             llvm::function_ref<bool (Operand *)> func) const {
  assert(getFunction()->hasOwnership()
         && isOnStack()
         && "only meaningful for OSSA stack closures");
  bool noUsers = true;

  if (!visitRecursivelyLifetimeEndingUses(this, noUsers, func)) {
    return false;
  }
  return !noUsers;
}

PartialApplyInst *
DestroyValueInst::getNonescapingClosureAllocation() const {
  SILValue operand = getOperand();
  auto operandFnTy = operand->getType().getAs<SILFunctionType>();
  // The query doesn't make sense if we aren't operating on a noescape closure
  // to begin with.
  if (!operandFnTy || !operandFnTy->isTrivialNoEscape()) {
    return nullptr;
  }

  // Look through marker and conversion instructions that would forward
  // ownership of the original partial application.
  while (true) {
    if (auto mdi = dyn_cast<MarkDependenceInst>(operand)) {
      operand = mdi->getValue();
      continue;
    } else if (isa<ConvertEscapeToNoEscapeInst>(operand)
               || isa<ThinToThickFunctionInst>(operand)) {
      // Stop at a conversion from escaping closure, since there's no stack
      // allocation in that case.
      return nullptr;
    } else if (auto convert = ConversionOperation(operand)) {
      operand = convert.getConverted();
      continue;
    } else if (auto pa = dyn_cast<PartialApplyInst>(operand)) {
      // If we found the `[on_stack]` partial apply, we're done.
      if (pa->isOnStack()) {
        return pa;
      }
      // Any other kind of partial apply fails to pass muster.
      return nullptr;
    } else {
      // The original partial_apply instruction should only be forwarded
      // through one of the above instructions. Anything else should lead us
      // to a copy or borrow of the closure from somewhere else.
      assert((isa<CopyValueInst>(operand)
              || isa<SILArgument>(operand)
              || isa<DifferentiableFunctionInst>(operand)
              || isa<DifferentiableFunctionExtractInst>(operand)
              || isa<LoadInst>(operand)
              || (operand->dump(), false))
             && "unexpected forwarding instruction for noescape closure");
      return nullptr;
    }
  }
}

#ifndef NDEBUG

//---
// Static verification of multiple value properties.
//

// Make sure that all subclasses of MultipleValueInstruction implement
// getAllResults()
#define MULTIPLE_VALUE_INST(ID, TEXTUALNAME, PARENT, MEMBEHAVIOR, MAYRELEASE)  \
  static_assert(IMPLEMENTS_METHOD(ID, PARENT, getAllResults,                   \
                                  SILInstructionResultArray() const),          \
                #ID " does not implement SILInstructionResultArray "           \
                    "getAllResults() const?!");

// Check that all subclasses of MultipleValueInstructionResult are the same size
// as MultipleValueInstructionResult.
//
// If this changes, we just need to expand the size of SILInstructionResultArray
// to contain a stride. But we assume this now so we should enforce it.
#define MULTIPLE_VALUE_INST_RESULT(ID, PARENT)                                 \
  static_assert(                                                               \
      sizeof(ID) == sizeof(PARENT) && alignof(ID) == alignof(PARENT),          \
      "Expected all multiple value inst result to be the same size?!");
#include "swift/SIL/SILNodes.def"

#endif
