//===--- FieldSensitivePrunedLiveness.cpp ---------------------------------===//
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

#include "swift/SIL/FieldSensitivePrunedLiveness.h"
#include "swift/AST/TypeExpansionContext.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/SmallBitVector.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/ScopedAddressUtils.h"
#include "swift/SIL/Test.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace swift;

static llvm::cl::opt<bool> EmitLogging(
    "sil-move-only-checker-emit-pruned-liveness-logging");

#define PRUNED_LIVENESS_LOG(X) \
  do { \
    if (EmitLogging) { \
      LLVM_DEBUG(X); \
      } \
    } while (0)

// We can only analyze components of structs whose storage is fully accessible
// from Swift.
static StructDecl *getFullyReferenceableStruct(SILType ktypeTy) {
  auto structDecl = ktypeTy.getStructOrBoundGenericStruct();
  if (!structDecl || structDecl->hasUnreferenceableStorage())
    return nullptr;
  return structDecl;
}

//===----------------------------------------------------------------------===//
//                         MARK: TypeSubElementCount
//===----------------------------------------------------------------------===//

TypeSubElementCount::TypeSubElementCount(SILType type, SILModule &mod,
                                         TypeExpansionContext context)
    : number(1) {
  if (auto tupleType = type.getAs<TupleType>()) {
    unsigned numElements = 0;
    for (auto index : indices(tupleType.getElementTypes()))
      numElements +=
          TypeSubElementCount(type.getTupleElementType(index), mod, context);
    number = numElements;
    return;
  }

  if (auto *structDecl = getFullyReferenceableStruct(type)) {
    unsigned numElements = 0;
    for (auto *fieldDecl : structDecl->getStoredProperties())
      numElements += TypeSubElementCount(
          type.getFieldType(fieldDecl, mod, context), mod, context);
    number = numElements;

    // If we do not have any elements, just set our size to 1.
    if (number == 0)
      number = 1;
    if (type.isValueTypeWithDeinit()) {
      // 'self' has its own liveness represented as an additional field at the
      // end of the structure.
      ++number;
    }

    return;
  }

  if (auto *enumDecl = type.getEnumOrBoundGenericEnum()) {
    unsigned numElements = 0;
    for (auto *eltDecl : enumDecl->getAllElements()) {
      if (!eltDecl->hasAssociatedValues())
        continue;
      auto elt = type.getEnumElementType(eltDecl, mod, context);
      numElements += unsigned(TypeSubElementCount(elt, mod, context));
    }
    number = numElements + 1;
    if (type.isValueTypeWithDeinit()) {
      // 'self' has its own liveness represented as an additional field at the
      // end of the structure.
      ++number;
    }
    return;
  }

  // If this isn't a tuple, struct, or enum, it is a single element. This was
  // our default value, so we can just return.
}

TypeSubElementCount::TypeSubElementCount(SILValue value) : number(1) {
  auto whole = TypeSubElementCount(value->getType(), *value->getModule(),
                                   TypeExpansionContext(*value->getFunction()));
  // The value produced by a drop_deinit has one fewer subelement than that of
  // its type--the deinit bit is not included.
  if (isa<DropDeinitInst>(value)) {
    assert(value->getType().isValueTypeWithDeinit());
    whole = whole - 1;
  }
  number = whole;
}

//===----------------------------------------------------------------------===//
//                           MARK: SubElementNumber
//===----------------------------------------------------------------------===//

std::optional<SubElementOffset>
SubElementOffset::computeForAddress(SILValue projectionDerivedFromRoot,
                                    SILValue rootAddress) {
  unsigned finalSubElementOffset = 0;
  SILModule &mod = *rootAddress->getModule();

  LLVM_DEBUG(llvm::dbgs() << "computing element offset for root:\n";
             rootAddress->print(llvm::dbgs()));

  while (1) {
    LLVM_DEBUG(llvm::dbgs() << "projection: ";
               projectionDerivedFromRoot->print(llvm::dbgs()));

    // If we got to the root, we're done.
    if (rootAddress == projectionDerivedFromRoot)
      return {SubElementOffset(finalSubElementOffset)};

    if (auto *pbi = dyn_cast<ProjectBoxInst>(projectionDerivedFromRoot)) {
      projectionDerivedFromRoot = pbi->getOperand();
      continue;
    }

    if (auto *bai = dyn_cast<BeginAccessInst>(projectionDerivedFromRoot)) {
      projectionDerivedFromRoot = bai->getSource();
      continue;
    }

    if (auto *sbi = dyn_cast<StoreBorrowInst>(projectionDerivedFromRoot)) {
      projectionDerivedFromRoot = sbi->getDest();
      continue;
    }

    if (auto *m = dyn_cast<MoveOnlyWrapperToCopyableAddrInst>(
            projectionDerivedFromRoot)) {
      projectionDerivedFromRoot = m->getOperand();
      continue;
    }

    if (auto *oea =
            dyn_cast<OpenExistentialAddrInst>(projectionDerivedFromRoot)) {
      projectionDerivedFromRoot = oea->getOperand();
      continue;
    }

    if (auto *iea =
            dyn_cast<InitExistentialAddrInst>(projectionDerivedFromRoot)) {
      projectionDerivedFromRoot = iea->getOperand();
      continue;
    }

    if (auto *teai =
            dyn_cast<TupleElementAddrInst>(projectionDerivedFromRoot)) {
      SILType tupleType = teai->getOperand()->getType();

      // Keep track of what subelement is being referenced.
      for (unsigned i : range(teai->getFieldIndex())) {
        finalSubElementOffset += TypeSubElementCount(
            tupleType.getTupleElementType(i), mod,
            TypeExpansionContext(*rootAddress->getFunction()));
      }
      projectionDerivedFromRoot = teai->getOperand();
      continue;
    }

    if (auto *seai =
            dyn_cast<StructElementAddrInst>(projectionDerivedFromRoot)) {
      SILType type = seai->getOperand()->getType();

      // Keep track of what subelement is being referenced.
      StructDecl *structDecl = seai->getStructDecl();
      for (auto *fieldDecl : structDecl->getStoredProperties()) {
        if (fieldDecl == seai->getField())
          break;
        auto context = TypeExpansionContext(*rootAddress->getFunction());
        finalSubElementOffset += TypeSubElementCount(
            type.getFieldType(fieldDecl, mod, context), mod, context);
      }

      projectionDerivedFromRoot = seai->getOperand();
      continue;
    }

    if (auto *enumData = dyn_cast<UncheckedTakeEnumDataAddrInst>(
            projectionDerivedFromRoot)) {
      auto ty = enumData->getOperand()->getType();
      auto *enumDecl = enumData->getEnumDecl();
      for (auto *element : enumDecl->getAllElements()) {
        if (!element->hasAssociatedValues())
          continue;
        if (element == enumData->getElement())
          break;
        auto context = TypeExpansionContext(*rootAddress->getFunction());
        auto elementTy = ty.getEnumElementType(element, mod, context);
        finalSubElementOffset +=
            unsigned(TypeSubElementCount(elementTy, mod, context));
      }
      projectionDerivedFromRoot = enumData->getOperand();
      continue;
    }

    // Init enum data addr is treated like unchecked take enum data addr.
    if (auto *initData =
            dyn_cast<InitEnumDataAddrInst>(projectionDerivedFromRoot)) {
      projectionDerivedFromRoot = initData->getOperand();
      continue;
    }

    // A drop_deinit consumes the "self" bit at the end of its type.  The offset
    // is still to the beginning.
    if (auto dd = dyn_cast<DropDeinitInst>(projectionDerivedFromRoot)) {
      projectionDerivedFromRoot = dd->getOperand();
      continue;
    }

    // Look through wrappers.
    if (auto c2m = dyn_cast<CopyableToMoveOnlyWrapperAddrInst>(projectionDerivedFromRoot)) {
      projectionDerivedFromRoot = c2m->getOperand();
      continue;
    }
    if (auto m2c = dyn_cast<MoveOnlyWrapperToCopyableValueInst>(projectionDerivedFromRoot)) {
      projectionDerivedFromRoot = m2c->getOperand();
      continue;
    }

    // If we do not know how to handle this case, just return None.
    //
    // NOTE: We use to assert here, but since this is used for diagnostics, we
    // really do not want to abort. Instead, our caller can choose to abort if
    // they get back a None. This ensures that we do not abort in cases where we
    // just want to emit to the user a "I do not understand" error.
    LLVM_DEBUG(llvm::dbgs() << "unhandled projection derived from root:\n";
               projectionDerivedFromRoot->print(llvm::dbgs()));

    return std::nullopt;
  }
}

std::optional<SubElementOffset>
SubElementOffset::computeForValue(SILValue projectionDerivedFromRoot,
                                  SILValue rootAddress) {
  unsigned finalSubElementOffset = 0;
  SILModule &mod = *rootAddress->getModule();

  while (1) {
    // If we got to the root, we're done.
    if (rootAddress == projectionDerivedFromRoot)
      return {SubElementOffset(finalSubElementOffset)};

    // Look through these single operand instructions.
    if (isa<BeginBorrowInst>(projectionDerivedFromRoot) ||
        isa<CopyValueInst>(projectionDerivedFromRoot) ||
        isa<MoveOnlyWrapperToCopyableValueInst>(projectionDerivedFromRoot)) {
      projectionDerivedFromRoot =
          cast<SingleValueInstruction>(projectionDerivedFromRoot)
              ->getOperand(0);
      continue;
    }

    if (auto *teai = dyn_cast<TupleExtractInst>(projectionDerivedFromRoot)) {
      SILType tupleType = teai->getOperand()->getType();

      // Keep track of what subelement is being referenced.
      for (unsigned i : range(teai->getFieldIndex())) {
        finalSubElementOffset += TypeSubElementCount(
            tupleType.getTupleElementType(i), mod,
            TypeExpansionContext(*rootAddress->getFunction()));
      }
      projectionDerivedFromRoot = teai->getOperand();
      continue;
    }

    if (auto *mvir = dyn_cast<MultipleValueInstructionResult>(
            projectionDerivedFromRoot)) {
      if (auto *dsi = dyn_cast<DestructureStructInst>(mvir->getParent())) {
        SILType type = dsi->getOperand()->getType();

        // Keep track of what subelement is being referenced.
        unsigned resultIndex = mvir->getIndex();
        StructDecl *structDecl = dsi->getStructDecl();
        for (auto pair : llvm::enumerate(structDecl->getStoredProperties())) {
          if (pair.index() == resultIndex)
            break;
          auto context = TypeExpansionContext(*rootAddress->getFunction());
          finalSubElementOffset += TypeSubElementCount(
              type.getFieldType(pair.value(), mod, context), mod, context);
        }

        projectionDerivedFromRoot = dsi->getOperand();
        continue;
      }

      if (auto *dti = dyn_cast<DestructureTupleInst>(mvir->getParent())) {
        SILType type = dti->getOperand()->getType();

        // Keep track of what subelement is being referenced.
        unsigned resultIndex = mvir->getIndex();
        for (unsigned i : range(resultIndex)) {
          auto context = TypeExpansionContext(*rootAddress->getFunction());
          finalSubElementOffset +=
              TypeSubElementCount(type.getTupleElementType(i), mod, context);
        }

        projectionDerivedFromRoot = dti->getOperand();
        continue;
      }
    }

    if (auto *seai = dyn_cast<StructExtractInst>(projectionDerivedFromRoot)) {
      SILType type = seai->getOperand()->getType();

      // Keep track of what subelement is being referenced.
      StructDecl *structDecl = seai->getStructDecl();
      for (auto *fieldDecl : structDecl->getStoredProperties()) {
        if (fieldDecl == seai->getField())
          break;
        auto context = TypeExpansionContext(*rootAddress->getFunction());
        finalSubElementOffset += TypeSubElementCount(
            type.getFieldType(fieldDecl, mod, context), mod, context);
      }

      projectionDerivedFromRoot = seai->getOperand();
      continue;
    }

    // In the case of enums, we note that our representation is:
    //
    //                   ---------|Enum| ---
    //                  /                   \
    //                 /                     \
    //                v                       v
    //  |Bits for Max Sized Payload|    |Discrim Bit|
    //
    // So our payload is always going to start at the current field number since
    // we are the left most child of our parent enum. So we just need to look
    // through to our parent enum.
    //
    // Enum projections can happen either directly via an unchecked instruction…
    if (auto *enumData =
            dyn_cast<UncheckedEnumDataInst>(projectionDerivedFromRoot)) {
      projectionDerivedFromRoot = enumData->getOperand();
      continue;
    }
    
    // …or via the bb arg of a `switch_enum` successor.
    if (auto bbArg = dyn_cast<SILArgument>(projectionDerivedFromRoot)) {
      if (auto pred = bbArg->getParent()->getSinglePredecessorBlock()) {
        if (auto switchEnum = dyn_cast<SwitchEnumInst>(pred->getTerminator())) {
          projectionDerivedFromRoot = switchEnum->getOperand();
          continue;
        }
      }
    }

    // If we do not know how to handle this case, just return None.
    //
    // NOTE: We use to assert here, but since this is used for diagnostics, we
    // really do not want to abort. Instead, our caller can choose to abort if
    // they get back a None. This ensures that we do not abort in cases where we
    // just want to emit to the user a "I do not understand" error.
    return std::nullopt;
  }
}

//===----------------------------------------------------------------------===//
//                        MARK: TypeTreeLeafTypeRange
//===----------------------------------------------------------------------===//

/// Whether \p targetInst is dominated by one of the provided switch_enum_addr's
/// destination blocks whose corresponding enum element has no associated values
/// which need to be destroyed (i.e. either it has no associated values or they
/// are trivial).
static bool isDominatedByPayloadlessSwitchEnumAddrDests(
    SILInstruction *targetInst, ArrayRef<SwitchEnumAddrInst *> seais,
    DominanceInfo *domTree) {
  if (seais.empty())
    return false;
  auto *target = targetInst->getParent();
  for (auto *seai : seais) {
    if (!domTree->dominates(seai, targetInst)) {
      continue;
    }
    auto size = seai->getNumCases();
    auto ty = seai->getOperand()->getType();
    for (unsigned index = 0; index < size; ++index) {
      auto pair = seai->getCase(index);
      auto *eltDecl = pair.first;
      if (eltDecl->hasAssociatedValues()) {
        auto eltTy = ty.getEnumElementType(eltDecl, seai->getFunction());
        if (!eltTy.isTrivial(*seai->getFunction())) {
          continue;
        }
      }
      auto *block = pair.second;
      if (domTree->dominates(block, target))
        return true;
    }
  }
  return false;
}

void TypeTreeLeafTypeRange::constructFilteredProjections(
    SILValue value, SILInstruction *insertPt, SmallBitVector &filterBitVector,
    DominanceInfo *domTree,
    llvm::function_ref<bool(SILValue, TypeTreeLeafTypeRange, NeedsDestroy_t)>
        callback) {
  auto *fn = insertPt->getFunction();
  SILType type = value->getType();
  auto loc =
      (insertPt->getLoc().getKind() != SILLocation::ArtificialUnreachableKind)
          ? insertPt->getLoc()
          : RegularLocation::getAutoGeneratedLocation();

  PRUNED_LIVENESS_LOG(llvm::dbgs() << "ConstructFilteredProjection. Bv: "
                          << filterBitVector << '\n');
  SILBuilderWithScope builder(insertPt);

  auto noneSet = [](SmallBitVector &bv, unsigned start, unsigned end) {
    return llvm::none_of(range(start, end),
                         [&](unsigned index) { return bv[index]; });
  };
  auto allSet = [](SmallBitVector &bv, unsigned start, unsigned end) {
    return llvm::all_of(range(start, end),
                        [&](unsigned index) { return bv[index]; });
  };

  if (auto *structDecl = type.getStructOrBoundGenericStruct()) {
    unsigned start = startEltOffset;
    for (auto *varDecl : structDecl->getStoredProperties()) {
      auto nextType = type.getFieldType(varDecl, fn);
      unsigned next = start + TypeSubElementCount(nextType, fn);

      // If we do not have any set bits, do not create the struct element addr
      // for this entry.
      if (noneSet(filterBitVector, start, next)) {
        start = next;
        continue;
      }

      auto newValue = builder.createStructElementAddr(loc, value, varDecl);
      callback(newValue, TypeTreeLeafTypeRange(start, next), NeedsDestroy);
      start = next;
    }
    if (start == 0) {
      ++start;
    }
    if (type.isValueTypeWithDeinit()) {
      // 'self' has its own liveness
      ++start;
    }
    assert(start == endEltOffset);
    return;
  }

  if (auto *enumDecl = type.getEnumOrBoundGenericEnum()) {
    struct ElementRecord {
      EnumElementDecl *element;
      unsigned start;
      unsigned next;
    };
    SmallVector<ElementRecord, 2> projectedElements;
    unsigned runningStart = startEltOffset;
    for (auto *eltDecl : enumDecl->getAllElements()) {
      if (!eltDecl->hasAssociatedValues())
        continue;

      auto eltTy = type.getEnumElementType(eltDecl, fn);
      unsigned next = runningStart + TypeSubElementCount(eltTy, fn);
      if (noneSet(filterBitVector, runningStart, next)) {
        runningStart = next;
        continue;
      }

      projectedElements.push_back({eltDecl, runningStart, next});
      runningStart = next;
    }
    assert((runningStart + 1 + (type.isValueTypeWithDeinit() ? 1 : 0)) ==
           endEltOffset);

    if (!allSet(filterBitVector, startEltOffset, endEltOffset)) {
      TinyPtrVector<SwitchEnumAddrInst *> seais;
      for (auto record : projectedElements) {
        // Find a preexisting unchecked_take_enum_data_addr that dominates
        // insertPt.
        bool foundProjection = false;
        StackList<SILValue> worklist(value->getFunction());
        worklist.push_back(value);
        while (!worklist.empty()) {
          auto v = worklist.pop_back_val();
          for (auto *user : v->getUsers()) {
            if (auto *ddi = dyn_cast<DropDeinitInst>(user)) {
              worklist.push_back(ddi);
              continue;
            }
            if (auto *seai = dyn_cast<SwitchEnumAddrInst>(user)) {
              seais.push_back(seai);
            }
            auto *utedai = dyn_cast<UncheckedTakeEnumDataAddrInst>(user);
            if (!utedai) {
              continue;
            }
            if (utedai->getElement() != record.element) {
              continue;
            }
            if (!domTree->dominates(utedai, insertPt)) {
              continue;
            }

            callback(utedai, TypeTreeLeafTypeRange(record.start, record.next),
                     NeedsDestroy);
            foundProjection = true;
          }
        }
        (void)foundProjection;
        assert(foundProjection ||
               llvm::count_if(
                   enumDecl->getAllElements(),
                   [](auto *elt) { return elt->hasAssociatedValues(); }) == 1 ||
               isDominatedByPayloadlessSwitchEnumAddrDests(insertPt, seais,
                                                           domTree));
      }
      return;
    }

    // Then just pass back our enum base value as the pointer.
    callback(value, TypeTreeLeafTypeRange(startEltOffset, endEltOffset),
             NeedsDestroy);
    return;
  }

  if (auto tupleType = type.getAs<TupleType>()) {
    unsigned start = startEltOffset;
    for (unsigned index : indices(tupleType.getElementTypes())) {
      auto nextType = type.getTupleElementType(index);
      unsigned next = start + TypeSubElementCount(nextType, fn);

      if (noneSet(filterBitVector, start, next)) {
        start = next;
        continue;
      }

      auto newValue = builder.createTupleElementAddr(loc, value, index);
      callback(newValue, TypeTreeLeafTypeRange(start, next), NeedsDestroy);
      start = next;
    }
    assert(start == endEltOffset);
    return;
  }

  llvm_unreachable("Not understand subtype");
}

void TypeTreeLeafTypeRange::get(
    Operand *op, SILValue rootValue,
    SmallVectorImpl<TypeTreeLeafTypeRange> &ranges) {
  auto projectedValue = op->get();
  auto startEltOffset = SubElementOffset::compute(projectedValue, rootValue);
  if (!startEltOffset)
    return;

  // A drop_deinit only consumes the deinit bit of its operand.
  if (isa<DropDeinitInst>(op->getUser())) {
    auto upperBound = *startEltOffset + TypeSubElementCount(projectedValue);
    ranges.push_back({upperBound - 1, upperBound});
    return;
  }

  // An `inject_enum_addr` only initializes the enum tag.
  if (auto inject = dyn_cast<InjectEnumAddrInst>(op->getUser())) {
    // Subtract the deinit bit, if any: the discriminator bit is before it:
    //
    // [ case1 bits ..., case2 bits, ..., discriminator bit, deinit bit ]
    auto deinitBits = projectedValue->getType().isValueTypeWithDeinit() ? 1 : 0;
    auto upperBound =
        *startEltOffset + TypeSubElementCount(projectedValue) - deinitBits;
    ranges.push_back({upperBound - 1, upperBound});
    return;
  }

  if (auto *utedai = dyn_cast<UncheckedTakeEnumDataAddrInst>(op->getUser())) {
    auto *selected = utedai->getElement();
    auto *enumDecl = utedai->getEnumDecl();
    unsigned numAtoms = 0;
    for (auto *element : enumDecl->getAllElements()) {
      if (!element->hasAssociatedValues()) {
        continue;
      }
      auto elementTy = projectedValue->getType().getEnumElementType(
          element, op->getFunction());
      auto elementAtoms =
          unsigned(TypeSubElementCount(elementTy, op->getFunction()));
      if (element != selected) {
        ranges.push_back({*startEltOffset + numAtoms,
                          *startEltOffset + numAtoms + elementAtoms});
      }
      numAtoms += elementAtoms;
    }
    // The discriminator bit is consumed.
    ranges.push_back(
        {*startEltOffset + numAtoms, *startEltOffset + numAtoms + 1});
    // The deinit bit is _not_ consumed.  A drop_deinit is required to
    // consumingly switch an enum with a deinit.
    return;
  }

  // Uses that borrow a value do not involve the deinit bit.
  //
  // FIXME: This shouldn't be limited to applies.
  unsigned deinitBitOffset = 0;
  if (op->get()->getType().isValueTypeWithDeinit() &&
      op->getOperandOwnership() == OperandOwnership::Borrow &&
      ApplySite::isa(op->getUser())) {
    deinitBitOffset = 1;
  }

  ranges.push_back({*startEltOffset, *startEltOffset +
                                         TypeSubElementCount(projectedValue) -
                                         deinitBitOffset});
}

void TypeTreeLeafTypeRange::constructProjectionsForNeededElements(
    SILValue rootValue, SILInstruction *insertPt, DominanceInfo *domTree,
    SmallBitVector &neededElements,
    SmallVectorImpl<std::tuple<SILValue, TypeTreeLeafTypeRange, NeedsDestroy_t>>
        &resultingProjections) {
  TypeTreeLeafTypeRange rootRange(rootValue);
  (void)rootRange;
  assert(rootRange.size() == neededElements.size());

  StackList<std::tuple<SILValue, TypeTreeLeafTypeRange, NeedsDestroy_t>>
      worklist(insertPt->getFunction());
  worklist.push_back({rootValue, rootRange, NeedsDestroy});

  // Temporary vector we use for our computation.
  SmallBitVector tmp(neededElements.size());

  auto allInRange = [](const SmallBitVector &bv, TypeTreeLeafTypeRange span) {
    return llvm::all_of(span.getRange(),
                        [&bv](unsigned index) { return bv[index]; });
  };

  while (!worklist.empty()) {
    auto pair = worklist.pop_back_val();
    SILValue value;
    TypeTreeLeafTypeRange range;
    NeedsDestroy_t needsDestroy;
    std::tie(value, range, needsDestroy) = pair;

    tmp.reset();
    tmp.set(range.startEltOffset, range.endEltOffset);

    tmp &= neededElements;

    // If we do not have any unpaired bits in this range, just continue... we do
    // not have any further work to do.
    if (tmp.none()) {
      continue;
    }

    // Otherwise, we had some sort of overlap. First lets see if we have
    // everything set in the range. In that case, we just add this range to the
    // result and continue.
    if (allInRange(tmp, range)) {
      resultingProjections.emplace_back(value, range, needsDestroy);
      continue;
    }

    // Otherwise, we have a partial range. We need to split our range and then
    // recursively process those ranges looking for subranges that have
    // completely set bits.
    range.constructFilteredProjections(
        value, insertPt, neededElements, domTree,
        [&](SILValue subType, TypeTreeLeafTypeRange range,
            NeedsDestroy_t needsDestroy) -> bool {
          worklist.push_back({subType, range, needsDestroy});
          return true;
        });
  }
}

void TypeTreeLeafTypeRange::visitContiguousRanges(
    SmallBitVector const &bits,
    llvm::function_ref<void(TypeTreeLeafTypeRange)> callback) {
  if (bits.size() == 0)
    return;

  std::optional<unsigned> current = std::nullopt;
  for (unsigned bit = 0, size = bits.size(); bit < size; ++bit) {
    auto isSet = bits.test(bit);
    if (current) {
      if (!isSet) {
        callback(TypeTreeLeafTypeRange(*current, bit));
        current = std::nullopt;
      }
    } else if (isSet) {
      current = bit;
    }
  }
  if (current) {
    callback(TypeTreeLeafTypeRange(*current, bits.size()));
  }
}

//===----------------------------------------------------------------------===//
//                    MARK: FieldSensitivePrunedLiveBlocks
//===----------------------------------------------------------------------===//

void FieldSensitivePrunedLiveBlocks::computeScalarUseBlockLiveness(
    SILBasicBlock *userBB, unsigned bitNo) {
  // If, we are visiting this block, then it is not already LiveOut. Mark it
  // LiveWithin to indicate a liveness boundary within the block.
  markBlockLive(userBB, bitNo, LiveWithin);

  BasicBlockWorklist worklist(userBB->getFunction());
  worklist.push(userBB);

  while (auto *block = worklist.pop()) {
    // The popped `bb` is live; now mark all its predecessors LiveOut.
    //
    // Traversal terminates at any previously visited block, including the
    // blocks initialized as definition blocks.
    for (auto *predBlock : block->getPredecessorBlocks()) {
      switch (getBlockLiveness(predBlock, bitNo)) {
      case Dead:
        worklist.pushIfNotVisited(predBlock);
        LLVM_FALLTHROUGH;
      case LiveWithin:
        markBlockLive(predBlock, bitNo, LiveOut);
        break;
      case DeadToLiveEdge:
      case LiveOut:
        break;
      }
    }
  }
}

/// Update the current def's liveness based on one specific use instruction.
///
/// Return the updated liveness of the \p use block (LiveOut or LiveWithin).
///
/// Terminators are not live out of the block.
void FieldSensitivePrunedLiveBlocks::updateForUse(
    SILInstruction *user, unsigned startBitNo, unsigned endBitNo,
    SmallBitVector const &useBeforeDefBits,
    SmallVectorImpl<IsLive> &resultingLivenessInfo) {
  assert(isInitialized());
  resultingLivenessInfo.clear();

  SWIFT_ASSERT_ONLY(seenUse = true);

  auto *bb = user->getParent();
  getBlockLiveness(bb, startBitNo, endBitNo, resultingLivenessInfo);
  assert(resultingLivenessInfo.size() == (endBitNo - startBitNo));

  for (unsigned index : indices(resultingLivenessInfo)) {
    unsigned specificBitNo = startBitNo + index;
    auto isUseBeforeDef = useBeforeDefBits.test(specificBitNo);
    switch (resultingLivenessInfo[index]) {
    case LiveOut:
    case LiveWithin:
      if (!isUseBeforeDef) {
        continue;
      } else {
        LLVM_FALLTHROUGH;
      }
    case DeadToLiveEdge:
    case Dead: {
      // This use block has not yet been marked live. Mark it and its
      // predecessor blocks live.
      computeScalarUseBlockLiveness(bb, specificBitNo);
      resultingLivenessInfo[index] = getBlockLiveness(bb, specificBitNo);
      continue;
    }
    }
    llvm_unreachable("covered switch");
  }
}

llvm::StringRef
FieldSensitivePrunedLiveBlocks::getStringRef(IsLive isLive) const {
  switch (isLive) {
  case Dead:
    return "Dead";
  case LiveWithin:
    return "LiveWithin";
  case DeadToLiveEdge:
    return "DeadToLiveEdge";
  case LiveOut:
    return "LiveOut";
  }
  llvm_unreachable("Covered switch?!");
}

void FieldSensitivePrunedLiveBlocks::print(llvm::raw_ostream &OS) const {
  if (!discoveredBlocks) {
    OS << "No deterministic live block list\n";
    return;
  }
  SmallVector<IsLive, 8> isLive;
  for (auto *block : *discoveredBlocks) {
    block->printAsOperand(OS);
    OS << ": ";
    for (unsigned i : range(getNumBitsToTrack()))
      OS << getStringRef(this->getBlockLiveness(block, i)) << ", ";
    OS << "\n";
  }
}

void FieldSensitivePrunedLiveBlocks::dump() const { print(llvm::dbgs()); }

//===----------------------------------------------------------------------===//
//                   FieldSensitivePrunedLivenessBoundary
//===----------------------------------------------------------------------===//

void FieldSensitivePrunedLivenessBoundary::print(llvm::raw_ostream &OS) const {
  for (auto pair : lastUsers) {
    auto *user = pair.first;
    auto bits = pair.second;
    OS << "last user: " << *user 
       << "\tat " << bits << "\n";
  }
  for (auto pair : boundaryEdges) {
    auto *block = pair.first;
    auto bits = pair.second;
    OS << "boundary edge: ";
    block->printAsOperand(OS);
    OS << "\n" << "\tat " << bits << "\n";
  }
  if (!deadDefs.empty()) {
    for (auto pair : deadDefs) {
      auto *deadDef = pair.first;
      auto bits = pair.second;
      OS << "dead def: " << *deadDef 
         << "\tat " << bits << "\n";
    }
  }
}

void FieldSensitivePrunedLivenessBoundary::dump() const {
  print(llvm::dbgs());
}

//===----------------------------------------------------------------------===//
//                        MARK: FieldSensitiveLiveness
//===----------------------------------------------------------------------===//

void FieldSensitivePrunedLiveness::updateForUse(
    SILInstruction *user, TypeTreeLeafTypeRange range, bool lifetimeEnding,
    SmallBitVector const &useBeforeDefBits) {
  SmallVector<FieldSensitivePrunedLiveBlocks::IsLive, 8> resultingLiveness;
  liveBlocks.updateForUse(user, range.startEltOffset, range.endEltOffset,
                          useBeforeDefBits, resultingLiveness);

  addInterestingUser(user, range, lifetimeEnding);
}

void FieldSensitivePrunedLiveness::updateForUse(
    SILInstruction *user, SmallBitVector const &bits, bool lifetimeEnding,
    SmallBitVector const &useBeforeDefBits) {
  for (auto bit : bits.set_bits()) {
    liveBlocks.updateForUse(user, bit, useBeforeDefBits.test(bit));
  }

  addInterestingUser(user, bits, lifetimeEnding);
}

void FieldSensitivePrunedLiveness::extendToNonUse(
    SILInstruction *user, TypeTreeLeafTypeRange range,
    SmallBitVector const &useBeforeDefBits) {
  SmallVector<FieldSensitivePrunedLiveBlocks::IsLive, 8> resultingLiveness;
  liveBlocks.updateForUse(user, range.startEltOffset, range.endEltOffset,
                          useBeforeDefBits, resultingLiveness);

  extendToNonUse(user, range);
}

void FieldSensitivePrunedLiveness::extendToNonUse(
    SILInstruction *user, SmallBitVector const &bits,
    SmallBitVector const &useBeforeDefBits) {
  for (auto bit : bits.set_bits()) {
    liveBlocks.updateForUse(user, bit, useBeforeDefBits.test(bit));
  }

  extendToNonUse(user, bits);
}

void FieldSensitivePrunedLiveness::print(llvm::raw_ostream &os) const {
  liveBlocks.print(os);
  for (auto &userAndInterest : users) {
    for (size_t bit = 0, size = userAndInterest.second.liveBits.size();
         bit < size; ++bit) {
      auto isLive = userAndInterest.second.liveBits.test(bit);
      auto isConsuming = userAndInterest.second.consumingBits.test(bit);
      if (!isLive && !isConsuming) {
        continue;
      } else if (!isLive && isConsuming) {
        os << "non-user: ";
      } else if (isLive && isConsuming) {
        os << "lifetime-ending user: ";
      } else if (isLive && !isConsuming) {
        os << "regular user: ";
      }
      os << *userAndInterest.first << "\tat " << bit << "\n";
    }
  }
}

namespace swift::test {
// Arguments:
// - SILValue: def whose pruned liveness will be calculated
// - the string "uses:"
// - variadic list of live-range user instructions
// Dumps:
// -
static FunctionTest FieldSensitiveSSAUseLivenessTest(
    "fs_ssa_use_liveness", [](auto &function, auto &arguments, auto &test) {
      auto value = arguments.takeValue();
      auto begin = (unsigned)arguments.takeUInt();
      auto end = (unsigned)arguments.takeUInt();

      SmallVector<SILBasicBlock *, 8> discoveredBlocks;
      FieldSensitiveSSAPrunedLiveRange liveness(&function, &discoveredBlocks);
      liveness.init(value);
      liveness.initializeDef(value, TypeTreeLeafTypeRange(begin, end));

      auto argument = arguments.takeArgument();
      if (cast<StringArgument>(argument).getValue() != "uses:") {
        llvm::report_fatal_error(
            "test specification expects the 'uses:' label\n");
      }
      while (arguments.hasUntaken()) {
        auto *inst = arguments.takeInstruction();
        auto kindString = arguments.takeString();
        enum Kind {
          NonUse,
          Ending,
          NonEnding,
        };
        auto kind = llvm::StringSwitch<std::optional<Kind>>(kindString)
                        .Case("non-use", Kind::NonUse)
                        .Case("ending", Kind::Ending)
                        .Case("non-ending", Kind::NonEnding)
                        .Default(std::nullopt);
        if (!kind.has_value()) {
          llvm::errs() << "Unknown kind: " << kindString << "\n";
          llvm::report_fatal_error("Bad user kind.  Value must be one of "
                                   "'non-use', 'ending', 'non-ending'");
        }
        auto begin = (unsigned)arguments.takeUInt();
        auto end = (unsigned)arguments.takeUInt();
        switch (kind.value()) {
        case Kind::NonUse:
          liveness.extendToNonUse(inst, TypeTreeLeafTypeRange(begin, end));
          break;
        case Kind::Ending:
          liveness.updateForUse(inst, TypeTreeLeafTypeRange(begin, end),
                                /*lifetimeEnding*/ true);
          break;
        case Kind::NonEnding:
          liveness.updateForUse(inst, TypeTreeLeafTypeRange(begin, end),
                                /*lifetimeEnding*/ false);
          break;
        }
      }

      liveness.print(llvm::outs());

      FieldSensitivePrunedLivenessBoundary boundary(1);
      liveness.computeBoundary(boundary);
      boundary.print(llvm::outs());
    });

} // end namespace swift::test

//===----------------------------------------------------------------------===//
//                    MARK: FieldSensitivePrunedLiveRange
//===----------------------------------------------------------------------===//

template <typename LivenessWithDefs>
bool FieldSensitivePrunedLiveRange<LivenessWithDefs>::isWithinBoundary(
    SILInstruction *inst, SmallBitVector const &bits) const {
  assert(asImpl().isInitialized());

  PRUNED_LIVENESS_LOG(llvm::dbgs()
                      << "FieldSensitivePrunedLiveRange::isWithinBoundary!\n"
                      << "Bits: " << bits << "\n");

  // If we do not have any span, return true since we have no counter examples.
  if (bits.empty()) {
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "    span is empty! Returning true!\n");
    return true;
  }

  using IsLive = FieldSensitivePrunedLiveBlocks::IsLive;

  auto *block = inst->getParent();

  SmallVector<IsLive, 8> outVector;
  getBlockLiveness(block, bits, outVector);

  for (auto bitAndIndex : llvm::enumerate(bits.set_bits())) {
    unsigned bit = bitAndIndex.value();
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Visiting bit: " << bit << '\n');
    bool isLive = false;
    switch (outVector[bitAndIndex.index()]) {
    case FieldSensitivePrunedLiveBlocks::DeadToLiveEdge:
    case FieldSensitivePrunedLiveBlocks::Dead:
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "        Dead... continuing!\n");
      // We are only not within the boundary if all of our bits are dead. We
      // track this via allDeadBits. So, just continue.
      continue;
    case FieldSensitivePrunedLiveBlocks::LiveOut:
      // If we are LiveOut and are not a def block, then we know that we are
      // within the boundary for this bit. We consider ourselves to be within
      // the boundary if /any/ of our bits are within the boundary. So return
      // true.
      if (!asImpl().isDefBlock(block, bit)) {
        PRUNED_LIVENESS_LOG(
            llvm::dbgs()
            << "        LiveOut... but not in a def block... returning true "
               "since we are within the boundary for at least one bit");
        return true;
      }

      isLive = true;
      PRUNED_LIVENESS_LOG(llvm::dbgs()
                 << "        LiveOut, but a def block... searching block!\n");
      [[clang::fallthrough]];
    case FieldSensitivePrunedLiveBlocks::LiveWithin:
      bool shouldContinue = false;
      if (!isLive)
        PRUNED_LIVENESS_LOG(llvm::dbgs() << "        LiveWithin... searching block!\n");

      // Now check if the instruction is between a last use and a definition.
      for (auto &blockInst : llvm::reverse(*block)) {
        PRUNED_LIVENESS_LOG(llvm::dbgs() << "        Inst: Live: "
                                << (isLive ? "true" : "false") << "\n"
                                << "    " << blockInst);

        // First if we see a def, set isLive to false.
        if (asImpl().isDef(&blockInst, bit)) {
          PRUNED_LIVENESS_LOG(llvm::dbgs()
                     << "        Inst is a def... marking live to false!\n");
          isLive = false;
        }

        // Then check if we found our instruction in the block...
        if (&blockInst == inst) {
          PRUNED_LIVENESS_LOG(llvm::dbgs()
                     << "        Inst is inst we are looking for.\n");

          // If we are live in the block when we reach the inst... we must be in
          // the block.
          if (isLive) {
            PRUNED_LIVENESS_LOG(llvm::dbgs()
                       << "        Inst was live... so returning true!\n");
            return true;
          }

          // Otherwise, we know that we are not within the boundary for this
          // def... continue.
          shouldContinue = true;
          PRUNED_LIVENESS_LOG(llvm::dbgs()
                     << "        Inst was dead... so breaking out of loop!\n");
          break;
        }

        // If we are not live and have an interesting user that maps to our bit,
        // mark this bit as being live again.
        if (!isLive) {
          bool isInteresting = isInterestingUser(&blockInst, bit);
          PRUNED_LIVENESS_LOG(llvm::dbgs()
                     << "        Inst was dead... Is InterestingUser: "
                     << (isInteresting ? "true" : "false") << '\n');
          isLive |= isInteresting;
        }
      }

      // If we broke out of the inner loop, continue.
      if (shouldContinue)
        continue;
      llvm_unreachable("Inst not in parent block?!");
    }
  }

  // We succeeded in proving we are not within the boundary for any of our bits.
  return false;
}

static StringRef getStringRef(FieldSensitivePrunedLiveBlocks::IsLive isLive) {
  switch (isLive) {
  case FieldSensitivePrunedLiveBlocks::Dead:
    return "Dead";
  case FieldSensitivePrunedLiveBlocks::LiveWithin:
    return "LiveWithin";
  case FieldSensitivePrunedLiveBlocks::DeadToLiveEdge:
    return "DeadToLiveEdge";
  case FieldSensitivePrunedLiveBlocks::LiveOut:
    return "LiveOut";
  }
}

template <typename LivenessWithDefs>
void FieldSensitivePrunedLiveRange<LivenessWithDefs>::computeBoundary(
    FieldSensitivePrunedLivenessBoundary &boundary) const {
  assert(asImpl().isInitialized());

  PRUNED_LIVENESS_LOG(llvm::dbgs() << "Liveness Boundary Compuation!\n");

  using IsLive = FieldSensitivePrunedLiveBlocks::IsLive;
  SmallVector<IsLive, 8> isLiveTmp;
  for (SILBasicBlock *block : getDiscoveredBlocks()) {
    SWIFT_DEFER { isLiveTmp.clear(); };
    getBlockLiveness(block, isLiveTmp);

    PRUNED_LIVENESS_LOG(llvm::dbgs()
               << "Checking for boundary in bb" << block->getDebugID() << '\n');

    // Process each block that has not been visited and is not LiveOut.
    bool foundAnyNonDead = false;
    for (auto pair : llvm::enumerate(isLiveTmp)) {
      unsigned index = pair.index();
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "Bit: " << index << ". Liveness: "
                              << getStringRef(pair.value()) << '\n');
      switch (pair.value()) {
      case FieldSensitivePrunedLiveBlocks::LiveOut:
        for (SILBasicBlock *succBB : block->getSuccessors()) {
          if (FieldSensitivePrunedLiveBlocks::isDead(
                getBlockLiveness(succBB, index))) {
            // If the basic block ends in unreachable, don't consider it a
            // boundary.
            // TODO: Should also do this if the block's successors all always
            // end in unreachable too.
            if (isa<UnreachableInst>(succBB->getTerminator())) {
              PRUNED_LIVENESS_LOG(llvm::dbgs() << "succBB ends in unreachable, skipping as boundary edge: bb"
                                      << succBB->getDebugID() << '\n');
            } else {
              PRUNED_LIVENESS_LOG(llvm::dbgs() << "Marking succBB as boundary edge: bb"
                                      << succBB->getDebugID() << '\n');
              boundary.getBoundaryEdgeBits(succBB).set(index);
            }
          }
        }
        asImpl().findBoundariesInBlock(block, index, /*isLiveOut*/ true,
                                       boundary);
        foundAnyNonDead = true;
        break;
      case FieldSensitivePrunedLiveBlocks::LiveWithin: {
        asImpl().findBoundariesInBlock(block, index, /*isLiveOut*/ false,
                                       boundary);
        foundAnyNonDead = true;
        break;
      }
      case FieldSensitivePrunedLiveBlocks::DeadToLiveEdge:
        foundAnyNonDead = true;
        LLVM_FALLTHROUGH;
      case FieldSensitivePrunedLiveBlocks::Dead:
        // We do not assert here like in the normal pruned liveness
        // implementation since we can have dead on some bits and liveness along
        // others.
        break;
      }
    }
    assert(foundAnyNonDead && "We should have found atleast one non-dead bit");
  }
}

namespace swift::test {
// Arguments:
// - value: entity whose fields' livenesses are being computed
// - string: "defs:"
// - variadic list of triples consisting of
//   - value: a live-range defining value
//   - int: the beginning of the range of fields defined by the value
//   - int: the end of the range of the fields defined by the value
// - the string "uses:"
// - variadic list of quadruples consisting of
//   - instruction: a live-range user
//   - bool: whether the user is lifetime-ending
//   - int: the beginning of the range of fields used by the instruction
//   - int: the end of the range of fields used by the instruction
// Dumps:
// - the liveness result and boundary
//
// Computes liveness for the specified def nodes by considering the
// specified uses. The actual uses of the def nodes are ignored.
//
// This is useful for testing non-ssa liveness, for example, of memory
// locations. In that case, the def nodes may be stores and the uses may be
// destroy_addrs.
static FunctionTest FieldSensitiveMultiDefUseLiveRangeTest(
    "fieldsensitive-multidefuse-liverange",
    [](auto &function, auto &arguments, auto &test) {
      SmallVector<SILBasicBlock *, 8> discoveredBlocks;
      auto value = arguments.takeValue();
      FieldSensitiveMultiDefPrunedLiveRange liveness(&function, value,
                                                     &discoveredBlocks);

      llvm::outs() << "FieldSensitive MultiDef lifetime analysis:\n";
      if (arguments.takeString() != "defs:") {
        llvm::report_fatal_error(
            "test specification expects the 'defs:' label\n");
      }
      while (true) {
        auto argument = arguments.takeArgument();
        if (isa<StringArgument>(argument)) {
          if (cast<StringArgument>(argument).getValue() != "uses:") {
            llvm::report_fatal_error(
                "test specification expects the 'uses:' label\n");
          }
          break;
        }
        auto begin = arguments.takeUInt();
        auto end = arguments.takeUInt();
        TypeTreeLeafTypeRange range(begin, end);
        if (isa<InstructionArgument>(argument)) {
          auto *instruction = cast<InstructionArgument>(argument).getValue();
          llvm::outs() << "  def in range [" << begin << ", " << end
                       << ") instruction: " << *instruction;
          liveness.initializeDef(instruction, range);
          continue;
        }
        if (isa<ValueArgument>(argument)) {
          SILValue value = cast<ValueArgument>(argument).getValue();
          llvm::outs() << "  def in range [" << begin << ", " << end
                       << ") value: " << value;
          liveness.initializeDef(value, range);
          continue;
        }
        llvm::report_fatal_error(
            "test specification expects the 'uses:' label\n");
      }
      liveness.finishedInitializationOfDefs();
      while (arguments.hasUntaken()) {
        auto *inst = arguments.takeInstruction();
        auto lifetimeEnding = arguments.takeBool();
        auto begin = arguments.takeUInt();
        auto end = arguments.takeUInt();
        TypeTreeLeafTypeRange range(begin, end);
        liveness.updateForUse(inst, range, lifetimeEnding);
      }
      liveness.print(llvm::outs());

      FieldSensitivePrunedLivenessBoundary boundary(
          liveness.getNumSubElements());
      liveness.computeBoundary(boundary);
      boundary.print(llvm::outs());
    });
} // end namespace swift::test

bool FieldSensitiveMultiDefPrunedLiveRange::isUserBeforeDef(
    SILInstruction *user, unsigned element) const {
  auto *block = user->getParent();
  if (!isDefBlock(block, element))
    return false;

  if (llvm::any_of(block->getArguments(), [this, element](SILArgument *arg) {
        return isDef(arg, element);
      })) {
    return false;
  }

  auto *current = user;
  while (true) {
    // If user is also a def, then the use is considered before the def.
    current = current->getPreviousInstruction();
    if (!current)
      return true;

    if (isDef(current, element))
      return false;
  }
}

template <typename LivenessWithDefs>
void FieldSensitivePrunedLiveRange<LivenessWithDefs>::updateForUse(
    SILInstruction *user, TypeTreeLeafTypeRange range, bool lifetimeEnding) {
  SmallBitVector useBeforeDefBits(getNumSubElements());
  asImpl().isUserBeforeDef(user, range.getRange(), useBeforeDefBits);
  FieldSensitivePrunedLiveness::updateForUse(user, range, lifetimeEnding,
                                             useBeforeDefBits);
}

template <typename LivenessWithDefs>
void FieldSensitivePrunedLiveRange<LivenessWithDefs>::updateForUse(
    SILInstruction *user, SmallBitVector const &bits, bool lifetimeEnding) {
  SmallBitVector useBeforeDefBits(getNumSubElements());
  asImpl().isUserBeforeDef(user, bits.set_bits(), useBeforeDefBits);
  FieldSensitivePrunedLiveness::updateForUse(user, bits, lifetimeEnding,
                                             useBeforeDefBits);
}

template <typename LivenessWithDefs>
void FieldSensitivePrunedLiveRange<LivenessWithDefs>::extendToNonUse(
    SILInstruction *user, TypeTreeLeafTypeRange range) {
  SmallBitVector useBeforeDefBits(getNumSubElements());
  asImpl().isUserBeforeDef(user, range.getRange(), useBeforeDefBits);
  FieldSensitivePrunedLiveness::extendToNonUse(user, range, useBeforeDefBits);
}

template <typename LivenessWithDefs>
void FieldSensitivePrunedLiveRange<LivenessWithDefs>::extendToNonUse(
    SILInstruction *user, SmallBitVector const &bits) {
  SmallBitVector useBeforeDefBits(getNumSubElements());
  asImpl().isUserBeforeDef(user, bits.set_bits(), useBeforeDefBits);
  FieldSensitivePrunedLiveness::extendToNonUse(user, bits, useBeforeDefBits);
}

//===----------------------------------------------------------------------===//
//                    MARK: Boundary Computation Utilities
//===----------------------------------------------------------------------===//

/// Given live-within (non-live-out) \p block, find the last user.
void findBoundaryInNonDefBlock(SILBasicBlock *block, unsigned bitNo,
                               FieldSensitivePrunedLivenessBoundary &boundary,
                               const FieldSensitivePrunedLiveness &liveness) {
  assert(liveness.getBlockLiveness(block, bitNo) ==
         FieldSensitivePrunedLiveBlocks::LiveWithin);

  PRUNED_LIVENESS_LOG(llvm::dbgs() << "Looking for boundary in non-def block\n");
  for (SILInstruction &inst : llvm::reverse(*block)) {
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "Visiting: " << inst);
    if (liveness.isInterestingUser(&inst, bitNo)) {
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Is interesting user for this bit!\n");
      boundary.getLastUserBits(&inst).set(bitNo);
      return;
    }
  }
  llvm_unreachable("live-within block must contain an interesting use");
}

/// Given a live-within \p block that contains an SSA definition, and knowledge
/// that all live uses are dominated by that single definition, find either the
/// last user or a dead def.
///
/// A live range with a single definition cannot have any uses above that
/// definition in the same block. This even holds for unreachable self-loops.
///
/// Precondition: Caller must have chwecked that ssaDef's span contains bitNo.
void findBoundaryInSSADefBlock(SILNode *ssaDef, unsigned bitNo,
                               FieldSensitivePrunedLivenessBoundary &boundary,
                               const FieldSensitivePrunedLiveness &liveness) {
  // defInst is null for argument defs.
  PRUNED_LIVENESS_LOG(llvm::dbgs() << "Searching using findBoundaryInSSADefBlock.\n");
  SILInstruction *defInst = dyn_cast<SILInstruction>(ssaDef);
  for (SILInstruction &inst : llvm::reverse(*getDefinedInBlock(ssaDef))) {
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "Visiting: " << inst);
    if (&inst == defInst) {
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Found dead def: " << *defInst);
      boundary.getDeadDefsBits(cast<SILNode>(&inst)).set(bitNo);
      return;
    }
    if (liveness.isInterestingUser(&inst, bitNo)) {
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Found interesting user: " << inst);
      boundary.getLastUserBits(&inst).set(bitNo);
      return;
    }
  }

  if (auto *deadArg = dyn_cast<SILArgument>(ssaDef)) {
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Found dead arg: " << *deadArg);
    boundary.getDeadDefsBits(deadArg).set(bitNo);
    return;
  }
  
  // If we searched the success branch of a try_apply and found no uses, then
  // the try_apply itself is a dead def.
  if (isa<TryApplyInst>(ssaDef)) {
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Found dead try_apply: " << *ssaDef);
    boundary.getDeadDefsBits(ssaDef).set(bitNo);
    return;
  }
  
  llvm_unreachable("def not found?!");
}

//===----------------------------------------------------------------------===//
//                   MARK: FieldSensitiveSSAPrunedLiveRange
//===----------------------------------------------------------------------===//

namespace swift {
template class FieldSensitivePrunedLiveRange<FieldSensitiveSSAPrunedLiveRange>;
} // namespace swift

void FieldSensitiveSSAPrunedLiveRange::findBoundariesInBlock(
    SILBasicBlock *block, unsigned bitNo, bool isLiveOut,
    FieldSensitivePrunedLivenessBoundary &boundary) const {
  assert(isInitialized());

  // For SSA, a live-out block cannot have a boundary.
  if (isLiveOut)
    return;

  // Handle live-within block
  if (!isDefBlock(block, bitNo)) {
    findBoundaryInNonDefBlock(block, bitNo, boundary, *this);
    return;
  }

  // Find either the last user or a dead def
  assert(def.second->contains(bitNo));
  auto *defInst = def.first->getDefiningInstruction();
  SILNode *defNode =
      defInst ? cast<SILNode>(defInst) : cast<SILArgument>(def.first);
  findBoundaryInSSADefBlock(defNode, bitNo, boundary, *this);
}

//===----------------------------------------------------------------------===//
//                MARK: FieldSensitiveMultiDefPrunedLiveRange
//===----------------------------------------------------------------------===//

namespace swift {
template class FieldSensitivePrunedLiveRange<
    FieldSensitiveMultiDefPrunedLiveRange>;
} // namespace swift

void FieldSensitiveMultiDefPrunedLiveRange::findBoundariesInBlock(
    SILBasicBlock *block, unsigned bitNo, bool isLiveOut,
    FieldSensitivePrunedLivenessBoundary &boundary) const {
  assert(isInitialized());

  PRUNED_LIVENESS_LOG(llvm::dbgs() << "Checking for boundary in bb"
                          << block->getDebugID() << " for bit: " << bitNo
                          << ". Is Live: " << (isLiveOut ? "true" : "false")
                          << '\n');

  if (!isDefBlock(block, bitNo)) {
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Not a def block for this bit?!\n");
    // A live-out block that does not contain any defs cannot have a boundary.
    if (isLiveOut) {
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Is live out... nothing further to do.\n");
      return;
    }

    PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Is LiveWithin, so looking for boundary "
                               "in non-def block?!\n");
    findBoundaryInNonDefBlock(block, bitNo, boundary, *this);
    return;
  }

  PRUNED_LIVENESS_LOG(llvm::dbgs() << "Is def block!\n");

  // Handle def blocks...
  //
  // First, check for an SSA live range
  if (defs.size() == 1) {
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "Has single def...\n");
    // For SSA, a live-out block cannot have a boundary.
    if (isLiveOut) {
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "Is live out... no further work to do...\n");
      return;
    }

    PRUNED_LIVENESS_LOG(llvm::dbgs() << "Is live within... checking for boundary "
                               "using SSA def block impl.\n");
    assert(defs.vector_begin()->second->contains(bitNo));
    findBoundaryInSSADefBlock(defs.vector_begin()->first, bitNo, boundary,
                              *this);
    return;
  }

  PRUNED_LIVENESS_LOG(llvm::dbgs() << "Has multiple defs!\n");

  // Handle a live-out or live-within block with potentially multiple defs
#ifndef NDEBUG
  // We only use prevCount when checking a specific invariant when asserts are
  // enabled. boundary.getNumLastUsersAndDeadDefs actually asserts if you try to
  // call it in a non-asserts compiler since it is relatively inefficient and
  // not needed.
  unsigned prevCount = boundary.getNumLastUsersAndDeadDefs(bitNo);
#endif
  bool isLive = isLiveOut;
  for (auto &inst : llvm::reverse(*block)) {
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "Visiting: " << inst);
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Initial IsLive: "
                            << (isLive ? "true" : "false") << '\n');

    // Check if the instruction is a def before checking whether it is a
    // use. The same instruction can be both a dead def and boundary use.
    if (isDef(&inst, bitNo)) {
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Is a def inst!\n");
      if (!isLive) {
        PRUNED_LIVENESS_LOG(llvm::dbgs() << "        We are not live... so mark as dead "
                                   "def and keep isLive false!\n");
        boundary.getDeadDefsBits(cast<SILNode>(&inst)).set(bitNo);
      } else {
        PRUNED_LIVENESS_LOG(
            llvm::dbgs()
            << "        Is live usage... so just mark isLive to false.\n");
      }
      isLive = false;
    }

    // Note: the same instruction could potentially be both a dead def and last
    // user. The liveness boundary supports this, although it won't happen in
    // any context where we care about inserting code on the boundary.
    PRUNED_LIVENESS_LOG(llvm::dbgs()
               << "    Checking if this inst is also a last user...\n");
    if (!isLive) {
      if (isInterestingUser(&inst, bitNo)) {
        PRUNED_LIVENESS_LOG(
            llvm::dbgs()
            << "        Was interesting user! Moving from dead -> live!\n");
        boundary.getLastUserBits(&inst).set(bitNo);
        isLive = true;
      } else {
        PRUNED_LIVENESS_LOG(llvm::dbgs()
                   << "        Not interesting user... keeping dead!\n");
      }
    } else {
      PRUNED_LIVENESS_LOG(llvm::dbgs()
                 << "        Was live already, so cannot be a last user!\n");
    }
  }

  PRUNED_LIVENESS_LOG(llvm::dbgs() << "Finished processing block instructions... now "
                             "checking for dead arguments if dead!\n");
  if (!isLive) {
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Not live! Checking for dead args!\n");
    for (SILArgument *deadArg : block->getArguments()) {
      auto iter = defs.find(deadArg);
      if (iter.has_value() &&
          llvm::any_of(*iter, [&](TypeTreeLeafTypeRange span) {
            return span.contains(bitNo);
          })) {
        PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Found dead arg: " << *deadArg);
        boundary.getDeadDefsBits(deadArg).set(bitNo);
      }
    }

    // If all of our single predecessors are LiveOut and we are not live, then
    // we need to mark ourselves as a boundary block so we clean up the live out
    // value.
    //
    // TODO: What if we have a mix/match of LiveWithin and LiveOut.
    if (!block->pred_empty()) {
      if (llvm::all_of(block->getPredecessorBlocks(),
                       [&](SILBasicBlock *predBlock) -> bool {
                         return getBlockLiveness(predBlock, bitNo) ==
                                FieldSensitivePrunedLiveBlocks::IsLive::LiveOut;
                       })) {
        boundary.getBoundaryEdgeBits(block).set(bitNo);
      }
    }
  } else {
    PRUNED_LIVENESS_LOG(llvm::dbgs()
               << "    Live at beginning of block! No dead args!\n");
  }

  assert(
      (isLiveOut || prevCount < boundary.getNumLastUsersAndDeadDefs(bitNo)) &&
      "findBoundariesInBlock must be called on a live block");
}

bool FieldSensitiveMultiDefPrunedLiveRange::findEarlierConsumingUse(
    SILInstruction *inst, unsigned index,
    llvm::function_ref<bool(SILInstruction *)> callback) const {
  PRUNED_LIVENESS_LOG(
      llvm::dbgs()
      << "Performing single block search for consuming use for bit: " << index
      << "!\n");

  // Walk our block back from inst looking for defs or a consuming use. If we
  // see a def, return true. If we see a use, we keep processing if the callback
  // returns true... and return false early if the callback returns false.
  for (auto ii = std::next(inst->getReverseIterator()),
            ie = inst->getParent()->rend();
       ii != ie; ++ii) {
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "Visiting: " << *ii);
    // If we have a def, then we are automatically done.
    if (isDef(&*ii, index)) {
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Is Def! Returning true!\n");
      return true;
    }

    // If we have a consuming use, emit the error.
    if (isInterestingUser(&*ii, index) ==
        IsInterestingUser::LifetimeEndingUse) {
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Is Lifetime Ending Use!\n");
      if (!callback(&*ii)) {
        PRUNED_LIVENESS_LOG(llvm::dbgs()
                            << "    Callback returned false... exiting!\n");
        return false;
      }
      PRUNED_LIVENESS_LOG(llvm::dbgs()
                          << "    Callback returned true... continuing!\n");
    }

    // Otherwise, keep going.
  }

  // Then check our argument defs.
  for (auto *arg : inst->getParent()->getArguments()) {
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "Visiting arg: " << *arg);
    if (isDef(arg, index)) {
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Found def. Returning true!\n");
      return true;
    }
  }

  PRUNED_LIVENESS_LOG(llvm::dbgs() << "Finished single block. Didn't find "
                                      "anything... Performing interprocedural");

  // Ok, we now know that we need to look further back.
  BasicBlockWorklist worklist(inst->getFunction());
  for (auto *predBlock : inst->getParent()->getPredecessorBlocks()) {
    worklist.pushIfNotVisited(predBlock);
  }

  while (auto *next = worklist.pop()) {
    PRUNED_LIVENESS_LOG(llvm::dbgs()
                        << "Checking block bb" << next->getDebugID() << '\n');
    for (auto ii = next->rbegin(), ie = next->rend(); ii != ie; ++ii) {
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "Visiting: " << *ii);
      // If we have a def, then we are automatically done.
      if (isDef(&*ii, index)) {
        PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Is Def! Returning true!\n");
        return true;
      }

      // If we have a consuming use, emit the error.
      if (isInterestingUser(&*ii, index) ==
          IsInterestingUser::LifetimeEndingUse) {
        PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Is Lifetime Ending Use!\n");
        if (!callback(&*ii)) {
          PRUNED_LIVENESS_LOG(llvm::dbgs()
                              << "    Callback returned false... exiting!\n");
          return false;
        }
        PRUNED_LIVENESS_LOG(llvm::dbgs()
                            << "    Callback returned true... continuing!\n");
      }

      // Otherwise, keep going.
    }

    for (auto *arg : next->getArguments()) {
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "Visiting arg: " << *arg);
      if (isDef(arg, index)) {
        PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Found def. Returning true!\n");
        return true;
      }
    }

    PRUNED_LIVENESS_LOG(llvm::dbgs()
                        << "Didn't find anything... visiting predecessors!\n");
    for (auto *predBlock : next->getPredecessorBlocks()) {
      worklist.pushIfNotVisited(predBlock);
    }
  }

  return true;
}
