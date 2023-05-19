//===--- CSE.cpp - Simple and fast CSE pass -------------------------------===//
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
// This pass performs a simple dominator tree walk that eliminates trivially
// redundant instructions.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-cse"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/NodeBits.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/ArraySemantic.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/SimplifyInstruction.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/BasicBlockOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/OwnershipOptUtils.h"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/StackNesting.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopedHashTable.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/RecyclingAllocator.h"

STATISTIC(NumOpenExtRemoved,
          "Number of open_existential_addr instructions removed");

STATISTIC(NumSimplify, "Number of instructions simplified or DCE'd");
STATISTIC(NumCSE,      "Number of instructions CSE'd");

using namespace swift;

//===----------------------------------------------------------------------===//
//                                Simple Value
//===----------------------------------------------------------------------===//

namespace {
/// SimpleValue - Instances of this struct represent available values in the
/// scoped hash table.
struct SimpleValue {
  SILInstruction *Inst;

  SimpleValue(SILInstruction *I) : Inst(I) { }

  bool isSentinel() const {
    return Inst == llvm::DenseMapInfo<SILInstruction *>::getEmptyKey() ||
           Inst == llvm::DenseMapInfo<SILInstruction *>::getTombstoneKey();
  }
};
} // end anonymous namespace

namespace llvm {
template <> struct DenseMapInfo<SimpleValue> {
  static inline SimpleValue getEmptyKey() {
    return DenseMapInfo<SILInstruction *>::getEmptyKey();
  }
  static inline SimpleValue getTombstoneKey() {
    return DenseMapInfo<SILInstruction *>::getTombstoneKey();
  }
  static unsigned getHashValue(SimpleValue Val);
  static bool isEqual(SimpleValue LHS, SimpleValue RHS);
};
} // end namespace llvm

SILValue tryLookThroughOwnershipInsts(const Operand *op) {
  auto opValue = op->get();
  auto opOwnership = op->getOperandOwnership();

  // Escaped values are dependent on the base value lifetime.
  // OSSA RAUW does not lifetime extend base value for an escaped value.
  // Don't look through ownership instructions for such values.

  // Theoritically, it should be possible to look through ownership instructions
  // for a bitwise escape, barring any dependent instructions like
  // mark_dependence. Not doing it here to be conservative.
  if (opOwnership == OperandOwnership::PointerEscape ||
      opOwnership == OperandOwnership::BitwiseEscape ||
      opOwnership == OperandOwnership::ForwardingUnowned) {
    return opValue;
  }

  return lookThroughOwnershipInsts(opValue);
}

namespace {
class HashVisitor : public SILInstructionVisitor<HashVisitor, llvm::hash_code> {
  using hash_code = llvm::hash_code;

public:
  hash_code visitSILInstruction(SILInstruction *) {
    llvm_unreachable("No hash implemented for the given type");
  }

  hash_code visitBridgeObjectToRefInst(BridgeObjectToRefInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitBridgeObjectToWordInst(BridgeObjectToWordInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitClassifyBridgeObjectInst(ClassifyBridgeObjectInst *X) {
    return llvm::hash_combine(
        X->getKind(), tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitValueToBridgeObjectInst(ValueToBridgeObjectInst *X) {
    return llvm::hash_combine(
        X->getKind(), tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitRefToBridgeObjectInst(RefToBridgeObjectInst *X) {
    if (X->getFunction()->hasOwnership()) {
      auto TransformedOpValues =
          X->getOperandValues(tryLookThroughOwnershipInsts, false);
      return llvm::hash_combine(
          X->getKind(), X->getType(),
          llvm::hash_combine_range(TransformedOpValues.begin(),
                                   TransformedOpValues.end()));
    }
    OperandValueArrayRef Operands(X->getAllOperands());
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        llvm::hash_combine_range(Operands.begin(), Operands.end()));
  }

  hash_code visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitUncheckedBitwiseCastInst(UncheckedBitwiseCastInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitUncheckedAddrCastInst(UncheckedAddrCastInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitFunctionRefInst(FunctionRefInst *X) {
    return llvm::hash_combine(X->getKind(), X->getReferencedFunction());
  }

  hash_code visitGlobalAddrInst(GlobalAddrInst *X) {
    return llvm::hash_combine(X->getKind(), X->getReferencedGlobal());
  }

  hash_code visitIntegerLiteralInst(IntegerLiteralInst *X) {
    return llvm::hash_combine(X->getKind(), X->getType(), X->getValue());
  }

  hash_code visitFloatLiteralInst(FloatLiteralInst *X) {
    return llvm::hash_combine(X->getKind(), X->getType(), X->getBits());
  }

  hash_code visitRefElementAddrInst(RefElementAddrInst *X) {
    return llvm::hash_combine(X->getKind(),
                              tryLookThroughOwnershipInsts(&X->getOperandRef()),
                              X->getField());
  }

  hash_code visitRefTailAddrInst(RefTailAddrInst *X) {
    return llvm::hash_combine(
        X->getKind(), tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitProjectBoxInst(ProjectBoxInst *X) {
    return llvm::hash_combine(
        X->getKind(), tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitRefToRawPointerInst(RefToRawPointerInst *X) {
    return llvm::hash_combine(
        X->getKind(), tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitRawPointerToRefInst(RawPointerToRefInst *X) {
    return llvm::hash_combine(
        X->getKind(), tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

#define LOADABLE_REF_STORAGE(Name, ...)                                        \
  hash_code visit##Name##ToRefInst(Name##ToRefInst *X) {                       \
    return llvm::hash_combine(                                                 \
        X->getKind(), tryLookThroughOwnershipInsts(&X->getOperandRef()));      \
  }                                                                            \
  hash_code visitRefTo##Name##Inst(RefTo##Name##Inst *X) {                     \
    return llvm::hash_combine(                                                 \
        X->getKind(), tryLookThroughOwnershipInsts(&X->getOperandRef()));      \
  }
#include "swift/AST/ReferenceStorage.def"

  hash_code visitUpcastInst(UpcastInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitStringLiteralInst(StringLiteralInst *X) {
    return llvm::hash_combine(X->getKind(), X->getEncoding(), X->getValue());
  }

  hash_code visitStructInst(StructInst *X) {
    // This is safe since we are hashing the operands using the actual pointer
    // values of the values being used by the operand.
    if (X->getFunction()->hasOwnership()) {
      auto TransformedOpValues =
          X->getOperandValues(tryLookThroughOwnershipInsts, false);
      return llvm::hash_combine(
          X->getKind(), X->getStructDecl(),
          llvm::hash_combine_range(TransformedOpValues.begin(),
                                   TransformedOpValues.end()));
    }
    OperandValueArrayRef Operands(X->getAllOperands());
    return llvm::hash_combine(
        X->getKind(), X->getStructDecl(),
        llvm::hash_combine_range(Operands.begin(), Operands.end()));
  }

  hash_code visitStructExtractInst(StructExtractInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getStructDecl(), X->getField(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitStructElementAddrInst(StructElementAddrInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getStructDecl(), X->getField(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitCondFailInst(CondFailInst *X) {
    return llvm::hash_combine(X->getKind(), X->getOperand());
  }

  hash_code visitClassMethodInst(ClassMethodInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitSuperMethodInst(SuperMethodInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitTupleInst(TupleInst *X) {
    if (X->getFunction()->hasOwnership()) {
      auto TransformedOpValues =
          X->getOperandValues(tryLookThroughOwnershipInsts, false);
      return llvm::hash_combine(
          X->getKind(), X->getTupleType(),
          llvm::hash_combine_range(TransformedOpValues.begin(),
                                   TransformedOpValues.end()));
    }
    OperandValueArrayRef Operands(X->getAllOperands());
    return llvm::hash_combine(
        X->getKind(), X->getTupleType(),
        llvm::hash_combine_range(Operands.begin(), Operands.end()));
  }

  hash_code visitTupleExtractInst(TupleExtractInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getTupleType(), X->getFieldIndex(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitTupleElementAddrInst(TupleElementAddrInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getTupleType(), X->getFieldIndex(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitMetatypeInst(MetatypeInst *X) {
    return llvm::hash_combine(X->getKind(), X->getType());
  }

  hash_code visitValueMetatypeInst(ValueMetatypeInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitExistentialMetatypeInst(ExistentialMetatypeInst *X) {
    return llvm::hash_combine(X->getKind(), X->getType());
  }

  hash_code visitInitExistentialMetatypeInst(InitExistentialMetatypeInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()),
        llvm::hash_combine_range(X->getConformances().begin(),
                                 X->getConformances().end()));
  }

  hash_code visitObjCProtocolInst(ObjCProtocolInst *X) {
    return llvm::hash_combine(X->getKind(), X->getType(), X->getProtocol());
  }

  hash_code visitIndexRawPointerInst(IndexRawPointerInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        tryLookThroughOwnershipInsts(&X->getBaseOperandRef()), X->getIndex());
  }

  hash_code visitPointerToAddressInst(PointerToAddressInst *X) {
    return llvm::hash_combine(X->getKind(), X->getType(),
                              tryLookThroughOwnershipInsts(&X->getOperandRef()),
                              X->isStrict());
  }

  hash_code visitAddressToPointerInst(AddressToPointerInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitApplyInst(ApplyInst *X) {
    if (X->getFunction()->hasOwnership()) {
      auto TransformedOpValues =
          X->getOperandValues(tryLookThroughOwnershipInsts, false);
      return llvm::hash_combine(
          X->getKind(), X->getCallee(),
          llvm::hash_combine_range(TransformedOpValues.begin(),
                                   TransformedOpValues.end()));
    }
    OperandValueArrayRef Operands(X->getAllOperands());
    return llvm::hash_combine(
        X->getKind(), X->getCallee(),
        llvm::hash_combine_range(Operands.begin(), Operands.end()));
  }

  hash_code visitBuiltinInst(BuiltinInst *X) {
    if (X->getFunction()->hasOwnership()) {
      auto TransformedOpValues =
          X->getOperandValues(tryLookThroughOwnershipInsts, false);
      return llvm::hash_combine(
          X->getKind(), X->getName().get(),
          llvm::hash_combine_range(TransformedOpValues.begin(),
                                   TransformedOpValues.end()),
          X->hasSubstitutions());
    }
    OperandValueArrayRef Operands(X->getAllOperands());
    return llvm::hash_combine(
        X->getKind(), X->getName().get(),
        llvm::hash_combine_range(Operands.begin(), Operands.end()),
        X->hasSubstitutions());
  }

  hash_code visitEnumInst(EnumInst *X) {
    // We hash the enum by hashing its kind, element, and operand if it has one.
    if (!X->hasOperand())
      return llvm::hash_combine(X->getKind(), X->getElement());
    return llvm::hash_combine(
        X->getKind(), X->getElement(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitUncheckedEnumDataInst(UncheckedEnumDataInst *X) {
    // We hash the enum by hashing its kind, element, and operand.
    return llvm::hash_combine(
        X->getKind(), X->getElement(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitIndexAddrInst(IndexAddrInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        tryLookThroughOwnershipInsts(&X->getBaseOperandRef()), X->getIndex());
  }

  hash_code visitThickToObjCMetatypeInst(ThickToObjCMetatypeInst *X) {
    return llvm::hash_combine(X->getKind(),
                              tryLookThroughOwnershipInsts(&X->getOperandRef()),
                              X->getType());
  }

  hash_code visitObjCToThickMetatypeInst(ObjCToThickMetatypeInst *X) {
    return llvm::hash_combine(X->getKind(),
                              tryLookThroughOwnershipInsts(&X->getOperandRef()),
                              X->getType());
  }

  hash_code visitObjCMetatypeToObjectInst(ObjCMetatypeToObjectInst *X) {
    return llvm::hash_combine(X->getKind(),
                              tryLookThroughOwnershipInsts(&X->getOperandRef()),
                              X->getType());
  }

  hash_code visitObjCExistentialMetatypeToObjectInst(
      ObjCExistentialMetatypeToObjectInst *X) {
    return llvm::hash_combine(X->getKind(),
                              tryLookThroughOwnershipInsts(&X->getOperandRef()),
                              X->getType());
  }

  hash_code visitUncheckedRefCastInst(UncheckedRefCastInst *X) {
    return llvm::hash_combine(X->getKind(),
                              tryLookThroughOwnershipInsts(&X->getOperandRef()),
                              X->getType());
  }

  hash_code visitSelectEnumInstBase(SelectEnumInstBase *X) {
    auto hash = llvm::hash_combine(
        X->getKind(), tryLookThroughOwnershipInsts(&X->getEnumOperandRef()),
        X->getType(), X->hasDefault());

    for (unsigned i = 0, e = X->getNumCases(); i < e; ++i) {
      hash = llvm::hash_combine(hash, X->getCase(i).first,
                                X->getCase(i).second);
    }
    
    if (X->hasDefault())
      hash = llvm::hash_combine(hash, X->getDefaultResult());
    
    return hash;
  }

  hash_code visitSelectEnumInst(SelectEnumInst *X) {
    return visitSelectEnumInstBase(X);
  }

  hash_code visitSelectEnumAddrInst(SelectEnumAddrInst *X) {
    return visitSelectEnumInstBase(X);
  }

  hash_code visitSelectValueInst(SelectValueInst *X) {
    auto hash = llvm::hash_combine(
        X->getKind(), tryLookThroughOwnershipInsts(&X->getAllOperands()[0]),
        X->getType(), X->hasDefault());

    for (unsigned i = 0, e = X->getNumCases(); i < e; ++i) {
      hash = llvm::hash_combine(hash, X->getCase(i).first,
                                X->getCase(i).second);
    }

    if (X->hasDefault())
      hash = llvm::hash_combine(hash, X->getDefaultResult());

    return hash;
  }

  hash_code visitWitnessMethodInst(WitnessMethodInst *X) {
    if (X->getFunction()->hasOwnership()) {
      auto TransformedOpValues =
          X->getOperandValues(tryLookThroughOwnershipInsts, false);
      return llvm::hash_combine(
          X->getKind(), X->getLookupType().getPointer(), X->getMember(),
          X->getConformance(), X->getType(),
          !X->getTypeDependentOperands().empty(),
          llvm::hash_combine_range(TransformedOpValues.begin(),
                                   TransformedOpValues.end()));
    }

    OperandValueArrayRef Operands(X->getAllOperands());
    return llvm::hash_combine(
        X->getKind(), X->getLookupType().getPointer(), X->getMember(),
        X->getConformance(), X->getType(),
        !X->getTypeDependentOperands().empty(),
        llvm::hash_combine_range(Operands.begin(), Operands.end()));
  }

  hash_code visitMarkDependenceInst(MarkDependenceInst *X) {
    if (X->getFunction()->hasOwnership()) {
      auto TransformedOpValues =
          X->getOperandValues(tryLookThroughOwnershipInsts, false);
      return llvm::hash_combine(
          X->getKind(), X->getType(),
          llvm::hash_combine_range(TransformedOpValues.begin(),
                                   TransformedOpValues.end()));
    }
    OperandValueArrayRef Operands(X->getAllOperands());
    return llvm::hash_combine(
        X->getKind(), X->getType(),
        llvm::hash_combine_range(Operands.begin(), Operands.end()));
  }

  hash_code visitOpenExistentialRefInst(OpenExistentialRefInst *X) {
    auto ArchetypeTy = X->getType().castTo<ArchetypeType>();
    auto ConformsTo = ArchetypeTy->getConformsTo();
    return llvm::hash_combine(
        X->getKind(), tryLookThroughOwnershipInsts(&X->getOperandRef()),
        llvm::hash_combine_range(ConformsTo.begin(), ConformsTo.end()));
  }

  hash_code visitScalarPackIndexInst(ScalarPackIndexInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getIndexedPackType(), X->getComponentIndex());
  }

  hash_code visitDynamicPackIndexInst(DynamicPackIndexInst *X) {
    return llvm::hash_combine(
        X->getKind(), X->getIndexedPackType(),
        tryLookThroughOwnershipInsts(&X->getOperandRef()));
  }

  hash_code visitTuplePackElementAddrInst(TuplePackElementAddrInst *X) {
    OperandValueArrayRef Operands(X->getAllOperands());
    return llvm::hash_combine(
        X->getKind(),
        llvm::hash_combine_range(Operands.begin(), Operands.end()),
        X->getElementType());
  }
};
} // end anonymous namespace

unsigned llvm::DenseMapInfo<SimpleValue>::getHashValue(SimpleValue Val) {
  return HashVisitor().visit(Val.Inst);
}

bool llvm::DenseMapInfo<SimpleValue>::isEqual(SimpleValue LHS,
                                              SimpleValue RHS) {
  SILInstruction *LHSI = LHS.Inst, *RHSI = RHS.Inst;
  if (LHS.isSentinel() || RHS.isSentinel())
    return LHSI == RHSI;

  auto LOpen = dyn_cast<OpenExistentialRefInst>(LHSI);
  auto ROpen = dyn_cast<OpenExistentialRefInst>(RHSI);
  if (LOpen && ROpen) {
    // Check operands.
    auto *LOp = &LOpen->getOperandRef();
    auto *ROp = &ROpen->getOperandRef();
    if (tryLookThroughOwnershipInsts(LOp) != tryLookThroughOwnershipInsts(ROp))
      return false;

    // Consider the types of two open_existential_ref instructions to be equal,
    // if the sets of protocols they conform to are equal ...
    auto LHSArchetypeTy = LOpen->getType().castTo<ArchetypeType>();
    auto RHSArchetypeTy = ROpen->getType().castTo<ArchetypeType>();

    auto LHSConformsTo = LHSArchetypeTy->getConformsTo();
    auto RHSConformsTo = RHSArchetypeTy->getConformsTo();
    if (LHSConformsTo != RHSConformsTo)
      return false;

    // ... and other constraints are equal.
    if (LHSArchetypeTy->getSuperclass().getPointer() !=
        RHSArchetypeTy->getSuperclass().getPointer())
      return false;

    if (LHSArchetypeTy->getLayoutConstraint() !=
        RHSArchetypeTy->getLayoutConstraint())
      return false;

    return true;
  }
  auto opCmp = [&](const Operand *op1, const Operand *op2) -> bool {
    if (op1 == op2)
      return true;
    if (tryLookThroughOwnershipInsts(op1) == tryLookThroughOwnershipInsts(op2))
      return true;
    return false;
  };
  bool isEqual =
      LHSI->getKind() == RHSI->getKind() && LHSI->isIdenticalTo(RHSI, opCmp);
#ifndef NDEBUG
  if (isEqual && getHashValue(LHS) != getHashValue(RHS)) {
    llvm::dbgs() << "LHS: ";
    LHSI->dump();
    llvm::dbgs() << "RHS: ";
    RHSI->dump();
    llvm::dbgs() << "In function:\n";
    LHSI->getFunction()->dump();
    llvm_unreachable("Mismatched isEqual and getHashValue() function in CSE\n");
  }
#endif
  return isEqual;
}

namespace {
// A very simple cloner for cloning instructions inside
// the same function. The only interesting thing it does
// is remapping the archetypes when it is required.
class InstructionCloner : public SILCloner<InstructionCloner> {
  friend class SILCloner<InstructionCloner>;
  friend class SILInstructionVisitor<InstructionCloner>;
  SILInstruction *Result = nullptr;

public:
  InstructionCloner(SILFunction *F) : SILCloner(*F) {}

  static SILInstruction *doIt(SILInstruction *I) {
    InstructionCloner TC(I->getFunction());
    return TC.clone(I);
  }

  SILInstruction *clone(SILInstruction *I) {
    visit(I);
    return Result;
  }

  void postProcess(SILInstruction *Orig, SILInstruction *Cloned) {
    assert(Orig->getFunction() == &getBuilder().getFunction() &&
           "cloning between functions is not supported");

    Result = Cloned;
    SILCloner<InstructionCloner>::postProcess(Orig, Cloned);
  }
  SILValue getMappedValue(SILValue Value) { return Value; }
  SILBasicBlock *remapBasicBlock(SILBasicBlock *BB) { return BB; }
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                               CSE Interface
//===----------------------------------------------------------------------===//

namespace swift {

/// CSE - This pass does a simple depth-first walk over the dominator tree,
/// eliminating trivially redundant instructions and using simplifyInstruction
/// to canonicalize things as it goes. It is intended to be fast and catch
/// obvious cases so that SILCombine and other passes are more effective.
///
/// It also optimizes calls to lazy property getters: If such a call is
/// dominated by another call to the same getter, it is replaced by a direct
/// load of the property - assuming that it is already computed.
class CSE {
public:
  typedef llvm::ScopedHashTableVal<SimpleValue, ValueBase *> SimpleValueHTType;
  typedef llvm::RecyclingAllocator<llvm::BumpPtrAllocator, SimpleValueHTType>
  AllocatorTy;
  typedef llvm::ScopedHashTable<SimpleValue, SILInstruction *,
                                llvm::DenseMapInfo<SimpleValue>,
                                AllocatorTy> ScopedHTType;

  /// AvailableValues - This scoped hash table contains the current values of
  /// all of our simple scalar expressions.  As we walk down the domtree, we
  /// look to see if instructions are in this: if so, we replace them with what
  /// we find, otherwise we insert them so that dominated values can succeed in
  /// their lookup.
  ScopedHTType *AvailableValues;

  BasicCalleeAnalysis *BCA;

  SILOptFunctionBuilder &FuncBuilder;

  DeadEndBlocks &DeadEndBBs;

  OwnershipFixupContext &RAUWFixupContext;

  /// The set of calls to lazy property getters which can be replace by a direct
  /// load of the property value.
  llvm::SmallVector<ApplyInst *, 8> lazyPropertyGetters;

  CSE(bool RunsOnHighLevelSil, BasicCalleeAnalysis *BCA,
      SILOptFunctionBuilder &FuncBuilder, DeadEndBlocks &DeadEndBBs,
      OwnershipFixupContext &RAUWFixupContext)
      : BCA(BCA), FuncBuilder(FuncBuilder), DeadEndBBs(DeadEndBBs),
        RAUWFixupContext(RAUWFixupContext),
        RunsOnHighLevelSil(RunsOnHighLevelSil) {}

  bool processFunction(SILFunction &F, DominanceInfo *DT);

  bool processLazyPropertyGetters(SILFunction &F);

  bool canHandle(SILInstruction *Inst);

private:
  
  /// True if CSE is done on high-level SIL, i.e. semantic calls are not inlined
  /// yet. In this case some semantic calls can be CSEd.
  bool RunsOnHighLevelSil;
  
  // NodeScope - almost a POD, but needs to call the constructors for the
  // scoped hash tables so that a new scope gets pushed on. These are RAII so
  // that the scope gets popped when the NodeScope is destroyed.
  class NodeScope {
   public:
    NodeScope(ScopedHTType *availableValues) : Scope(*availableValues) {}

   private:
    NodeScope(const NodeScope &) = delete;
    void operator=(const NodeScope &) = delete;

    ScopedHTType::ScopeTy Scope;
  };

  // StackNode - contains all the needed information to create a stack for doing
  // a depth first traversal of the tree. This includes scopes for values and
  // loads as well as the generation. There is a child iterator so that the
  // children do not need to be store separately.
  class StackNode {
   public:
    StackNode(ScopedHTType *availableValues, DominanceInfoNode *n,
              DominanceInfoNode::const_iterator child,
              DominanceInfoNode::const_iterator end)
        : Node(n), ChildIter(child), EndIter(end), Scopes(availableValues),
      Processed(false) {}

    // Accessors.
    DominanceInfoNode *node() { return Node; }
    DominanceInfoNode::const_iterator childIter() { return ChildIter; }
    DominanceInfoNode *nextChild() {
      DominanceInfoNode *child = *ChildIter;
      ++ChildIter;
      return child;
    }
    DominanceInfoNode::const_iterator end() { return EndIter; }
    bool isProcessed() { return Processed; }
    void process() { Processed = true; }

   private:
    StackNode(const StackNode &) = delete;
    void operator=(const StackNode &) = delete;

    // Members.
    DominanceInfoNode *Node;
    DominanceInfoNode::const_iterator ChildIter;
    DominanceInfoNode::const_iterator EndIter;
    NodeScope Scopes;
    bool Processed;
  };

  bool processNode(DominanceInfoNode *Node);
  bool processOpenExistentialRef(OpenExistentialRefInst *Inst,
                                 OpenExistentialRefInst *V);
};
} // namespace swift

//===----------------------------------------------------------------------===//
//                             CSE Implementation
//===----------------------------------------------------------------------===//

bool CSE::processFunction(SILFunction &Fm, DominanceInfo *DT) {
  std::vector<StackNode *> nodesToProcess;

  // Tables that the pass uses when walking the domtree.
  ScopedHTType AVTable;
  AvailableValues = &AVTable;

  bool Changed = false;

  // Process the root node.
  nodesToProcess.push_back(new StackNode(AvailableValues, DT->getRootNode(),
                  DT->getRootNode()->begin(),
                  DT->getRootNode()->end()));

  // Process the stack.
  while (!nodesToProcess.empty()) {
    // Grab the first item off the stack. Set the current generation, remove
    // the node from the stack, and process it.
    StackNode *NodeToProcess = nodesToProcess.back();

    // Check if the node needs to be processed.
    if (!NodeToProcess->isProcessed()) {
      // Process the node.
      Changed |= processNode(NodeToProcess->node());
      NodeToProcess->process();

    } else if (NodeToProcess->childIter() != NodeToProcess->end()) {
      // Push the next child onto the stack.
      DominanceInfoNode *child = NodeToProcess->nextChild();
      nodesToProcess.push_back(
          new StackNode(AvailableValues, child, child->begin(), child->end()));
    } else {
      // It has been processed, and there are no more children to process,
      // so delete it and pop it off the stack.
      delete NodeToProcess;
      nodesToProcess.pop_back();
    }
  } // while (!nodes...)

  return Changed;
}

/// Replace lazy property getters (which are dominated by the same getter)
/// by a direct load of the value.
bool CSE::processLazyPropertyGetters(SILFunction &F) {
  bool changed = false;
  bool invalidatedStackNesting = false;
  for (ApplyInst *ai : lazyPropertyGetters) {
    SILFunction *getter = ai->getReferencedFunctionOrNull();
    assert(getter && getter->isLazyPropertyGetter());
    SILBasicBlock *callBlock = ai->getParent();

    // Inline the getter...
    InstructionDeleter deleter;
    SILInliner::inlineFullApply(ai, SILInliner::InlineKind::PerformanceInline,
                                FuncBuilder, deleter);
    deleter.cleanupDeadInstructions();
    
    // ...and fold the switch_enum in the first block to the Optional.some case.
    // The Optional.none branch becomes dead.
    auto *sei = cast<SwitchEnumInst>(callBlock->getTerminator());
    ASTContext &ctxt = callBlock->getParent()->getModule().getASTContext();
    EnumElementDecl *someDecl = ctxt.getOptionalSomeDecl();
    SILBasicBlock *someDest = sei->getCaseDestination(someDecl);
    assert(someDest->getNumArguments() == 1);
    SILValue enumVal = sei->getOperand();
    SILBuilder builder(sei);
    SILType ty = enumVal->getType().getEnumElementType(someDecl,
                           sei->getModule(), builder.getTypeExpansionContext());
    auto *ued =
        builder.createUncheckedEnumData(sei->getLoc(), enumVal, someDecl, ty);
    builder.createBranch(sei->getLoc(), someDest, { ued });
    sei->eraseFromParent();
    // When inlining an OSSA function into a non-OSSA function, ownership of
    // nonescaping closures is lowered.  At that point, they are recognized as
    // stack users.  Since they weren't recognized as such before, they may not
    // satisfy stack discipline.  Fix that up now.
    if (getter->hasOwnership() && !ai->getFunction()->hasOwnership()) {
      invalidatedStackNesting = true;
    }
    changed = true;
    ++NumCSE;
  }
  if (invalidatedStackNesting) {
    StackNesting::fixNesting(&F);
  }
  return changed;
}

/// Update SIL basic block's arguments types which refer to opened
/// archetypes. Replace such types by performing type substitutions
/// according to the provided type substitution map.
static void updateBasicBlockArgTypes(SILBasicBlock *BB,
                                     ArchetypeType *OldOpenedArchetype,
                                     ArchetypeType *NewOpenedArchetype) {
  // Check types of all BB arguments.
  for (auto *Arg : BB->getSILPhiArguments()) {
    if (!Arg->getType().hasOpenedExistential())
      continue;
    // Type of this BB argument uses an opened existential.
    // Try to apply substitutions to it and if it produces a different type,
    // use this type as new type of the BB argument.
    auto OldArgType = Arg->getType();
    auto NewArgType = OldArgType.subst(BB->getModule(),
                                       [&](SubstitutableType *type) -> Type {
                                         if (type == OldOpenedArchetype)
                                           return NewOpenedArchetype;
                                         return type;
                                       },
                                       MakeAbstractConformanceForGenericType());
    if (NewArgType == Arg->getType())
      continue;
    // Replace the type of this BB argument. The type of a BBArg
    // can only be changed using replaceBBArg, if the BBArg has no uses.
    // So, make it look as if it has no uses.

    // First collect all uses, before changing the type.
    SmallVector<Operand *, 4> OriginalArgUses;
    for (auto *ArgUse : Arg->getUses()) {
      OriginalArgUses.push_back(ArgUse);
    }
    // Then replace all uses by an undef.
    Arg->replaceAllUsesWith(SILUndef::get(Arg->getType(), *BB->getParent()));
    // Replace the type of the BB argument.
    auto *NewArg = BB->replacePhiArgument(Arg->getIndex(), NewArgType,
                                          Arg->getOwnershipKind(),
                                          Arg->getDecl());
    // Restore all uses to refer to the BB argument with updated type.
    for (auto ArgUse : OriginalArgUses) {
      ArgUse->set(NewArg);
    }
  }
}

/// Handle CSE of open_existential_ref instructions.
/// Returns true if uses of open_existential_ref can
/// be replaced by a dominating instruction.
/// \Inst is the open_existential_ref instruction
/// \V is the dominating open_existential_ref instruction
bool CSE::processOpenExistentialRef(OpenExistentialRefInst *Inst,
                                    OpenExistentialRefInst *VI) {
  llvm::SmallSetVector<SILInstruction *, 16> Candidates;
  const auto OldOpenedArchetype = Inst->getDefinedOpenedArchetype();
  const auto NewOpenedArchetype = VI->getDefinedOpenedArchetype();

  // Collect all candidates that may contain opened archetypes
  // that need to be replaced.
  for (auto Use : Inst->getUses()) {
    auto User = Use->getUser();
    if (!User->getTypeDependentOperands().empty()) {
      if (canHandle(User)) {
        auto It = AvailableValues->begin(User);
        if (It != AvailableValues->end()) {
          return false;
        }
      }
    }

    Candidates.insert(User);
  }

  // Now process candidates.
  // Use a cloner. It makes copying the instruction and remapping of
  // opened archetypes trivial.
  InstructionCloner Cloner(Inst->getFunction());
  Cloner.registerLocalArchetypeRemapping(
      OldOpenedArchetype->castTo<ArchetypeType>(), NewOpenedArchetype);
  auto &Builder = Cloner.getBuilder();

  InstructionSet Processed(Inst->getFunction());
  // Now clone each candidate and replace the opened archetype
  // by a dominating one.
  while (!Candidates.empty()) {
    auto Candidate = Candidates.pop_back_val();
    if (Processed.contains(Candidate))
      continue;

    if (isa<TermInst>(Candidate)) {
      // The current use of the opened archetype is a terminator instruction.
      // Check if any of the successor BBs uses this opened archetype in the
      // types of its basic block arguments. If this is the case, replace
      // those uses by the new opened archetype.
      // FIXME: What about uses of those arguments?
      for (auto *Successor : Candidate->getParent()->getSuccessorBlocks()) {
        if (Successor->args_empty())
          continue;
        // If a BB has any arguments, update their types if necessary.
        updateBasicBlockArgTypes(Successor, OldOpenedArchetype,
                                 NewOpenedArchetype);
      }
    }

    // Compute if a candidate depends on the old opened archetype.
    // It always does if it has any type-dependent operands.
    bool DependsOnOldOpenedArchetype =
      !Candidate->getTypeDependentOperands().empty();

    // Look for dependencies propagated via the candidate's results.
    for (auto CandidateResult : Candidate->getResults()) {
      if (CandidateResult->use_empty() ||
          !CandidateResult->getType().hasOpenedExistential())
        continue;

      // Check if the result type depends on this specific opened existential.
      auto ResultDependsOnOldOpenedArchetype =
          CandidateResult->getType().getASTType().findIf(
              [&OldOpenedArchetype](Type t) -> bool {
                return (CanType(t) == OldOpenedArchetype);
              });

      // If it does, the candidate depends on the opened existential.
      if (ResultDependsOnOldOpenedArchetype) {
        DependsOnOldOpenedArchetype = true;

        // The users of this candidate are new candidates.
        for (auto Use : CandidateResult->getUses()) {
          Candidates.insert(Use->getUser());
        }
      }
    }
    // Remember that this candidate was processed already.
    Processed.insert(Candidate);

    // No need to clone if there is no dependency on the old opened archetype.
    if (!DependsOnOldOpenedArchetype)
      continue;

    Builder.setInsertionPoint(Candidate);
    auto NewI = Cloner.clone(Candidate);
    // Result types of candidate's uses instructions may be using this archetype.
    // Thus, we need to try to replace it there.
    Candidate->replaceAllUsesPairwiseWith(NewI);
    eraseFromParentWithDebugInsts(Candidate);
  }
  return true;
}

/// Returns true if \p ai is a call to a lazy property getter, which we can
/// handle.
static bool isLazyPropertyGetter(ApplyInst *ai) {
  SILFunction *callee = ai->getReferencedFunctionOrNull();
  if (!callee || callee->isExternalDeclaration() ||
      !callee->isLazyPropertyGetter())
    return false;

  // We cannot inline a non-ossa function into an ossa function
  if (ai->getFunction()->hasOwnership() && !callee->hasOwnership())
    return false;

  // Only handle classes, but not structs.
  // Lazy property getters of structs have an indirect inout self parameter.
  // We don't know if the whole struct is overwritten between two getter calls.
  // In such a case, the lazy property could be reset to an Optional.none.
  // TODO: We could check this case with AliasAnalysis.
  if (ai->getArgument(0)->getType().isAddress())
    return false;

  // Check if the first block has a switch_enum of an Optional.
  // We don't handle getters of generic types, which have a switch_enum_addr.
  // This will be obsolete with opaque values anyway.
  auto *SEI = dyn_cast<SwitchEnumInst>(callee->getEntryBlock()->getTerminator());
  if (!SEI)
    return false;

  ASTContext &ctxt = SEI->getFunction()->getModule().getASTContext();
  EnumElementDecl *someDecl = ctxt.getOptionalSomeDecl();

  for (unsigned i = 0, e = SEI->getNumCases(); i != e; ++i) {
    auto Entry = SEI->getCase(i);
    if (Entry.first == someDecl) {
      SILBasicBlock *destBlock = Entry.second;
      return destBlock->getNumArguments() == 1;
    }
  }
  return false;
}

bool CSE::processNode(DominanceInfoNode *Node) {
  SILBasicBlock *BB = Node->getBlock();
  bool Changed = false;

  // See if any instructions in the block can be eliminated.  If so, do it.  If
  // not, add them to AvailableValues. Assume the block terminator can't be
  // erased.
  for (SILBasicBlock::iterator nextI = BB->begin(), E = BB->end();
       nextI != E;) {
    SILInstruction *Inst = &*nextI;
    ++nextI;

    LLVM_DEBUG(llvm::dbgs() << "SILCSE VISITING: " << *Inst << "\n");

    // Dead instructions should just be removed.
    if (isInstructionTriviallyDead(Inst)) {
      LLVM_DEBUG(llvm::dbgs() << "SILCSE DCE: " << *Inst << '\n');
      nextI = eraseFromParentWithDebugInsts(Inst);
      Changed = true;
      ++NumSimplify;
      continue;
    }

    // If the instruction can be simplified (e.g. X+0 = X) then replace it with
    // its simpler value.
    InstModCallbacks callbacks;
    nextI = simplifyAndReplaceAllSimplifiedUsesAndErase(Inst, callbacks,
                                                        &DeadEndBBs);
    if (callbacks.hadCallbackInvocation()) {
      ++NumSimplify;
      Changed = true;
      continue;
    }

    // If this is not a simple instruction that we can value number, skip it.
    if (!canHandle(Inst))
      continue;

    // If an instruction can be handled here, then it must also be handled
    // in isIdenticalTo, otherwise looking up a key in the map with fail to
    // match itself.
    assert(Inst->isIdenticalTo(Inst) &&
           "Inst must match itself for map to work");
    assert(llvm::DenseMapInfo<SimpleValue>::isEqual(Inst, Inst) &&
           "Inst must match itself for map to work");

    // Now that we know we have an instruction we understand see if the
    // instruction has an available value.  If so, use it.
    if (SILInstruction *AvailInst = AvailableValues->lookup(Inst)) {
      LLVM_DEBUG(llvm::dbgs() << "SILCSE CSE: " << *Inst << "  to: "
                              << *AvailInst << '\n');

      auto *AI = dyn_cast<ApplyInst>(Inst);
      if (AI && isLazyPropertyGetter(AI)) {
        // We do the actual transformation for lazy property getters later. It
        // changes the CFG and we don't want to disturb the dominator tree walk
        // here.
        lazyPropertyGetters.push_back(AI);
        continue;
      }
      if (!isa<OpenExistentialRefInst>(Inst) ||
          processOpenExistentialRef(cast<OpenExistentialRefInst>(Inst),
                                    cast<OpenExistentialRefInst>(AvailInst))) {
        if (Inst->getResults().empty()) {
          nextI = std::next(Inst->getIterator());
          Inst->eraseFromParent();
          Changed = true;
          ++NumCSE;
          continue;
        }
        if (!Inst->getFunction()->hasOwnership()) {
          Inst->replaceAllUsesPairwiseWith(AvailInst);
          nextI = std::next(Inst->getIterator());
          Inst->eraseFromParent();
          Changed = true;
          ++NumCSE;
          continue;
        }
        // TODO: Support MultipleValueInstructionResult in OSSA RAUW utility and
        // extend it here as well
        if (!isa<SingleValueInstruction>(Inst))
          continue;

        OwnershipRAUWHelper helper(RAUWFixupContext,
                                   cast<SingleValueInstruction>(Inst),
                                   cast<SingleValueInstruction>(AvailInst));
        // If RAUW requires cloning the original, then there's no point. If it
        // also requires introducing a copy and new borrow scope, then it's a
        // very bad idea.
        if (!helper.isValid() || helper.requiresCopyBorrowAndClone())
          continue;
        // Replace SingleValueInstruction using OSSA RAUW here
        nextI = helper.perform();
        Changed = true;
        ++NumCSE;
        continue;
      }
    }

    // Otherwise, just remember that this value is available.
    AvailableValues->insert(Inst, Inst);
    LLVM_DEBUG(llvm::dbgs() << "SILCSE Adding to value table: " << *Inst
                            << " -> " << *Inst << "\n");
  }

  return Changed;
}

bool CSE::canHandle(SILInstruction *Inst) {
  if (auto *AI = dyn_cast<ApplyInst>(Inst)) {
    if (!AI->mayReadOrWriteMemory())
      return true;
    
    if (RunsOnHighLevelSil) {
      ArraySemanticsCall SemCall(AI);
      switch (SemCall.getKind()) {
        case ArrayCallKind::kGetCount:
        case ArrayCallKind::kGetCapacity:
        case ArrayCallKind::kCheckIndex:
        case ArrayCallKind::kCheckSubscript:
          return SemCall.hasGuaranteedSelf();
        default:
          return false;
      }
    }

    if (!AI->getFunction()->hasOwnership()) {
      // In non-OSSA we don't balance CSE'd apply results which return an
      // owned value.
      if (auto ri = AI->getSingleResult()) {
        if (ri.value().getConvention() != ResultConvention::Unowned)
          return false;
      }
    }

    // We can CSE function calls which do not read or write memory and don't
    // have any other side effects.
    // Note that the function also may not contain any retains. And there are
    // functions which are read-none and have a retain, e.g. functions which
    // _convert_ a global_addr to a reference and retain it.
    auto MB = BCA->getMemoryBehavior(ApplySite(AI), /*observeRetains*/false);
    if (MB == MemoryBehavior::None)
      return true;
    
    if (isLazyPropertyGetter(AI))
      return true;
      
    if (SILFunction *callee = AI->getReferencedFunctionOrNull()) {
      if (callee->isGlobalInit())
        return true;
    }
    
    return false;
  }
  if (auto *BI = dyn_cast<BuiltinInst>(Inst)) {
    switch (BI->getBuiltinInfo().ID) {
    case BuiltinValueKind::OnFastPath:
      // Although the onFastPath builtin has no side-effects we don't want to
      // (re-)move it.
      return false;
    case BuiltinValueKind::Once:
    case BuiltinValueKind::OnceWithContext:
      return true;
    default:
      return !BI->mayReadOrWriteMemory();
    }
  }
  if (auto *EMI = dyn_cast<ExistentialMetatypeInst>(Inst)) {
    return !EMI->getOperand()->getType().isAddress();
  }
  switch (Inst->getKind()) {
  case SILInstructionKind::ClassMethodInst:
  case SILInstructionKind::SuperMethodInst:
  case SILInstructionKind::FunctionRefInst:
  case SILInstructionKind::GlobalAddrInst:
  case SILInstructionKind::IntegerLiteralInst:
  case SILInstructionKind::FloatLiteralInst:
  case SILInstructionKind::StringLiteralInst:
  case SILInstructionKind::StructInst:
  case SILInstructionKind::StructExtractInst:
  case SILInstructionKind::StructElementAddrInst:
  case SILInstructionKind::TupleInst:
  case SILInstructionKind::TupleExtractInst:
  case SILInstructionKind::TupleElementAddrInst:
  case SILInstructionKind::MetatypeInst:
  case SILInstructionKind::ValueMetatypeInst:
  case SILInstructionKind::ObjCProtocolInst:
  case SILInstructionKind::RefElementAddrInst:
  case SILInstructionKind::RefTailAddrInst:
  case SILInstructionKind::ProjectBoxInst:
  case SILInstructionKind::IndexRawPointerInst:
  case SILInstructionKind::IndexAddrInst:
  case SILInstructionKind::PointerToAddressInst:
  case SILInstructionKind::AddressToPointerInst:
  case SILInstructionKind::CondFailInst:
  case SILInstructionKind::EnumInst:
  case SILInstructionKind::UncheckedEnumDataInst:
  case SILInstructionKind::UncheckedTrivialBitCastInst:
  case SILInstructionKind::UncheckedBitwiseCastInst:
  case SILInstructionKind::RefToRawPointerInst:
  case SILInstructionKind::RawPointerToRefInst:
  case SILInstructionKind::UpcastInst:
  case SILInstructionKind::ThickToObjCMetatypeInst:
  case SILInstructionKind::ObjCToThickMetatypeInst:
  case SILInstructionKind::UncheckedRefCastInst:
  case SILInstructionKind::UncheckedAddrCastInst:
  case SILInstructionKind::ObjCMetatypeToObjectInst:
  case SILInstructionKind::ObjCExistentialMetatypeToObjectInst:
  case SILInstructionKind::SelectEnumInst:
  case SILInstructionKind::SelectValueInst:
  case SILInstructionKind::RefToBridgeObjectInst:
  case SILInstructionKind::BridgeObjectToRefInst:
  case SILInstructionKind::BridgeObjectToWordInst:
  case SILInstructionKind::ClassifyBridgeObjectInst:
  case SILInstructionKind::ValueToBridgeObjectInst:
  case SILInstructionKind::MarkDependenceInst:
  case SILInstructionKind::InitExistentialMetatypeInst:
  case SILInstructionKind::WitnessMethodInst:
  case SILInstructionKind::ScalarPackIndexInst:
  case SILInstructionKind::DynamicPackIndexInst:
  case SILInstructionKind::TuplePackElementAddrInst:
    // Intentionally we don't handle (prev_)dynamic_function_ref.
    // They change at runtime.
#define LOADABLE_REF_STORAGE(Name, ...) \
  case SILInstructionKind::RefTo##Name##Inst: \
  case SILInstructionKind::Name##ToRefInst:
#include "swift/AST/ReferenceStorage.def"
    return true;
    // TODO: open_existential_ref is not handled in OSSA currently
    // This is because it is non trivial to ownership rauw copy_value users of a
    // redundant open_existential_ref. Suppose we have an `open_existential_ref`
    // and we are trying to replace it with another `open_existential_ref` of
    // different type. If one of the users of the old `open_existential_ref` is
    // a `copy_value`, we cannot just replace the use. Because `copy_value`'s
    // result type will be the old type as well.
    //
    // In all other places in the compiler where such a rauw needs to be
    // handled, a remapping type is initialized in the cloner and the
    // instruction is cloned before providing to the rauw utility. This would
    // correctly replace all the result types. But since copy_value does not
    // have type dependent operands, we cannot handle it in a similar way.
    //
    // This is currently a TODO until we can implement a clean way to fix this
    // issue.
  case SILInstructionKind::OpenExistentialRefInst:
    return !Inst->getFunction()->hasOwnership();
  default:
    return false;
  }
}

using ApplyWitnessPair = std::pair<ApplyInst *, WitnessMethodInst *>;

/// Returns the Apply and WitnessMethod instructions that use the
/// open_existential_addr instructions, or null if at least one of the
/// instructions is missing.
static ApplyWitnessPair getOpenExistentialUsers(OpenExistentialAddrInst *OE) {
  ApplyInst *AI = nullptr;
  WitnessMethodInst *WMI = nullptr;
  ApplyWitnessPair Empty = std::make_pair(nullptr, nullptr);

  for (auto *UI : getNonDebugUses(OE)) {
    auto *User = UI->getUser();
    if (!isa<WitnessMethodInst>(User) &&
        User->isTypeDependentOperand(UI->getOperandNumber()))
      continue;
    // Check that we have a single Apply user.
    if (auto *AA = dyn_cast<ApplyInst>(User)) {
      if (AI)
        return Empty;

      AI = AA;
      continue;
    }

    // Check that we have a single WMI user.
    if (auto *W = dyn_cast<WitnessMethodInst>(User)) {
      if (WMI)
        return Empty;

      WMI = W;
      continue;
    }

    // Unknown instruction.
    return Empty;
  }

  // Both instructions need to exist.
  if (!WMI || !AI)
    return Empty;

  // Make sure that the WMI and AI match.
  if (AI->getCallee() != WMI)
    return Empty;

  // We have exactly the pattern that we expected.
  return std::make_pair(AI, WMI);
}

/// Try to CSE the users of \p From to the users of \p To.
/// The original users of \p To are passed in ToApplyWitnessUsers.
/// Returns true on success.
static bool tryToCSEOpenExtCall(OpenExistentialAddrInst *From,
                                OpenExistentialAddrInst *To,
                                ApplyWitnessPair ToApplyWitnessUsers,
                                DominanceInfo *DA) {
  assert(From != To && "Can't replace instruction with itself");

  ApplyInst *FromAI = nullptr;
  ApplyInst *ToAI = nullptr;
  WitnessMethodInst *FromWMI = nullptr;
  WitnessMethodInst *ToWMI = nullptr;
  std::tie(FromAI, FromWMI) = getOpenExistentialUsers(From);
  std::tie(ToAI, ToWMI) = ToApplyWitnessUsers;

  // Make sure that the OEA instruction has exactly two expected users.
  if (!FromAI || !ToAI || !FromWMI || !ToWMI)
    return false;

  // Make sure we are calling the same method.
  if (FromWMI->getMember() != ToWMI->getMember())
    return false;

  // We are going to reuse the TO-WMI, so make sure it dominates the call site.
  if (!DA->properlyDominates(ToWMI, FromWMI))
    return false;

  SILBuilder Builder(FromAI);

  assert(FromAI->getArguments().size() == ToAI->getArguments().size() &&
         "Invalid number of arguments");

  // Don't handle any apply instructions that involve substitutions.
  if (ToAI->getSubstitutionMap().getReplacementTypes().size() != 1)
    return false;

  // Prepare the Apply args.
  SmallVector<SILValue, 8> Args;
  for (auto Op : FromAI->getArguments()) {
      Args.push_back(Op == From ? To : Op);
  }

  ApplyInst *NAI = Builder.createApply(ToAI->getLoc(), ToWMI,
                                       ToAI->getSubstitutionMap(), Args,
                                       ToAI->getApplyOptions());
  FromAI->replaceAllUsesWith(NAI);
  FromAI->eraseFromParent();
  ++NumOpenExtRemoved;
  return true;
}

/// Try to CSE the users of the protocol that's passed in argument \p Arg.
/// \returns True if some instructions were modified.
static bool CSExistentialInstructions(SILFunctionArgument *Arg,
                                      DominanceInfo *DA) {
  ParameterConvention Conv = Arg->getKnownParameterInfo().getConvention();
  // We can assume that the address of Proto does not alias because the
  // calling convention is In or In-guaranteed.
  bool MayAlias = Conv != ParameterConvention::Indirect_In_Guaranteed &&
                  Conv != ParameterConvention::Indirect_In;
  if (MayAlias)
    return false;

  // Now check that the only uses of the protocol are witness_method,
  // open_existential_addr and destroy_addr. Also, collect all of the 'opens'.
  llvm::SmallVector<OpenExistentialAddrInst*, 8> Opens;
  for (auto *UI : getNonDebugUses(Arg)) {
    auto *User = UI->getUser();
    if (auto *Open = dyn_cast<OpenExistentialAddrInst>(User)) {
      Opens.push_back(Open);
      continue;
    }

    if (isa<WitnessMethodInst>(User) || isa<DestroyAddrInst>(User))
      continue;

    // Bail out if we found an instruction that we can't handle.
    return false;
  }

  // Find the best dominating 'open' for each open existential.
  llvm::SmallVector<OpenExistentialAddrInst*, 8> TopDominator(Opens);

  bool Changed = false;

  // Try to CSE the users of the current open_existential_addr instruction with
  // one of the other open_existential_addr that dominate it.
  int NumOpenInstr = Opens.size();
  for (int i = 0; i < NumOpenInstr; ++i) {
    // Try to find a better dominating 'open' for the i-th instruction.
    OpenExistentialAddrInst *SomeOpen = TopDominator[i];
    for (int j = 0; j < NumOpenInstr; ++j) {

      if (i == j || TopDominator[i] == TopDominator[j])
        continue;

      OpenExistentialAddrInst *DominatingOpen = TopDominator[j];

      if (DominatingOpen->getOperand() != SomeOpen->getOperand())
        continue;

      if (DA->properlyDominates(DominatingOpen, SomeOpen)) {
        // We found an open instruction that DominatingOpen dominates:
        TopDominator[i] = TopDominator[j];
      }
    }
  }


  // Inspect all of the open_existential_addr instructions and record the
  // apply-witness users. We need to save the original Apply-Witness users
  // because we'll be adding new users and we need to make sure that we can
  // find the original users.
  llvm::SmallVector<ApplyWitnessPair, 8> OriginalAW;
  for (int i=0; i < NumOpenInstr; ++i) {
    OriginalAW.push_back(getOpenExistentialUsers(TopDominator[i]));
  }

  // Perform the CSE for the open_existential_addr instruction and their
  // dominating instruction.
  for (int i=0; i < NumOpenInstr; ++i) {
    if (Opens[i] != TopDominator[i])
      Changed |= tryToCSEOpenExtCall(Opens[i], TopDominator[i],
                                     OriginalAW[i], DA);
  }

  return Changed;
}

/// Detect multiple calls to existential members and try to CSE the instructions
/// that perform the method lookup (the open_existential_addr and
/// witness_method):
///
/// open_existential_addr %0 : $*Pingable to $*@opened("1E467EB8-...")
/// witness_method $@opened("1E467EB8-...") Pingable, #Pingable.ping, %2
/// apply %3<@opened("1E467EB8-...") Pingable>(%2)
///
/// \returns True if some instructions were modified.
static bool CSEExistentialCalls(SILFunction *Func, DominanceInfo *DA) {
  bool Changed = false;
  for (auto *Arg : Func->getArgumentsWithoutIndirectResults()) {
    if (Arg->getType().isExistentialType()) {
      auto *FArg = cast<SILFunctionArgument>(Arg);
      Changed |= CSExistentialInstructions(FArg, DA);
    }
  }

  return Changed;
}

namespace {
class SILCSE : public SILFunctionTransform {
  
  /// True if CSE is done on high-level SIL, i.e. semantic calls are not inlined
  /// yet. In this case some semantic calls can be CSEd.
  /// We only CSE semantic calls on high-level SIL because we can be sure that
  /// e.g. an Array as SILValue is really immutable (including its content).
  bool RunsOnHighLevelSil;
  
  void run() override {
    LLVM_DEBUG(llvm::dbgs() << "***** CSE on function: "
                            << getFunction()->getName() << " *****\n");

    DominanceAnalysis* DA = getAnalysis<DominanceAnalysis>();

    auto *BCA = PM->getAnalysis<BasicCalleeAnalysis>();
    SILOptFunctionBuilder FuncBuilder(*this);

    auto *Fn = getFunction();
    DeadEndBlocks DeadEndBBs(Fn);
    InstModCallbacks callbacks;
    OwnershipFixupContext FixupCtx{callbacks, DeadEndBBs};
    CSE C(RunsOnHighLevelSil, BCA, FuncBuilder, DeadEndBBs, FixupCtx);
    bool Changed = false;

    // Perform the traditional CSE.
    Changed |= C.processFunction(*Fn, DA->get(Fn));

    // Perform CSE of existential and witness_method instructions.
    Changed |= CSEExistentialCalls(Fn, DA->get(Fn));

    // Handle calls to lazy property getters, which are collected in
    // processFunction().
    if (C.processLazyPropertyGetters(*Fn)) {
      // Cleanup the dead blocks from the inlined lazy property getters.
      removeUnreachableBlocks(*Fn);
      invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
    } else if (Changed) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::CallsAndInstructions);
    }
  }

public:
  SILCSE(bool RunsOnHighLevelSil) : RunsOnHighLevelSil(RunsOnHighLevelSil) {}
};
} // end anonymous namespace

SILTransform *swift::createCSE() {
  return new SILCSE(false);
}

SILTransform *swift::createHighLevelCSE() {
  return new SILCSE(true);
}
