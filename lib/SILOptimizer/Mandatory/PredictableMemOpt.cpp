//===--- PredictableMemOpt.cpp - Perform predictable memory optzns --------===//
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

#define DEBUG_TYPE "predictable-memopt"

#include "DIMemoryUseCollector.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFG.h"
#include "swift/SILOptimizer/Utils/Local.h"
#include "swift/SILOptimizer/Utils/SILSSAUpdater.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"

using namespace swift;

STATISTIC(NumLoadPromoted, "Number of loads promoted");
STATISTIC(NumDestroyAddrPromoted, "Number of destroy_addrs promoted");
STATISTIC(NumAllocRemoved, "Number of allocations completely removed");

//===----------------------------------------------------------------------===//
//                            Subelement Analysis
//===----------------------------------------------------------------------===//

// We can only analyze components of structs whose storage is fully accessible
// from Swift.
static StructDecl *
getFullyReferenceableStruct(SILType Ty) {
  auto SD = Ty.getStructOrBoundGenericStruct();
  if (!SD || SD->hasUnreferenceableStorage())
    return nullptr;
  return SD;
}

static unsigned getNumSubElements(SILType T, SILModule &M) {

  if (auto TT = T.getAs<TupleType>()) {
    unsigned NumElements = 0;
    for (auto index : indices(TT.getElementTypes()))
      NumElements += getNumSubElements(T.getTupleElementType(index), M);
    return NumElements;
  }
  
  if (auto *SD = getFullyReferenceableStruct(T)) {
    unsigned NumElements = 0;
    for (auto *D : SD->getStoredProperties())
      NumElements += getNumSubElements(T.getFieldType(D, M), M);
    return NumElements;
  }
  
  // If this isn't a tuple or struct, it is a single element.
  return 1;
}

/// getAccessPathRoot - Given an address, dive through any tuple/struct element
/// addresses to get the underlying value.
static SILValue getAccessPathRoot(SILValue Pointer) {
  while (1) {
    if (auto *TEAI = dyn_cast<TupleElementAddrInst>(Pointer))
      Pointer = TEAI->getOperand();
    else if (auto SEAI = dyn_cast<StructElementAddrInst>(Pointer))
      Pointer = SEAI->getOperand();
    else if (auto BAI = dyn_cast<BeginAccessInst>(Pointer))
      Pointer = BAI->getSource();
    else
      return Pointer;
  }
}

/// Compute the subelement number indicated by the specified pointer (which is
/// derived from the root by a series of tuple/struct element addresses) by
/// treating the type as a linearized namespace with sequential elements.  For
/// example, given:
///
///   root = alloc { a: { c: i64, d: i64 }, b: (i64, i64) }
///   tmp1 = struct_element_addr root, 1
///   tmp2 = tuple_element_addr tmp1, 0
///
/// This will return a subelement number of 2.
///
/// If this pointer is to within an existential projection, it returns ~0U.
static unsigned computeSubelement(SILValue Pointer,
                                  SingleValueInstruction *RootInst) {
  unsigned SubElementNumber = 0;
  SILModule &M = RootInst->getModule();
  
  while (1) {
    // If we got to the root, we're done.
    if (RootInst == Pointer)
      return SubElementNumber;

    if (auto *PBI = dyn_cast<ProjectBoxInst>(Pointer)) {
      Pointer = PBI->getOperand();
      continue;
    }

    if (auto *BAI = dyn_cast<BeginAccessInst>(Pointer)) {
      Pointer = BAI->getSource();
      continue;
    }

    if (auto *TEAI = dyn_cast<TupleElementAddrInst>(Pointer)) {
      SILType TT = TEAI->getOperand()->getType();
      
      // Keep track of what subelement is being referenced.
      for (unsigned i = 0, e = TEAI->getFieldNo(); i != e; ++i) {
        SubElementNumber += getNumSubElements(TT.getTupleElementType(i), M);
      }
      Pointer = TEAI->getOperand();
      continue;
    }

    if (auto *SEAI = dyn_cast<StructElementAddrInst>(Pointer)) {
      SILType ST = SEAI->getOperand()->getType();
      
      // Keep track of what subelement is being referenced.
      StructDecl *SD = SEAI->getStructDecl();
      for (auto *D : SD->getStoredProperties()) {
        if (D == SEAI->getField()) break;
        SubElementNumber += getNumSubElements(ST.getFieldType(D, M), M);
      }
      
      Pointer = SEAI->getOperand();
      continue;
    }

    
    assert(isa<InitExistentialAddrInst>(Pointer) &&
           "Unknown access path instruction");
    // Cannot promote loads and stores from within an existential projection.
    return ~0U;
  }
}

//===----------------------------------------------------------------------===//
//                              Available Value
//===----------------------------------------------------------------------===//

namespace {

class AvailableValueAggregator;

struct AvailableValue {
  friend class AvailableValueAggregator;

  /// If this gets too expensive in terms of copying, we can use an arena and a
  /// FrozenPtrSet like we do in ARC.
  using SetVector = llvm::SmallSetVector<SILInstruction *, 1>;

  SILValue Value;
  unsigned SubElementNumber;
  SetVector InsertionPoints;

  /// Just for updating.
  SmallVectorImpl<DIMemoryUse> *Uses;

public:
  AvailableValue() = default;

  /// Main initializer for available values.
  ///
  /// *NOTE* We assume that all available values start with a singular insertion
  /// point and insertion points are added by merging.
  AvailableValue(SILValue Value, unsigned SubElementNumber,
                 SILInstruction *InsertPoint)
      : Value(Value), SubElementNumber(SubElementNumber), InsertionPoints() {
    InsertionPoints.insert(InsertPoint);
  }

  /// Deleted copy constructor. This is a move only type.
  AvailableValue(const AvailableValue &) = delete;

  /// Deleted copy operator. This is a move only type.
  AvailableValue &operator=(const AvailableValue &) = delete;

  /// Move constructor.
  AvailableValue(AvailableValue &&Other)
      : Value(nullptr), SubElementNumber(~0), InsertionPoints() {
    std::swap(Value, Other.Value);
    std::swap(SubElementNumber, Other.SubElementNumber);
    std::swap(InsertionPoints, Other.InsertionPoints);
  }

  /// Move operator.
  AvailableValue &operator=(AvailableValue &&Other) {
    std::swap(Value, Other.Value);
    std::swap(SubElementNumber, Other.SubElementNumber);
    std::swap(InsertionPoints, Other.InsertionPoints);
    return *this;
  }

  operator bool() const { return bool(Value); }

  bool operator==(const AvailableValue &Other) const {
    return Value == Other.Value && SubElementNumber == Other.SubElementNumber;
  }

  bool operator!=(const AvailableValue &Other) const {
    return !(*this == Other);
  }

  SILValue getValue() const { return Value; }
  SILType getType() const { return Value->getType(); }
  unsigned getSubElementNumber() const { return SubElementNumber; }
  ArrayRef<SILInstruction *> getInsertionPoints() const {
    return InsertionPoints.getArrayRef();
  }

  void mergeInsertionPoints(const AvailableValue &Other) & {
    assert(Value == Other.Value && SubElementNumber == Other.SubElementNumber);
    InsertionPoints.set_union(Other.InsertionPoints);
  }

  void addInsertionPoint(SILInstruction *I) & { InsertionPoints.insert(I); }

  /// Return a new Available Value, for a projection. We still have the same
  /// insertion points thought.
  AvailableValue withProjection(SILValue NewValue,
                                unsigned NewSubEltNumber) const {
    return {NewValue, NewSubEltNumber, InsertionPoints};
  }

  /// Return a new available value with the same sub element number/insertion
  /// points, but with a new value.
  AvailableValue withReplacement(SILValue NewValue) const {
    return {NewValue, SubElementNumber, InsertionPoints};
  }

  void dump() const __attribute__((used));
  void print(llvm::raw_ostream &os) const;

private:
  /// Private constructor.
  AvailableValue(SILValue Value, unsigned SubElementNumber,
                 const SetVector &InsertPoints)
      : Value(Value), SubElementNumber(SubElementNumber),
        InsertionPoints(InsertPoints) {}
};

} // end anonymous namespace

void AvailableValue::dump() const { print(llvm::dbgs()); }

void AvailableValue::print(llvm::raw_ostream &os) const {
  os << "Available Value Dump. Value: ";
  if (getValue()) {
    os << getValue();
  } else {
    os << "NoValue;\n";
  }
  os << "SubElementNumber: " << getSubElementNumber() << "\n";
  os << "Insertion Points:\n";
  for (auto *I : getInsertionPoints()) {
    os << *I;
  }
}

namespace llvm {

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const AvailableValue &V) {
  V.print(os);
  return os;
}

} // end llvm namespace

//===----------------------------------------------------------------------===//
//                      Compensation Block Finding Code
//===----------------------------------------------------------------------===//

static void
findCompensationBlocks(SILInstruction *Load,
                       ArrayRef<SILInstruction *> InsertPts,
                       llvm::SmallVectorImpl<SILBasicBlock *> &Result) {
  // If we have one insert pt and that one insert pt and the load are in the
  // same block, we do not need to insert any compensation code. Just return.
  if (InsertPts.size() == 1 && Load->getParent() == InsertPts[0]->getParent())
    return;

  llvm::SmallPtrSet<SILBasicBlock *, 8> InsertPtBlocks;
  for (auto *I : InsertPts) {
    InsertPtBlocks.insert(I->getParent());
  }

  llvm::SmallVector<SILBasicBlock *, 32> Worklist;
  llvm::SmallPtrSet<SILBasicBlock *, 32> VisitedBlocks;
  llvm::SmallSetVector<SILBasicBlock *, 8> MustVisitBlocks;

  VisitedBlocks.insert(Load->getParent());
  for (auto *PredBB : Load->getParent()->getPredecessorBlocks()) {
    Worklist.push_back(PredBB);
    VisitedBlocks.insert(PredBB);
  }

  while (!Worklist.empty()) {
    auto *Block = Worklist.pop_back_val();

    // Otherwise, remove the block from MustVisitBlocks if it is in there.
    MustVisitBlocks.remove(Block);

    // Then add each successor block of Block that has not been visited yet to
    // the MustVisitBlocks set.
    for (auto *SuccBB : Block->getSuccessorBlocks()) {
      if (!VisitedBlocks.count(SuccBB)) {
        MustVisitBlocks.insert(SuccBB);
      }
    }

    // Then if this is one of our insertion blocks, continue so we do not keep
    // visiting predecessors.
    if (InsertPtBlocks.count(Block)) {
// In debug builds, remove block so we can verify that we found all of our
// insert pts.
#ifndef NDEBUG
      InsertPtBlocks.erase(Block);
#endif
      continue;
    }

    // And then add all unvisited predecessors to the worklist.
    copy_if(Block->getPredecessorBlocks(), std::back_inserter(Worklist),
            [&](SILBasicBlock *Block) -> bool {
              return VisitedBlocks.insert(Block).second;
            });
  }

  assert(InsertPtBlocks.empty() && "Failed to find all insert pt blocks?!");
  // Now that we are done, add all remaining must visit blocks to our result
  // list. These are the places where we must insert compensating code.
  copy(MustVisitBlocks, std::back_inserter(Result));
}

//===----------------------------------------------------------------------===//
//                           Subelement Extraction
//===----------------------------------------------------------------------===//

/// Given an aggregate value and an access path, non-destructively extract the
/// value indicated by the path.
static SILValue nonDestructivelyExtractSubElement(const AvailableValue &Val,
                                                  SILBuilder &B,
                                                  SILLocation Loc) {
  SILType ValTy = Val.getType();
  unsigned SubElementNumber = Val.getSubElementNumber();

  // Extract tuple elements.
  if (auto TT = ValTy.getAs<TupleType>()) {
    for (unsigned EltNo : indices(TT.getElementTypes())) {
      // Keep track of what subelement is being referenced.
      SILType EltTy = ValTy.getTupleElementType(EltNo);
      unsigned NumSubElt = getNumSubElements(EltTy, B.getModule());
      if (SubElementNumber < NumSubElt) {
        SILValue Ext = B.emitTupleExtract(Loc, Val.getValue(), EltNo);
        auto NewVal = Val.withProjection(Ext, SubElementNumber);
        return nonDestructivelyExtractSubElement(NewVal, B, Loc);
      }
      
      SubElementNumber -= NumSubElt;
    }
    
    llvm_unreachable("Didn't find field");
  }
  
  // Extract struct elements.
  if (auto *SD = getFullyReferenceableStruct(ValTy)) {
    for (auto *D : SD->getStoredProperties()) {
      auto fieldType = ValTy.getFieldType(D, B.getModule());
      unsigned NumSubElt = getNumSubElements(fieldType, B.getModule());
      
      if (SubElementNumber < NumSubElt) {
        SILValue Ext = B.emitStructExtract(Loc, Val.getValue(), D);
        auto NewVal = Val.withProjection(Ext, SubElementNumber);
        return nonDestructivelyExtractSubElement(NewVal, B, Loc);
      }
      
      SubElementNumber -= NumSubElt;
      
    }
    llvm_unreachable("Didn't find field");
  }
  
  // Otherwise, we're down to a scalar.
  assert(SubElementNumber == 0 && "Miscalculation indexing subelements");
  return Val.getValue();
}

namespace {

/// Given an aggregate value and an access path, extract the value indicated by
/// the path updating AvailableValues as we go. This ensures that the remaining
/// values that we produce from the destructure are available if we are looping
/// around gathering available values for an aggregate.
class DestructiveSubElementExtractor {
  AllocationInst *TheMemory;
  SILBuilder &B;
  SILLocation Loc;

  MutableArrayRef<AvailableValue> AvailableValueList;

public:
  DestructiveSubElementExtractor(
      AllocationInst *TheMemory, SILBuilder &B, SILLocation Loc,
      MutableArrayRef<AvailableValue> AvailableValueList)
      : TheMemory(TheMemory), B(B), Loc(Loc),
        AvailableValueList(AvailableValueList) {}

  SILValue extract(const AvailableValue &Val);

private:
  AvailableValue extractStructSubElement(const AvailableValue &Val,
                                         StructDecl *SD);
  AvailableValue extractTupleSubElement(const AvailableValue &Val,
                                        TupleType *TT);

  /// Given new destructure operations, update AvailableValues so that any items
  /// pointing at subtypes of the aggregate now point at the destructured
  /// results instead.
  void updateAvailableValues(const AvailableValue &Val,
                             ArrayRef<SILValue> DestructuredAggregate,
                             unsigned EltIndex);

  /// Has ownership been stripped out of the current function.
  bool isOwnershipEnabled() const {
    return TheMemory->getFunction()->hasQualifiedOwnership();
  }

  /// Given a tuple or a struct aggregate, destructure the value into its
  /// constituant parts.
  void destructureAggregate(SILValue Aggregate, SILLocation Loc,
                            llvm::SmallVectorImpl<SILValue> &Results);
};

} // end anonymous namespace

void DestructiveSubElementExtractor::destructureAggregate(
    SILValue Aggregate, SILLocation Loc,
    llvm::SmallVectorImpl<SILValue> &Results) {
  // If ownership is not enabled, we use individual extracts. Otherwise, we use
  // /real/ destructure operations.
  if (!isOwnershipEnabled()) {
    llvm::SmallVector<Projection, 8> Projections;
    Projection::getFirstLevelProjections(Aggregate->getType(),
                                         TheMemory->getModule(), Projections);
    transform(Projections, std::back_inserter(Results),
              [this, &Loc, &Aggregate](Projection &P) -> SILValue {
                return P.createObjectProjection(B, Loc, Aggregate).get();
              });
    return;
  }

  MultipleValueInstruction *MVI = nullptr;
  if (Aggregate->getType().is<TupleType>()) {
    MVI = B.createDestructureTuple(Loc, Aggregate);
  } else {
    assert(Aggregate->getType().getStructOrBoundGenericStruct() &&
           "Should have either a struct or a tuple here.");
    MVI = B.createDestructureStruct(Loc, Aggregate);
  }
  copy(MVI->getResults(), std::back_inserter(Results));
}

AvailableValue DestructiveSubElementExtractor::extractTupleSubElement(
    const AvailableValue &Agg, TupleType *TT) {
  llvm::SmallVector<SILValue, 8> DestructuredValues;
  unsigned SubElementNumber = Agg.getSubElementNumber();

  for (unsigned EltNo : indices(TT->getElementTypes())) {
    DestructuredValues.clear();

    // Keep track of what subelement is being referenced.
    SILType EltTy = Agg.getType().getTupleElementType(EltNo);
    unsigned NumSubElt = getNumSubElements(EltTy, B.getModule());
    if (SubElementNumber >= NumSubElt) {
      SubElementNumber -= NumSubElt;
      continue;
    }

    destructureAggregate(Agg.getValue(), Loc, DestructuredValues);
    updateAvailableValues(Agg, DestructuredValues, EltNo);
    return Agg.withProjection(DestructuredValues[EltNo], SubElementNumber);
  }

  llvm_unreachable("Didn't find field");
}

AvailableValue DestructiveSubElementExtractor::extractStructSubElement(
    const AvailableValue &Agg, StructDecl *SD) {
  llvm::SmallVector<SILValue, 8> DestructuredValues;
  unsigned SubElementNumber = Agg.getSubElementNumber();
  ;
  unsigned EltNo = 0;
  for (VarDecl *D : SD->getStoredProperties()) {
    DestructuredValues.clear();

    auto fieldType = Agg.getType().getFieldType(D, B.getModule());
    unsigned NumSubElt = getNumSubElements(fieldType, B.getModule());
    if (SubElementNumber >= NumSubElt) {
      SubElementNumber -= NumSubElt;
      ++EltNo;
      continue;
    }

    destructureAggregate(Agg.getValue(), Loc, DestructuredValues);
    updateAvailableValues(Agg, DestructuredValues, EltNo);
    return Agg.withProjection(DestructuredValues[EltNo], SubElementNumber);
  }
  llvm_unreachable("Didn't find field");
}

SILValue
DestructiveSubElementExtractor::extract(const AvailableValue &InputVal) {
  // We know that all uses of InputVal will use this value non-destructively
  // beyond our re-assignment of the loop induction variable. So this is safe to
  // do.
  AvailableValue *Val = const_cast<AvailableValue *>(&InputVal);
  Optional<AvailableValue> NewVal;

  while (true) {
    if (NewVal) {
      Val = &NewVal.getValue();
    }

    // This dynamically skips the first ieration.
    SILType AggTy = Val->getType();
    // Extract tuple elements.
    if (auto TT = AggTy.getAs<TupleType>()) {
      NewVal = extractTupleSubElement(*Val, TT);
      continue;
    }

    // Extract struct elements.
    if (auto *SD = getFullyReferenceableStruct(AggTy)) {
      NewVal = extractStructSubElement(*Val, SD);
      continue;
    }

    // Otherwise, we're down to a scalar.
    assert(Val->getSubElementNumber() == 0 &&
           "Miscalculation indexing subelements");
    return Val->getValue();
  }

  llvm_unreachable("Should never hit this");
}

void DestructiveSubElementExtractor::updateAvailableValues(
    const AvailableValue &Val, ArrayRef<SILValue> DestructuredAggregate,
    unsigned EltNo) {
  // Then for each leaf child element of the struct, add the new value.
  unsigned NumSubElts =
      getNumSubElements(Val.getType(), TheMemory->getModule());
  for (unsigned i : range(NumSubElts)) {
    auto &SubVal = AvailableValueList[Val.getSubElementNumber() + i];
    assert(SubVal.getValue() != nullptr &&
           "Since we are destructuring an already "
           "loaded value, so we should have /some/ "
           "value here");
    SubVal.getValue()->replaceAllUsesWith(DestructuredAggregate[i]);
    SubVal = Val.withProjection(DestructuredAggregate[i], i);
  }
}

//===----------------------------------------------------------------------===//
//                        Available Value Aggregation
//===----------------------------------------------------------------------===//

static bool anyMissing(unsigned StartSubElt, unsigned NumSubElts,
                       ArrayRef<AvailableValue> &Values) {
  while (NumSubElts) {
    if (!Values[StartSubElt])
      return true;
    ++StartSubElt;
    --NumSubElts;
  }
  return false;
}

namespace {

/// A class that aggregates available values, loading them if they are not
/// available.
class AvailableValueAggregator {
  SILModule &M;
  SILBuilderWithScope B;
  SILLocation Loc;
  SILInstruction *Inst;
  AllocationInst *TheMemory;
  LoadOwnershipQualifier Qual;
  MutableArrayRef<AvailableValue> AvailableValueList;
  SmallVectorImpl<DIMemoryUse> &Uses;

public:
  AvailableValueAggregator(AllocationInst *TheMemory, SILInstruction *Inst,
                           LoadOwnershipQualifier Qual,
                           MutableArrayRef<AvailableValue> AvailableValueList,
                           SmallVectorImpl<DIMemoryUse> &Uses)
      : M(Inst->getModule()), B(Inst), Loc(Inst->getLoc()), Inst(Inst),
        TheMemory(TheMemory), Qual(Qual),
        AvailableValueList(AvailableValueList), Uses(Uses) {}

  // This is intended to be passed by reference only once constructed.
  AvailableValueAggregator(const AvailableValueAggregator &) = delete;
  AvailableValueAggregator(AvailableValueAggregator &&) = delete;
  AvailableValueAggregator &
  operator=(const AvailableValueAggregator &) = delete;
  AvailableValueAggregator &operator=(AvailableValueAggregator &&) = delete;

  SILValue aggregateValues(SILType LoadTy, SILValue Address, unsigned FirstElt);

  void print(llvm::raw_ostream &os) const;
  void dump() const __attribute__((used));

private:
  bool hasOwnership() const { return B.getFunction().hasQualifiedOwnership(); }

  bool isAggregatingForTake() const {
    switch (Qual) {
    case LoadOwnershipQualifier::Take:
      return true;
    case LoadOwnershipQualifier::Copy:
    case LoadOwnershipQualifier::Trivial:
    case LoadOwnershipQualifier::Unqualified:
      return false;
    }
  }

  SILValue aggregateFullyAvailableValue(SILType LoadTy, unsigned FirstElt);
  SILValue aggregateTupleSubElts(TupleType *TT, SILType LoadTy,
                                 SILValue Address, unsigned FirstElt);
  SILValue aggregateStructSubElts(StructDecl *SD, SILType LoadTy,
                                  SILValue Address, unsigned FirstElt);
  SILValue handlePrimitiveValue(SILType LoadTy, SILValue Address,
                                unsigned FirstElt);
  SILValue handlePrimitiveValueNonDestructively(const AvailableValue &Val,
                                                SILType LoadTy);
  SILValue handlePrimitiveValueDestructively(const AvailableValue &Val,
                                             SILType LoadTy);
};

} // end anonymous namespace

void AvailableValueAggregator::dump() const { print(llvm::dbgs()); }

void AvailableValueAggregator::print(llvm::raw_ostream &os) const {
  os << "Available Value List, N = " << AvailableValueList.size()
     << ". Elts:\n";
  for (auto &V : AvailableValueList) {
    os << V;
  }
}

/// Given a bunch of primitive subelement values, build out the right aggregate
/// type (LoadTy) by emitting tuple and struct instructions as necessary.
SILValue AvailableValueAggregator::aggregateValues(SILType LoadTy,
                                                   SILValue Address,
                                                   unsigned FirstElt) {
  // Check to see if the requested value is fully available, as an aggregate.
  // This is a super-common case for single-element structs, but is also a
  // general answer for arbitrary structs and tuples as well.
  if (SILValue Result = aggregateFullyAvailableValue(LoadTy, FirstElt))
    return Result;

  // If we have a tuple type, then aggregate the tuple's elements into a full
  // tuple value.
  if (TupleType *TT = LoadTy.getAs<TupleType>())
    return aggregateTupleSubElts(TT, LoadTy, Address, FirstElt);

  // If we have a struct type, then aggregate the struct's elements into a full
  // struct value.
  if (auto *SD = getFullyReferenceableStruct(LoadTy))
    return aggregateStructSubElts(SD, LoadTy, Address, FirstElt);

  // Otherwise, we have a non-aggregate primitive. Load or extract the value.
  return handlePrimitiveValue(LoadTy, Address, FirstElt);
}

// See if we have this value is fully available. In such a case, return it as an
// aggregate. This is a super-common case for single-element structs, but is
// also a general answer for arbitrary structs and tuples as well.
SILValue
AvailableValueAggregator::aggregateFullyAvailableValue(SILType LoadTy,
                                                       unsigned FirstElt) {
  if (FirstElt >= AvailableValueList.size()) { // #Elements may be zero.
    return SILValue();
  }

  auto &FirstVal = AvailableValueList[FirstElt];

  // Make sure that the first element is available and is the correct type.
  if (!FirstVal || FirstVal.getSubElementNumber() != 0 ||
      FirstVal.getType() != LoadTy)
    return SILValue();

  // If the first element of this value is available, check that any extra
  // available values are from the same place as our first value.
  if (llvm::any_of(range(getNumSubElements(LoadTy, M)),
                   [&](unsigned Index) -> bool {
                     auto &Val = AvailableValueList[FirstElt + Index];
                     return Val.getValue() != FirstVal.getValue() ||
                            Val.getSubElementNumber() != Index;
                   }))
    return SILValue();

  // Ok, we have a fully available value! If we do not have ownership or we are
  // propagating a take, then just return the value.
  if (!hasOwnership() || isAggregatingForTake())
    return FirstVal.getValue();

  // On the other hand, if we have ownership, then we need to emit copies before
  // each insertion point and insert compensating destroys where we do not have
  // a load.
  ArrayRef<SILInstruction *> InsertPts = FirstVal.getInsertionPoints();

  llvm::SmallVector<SILBasicBlock *, 8> CompensatingBlocks;
  findCompensationBlocks(Inst, InsertPts, CompensatingBlocks);

  assert(!InsertPts.empty());
  if (InsertPts.size() == 1) {
    SILValue CopiedVal;
    {
      SavedInsertionPointRAII SavedInsertPt(B, InsertPts[0]);
      CopiedVal = B.emitCopyValueOperation(Loc, FirstVal.getValue());
    }

    for (auto *Block : CompensatingBlocks) {
      SavedInsertionPointRAII SavedInsertPt(B, &*Block->begin());
      B.emitDestroyValueOperation(Loc, CopiedVal);
    }

    return CopiedVal;
  }

  SILSSAUpdater Updater;
  Updater.Initialize(LoadTy);
  for (auto *I : InsertPts) {
    SavedInsertionPointRAII SavedInsertPt(B, I);
    SILValue Value = B.emitCopyValueOperation(Loc, FirstVal.getValue());
    Updater.AddAvailableValue(I->getParent(), Value);
  }

  // Now add compensating destroys.
  for (auto *Block : CompensatingBlocks) {
    SILValue V = Updater.GetValueInMiddleOfBlock(Block);
    SavedInsertionPointRAII SavedInsertPt(B, &*Block->begin());
    B.emitDestroyValueOperation(Loc, V);
  }

  return Updater.GetValueInMiddleOfBlock(B.getInsertionBB());
}

SILValue AvailableValueAggregator::aggregateTupleSubElts(TupleType *TT,
                                                         SILType LoadTy,
                                                         SILValue Address,
                                                         unsigned FirstElt) {
  SmallVector<SILValue, 4> ResultElts;

  for (unsigned EltNo : indices(TT->getElements())) {
    SILType EltTy = LoadTy.getTupleElementType(EltNo);
    unsigned NumSubElt = getNumSubElements(EltTy, M);

    // If we are missing any of the available values in this struct element,
    // compute an address to load from.
    SILValue EltAddr;
    if (anyMissing(FirstElt, NumSubElt, AvailableValueList))
      EltAddr =
          B.createTupleElementAddr(Loc, Address, EltNo, EltTy.getAddressType());

    ResultElts.push_back(aggregateValues(EltTy, EltAddr, FirstElt));
    FirstElt += NumSubElt;
  }

  return B.createTuple(Loc, LoadTy, ResultElts);
}

SILValue AvailableValueAggregator::aggregateStructSubElts(StructDecl *SD,
                                                          SILType LoadTy,
                                                          SILValue Address,
                                                          unsigned FirstElt) {
  SmallVector<SILValue, 4> ResultElts;

  for (auto *FD : SD->getStoredProperties()) {
    SILType EltTy = LoadTy.getFieldType(FD, M);
    unsigned NumSubElt = getNumSubElements(EltTy, M);

    // If we are missing any of the available values in this struct element,
    // compute an address to load from.
    SILValue EltAddr;
    if (anyMissing(FirstElt, NumSubElt, AvailableValueList))
      EltAddr =
          B.createStructElementAddr(Loc, Address, FD, EltTy.getAddressType());

    ResultElts.push_back(aggregateValues(EltTy, EltAddr, FirstElt));
    FirstElt += NumSubElt;
  }
  return B.createStruct(Loc, LoadTy, ResultElts);
}

SILValue AvailableValueAggregator::handlePrimitiveValueNonDestructively(
    const AvailableValue &Val, SILType LoadTy) {
  // If we have 1 insertion point, just extract the value and return.
  //
  // This saves us from having to spend compile time in the SSA updater in this
  // case.
  ArrayRef<SILInstruction *> InsertPts = Val.getInsertionPoints();
  llvm::SmallVector<SILBasicBlock *, 8> CompensatingBlocks;
  findCompensationBlocks(Inst, InsertPts, CompensatingBlocks);

  if (InsertPts.size() == 1) {
    SavedInsertionPointRAII SavedInsertPt(B, InsertPts[0]);

    SILValue Value = Val.getValue();
    if (B.getFunction().hasQualifiedOwnership() &&
        !Value->getType().isTrivial(M)) {
      Value = B.createBeginBorrow(Loc, Value);
    }

    SILValue EltVal =
        nonDestructivelyExtractSubElement(Val.withReplacement(Value), B, Loc);
    assert(EltVal->getType() == LoadTy && "Subelement types mismatch");

    if (B.getFunction().hasQualifiedOwnership() &&
        !Value->getType().isTrivial(M)) {
      if (Qual == LoadOwnershipQualifier::Copy) {
        EltVal = B.emitCopyValueOperation(Loc, EltVal);

        for (auto *Block : CompensatingBlocks) {
          SavedInsertionPointRAII SavedInsertPt(B, &*Block->begin());
          B.emitDestroyValueOperation(Loc, EltVal);
        }
      }

      // And insert the end_borrow.
      B.emitEndBorrowOperation(Loc, Value, Val.getValue());
    }

    return EltVal;
  }

  // If we have an available value, then we want to extract the subelement from
  // the borrowed aggregate before each insertion point.
  SILSSAUpdater Updater;
  Updater.Initialize(LoadTy);
  for (auto *I : Val.getInsertionPoints()) {
    SavedInsertionPointRAII SavedInsertPt(B, I);

    SILValue Value = Val.getValue();
    if (B.getFunction().hasQualifiedOwnership() &&
        !Value->getType().isTrivial(M)) {
      Value = B.createBeginBorrow(Loc, Value);
    }

    SILValue EltVal =
        nonDestructivelyExtractSubElement(Val.withReplacement(Value), B, Loc);
    assert(EltVal->getType() == LoadTy && "Subelement types mismatch");

    if (B.getFunction().hasQualifiedOwnership() &&
        !Value->getType().isTrivial(M)) {
      if (Qual == LoadOwnershipQualifier::Copy) {
        EltVal = B.emitCopyValueOperation(Loc, EltVal);
      }
      // And insert the end_borrow.
      B.emitEndBorrowOperation(Loc, Value, Val.getValue());
    }
    Updater.AddAvailableValue(I->getParent(), EltVal);
  }

  // Now add compensating destroys.
  for (auto *Block : CompensatingBlocks) {
    SILValue V = Updater.GetValueInMiddleOfBlock(Block);
    SavedInsertionPointRAII SavedInsertPt(B, &*Block->begin());
    B.emitDestroyValueOperation(Loc, V);
  }

  // Finally, grab the value from the SSA updater.
  SILValue EltVal = Updater.GetValueInMiddleOfBlock(B.getInsertionBB());
  assert(EltVal->getType() == LoadTy && "Subelement types mismatch");
  return EltVal;
}

SILValue AvailableValueAggregator::handlePrimitiveValueDestructively(
    const AvailableValue &Val, SILType LoadTy) {
  // If we have 1 insertion point, just extract the value and return.
  //
  // This saves us from having to spend compile time in the SSA updater in this
  // case.
  ArrayRef<SILInstruction *> InsertPts = Val.getInsertionPoints();
  if (InsertPts.size() == 1) {
    SavedInsertionPointRAII SavedInsertPt(B, InsertPts[0]);
    DestructiveSubElementExtractor Extractor(TheMemory, B, Loc,
                                             AvailableValueList);
    SILValue EltVal = Extractor.extract(Val);
    assert(EltVal->getType() == LoadTy && "Subelement types mismatch");
    return EltVal;
  }

  // If we have an available value, then we want to extract the subelement from
  // the borrowed aggregate before each insertion point.
  SILSSAUpdater Updater;
  Updater.Initialize(LoadTy);
  for (auto *I : Val.getInsertionPoints()) {
    SavedInsertionPointRAII SavedInsertPt(B, I);
    DestructiveSubElementExtractor Extractor(TheMemory, B, Loc,
                                             AvailableValueList);
    SILValue EltVal = Extractor.extract(Val);
    Updater.AddAvailableValue(I->getParent(), EltVal);
  }

  // Finally, grab the value from the SSA updater.
  SILValue EltVal = Updater.GetValueInMiddleOfBlock(B.getInsertionBB());
  assert(EltVal->getType() == LoadTy && "Subelement types mismatch");
  return EltVal;
}

// We have looked through all of the aggregate values and finally found a
// "primitive value". If the value is available, use it (extracting if we need
// to), otherwise emit a load of the value with the appropriate qualifier.
SILValue AvailableValueAggregator::handlePrimitiveValue(SILType LoadTy,
                                                        SILValue Address,
                                                        unsigned FirstElt) {
  auto &Val = AvailableValueList[FirstElt];

  // If the value is not available, load the value and update our use list.
  if (!Val) {
    LoadInst *LI = nullptr;
    if (B.getFunction().hasUnqualifiedOwnership()) {
      LI = B.createLoad(Loc, Address, LoadOwnershipQualifier::Unqualified);
    } else {
      LI = B.createTrivialLoadOr(Loc, Address, Qual);
    }
    assert(LI);

    Uses.push_back(DIMemoryUse(LI, DIUseKind::Load, FirstElt,
                               getNumSubElements(LI->getType(), M)));
    return LI;
  }

  if (!hasOwnership() || !isAggregatingForTake()) {
    SILValue EltVal = handlePrimitiveValueNonDestructively(Val, LoadTy);
    assert(EltVal->getType() == LoadTy && "Subelement types mismatch");
    return EltVal;
  }

  // If we are supposed to be performing a take, destructure the value. We
  // update the available values of the rest of the destructured elements, so
  // this destructuring will only occur once. The 2nd time around, we will just
  // use the newly available destructured values.
  assert(Qual == LoadOwnershipQualifier::Take);
  SILValue EltVal = handlePrimitiveValueDestructively(Val, LoadTy);
  assert(EltVal->getType() == LoadTy && "Subelement types mismatch");
  return EltVal;
}

//===----------------------------------------------------------------------===//
//                          Allocation Optimization
//===----------------------------------------------------------------------===//

namespace {

/// This performs load promotion and deletes synthesized allocations if all
/// loads can be removed.
class AllocOptimize {

  SILModule &Module;

  /// This is either an alloc_box or alloc_stack instruction.
  AllocationInst *TheMemory;

  /// This is the SILType of the memory object.
  SILType MemoryType;

  /// The number of primitive subelements across all elements of this memory
  /// value.
  unsigned NumMemorySubElements;

  SmallVectorImpl<DIMemoryUse> &Uses;
  SmallVectorImpl<SILInstruction *> &Releases;

  llvm::SmallPtrSet<SILBasicBlock *, 32> HasLocalDefinition;

  /// This is a map of uses that are not loads (i.e., they are Stores,
  /// InOutUses, and Escapes), to their entry in Uses.
  llvm::SmallDenseMap<SILInstruction *, unsigned, 16> NonLoadUses;

  /// Does this value escape anywhere in the function.
  bool HasAnyEscape = false;

public:
  AllocOptimize(AllocationInst *TheMemory, SmallVectorImpl<DIMemoryUse> &Uses,
                SmallVectorImpl<SILInstruction *> &Releases);

  bool doIt();

private:
  bool promoteLoad(SILInstruction *Inst);
  void promoteDestroyAddr(DestroyAddrInst *DAI,
                          MutableArrayRef<AvailableValue> Values);
  bool
  canPromoteDestroyAddr(DestroyAddrInst *DAI,
                        llvm::SmallVectorImpl<AvailableValue> &AvailableValues);

  // Load promotion.
  bool hasEscapedAt(SILInstruction *I);
  void updateAvailableValues(SILInstruction *Inst,
                             llvm::SmallBitVector &RequiredElts,
                             SmallVectorImpl<AvailableValue> &Result,
                             llvm::SmallBitVector &ConflictingValues);
  void computeAvailableValues(SILInstruction *StartingFrom,
                              llvm::SmallBitVector &RequiredElts,
                              SmallVectorImpl<AvailableValue> &Result);
  void computeAvailableValuesFrom(
      SILBasicBlock::iterator StartingFrom, SILBasicBlock *BB,
      llvm::SmallBitVector &RequiredElts,
      SmallVectorImpl<AvailableValue> &Result,
      llvm::SmallDenseMap<SILBasicBlock *, llvm::SmallBitVector, 32>
          &VisitedBlocks,
      llvm::SmallBitVector &ConflictingValues);

  void explodeCopyAddr(CopyAddrInst *CAI);

  bool tryToRemoveDeadAllocation();
};

} // end anonymous namespace


AllocOptimize::AllocOptimize(AllocationInst *TheMemory,
                             SmallVectorImpl<DIMemoryUse> &Uses,
                             SmallVectorImpl<SILInstruction*> &Releases)
: Module(TheMemory->getModule()), TheMemory(TheMemory), Uses(Uses),
  Releases(Releases) {
  
  // Compute the type of the memory object.
  if (auto *ABI = dyn_cast<AllocBoxInst>(TheMemory)) {
    assert(ABI->getBoxType()->getLayout()->getFields().size() == 1
           && "optimizing multi-field boxes not implemented");
    MemoryType = ABI->getBoxType()->getFieldType(ABI->getModule(), 0);
  } else {
    assert(isa<AllocStackInst>(TheMemory));
    MemoryType = cast<AllocStackInst>(TheMemory)->getElementType();
  }
  
  NumMemorySubElements = getNumSubElements(MemoryType, Module);
  
  // The first step of processing an element is to collect information about the
  // element into data structures we use later.
  for (unsigned ui = 0, e = Uses.size(); ui != e; ++ui) {
    auto &Use = Uses[ui];
    assert(Use.Inst && "No instruction identified?");
    
    // Keep track of all the uses that aren't loads.
    if (Use.Kind == DIUseKind::Load)
      continue;
    
    NonLoadUses[Use.Inst] = ui;
    
    HasLocalDefinition.insert(Use.Inst->getParent());
    
    if (Use.Kind == DIUseKind::Escape) {
      // Determine which blocks the value can escape from.  We aren't allowed to
      // promote loads in blocks reachable from an escape point.
      HasAnyEscape = true;
    }
  }
  
  // If isn't really a use, but we account for the alloc_box/mark_uninitialized
  // as a use so we see it in our dataflow walks.
  NonLoadUses[TheMemory] = ~0U;
  HasLocalDefinition.insert(TheMemory->getParent());
}


/// hasEscapedAt - Return true if the box has escaped at the specified
/// instruction.  We are not allowed to do load promotion in an escape region.
bool AllocOptimize::hasEscapedAt(SILInstruction *I) {
  // FIXME: This is not an aggressive implementation.  :)
  
  // TODO: At some point, we should special case closures that just *read* from
  // the escaped value (by looking at the body of the closure).  They should not
  // prevent load promotion, and will allow promoting values like X in regions
  // dominated by "... && X != 0".
  return HasAnyEscape;
}

/// The specified instruction is a non-load access of the element being
/// promoted.  See if it provides a value or refines the demanded element mask
/// used for load promotion.
void AllocOptimize::updateAvailableValues(
    SILInstruction *Inst, llvm::SmallBitVector &RequiredElts,
    SmallVectorImpl<AvailableValue> &Result,
    llvm::SmallBitVector &ConflictingValues) {

  // Handle store and assign.
  if (auto *SI = dyn_cast<StoreInst>(Inst)) {
    unsigned StartSubElt = computeSubelement(SI->getDest(), TheMemory);
    assert(StartSubElt != ~0U && "Store within enum projection not handled");
    SILType ValTy = SI->getSrc()->getType();

    for (unsigned i = 0, e = getNumSubElements(ValTy, Module); i != e; ++i) {
      // If this element is not required, don't fill it in.
      if (!RequiredElts[StartSubElt+i]) continue;
      
      // If there is no result computed for this subelement, record it.  If
      // there already is a result, check it for conflict.  If there is no
      // conflict, then we're ok.
      auto &Entry = Result[StartSubElt+i];
      if (!Entry) {
        Entry = {SI->getSrc(), i, Inst};
      } else {
        // TODO: This is /really/, /really/, conservative. This basically means
        // that if we do not have an identical store, we will not promote.
        if (Entry.getValue() != SI->getSrc() ||
            Entry.getSubElementNumber() != i) {
          ConflictingValues[StartSubElt + i] = true;
        } else {
          Entry.addInsertionPoint(Inst);
        }
      }

      // This element is now provided.
      RequiredElts[StartSubElt+i] = false;
    }
    
    return;
  }
  
  // If we get here with a copy_addr, it must be storing into the element. Check
  // to see if any loaded subelements are being used, and if so, explode the
  // copy_addr to its individual pieces.
  if (auto *CAI = dyn_cast<CopyAddrInst>(Inst)) {
    unsigned StartSubElt = computeSubelement(Inst->getOperand(1), TheMemory);
    assert(StartSubElt != ~0U && "Store within enum projection not handled");
    SILType ValTy = Inst->getOperand(1)->getType();
    
    bool AnyRequired = false;
    for (unsigned i = 0, e = getNumSubElements(ValTy, Module); i != e; ++i) {
      // If this element is not required, don't fill it in.
      AnyRequired = RequiredElts[StartSubElt+i];
      if (AnyRequired) break;
    }
    
    // If this is a copy addr that doesn't intersect the loaded subelements,
    // just continue with an unmodified load mask.
    if (!AnyRequired)
      return;
    
    // If the copyaddr is of a non-loadable type, we can't promote it.  Just
    // consider it to be a clobber.
    if (CAI->getOperand(0)->getType().isLoadable(Module)) {
      // Otherwise, some part of the copy_addr's value is demanded by a load, so
      // we need to explode it to its component pieces.  This only expands one
      // level of the copyaddr.
      explodeCopyAddr(CAI);
      
      // The copy_addr doesn't provide any values, but we've arranged for our
      // iterators to visit the newly generated instructions, which do.
      return;
    }
  }

  // TODO: inout apply's should only clobber pieces passed in.
  
  // Otherwise, this is some unknown instruction, conservatively assume that all
  // values are clobbered.
  RequiredElts.clear();
  ConflictingValues = llvm::SmallBitVector(Result.size(), true);
  return;
}


/// Try to find available values of a set of subelements of the current value,
/// starting right before the specified instruction.
///
/// The bitvector indicates which subelements we're interested in, and result
/// captures the available value (plus an indicator of which subelement of that
/// value is needed).
///
void AllocOptimize::computeAvailableValues(
    SILInstruction *StartingFrom, llvm::SmallBitVector &RequiredElts,
    SmallVectorImpl<AvailableValue> &Result) {
  llvm::SmallDenseMap<SILBasicBlock*, llvm::SmallBitVector, 32> VisitedBlocks;
  llvm::SmallBitVector ConflictingValues(Result.size());

  computeAvailableValuesFrom(StartingFrom->getIterator(),
                             StartingFrom->getParent(), RequiredElts, Result,
                             VisitedBlocks, ConflictingValues);

  // If we have any conflicting values, explicitly mask them out of the result,
  // so we don't pick one arbitrary available value.
  if (!ConflictingValues.none())
    for (unsigned i = 0, e = Result.size(); i != e; ++i)
      if (ConflictingValues[i])
        Result[i] = {};

  return;
}

void AllocOptimize::computeAvailableValuesFrom(
    SILBasicBlock::iterator StartingFrom, SILBasicBlock *BB,
    llvm::SmallBitVector &RequiredElts, SmallVectorImpl<AvailableValue> &Result,
    llvm::SmallDenseMap<SILBasicBlock *, llvm::SmallBitVector, 32>
        &VisitedBlocks,
    llvm::SmallBitVector &ConflictingValues) {
  assert(!RequiredElts.none() && "Scanning with a goal of finding nothing?");
  
  // If there is a potential modification in the current block, scan the block
  // to see if the store or escape is before or after the load.  If it is
  // before, check to see if it produces the value we are looking for.
  if (HasLocalDefinition.count(BB)) {
    for (SILBasicBlock::iterator BBI = StartingFrom; BBI != BB->begin();) {
      SILInstruction *TheInst = &*std::prev(BBI);

      // If this instruction is unrelated to the element, ignore it.
      if (!NonLoadUses.count(TheInst)) {
        --BBI;
        continue;
      }
      
      // Given an interesting instruction, incorporate it into the set of
      // results, and filter down the list of demanded subelements that we still
      // need.
      updateAvailableValues(TheInst, RequiredElts, Result, ConflictingValues);
      
      // If this satisfied all of the demanded values, we're done.
      if (RequiredElts.none())
        return;
      
      // Otherwise, keep scanning the block.  If the instruction we were looking
      // at just got exploded, don't skip the next instruction.
      if (&*std::prev(BBI) == TheInst)
        --BBI;
    }
  }
  
  
  // Otherwise, we need to scan up the CFG looking for available values.
  for (auto PI = BB->pred_begin(), E = BB->pred_end(); PI != E; ++PI) {
    SILBasicBlock *PredBB = *PI;
    
    // If the predecessor block has already been visited (potentially due to a
    // cycle in the CFG), don't revisit it.  We can do this safely because we
    // are optimistically assuming that all incoming elements in a cycle will be
    // the same.  If we ever detect a conflicting element, we record it and do
    // not look at the result.
    auto Entry = VisitedBlocks.insert({PredBB, RequiredElts});
    if (!Entry.second) {
      // If we are revisiting a block and asking for different required elements
      // then anything that isn't agreeing is in conflict.
      const auto &PrevRequired = Entry.first->second;
      if (PrevRequired != RequiredElts) {
        ConflictingValues |= (PrevRequired ^ RequiredElts);
        
        RequiredElts &= ~ConflictingValues;
        if (RequiredElts.none())
          return;
      }
      continue;
    }
    
    // Make sure to pass in the same set of required elements for each pred.
    llvm::SmallBitVector Elts = RequiredElts;
    computeAvailableValuesFrom(PredBB->end(), PredBB, Elts, Result,
                               VisitedBlocks, ConflictingValues);
    
    // If we have any conflicting values, don't bother searching for them.
    RequiredElts &= ~ConflictingValues;
    if (RequiredElts.none())
      return;
  }
}

/// If we are able to optimize \p Inst, return the source address that
/// instruction is loading from. If we can not optimize \p Inst, then just
/// return an empty SILValue.
static SILValue tryFindSrcAddrForLoad(SILInstruction *Inst) {
  // We only handle load [copy], load [trivial] and copy_addr right now.
  if (auto *LI = dyn_cast<LoadInst>(Inst))
    return LI->getOperand();

  // If this is a CopyAddr, verify that the element type is loadable.  If not,
  // we can't explode to a load.
  auto *CAI = dyn_cast<CopyAddrInst>(Inst);
  if (!CAI || !CAI->getSrc()->getType().isLoadable(CAI->getModule()))
    return SILValue();
  return CAI->getSrc();
}

/// At this point, we know that this element satisfies the definitive init
/// requirements, so we can try to promote loads to enable SSA-based dataflow
/// analysis.  We know that accesses to this element only access this element,
/// cross element accesses have been scalarized.
///
/// This returns true if the load has been removed from the program.
bool AllocOptimize::promoteLoad(SILInstruction *Inst) {
  // Note that we intentionally don't support forwarding of weak pointers,
  // because the underlying value may drop be deallocated at any time.  We would
  // have to prove that something in this function is holding the weak value
  // live across the promoted region and that isn't desired for a stable
  // diagnostics pass this like one.

  // First attempt to find a source addr for our "load" instruction. If we fail
  // to find a valid value, just return.
  SILValue SrcAddr = tryFindSrcAddrForLoad(Inst);
  if (!SrcAddr)
    return false;

  // If the box has escaped at this instruction, we can't safely promote the
  // load.
  if (hasEscapedAt(Inst))
    return false;

  SILType LoadTy = SrcAddr->getType().getObjectType();

  // If this is a load/copy_addr from a struct field that we want to promote,
  // compute the access path down to the field so we can determine precise
  // def/use behavior.
  unsigned FirstElt = computeSubelement(SrcAddr, TheMemory);

  // If this is a load from within an enum projection, we can't promote it since
  // we don't track subelements in a type that could be changing.
  if (FirstElt == ~0U)
    return false;
  
  unsigned NumLoadSubElements = getNumSubElements(LoadTy, Module);
  
  // Set up the bitvector of elements being demanded by the load.
  llvm::SmallBitVector RequiredElts(NumMemorySubElements);
  RequiredElts.set(FirstElt, FirstElt+NumLoadSubElements);

  SmallVector<AvailableValue, 8> AvailableValues;
  AvailableValues.resize(NumMemorySubElements);
  
  // Find out if we have any available values.  If no bits are demanded, we
  // trivially succeed. This can happen when there is a load of an empty struct.
  if (NumLoadSubElements != 0) {
    computeAvailableValues(Inst, RequiredElts, AvailableValues);
    
    // If there are no values available at this load point, then we fail to
    // promote this load and there is nothing to do.
    bool AnyAvailable = false;
    for (unsigned i = FirstElt, e = i+NumLoadSubElements; i != e; ++i)
      if (AvailableValues[i].getValue()) {
        AnyAvailable = true;
        break;
      }
    
    if (!AnyAvailable)
      return false;
  }
  
  // Ok, we have some available values.  If we have a copy_addr, explode it now,
  // exposing the load operation within it.  Subsequent optimization passes will
  // see the load and propagate the available values into it.
  if (auto *CAI = dyn_cast<CopyAddrInst>(Inst)) {
    explodeCopyAddr(CAI);
    
    // This is removing the copy_addr, but explodeCopyAddr takes care of
    // removing the instruction from Uses for us, so we return false.
    return false;
  }
  
  // Aggregate together all of the subelements into something that has the same
  // type as the load did, and emit smaller) loads for any subelements that were
  // not available.
  auto *LI = cast<LoadInst>(Inst);
  AvailableValueAggregator Agg(TheMemory, LI, LI->getOwnershipQualifier(),
                               AvailableValues, Uses);
  SILValue NewVal = Agg.aggregateValues(LoadTy, LI->getOperand(), FirstElt);

  ++NumLoadPromoted;
  
  // Simply replace the load.
  DEBUG(llvm::dbgs() << "  *** Promoting load: " << *LI << "\n");
  DEBUG(llvm::dbgs() << "      To value: " << *NewVal << "\n");

  LI->replaceAllUsesWith(NewVal);
  SILValue Addr = LI->getOperand();
  LI->eraseFromParent();
  if (auto *AddrI = Addr->getDefiningInstruction())
    recursivelyDeleteTriviallyDeadInstructions(AddrI);

  return true;
}

/// Return true if we can promote the given destroy.
bool AllocOptimize::canPromoteDestroyAddr(
    DestroyAddrInst *DAI,
    llvm::SmallVectorImpl<AvailableValue> &AvailableValues) {
  SILValue Address = DAI->getOperand();

  // We cannot promote destroys of address-only types, because we can't expose
  // the load.
  SILType LoadTy = Address->getType().getObjectType();
  if (LoadTy.isAddressOnly(Module))
    return false;
  
  // If the box has escaped at this instruction, we can't safely promote the
  // load.
  if (hasEscapedAt(DAI))
    return false;
  
  // Compute the access path down to the field so we can determine precise
  // def/use behavior.
  unsigned FirstElt = computeSubelement(Address, TheMemory);
  assert(FirstElt != ~0U && "destroy within enum projection is not valid");
  unsigned NumLoadSubElements = getNumSubElements(LoadTy, Module);
  
  // Set up the bitvector of elements being demanded by the load.
  llvm::SmallBitVector RequiredElts(NumMemorySubElements);
  RequiredElts.set(FirstElt, FirstElt+NumLoadSubElements);

  // Find out if we have any available values.  If no bits are demanded, we
  // trivially succeed. This can happen when there is a load of an empty struct.
  if (NumLoadSubElements == 0)
    return true;

  llvm::SmallVector<AvailableValue, 8> TmpList;
  TmpList.resize(NumMemorySubElements);
  computeAvailableValues(DAI, RequiredElts, TmpList);

  // If some value is not available at this load point, then we fail.
  if (llvm::any_of(range(FirstElt, FirstElt + NumLoadSubElements),
                   [&](unsigned i) -> bool { return !TmpList[i]; }))
    return false;

  // Now that we have our final list, move the temporary lists contents into
  // AvailableValues.
  std::move(TmpList.begin(), TmpList.end(),
            std::back_inserter(AvailableValues));

  return true;
}

/// promoteDestroyAddr - DestroyAddr is a composed operation merging
/// load+strong_release.  If the implicit load's value is available, explode it.
///
/// Note that we handle the general case of a destroy_addr of a piece of the
/// memory object, not just destroy_addrs of the entire thing.
void AllocOptimize::promoteDestroyAddr(
    DestroyAddrInst *DAI, MutableArrayRef<AvailableValue> AvailableValues) {
  SILValue Address = DAI->getOperand();
  SILType LoadTy = Address->getType().getObjectType();

  // Compute the access path down to the field so we can determine precise
  // def/use behavior.
  unsigned FirstElt = computeSubelement(Address, TheMemory);

  // Aggregate together all of the subelements into something that has the same
  // type as the load did, and emit smaller) loads for any subelements that were
  // not available.
  AvailableValueAggregator Agg(TheMemory, DAI, LoadOwnershipQualifier::Take,
                               AvailableValues, Uses);
  SILValue NewVal = Agg.aggregateValues(LoadTy, Address, FirstElt);

  ++NumDestroyAddrPromoted;
  
  DEBUG(llvm::dbgs() << "  *** Promoting destroy_addr: " << *DAI << "\n");
  DEBUG(llvm::dbgs() << "      To value: " << *NewVal << "\n");

  SILBuilderWithScope(DAI).emitDestroyValueOperation(DAI->getLoc(), NewVal);
  DAI->eraseFromParent();
}

/// Explode a copy_addr instruction of a loadable type into lower level
/// operations like loads, stores, retains, releases, retain_value, etc.
void AllocOptimize::explodeCopyAddr(CopyAddrInst *CAI) {
  DEBUG(llvm::dbgs() << "  -- Exploding copy_addr: " << *CAI << "\n");
  
  SILType ValTy = CAI->getDest()->getType().getObjectType();
  auto &TL = Module.getTypeLowering(ValTy);
  
  // Keep track of the new instructions emitted.
  SmallVector<SILInstruction*, 4> NewInsts;
  SILBuilder B(CAI, &NewInsts);
  B.setCurrentDebugScope(CAI->getDebugScope());
  
  // Use type lowering to lower the copyaddr into a load sequence + store
  // sequence appropriate for the type.
  SILValue StoredValue = TL.emitLoadOfCopy(B, CAI->getLoc(), CAI->getSrc(),
                                           CAI->isTakeOfSrc());
  
  TL.emitStoreOfCopy(B, CAI->getLoc(), StoredValue, CAI->getDest(),
                     CAI->isInitializationOfDest());

  // Update our internal state for this being gone.
  NonLoadUses.erase(CAI);
  
  // Remove the copy_addr from Uses.  A single copy_addr can appear multiple
  // times if the source and dest are to elements within a single aggregate, but
  // we only want to pick up the CopyAddrKind from the store.
  DIMemoryUse LoadUse, StoreUse;
  for (auto &Use : Uses) {
    if (Use.Inst != CAI) continue;
    
    if (Use.Kind == DIUseKind::Load) {
      assert(LoadUse.isInvalid());
      LoadUse = Use;
    } else {
      assert(StoreUse.isInvalid());
      StoreUse = Use;
    }
    
    Use.Inst = nullptr;
    
    // Keep scanning in case the copy_addr appears multiple times.
  }
  
  assert((LoadUse.isValid() || StoreUse.isValid()) &&
         "we should have a load or a store, possibly both");
  assert(StoreUse.isInvalid() || StoreUse.Kind == Assign ||
         StoreUse.Kind == PartialStore || StoreUse.Kind == Initialization);
  
  // Now that we've emitted a bunch of instructions, including a load and store
  // but also including other stuff, update the internal state of
  // LifetimeChecker to reflect them.
  
  // Update the instructions that touch the memory.  NewInst can grow as this
  // iterates, so we can't use a foreach loop.
  for (auto *NewInst : NewInsts) {
    switch (NewInst->getKind()) {
    default:
      NewInst->dump();
      llvm_unreachable("Unknown instruction generated by copy_addr lowering");
      
    case SILInstructionKind::StoreInst:
      // If it is a store to the memory object (as oppose to a store to
      // something else), track it as an access.
      if (StoreUse.isValid()) {
        StoreUse.Inst = NewInst;
        NonLoadUses[NewInst] = Uses.size();
        Uses.push_back(StoreUse);
      }
      continue;
      
    case SILInstructionKind::LoadInst:
      // If it is a load from the memory object (as oppose to a load from
      // something else), track it as an access.  We need to explicitly check to
      // see if the load accesses "TheMemory" because it could either be a load
      // for the copy_addr source, or it could be a load corresponding to the
      // "assign" operation on the destination of the copyaddr.
      if (LoadUse.isValid() &&
          getAccessPathRoot(NewInst->getOperand(0)) == TheMemory) {
        LoadUse.Inst = NewInst;
        Uses.push_back(LoadUse);
      }
      continue;

    case SILInstructionKind::CopyValueInst:
      llvm_unreachable("Should never see a copy_value here. We use load "
                       "[copy]");

    case SILInstructionKind::RetainValueInst:
    case SILInstructionKind::StrongRetainInst:
    case SILInstructionKind::StrongReleaseInst:
    case SILInstructionKind::UnownedRetainInst:
    case SILInstructionKind::UnownedReleaseInst:
    case SILInstructionKind::DestroyValueInst:
    case SILInstructionKind::ReleaseValueInst:   // Destroy overwritten value
      // These are ignored.
      continue;
    }
  }

  // Next, remove the copy_addr itself.
  CAI->eraseFromParent();
}

/// tryToRemoveDeadAllocation - If the allocation is an autogenerated allocation
/// that is only stored to (after load promotion) then remove it completely.
bool AllocOptimize::tryToRemoveDeadAllocation() {
  assert((isa<AllocBoxInst>(TheMemory) || isa<AllocStackInst>(TheMemory)) &&
         "Unhandled allocation case");

  // We don't want to remove allocations that are required for useful debug
  // information at -O0.  As such, we only remove allocations if:
  //
  // 1. They are in a transparent function.
  // 2. They are in a normal function, but didn't come from a VarDecl, or came
  //    from one that was autogenerated or inlined from a transparent function.
  SILLocation Loc = TheMemory->getLoc();
  if (!TheMemory->getFunction()->isTransparent() &&
      Loc.getAsASTNode<VarDecl>() && !Loc.isAutoGenerated() &&
      !Loc.is<MandatoryInlinedLocation>())
    return false;

  // Check the uses list to see if there are any non-store uses left over after
  // load promotion and other things DI does.
  for (auto &U : Uses) {
    // Ignore removed instructions.
    if (U.Inst == nullptr) continue;

    switch (U.Kind) {
    case DIUseKind::SelfInit:
    case DIUseKind::SuperInit:
      llvm_unreachable("Can't happen on allocations");
    case DIUseKind::Assign:
    case DIUseKind::PartialStore:
    case DIUseKind::InitOrAssign:
      break;    // These don't prevent removal.
    case DIUseKind::Initialization:
      if (!isa<ApplyInst>(U.Inst) &&
          // A copy_addr that is not a take affects the retain count
          // of the source.
          (!isa<CopyAddrInst>(U.Inst) ||
           cast<CopyAddrInst>(U.Inst)->isTakeOfSrc()))
        break;
      // FALL THROUGH.
     LLVM_FALLTHROUGH;
    case DIUseKind::Load:
    case DIUseKind::IndirectIn:
    case DIUseKind::InOutUse:
    case DIUseKind::Escape:
      DEBUG(llvm::dbgs() << "*** Failed to remove autogenerated alloc: "
            "kept alive by: " << *U.Inst);
      return false;   // These do prevent removal.
    }
  }

  // If the memory object has non-trivial type, then removing the deallocation
  // will drop any releases.  Check that there is nothing preventing removal.
  llvm::SmallVector<unsigned, 8> DestroyAddrIndices;
  llvm::SmallVector<AvailableValue, 32> AvailableValueList;
  llvm::SmallVector<unsigned, 8> AvailableValueStartOffsets;

  if (!MemoryType.isTrivial(Module)) {
    for (auto P : llvm::enumerate(Releases)) {
      auto *R = P.value();
      if (R == nullptr || isa<DeallocStackInst>(R) || isa<DeallocBoxInst>(R))
        continue;

      // We stash all of the destroy_addr that we see.
      if (auto *DAI = dyn_cast<DestroyAddrInst>(R)) {
        AvailableValueStartOffsets.push_back(AvailableValueList.size());
        // Make sure we can actually promote this destroy addr. If we can not,
        // then we must bail. In order to not gather available values twice, we
        // gather the available values here that we will use to promote the
        // values.
        if (!canPromoteDestroyAddr(DAI, AvailableValueList))
          return false;
        DestroyAddrIndices.push_back(P.index());
        continue;
      }

      DEBUG(llvm::dbgs() << "*** Failed to remove autogenerated alloc: "
            "kept alive by release: " << *R);
      return false;
    }
  }

  // If we reached this point, we can promote all of our destroy_addr.
  for (auto P : llvm::enumerate(DestroyAddrIndices)) {
    unsigned DestroyAddrIndex = P.value();
    unsigned AvailableValueIndex = P.index();
    unsigned StartOffset = AvailableValueStartOffsets[AvailableValueIndex];
    unsigned Count;

    if ((AvailableValueStartOffsets.size() - 1) != AvailableValueIndex) {
      Count = AvailableValueStartOffsets[AvailableValueIndex + 1] - StartOffset;
    } else {
      Count = AvailableValueList.size() - StartOffset;
    }

    MutableArrayRef<AvailableValue> Values(&AvailableValueList[StartOffset],
                                           Count);
    auto *DAI = cast<DestroyAddrInst>(Releases[DestroyAddrIndex]);
    promoteDestroyAddr(DAI, Values);
    Releases[DestroyAddrIndex] = nullptr;
  }

  DEBUG(llvm::dbgs() << "*** Removing autogenerated alloc_stack: "<<*TheMemory);

  // If it is safe to remove, do it.  Recursively remove all instructions
  // hanging off the allocation instruction, then return success.  Let the
  // caller remove the allocation itself to avoid iterator invalidation.
  eraseUsesOfInstruction(TheMemory);

  return true;
}

/// doIt - returns true on error.
bool AllocOptimize::doIt() {
  bool Changed = false;

  // Don't try to optimize incomplete aggregates.
  if (MemoryType.aggregateHasUnreferenceableStorage())
    return false;

  // If we've successfully checked all of the definitive initialization
  // requirements, try to promote loads.  This can explode copy_addrs, so the
  // use list may change size.
  for (unsigned i = 0; i != Uses.size(); ++i) {
    auto &Use = Uses[i];
    // Ignore entries for instructions that got expanded along the way.
    if (Use.Inst && Use.Kind == DIUseKind::Load) {
      if (promoteLoad(Use.Inst)) {
        Uses[i].Inst = nullptr;  // remove entry if load got deleted.
        Changed = true;
      }
    }
  }

  // If this is an allocation, try to remove it completely.
  Changed |= tryToRemoveDeadAllocation();

  return Changed;
}

static bool optimizeMemoryAllocations(SILFunction &Fn) {
  bool Changed = false;
  for (auto &BB : Fn) {
    auto I = BB.begin(), E = BB.end();
    while (I != E) {
      SILInstruction *Inst = &*I;
      if (!isa<AllocBoxInst>(Inst) && !isa<AllocStackInst>(Inst)) {
        ++I;
        continue;
      }
      auto Alloc = cast<AllocationInst>(Inst);

      DEBUG(llvm::dbgs() << "*** DI Optimize looking at: " << *Alloc << "\n");
      DIMemoryObjectInfo MemInfo(Alloc);

      // Set up the datastructure used to collect the uses of the allocation.
      SmallVector<DIMemoryUse, 16> Uses;
      SmallVector<SILInstruction*, 4> Releases;
      
      // Walk the use list of the pointer, collecting them.
      collectDIElementUsesFrom(MemInfo, Uses, Releases);

      Changed |= AllocOptimize(Alloc, Uses, Releases).doIt();
      
      // Carefully move iterator to avoid invalidation problems.
      ++I;
      if (Alloc->use_empty()) {
        Alloc->eraseFromParent();
        ++NumAllocRemoved;
        Changed = true;
      }
    }
  }
  return Changed;
}

static void breakCriticalEdgesWithNonTrivialArgs(SILFunction &Fn) {
  // Find our targets.
  llvm::SmallVector<std::pair<SILBasicBlock *, unsigned>, 8> Targets;
  for (auto &Block : Fn) {
    auto *CBI = dyn_cast<CondBranchInst>(Block.getTerminator());
    if (!CBI)
      continue;

    // See if our true index is a critical edge. If so, add block to the list
    // and continue. If the false edge is also critical, we will handle it at
    // the same time.
    if (isCriticalEdge(CBI, CondBranchInst::TrueIdx)) {
      Targets.emplace_back(&Block, CondBranchInst::TrueIdx);
    }

    if (!isCriticalEdge(CBI, CondBranchInst::FalseIdx)) {
      continue;
    }

    Targets.emplace_back(&Block, CondBranchInst::FalseIdx);
  }

  for (auto P : Targets) {
    SILBasicBlock *Block = P.first;
    unsigned Index = P.second;
    auto *Result = splitCriticalEdge(Block->getTerminator(), Index);
    (void)Result;
    assert(Result);
  }
}

namespace {

class PredictableMemoryOptimizations : public SILFunctionTransform {
  /// The entry point to the transformation.
  void run() override {
    if (optimizeMemoryAllocations(*getFunction())) {
      // See if we need to break any critical edges with non-trivial
      // arguments. This is an invariant of the ownership verifier.
      breakCriticalEdgesWithNonTrivialArgs(*getFunction());
      invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
    }
  }
};

} // end anonymous namespace


SILTransform *swift::createPredictableMemoryOptimizations() {
  return new PredictableMemoryOptimizations();
}
