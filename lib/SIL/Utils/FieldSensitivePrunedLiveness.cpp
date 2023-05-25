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
#include "swift/Basic/Defer.h"
#include "swift/Basic/SmallBitVector.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/ScopedAddressUtils.h"
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
    if (numElements == 0)
      number = 1;

    return;
  }

  // If we have an enum, we add one for tracking if the base enum is set and use
  // the remaining bits for the max sized payload. This ensures that if we have
  // a smaller sized payload, we still get all of the bits set, allowing for a
  // homogeneous representation.
  if (auto *enumDecl = type.getEnumOrBoundGenericEnum()) {
    unsigned numElements = 0;
    for (auto *eltDecl : enumDecl->getAllElements()) {
      if (!eltDecl->hasAssociatedValues())
        continue;
      auto elt = type.getEnumElementType(eltDecl, mod, context);
      numElements = std::max(numElements,
                             unsigned(TypeSubElementCount(elt, mod, context)));
    }
    number = numElements + 1;
    return;
  }

  // If this isn't a tuple, struct, or enum, it is a single element. This was
  // our default value, so we can just return.
}

//===----------------------------------------------------------------------===//
//                           MARK: SubElementNumber
//===----------------------------------------------------------------------===//

Optional<SubElementOffset>
SubElementOffset::computeForAddress(SILValue projectionDerivedFromRoot,
                                    SILValue rootAddress) {
  unsigned finalSubElementOffset = 0;
  SILModule &mod = *rootAddress->getModule();

  while (1) {
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
    if (auto *enumData = dyn_cast<UncheckedTakeEnumDataAddrInst>(
            projectionDerivedFromRoot)) {
      projectionDerivedFromRoot = enumData->getOperand();
      continue;
    }

    // Init enum data addr is treated like unchecked take enum data addr.
    if (auto *initData =
            dyn_cast<InitEnumDataAddrInst>(projectionDerivedFromRoot)) {
      projectionDerivedFromRoot = initData->getOperand();
      continue;
    }

    // If we do not know how to handle this case, just return None.
    //
    // NOTE: We use to assert here, but since this is used for diagnostics, we
    // really do not want to abort. Instead, our caller can choose to abort if
    // they get back a None. This ensures that we do not abort in cases where we
    // just want to emit to the user a "I do not understand" error.
    return None;
  }
}

Optional<SubElementOffset>
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
    if (auto *enumData =
            dyn_cast<UncheckedEnumDataInst>(projectionDerivedFromRoot)) {
      projectionDerivedFromRoot = enumData->getOperand();
      continue;
    }

    // If we do not know how to handle this case, just return None.
    //
    // NOTE: We use to assert here, but since this is used for diagnostics, we
    // really do not want to abort. Instead, our caller can choose to abort if
    // they get back a None. This ensures that we do not abort in cases where we
    // just want to emit to the user a "I do not understand" error.
    return None;
  }
}

//===----------------------------------------------------------------------===//
//                        MARK: TypeTreeLeafTypeRange
//===----------------------------------------------------------------------===//

void TypeTreeLeafTypeRange::constructFilteredProjections(
    SILValue value, SILInstruction *insertPt, SmallBitVector &filterBitVector,
    llvm::function_ref<bool(SILValue, TypeTreeLeafTypeRange)> callback) {
  auto *fn = insertPt->getFunction();
  SILType type = value->getType();

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

      auto newValue =
          builder.createStructElementAddr(insertPt->getLoc(), value, varDecl);
      callback(newValue, TypeTreeLeafTypeRange(start, next));
      start = next;
    }
    assert(start == endEltOffset);
    return;
  }

  // We only allow for enums that can be completely destroyed. If there is code
  // where an enum should be partially destroyed, we need to treat the
  // unchecked_take_enum_data_addr as a separate value whose liveness we are
  // tracking.
  if (auto *enumDecl = type.getEnumOrBoundGenericEnum()) {
    unsigned start = startEltOffset;

    unsigned maxSubEltCount = 0;
    for (auto *eltDecl : enumDecl->getAllElements()) {
      if (!eltDecl->hasAssociatedValues())
        continue;
      auto nextType = type.getEnumElementType(eltDecl, fn);
      maxSubEltCount =
          std::max(maxSubEltCount, unsigned(TypeSubElementCount(nextType, fn)));
    }

    // Add a bit for the case bit.
    unsigned next = maxSubEltCount + 1;

    // Make sure we are all set.
    assert(allSet(filterBitVector, start, next));

    // Then just pass back our enum base value as the pointer.
    callback(value, TypeTreeLeafTypeRange(start, next));

    // Then set start to next and assert we covered the entire end elt offset.
    start = next;
    assert(start == endEltOffset);
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

      auto newValue =
          builder.createTupleElementAddr(insertPt->getLoc(), value, index);
      callback(newValue, TypeTreeLeafTypeRange(start, next));
      start = next;
    }
    assert(start == endEltOffset);
    return;
  }

  llvm_unreachable("Not understand subtype");
}

void TypeTreeLeafTypeRange::constructProjectionsForNeededElements(
    SILValue rootValue, SILInstruction *insertPt,
    SmallBitVector &neededElements,
    SmallVectorImpl<std::pair<SILValue, TypeTreeLeafTypeRange>>
        &resultingProjections) {
  TypeTreeLeafTypeRange rootRange(rootValue);
  (void)rootRange;
  assert(rootRange.size() == neededElements.size());

  StackList<std::pair<SILValue, TypeTreeLeafTypeRange>> worklist(
      insertPt->getFunction());
  worklist.push_back({rootValue, rootRange});

  // Temporary vector we use for our computation.
  SmallBitVector tmp(neededElements.size());

  auto allInRange = [](const SmallBitVector &bv, TypeTreeLeafTypeRange span) {
    return llvm::all_of(span.getRange(),
                        [&bv](unsigned index) { return bv[index]; });
  };

  while (!worklist.empty()) {
    auto pair = worklist.pop_back_val();
    auto value = pair.first;
    auto range = pair.second;

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
      resultingProjections.emplace_back(value, range);
      continue;
    }

    // Otherwise, we have a partial range. We need to split our range and then
    // recursively process those ranges looking for subranges that have
    // completely set bits.
    range.constructFilteredProjections(
        value, insertPt, neededElements,
        [&](SILValue subType, TypeTreeLeafTypeRange range) -> bool {
          worklist.push_back({subType, range});
          return true;
        });
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
    SmallVectorImpl<IsLive> &resultingLivenessInfo) {
  assert(isInitialized());
  resultingLivenessInfo.clear();

  SWIFT_ASSERT_ONLY(seenUse = true);

  auto *bb = user->getParent();
  getBlockLiveness(bb, startBitNo, endBitNo, resultingLivenessInfo);
  assert(resultingLivenessInfo.size() == (endBitNo - startBitNo));

  for (unsigned index : indices(resultingLivenessInfo)) {
    unsigned specificBitNo = startBitNo + index;
    switch (resultingLivenessInfo[index]) {
    case LiveOut:
    case LiveWithin:
      continue;
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
//                        MARK: FieldSensitiveLiveness
//===----------------------------------------------------------------------===//

void FieldSensitivePrunedLiveness::updateForUse(SILInstruction *user,
                                                TypeTreeLeafTypeRange range,
                                                bool lifetimeEnding) {
  SmallVector<FieldSensitivePrunedLiveBlocks::IsLive, 8> resultingLiveness;
  liveBlocks.updateForUse(user, range.startEltOffset, range.endEltOffset,
                          resultingLiveness);

  addInterestingUser(user, range, lifetimeEnding);
}

//===----------------------------------------------------------------------===//
//                    MARK: FieldSensitivePrunedLiveRange
//===----------------------------------------------------------------------===//

template <typename LivenessWithDefs>
bool FieldSensitivePrunedLiveRange<LivenessWithDefs>::isWithinBoundary(
    SILInstruction *inst, TypeTreeLeafTypeRange span) const {
  assert(asImpl().isInitialized());

  PRUNED_LIVENESS_LOG(
      llvm::dbgs() << "FieldSensitivePrunedLiveRange::isWithinBoundary!\n"
                   << "Span: ";
      span.print(llvm::dbgs()); llvm::dbgs() << '\n');

  // If we do not have any span, return true since we have no counter examples.
  if (span.empty()) {
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "    span is empty! Returning true!\n");
    return true;
  }

  using IsLive = FieldSensitivePrunedLiveBlocks::IsLive;

  auto *block = inst->getParent();

  SmallVector<IsLive, 8> outVector;
  getBlockLiveness(block, span, outVector);

  for (auto pair : llvm::enumerate(outVector)) {
    unsigned bit = span.startEltOffset + pair.index();
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Visiting bit: " << bit << '\n');
    bool isLive = false;
    switch (pair.value()) {
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
          auto interestingUser = isInterestingUser(&blockInst);
          bool isInteresting =
              interestingUser.first && interestingUser.second->contains(bit);
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
          if (getBlockLiveness(succBB, index) ==
              FieldSensitivePrunedLiveBlocks::Dead) {
            PRUNED_LIVENESS_LOG(llvm::dbgs() << "Marking succBB as boundary edge: bb"
                                    << succBB->getDebugID() << '\n');
            boundary.getBoundaryEdgeBits(succBB).set(index);
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

template <typename LivenessWithDefs>
void FieldSensitivePrunedLiveRange<LivenessWithDefs>::updateForUse(
    SILInstruction *user, TypeTreeLeafTypeRange range, bool lifetimeEnding) {
  PRUNED_LIVENESS_LOG(
      llvm::dbgs()
      << "Begin FieldSensitivePrunedLiveRange<LivenessWithDefs>::updateForUse "
         "for: "
      << *user);
  PRUNED_LIVENESS_LOG(
      llvm::dbgs() << "Looking for def instruction earlier in the block!\n");

  auto *parentBlock = user->getParent();
  for (auto ii = std::next(user->getReverseIterator()),
            ie = parentBlock->rend();
       ii != ie; ++ii) {
    // If we find the def, just mark this instruction as being an interesting
    // instruction.
    if (asImpl().isDef(&*ii, range)) {
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Found def: " << *ii);
      PRUNED_LIVENESS_LOG(llvm::dbgs()
                 << "    Marking inst as interesting user and returning!\n");
      addInterestingUser(user, range, lifetimeEnding);
      return;
    }
  }

  // Otherwise, just delegate to our parent class's update for use. This will
  // update liveness for our predecessor blocks and add this instruction as an
  // interesting user.
  PRUNED_LIVENESS_LOG(llvm::dbgs() << "No defs found! Delegating to "
                             "FieldSensitivePrunedLiveness::updateForUse.\n");
  FieldSensitivePrunedLiveness::updateForUse(user, range, lifetimeEnding);
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
    auto interestingUser = liveness.isInterestingUser(&inst);
    if (interestingUser.first && interestingUser.second->contains(bitNo)) {
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
  for (SILInstruction &inst : llvm::reverse(*ssaDef->getParentBlock())) {
    PRUNED_LIVENESS_LOG(llvm::dbgs() << "Visiting: " << inst);
    if (&inst == defInst) {
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Found dead def: " << *defInst);
      boundary.getDeadDefsBits(cast<SILNode>(&inst)).set(bitNo);
      return;
    }
    auto interestingUser = liveness.isInterestingUser(&inst);
    if (interestingUser.first && interestingUser.second->contains(bitNo)) {
      PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Found interesting user: " << inst);
      boundary.getLastUserBits(&inst).set(bitNo);
      return;
    }
  }

  auto *deadArg = cast<SILArgument>(ssaDef);
  PRUNED_LIVENESS_LOG(llvm::dbgs() << "    Found dead arg: " << *deadArg);
  boundary.getDeadDefsBits(deadArg).set(bitNo);
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
      auto interestingUser = isInterestingUser(&inst);
      if (interestingUser.first && interestingUser.second->contains(bitNo)) {
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

  assert((isLiveOut ||
          prevCount < boundary.getNumLastUsersAndDeadDefs(bitNo)) &&
         "findBoundariesInBlock must be called on a live block");
}
