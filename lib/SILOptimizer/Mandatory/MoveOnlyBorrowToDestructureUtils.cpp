//===--- MoveOnlyBorrowToDestructureTransform.cpp -------------------------===//
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
///
/// \file This is a transform that converts the borrow + gep pattern to
/// destructures or emits an error if it cannot be done. It is assumed that it
/// runs immediately before move checking of objects runs. This ensures that the
/// move checker does not need to worry about this problem and instead can just
/// check that the newly inserted destructures do not cause move only errors.
///
/// This is written as a utility so that we can have a utility pass that tests
/// this directly but also invoke this via the move only object checker.
///
/// TODO: Move this to SILOptimizer/Utils.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-move-only-checker"

#include "MoveOnlyBorrowToDestructureUtils.h"
#include "MoveOnlyDiagnostics.h"
#include "MoveOnlyObjectCheckerUtils.h"
#include "MoveOnlyTypeUtils.h"

#include "swift/Basic/BlotSetVector.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/SIL/FieldSensitivePrunedLiveness.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallBitVector.h"

using namespace swift;
using namespace swift::siloptimizer;
using namespace swift::siloptimizer::borrowtodestructure;

//===----------------------------------------------------------------------===//
//                              MARK: Utilities
//===----------------------------------------------------------------------===//

/// Return a loc that can be used regardless if \p inst is a terminator or not.
static SILLocation getSafeLoc(SILInstruction *inst) {
  if (isa<TermInst>(inst))
    return RegularLocation::getDiagnosticsOnlyLocation(inst->getLoc(),
                                                       inst->getModule());
  return inst->getLoc();
}

static void addCompensatingDestroys(SSAPrunedLiveness &liveness,
                                    PrunedLivenessBoundary &boundary,
                                    SILValue value) {
  InstructionSet consumingInsts(value->getFunction());
  liveness.initializeDef(value);
  for (auto *use : value->getUses()) {
    if (use->isConsuming())
      consumingInsts.insert(use->getUser());
    liveness.updateForUse(use->getUser(), use->isConsuming());
    if (auto *bbi = dyn_cast<BeginBorrowInst>(use->getUser())) {
      for (auto *ebi : bbi->getEndBorrows()) {
        liveness.updateForUse(ebi, false /*use is consuming*/);
      }
    }
  }
  liveness.computeBoundary(boundary);
  for (auto *user : boundary.lastUsers) {
    // If this is a consuming inst, just continue.
    if (consumingInsts.contains(user))
      continue;
    // Otherwise, we need to insert a destroy_value afterwards.
    auto *next = user->getNextInstruction();
    SILBuilderWithScope builder(next);
    builder.createDestroyValue(getSafeLoc(next), value);
  }

  // Insert destroy_value along all boundary edges.
  for (auto *edge : boundary.boundaryEdges) {
    SILBuilderWithScope builder(edge->begin());
    builder.createDestroyValue(getSafeLoc(&*edge->begin()), value);
  }

  // If we have a dead def, insert the destroy_value immediately at the def.
  for (auto *deadDef : boundary.deadDefs) {
    SILInstruction *nextInst = nullptr;
    if (auto *inst = dyn_cast<SILInstruction>(deadDef)) {
      nextInst = inst->getNextInstruction();
    } else if (auto *arg = dyn_cast<SILArgument>(deadDef)) {
      nextInst = arg->getNextInstruction();
    } else {
      llvm_unreachable("Unhandled dead def?!");
    }
    SILBuilderWithScope builder(nextInst);
    builder.createDestroyValue(getSafeLoc(nextInst), value);
  }
}

//===----------------------------------------------------------------------===//
//                           MARK: Available Values
//===----------------------------------------------------------------------===//

namespace {

// We reserve more bits that we need at the beginning so that we can avoid
// reallocating and potentially breaking our internal mutable array ref
// points into the data store.
struct AvailableValues {
  MutableArrayRef<SILValue> values;

  SILValue operator[](unsigned index) const { return values[index]; }
  SILValue &operator[](unsigned index) { return values[index]; }
  unsigned size() const { return values.size(); }

  AvailableValues() : values() {}
  AvailableValues(MutableArrayRef<SILValue> values) : values(values) {}

  void print(llvm::raw_ostream &os, const char *prefix = nullptr) const;
  SWIFT_DEBUG_DUMP;
};

struct AvailableValueStore {
  std::vector<SILValue> dataStore;
  llvm::DenseMap<SILBasicBlock *, AvailableValues> blockToValues;
  unsigned nextOffset = 0;
  unsigned numBits;

  AvailableValueStore(const FieldSensitivePrunedLiveness &liveness)
      : dataStore(liveness.getDiscoveredBlocks().size() *
                  liveness.getNumSubElements()),
        numBits(liveness.getNumSubElements()) {}

  std::pair<AvailableValues *, bool> get(SILBasicBlock *block) {
    auto iter = blockToValues.try_emplace(block, AvailableValues());

    if (!iter.second) {
      return {&iter.first->second, false};
    }

    iter.first->second.values =
        MutableArrayRef<SILValue>(&dataStore[nextOffset], numBits);
    nextOffset += numBits;
    return {&iter.first->second, true};
  }
};

} // namespace

void AvailableValues::print(llvm::raw_ostream &os, const char *prefix) const {
  if (prefix)
    os << prefix;
  os << "Dumping AvailableValues!\n";
  for (auto pair : llvm::enumerate(values)) {
    if (prefix)
      os << prefix;
    os << "    values[" << pair.index() << "] = ";
    if (pair.value()) {
      os << *pair.value();
    } else {
      os << "None\n";
    }
  }
}

void AvailableValues::dump() const { print(llvm::dbgs(), nullptr); }

//===----------------------------------------------------------------------===//
//                        MARK: Private Implementation
//===----------------------------------------------------------------------===//

struct borrowtodestructure::Implementation {
  BorrowToDestructureTransform &interface;

  Optional<AvailableValueStore> blockToAvailableValues;

  /// The liveness that we use for all borrows or for individual switch_enum
  /// arguments.
  FieldSensitiveSSAPrunedLiveRange liveness;

  /// The copy_value we insert upon our mark_must_check or switch_enum argument
  /// so that we have an independent owned value.
  SILValue initialValue;

  using InterestingUser = FieldSensitivePrunedLiveness::InterestingUser;
  SmallFrozenMultiMap<SILBasicBlock *, std::pair<Operand *, InterestingUser>, 8>
      blocksToUses;

  /// A frozen multi-map we use to diagnose consuming uses that are used by the
  /// same instruction as another consuming use or non-consuming use.
  SmallFrozenMultiMap<SILInstruction *, Operand *, 8>
      instToInterestingOperandIndexMap;

  SmallVector<Operand *, 8> destructureNeedingUses;

  Implementation(BorrowToDestructureTransform &interface,
                 SmallVectorImpl<SILBasicBlock *> &discoveredBlocks)
      : interface(interface),
        liveness(interface.mmci->getFunction(), &discoveredBlocks) {}

  void clear() {
    liveness.clear();
    initialValue = SILValue();
  }

  void init(SILValue rootValue) {
    clear();
    liveness.init(rootValue);
    liveness.initializeDef(rootValue, TypeTreeLeafTypeRange(rootValue));
  }

  bool gatherUses(SILValue value);

  /// Once we have gathered up all of our destructure uses and liveness
  /// requiring uses, validate that all of our destructure uses are on our
  /// boundary. Once we have done this, we know that it is safe to perform our
  /// transform.
  void checkDestructureUsesOnBoundary() const;

  /// Check for cases where we have two consuming uses on the same instruction
  /// or a consuming/non-consuming use on the same instruction.
  void checkForErrorsOnSameInstruction();

  /// Rewrite all of the uses of our borrow on our borrow operand, performing
  /// destructures as appropriate.
  void rewriteUses(InstructionDeleter *deleter = nullptr);

  void cleanup();

  AvailableValues &computeAvailableValues(SILBasicBlock *block);

  /// Returns mark_must_check if we are processing borrows or the enum argument
  /// if we are processing switch_enum.
  SILValue getRootValue() const { return liveness.getRootValue(); }

  DiagnosticEmitter &getDiagnostics() const {
    return interface.diagnosticEmitter;
  }

  /// Always returns the actual root mark_must_check for both switch_enum args
  /// and normal borrow user checks.
  MarkMustCheckInst *getMarkedValue() const { return interface.mmci; }

  PostOrderFunctionInfo *getPostOrderFunctionInfo() {
    return interface.getPostOrderFunctionInfo();
  }

  IntervalMapAllocator::Allocator &getAllocator() {
    return interface.allocator.get();
  }
};

bool Implementation::gatherUses(SILValue value) {
  LLVM_DEBUG(llvm::dbgs() << "Gathering uses for: " << *value);
  StackList<Operand *> useWorklist(value->getFunction());

  for (auto *use : value->getUses()) {
    useWorklist.push_back(use);
  }

  while (!useWorklist.empty()) {
    auto *nextUse = useWorklist.pop_back_val();
    LLVM_DEBUG(llvm::dbgs() << "    NextUse: " << *nextUse->getUser());
    LLVM_DEBUG(llvm::dbgs() << "    Operand Ownership: "
                            << nextUse->getOperandOwnership() << '\n');
    switch (nextUse->getOperandOwnership()) {
    case OperandOwnership::NonUse:
      continue;

    // Conservatively treat a conversion to an unowned value as a pointer
    // escape. If we see this in the SIL, fail and return false so we emit a
    // "compiler doesn't understand error".
    case OperandOwnership::ForwardingUnowned:
    case OperandOwnership::PointerEscape:
      LLVM_DEBUG(llvm::dbgs()
                 << "        Found forwarding unowned or pointer escape!\n");
      return false;

    // These might be uses that we need to perform a destructure or insert
    // struct_extracts for.
    case OperandOwnership::TrivialUse:
    case OperandOwnership::InstantaneousUse:
    case OperandOwnership::UnownedInstantaneousUse:
    case OperandOwnership::InteriorPointer:
    case OperandOwnership::BitwiseEscape: {
      // Look through copy_value of a move only value. We treat copy_value of
      // copyable values as normal uses.
      if (auto *cvi = dyn_cast<CopyValueInst>(nextUse->getUser())) {
        if (cvi->getOperand()->getType().isMoveOnly()) {
          LLVM_DEBUG(llvm::dbgs() << "        Found copy value of move only "
                                     "field... looking through!\n");
          for (auto *use : cvi->getUses())
            useWorklist.push_back(use);
          continue;
        }

        // If we don't have a copy of a move only type, we just reat this as a
        // normal use, so we fall through.
      }

      auto leafRange =
          TypeTreeLeafTypeRange::get(nextUse->get(), getRootValue());
      if (!leafRange) {
        LLVM_DEBUG(llvm::dbgs() << "        Failed to compute leaf range?!\n");
        return false;
      }

      LLVM_DEBUG(llvm::dbgs() << "        Found non lifetime ending use!\n");
      blocksToUses.insert(nextUse->getParentBlock(),
                          {nextUse,
                           {liveness.getNumSubElements(), *leafRange,
                            false /*is lifetime ending*/}});
      liveness.updateForUse(nextUse->getUser(), *leafRange,
                            false /*is lifetime ending*/);
      instToInterestingOperandIndexMap.insert(nextUse->getUser(), nextUse);
      continue;
    }

    case OperandOwnership::ForwardingConsume:
    case OperandOwnership::DestroyingConsume: {
      // Ignore destroy_value, we are going to eliminate them.
      if (isa<DestroyValueInst>(nextUse->getUser())) {
        LLVM_DEBUG(llvm::dbgs() << "        Found destroy value!\n");
        continue;
      }

      auto leafRange =
          TypeTreeLeafTypeRange::get(nextUse->get(), getRootValue());
      if (!leafRange) {
        LLVM_DEBUG(llvm::dbgs() << "        Failed to compute leaf range?!\n");
        return false;
      }

      LLVM_DEBUG(llvm::dbgs() << "        Found lifetime ending use!\n");
      destructureNeedingUses.push_back(nextUse);
      blocksToUses.insert(nextUse->getParentBlock(),
                          {nextUse,
                           {liveness.getNumSubElements(), *leafRange,
                            true /*is lifetime ending*/}});
      liveness.updateForUse(nextUse->getUser(), *leafRange,
                            true /*is lifetime ending*/);
      instToInterestingOperandIndexMap.insert(nextUse->getUser(), nextUse);
      continue;
    }

    case OperandOwnership::GuaranteedForwarding:
      // Look through guaranteed forwarding.
      ForwardingOperand(nextUse).visitForwardedValues([&](SILValue value) {
        for (auto *use : value->getUses()) {
          useWorklist.push_back(use);
        }
        return true;
      });
      continue;

    case OperandOwnership::Borrow: {
      // Look through borrows.
      if (auto *bbi = dyn_cast<BeginBorrowInst>(nextUse->getUser())) {
        LLVM_DEBUG(llvm::dbgs() << "        Found recursive borrow!\n");
        for (auto *use : bbi->getUses()) {
          useWorklist.push_back(use);
        }
        continue;
      }

      auto leafRange =
          TypeTreeLeafTypeRange::get(nextUse->get(), getRootValue());
      if (!leafRange) {
        LLVM_DEBUG(llvm::dbgs() << "        Failed to compute leaf range?!\n");
        return false;
      }

      // Otherwise, treat it as a normal use.
      LLVM_DEBUG(llvm::dbgs() << "        Treating non-begin_borrow borrow as "
                                 "a non lifetime ending use!\n");
      blocksToUses.insert(nextUse->getParentBlock(),
                          {nextUse,
                           {liveness.getNumSubElements(), *leafRange,
                            false /*is lifetime ending*/}});
      liveness.updateForUse(nextUse->getUser(), *leafRange,
                            false /*is lifetime ending*/);
      instToInterestingOperandIndexMap.insert(nextUse->getUser(), nextUse);

      continue;
    }
    case OperandOwnership::EndBorrow:
      LLVM_DEBUG(llvm::dbgs() << "        Found end borrow!\n");
      continue;
    case OperandOwnership::Reborrow:
      llvm_unreachable("Unsupported for now?!");
    }
  }
  return true;
}

void Implementation::checkForErrorsOnSameInstruction() {
  // At this point, we have emitted all boundary checks. We also now need to
  // check if any of our consuming uses that are on the boundary are used by the
  // same instruction as a different consuming or non-consuming use.
  instToInterestingOperandIndexMap.setFrozen();
  SmallBitVector usedBits(liveness.getNumSubElements());

  for (auto instRangePair : instToInterestingOperandIndexMap.getRange()) {
    SWIFT_DEFER { usedBits.reset(); };

    // First loop through our uses and handle any consuming twice errors. We
    // also setup usedBits to check for non-consuming uses that may overlap.
    Operand *badOperand = nullptr;
    Optional<TypeTreeLeafTypeRange> badRange;
    for (auto *use : instRangePair.second) {
      if (!use->isConsuming())
        continue;

      auto destructureUseSpan =
          *TypeTreeLeafTypeRange::get(use->get(), getRootValue());
      for (unsigned index : destructureUseSpan.getRange()) {
        if (usedBits[index]) {
          // If we get that we used the same bit twice, we have an error. We set
          // the badIndex error and break early.
          badOperand = use;
          badRange = destructureUseSpan;
          break;
        }

        usedBits[index] = true;
      }

      // If we set badOperand, break so we can emit an error for this
      // instruction.
      if (badOperand)
        break;
    }

    // If we did not set badIndex for consuming uses, we did not have any
    // conflicts among consuming uses. see if we have any conflicts with
    // non-consuming uses. Otherwise, we continue.
    if (!badOperand) {
      for (auto *use : instRangePair.second) {
        if (use->isConsuming())
          continue;

        auto destructureUseSpan =
            *TypeTreeLeafTypeRange::get(use->get(), getRootValue());
        for (unsigned index : destructureUseSpan.getRange()) {
          if (!usedBits[index])
            continue;

          // If we get that we used the same bit twice, we have an error. We set
          // the badIndex error and break early.
          badOperand = use;
          badRange = destructureUseSpan;
          break;
        }

        // If we set badOperand, break so we can emit an error for this
        // instruction.
        if (badOperand)
          break;
      }

      // If we even did not find a non-consuming use that conflicts, then
      // continue.
      if (!badOperand)
        continue;
    }

    // If badIndex is set, we broke out of the inner loop and need to emit an
    // error. Use a little more compile time to identify the other operand that
    // caused the failure. NOTE: badOperand /could/ be a non-consuming use, but
    // the use we are identifying here will always be consuming.
    usedBits.reset();

    // Reinitialize use bits with the bad bits.
    for (unsigned index : badRange->getRange())
      usedBits[index] = true;

    // Now loop back through looking for the original operand that set the used
    // bits. This will always be a consuming use.
    for (auto *use : instRangePair.second) {
      if (!use->isConsuming())
        continue;

      auto destructureUseSpan =
          *TypeTreeLeafTypeRange::get(use->get(), getRootValue());
      bool emittedError = false;
      for (unsigned index : destructureUseSpan.getRange()) {
        if (!usedBits[index])
          continue;

        if (badOperand->isConsuming())
          getDiagnostics().emitObjectInstConsumesValueTwice(getMarkedValue(),
                                                            use, badOperand);
        else
          getDiagnostics().emitObjectInstConsumesAndUsesValue(getMarkedValue(),
                                                              use, badOperand);
        emittedError = true;
      }

      // Once we have emitted the error, just break out of the loop.
      if (emittedError)
        break;
    }
  }
}

void Implementation::checkDestructureUsesOnBoundary() const {
  LLVM_DEBUG(llvm::dbgs() << "Checking destructure uses on boundary!\n");

  // Now that we have found all of our destructure needing uses and liveness
  // needing uses, make sure that none of our destructure needing uses are
  // within our boundary. If so, we have an automatic error since we have a
  // use-after-free.
  for (auto *use : destructureNeedingUses) {
    LLVM_DEBUG(llvm::dbgs()
               << "    DestructureNeedingUse: " << *use->getUser());

    auto destructureUseSpan =
        *TypeTreeLeafTypeRange::get(use->get(), getRootValue());
    if (!liveness.isWithinBoundary(use->getUser(), destructureUseSpan)) {
      LLVM_DEBUG(llvm::dbgs()
                 << "        On boundary or within boundary! No error!\n");
      continue;
    }

    // Emit an error. We have a use after free.
    //
    // NOTE: Since we are going to emit an error here, we do the boundary
    // computation to ensure that we only do the boundary computation once:
    // when we emit an error or once we know we need to do rewriting.
    //
    // TODO: Fix diagnostic to use destructure needing use and boundary
    // uses.
    LLVM_DEBUG(llvm::dbgs() << "        Within boundary! Emitting error!\n");
    FieldSensitivePrunedLivenessBoundary boundary(liveness.getNumSubElements());
    liveness.computeBoundary(boundary);
    getDiagnostics().emitObjectDestructureNeededWithinBorrowBoundary(
        getMarkedValue(), use->getUser(), destructureUseSpan, boundary);
  }
}

#ifndef NDEBUG
static void dumpSmallestTypeAvailable(
    SmallVectorImpl<Optional<std::pair<TypeOffsetSizePair, SILType>>>
        &smallestTypeAvailable) {
  LLVM_DEBUG(llvm::dbgs() << "            Dumping smallest type available!\n");
  for (auto pair : llvm::enumerate(smallestTypeAvailable)) {
    LLVM_DEBUG(llvm::dbgs() << "            value[" << pair.index() << "] = ");
    if (!pair.value()) {
      LLVM_DEBUG(llvm::dbgs() << "None\n");
      continue;
    }

    auto value = *pair.value();
    LLVM_DEBUG(llvm::dbgs() << "Span: " << value.first
                            << ". Type: " << value.second << '\n');
  }
}
#endif

/// When we compute available values, we have a few constraints:
///
/// 1. We want to be sure that we destructure as /late/ as possible. This
/// ensures that we match at the source level the assumption by users that they
/// can use entire valid parts as late as possible. If we were to do it earlier
/// we would emit errors too early.
AvailableValues &Implementation::computeAvailableValues(SILBasicBlock *block) {
  LLVM_DEBUG(llvm::dbgs() << "    Computing Available Values For bb"
                          << block->getDebugID() << '\n');

  // First grab our block. If we already have state for the block, just return
  // its available values. We already computed the available values and
  // potentially updated it with new destructured values for our block.
  auto pair = blockToAvailableValues->get(block);
  if (!pair.second) {
    LLVM_DEBUG(llvm::dbgs()
               << "        Already have values! Returning them!\n");
    LLVM_DEBUG(pair.first->print(llvm::dbgs(), "            "));
    return *pair.first;
  }

  LLVM_DEBUG(llvm::dbgs() << "        No values computed! Initializing!\n");
  auto &newValues = *pair.first;

  // Otherwise, we need to initialize our available values with predecessor
  // information.

  // First check if the block is the one associated with our mark must check
  // inst. If we are in this block, set all available value bits to our initial
  // value which is a copy_value of \p initial value. We add the copy_value to
  // ensure that from an OSSA perspective any any destructures we insert are
  // independent of any other copies. We assume that OSSA canonicalization will
  // remove the extra copy later after we run or emit an error if it can't.
  if (block == getRootValue()->getParentBlock()) {
    LLVM_DEBUG(llvm::dbgs()
               << "        In initial block, setting to initial value!\n");
    for (unsigned i : indices(newValues))
      newValues[i] = initialValue;
    LLVM_DEBUG(newValues.print(llvm::dbgs(), "        "));
    return newValues;
  }

  // Otherwise, we need to handle predecessors. Our strategy is to loop over all
  // predecessors and:
  //
  // 1. If we have the same value along all predecessors, for a specific bit, we
  // just let it through.
  //
  // 2. If we find values that describe the same set of set bits and they only
  // describe those bits, we phi them together.
  //
  // 3. If we find a value that is unavailable along one of the other paths but
  // /could/ be destructured such that we could phi the destructured value, we
  // destructure the value in the predecessor and use that for our phi.
  //
  // 4. We assume optimistically that loop back-edge predecessors always contain
  // all available values that come into the loop. The reason why this is true
  // is that we know that either:
  //
  //    a. Our value either begins within a loop meaning that we either never
  //    seen the back edge or the back edge block is where our mark must check
  //    inst is so we won't visit the back edge.
  //
  //    b. Our mark must check block is further up the loop nest than the loop
  //    back edge implying if we were to destructure in the loop, we would
  //    destructure multiple times. This would have then resulted in a liveness
  //    error in the liveness check we ran earlier, resulting in us not running
  //    this transformation.
  struct SkipBackEdgeFilter {
    unsigned targetBlockRPO;
    SILBasicBlock::pred_iterator pe;
    PostOrderFunctionInfo *pofi;

    SkipBackEdgeFilter(SILBasicBlock *block, PostOrderFunctionInfo *pofi)
        : targetBlockRPO(*pofi->getRPONumber(block)), pe(block->pred_end()),
          pofi(pofi) {}

    Optional<SILBasicBlock *> operator()(SILBasicBlock *predBlock) const {
      // If our predecessor block has a larger RPO number than our target block,
      // then their edge must be a backedge.
      if (targetBlockRPO < *pofi->getRPONumber(predBlock))
        return None;
      return predBlock;
    }
  };
  auto predsSkippingBackEdges = makeOptionalTransformRange(
      llvm::make_range(block->pred_begin(), block->pred_end()),
      SkipBackEdgeFilter(block, getPostOrderFunctionInfo()));

  // Loop over all available values for all predecessors and determine for each
  // sub-element number the smallest type over all available values if we have
  // an available value for each predecessor. This is implemented by storing an
  // Optional<TypeOffsetSizePair> in an array for each available value. If we
  // find any predecessor without an available value at all for that entry, we
  // set the Optional to be none. Otherwise, we intersect each
  // TypeOffsetSizePair derived from each available value by always taking the
  // smaller TypeOffsetSizePair. We know by construction that we always will
  // move down the type tree, not up the type tree (see NOTE 2 below).
  //
  // NOTE: Given a parent type and a child type which is the only child of the
  // parent type, we always mathematically take the top type. If we have to
  // later destructure an additional time to wire up a use, we do it at the
  // use site when we wire it up. When phi-ing/destructuring, we hide it from
  // the algorithm. This allows us to keep the invariant that our type in size
  // is always absolutely decreasing in size.
  //
  // NOTE 2: We can only move up the type tree by calling a constructor which is
  // always a +1 operation that is treated as a consuming operation end
  // point. In Swift at the source level, we never construct aggregates in a
  // forwarding guaranteed manner for move only types.
  LLVM_DEBUG(llvm::dbgs() << "        Computing smallest type available for "
                             "available values for block bb"
                          << block->getDebugID() << '\n');
  SmallVector<Optional<std::pair<TypeOffsetSizePair, SILType>>, 8>
      smallestTypeAvailable;
  {
    auto pi = predsSkippingBackEdges.begin();
    auto pe = predsSkippingBackEdges.end();
    assert(pi != pe && "If initial block then should have been the mark must "
                       "check inst block?!");

    {
      auto *bb = *pi;
      LLVM_DEBUG(llvm::dbgs() << "        Visiting first block bb"
                              << bb->getDebugID() << '\n');
      LLVM_DEBUG(llvm::dbgs()
                 << "        Recursively loading its available values to "
                    "compute initial smallest type available for block bb"
                 << block->getDebugID() << '\n');
      auto &predAvailableValues = computeAvailableValues(bb);
      LLVM_DEBUG(
          llvm::dbgs()
          << "        Computing initial smallest type available for block bb"
          << block->getDebugID() << '\n');
      for (unsigned i : range(predAvailableValues.size())) {
        if (predAvailableValues[i])
          smallestTypeAvailable.push_back(
              {{TypeOffsetSizePair(predAvailableValues[i], getRootValue()),
                predAvailableValues[i]->getType()}});
        else
          smallestTypeAvailable.emplace_back(None);
      }
      LLVM_DEBUG(llvm::dbgs() << "        Finished computing initial smallest "
                                 "type available for block bb"
                              << block->getDebugID() << '\n';
                 dumpSmallestTypeAvailable(smallestTypeAvailable));
    }
    LLVM_DEBUG(llvm::dbgs()
               << "        Visiting rest of preds and intersecting for block bb"
               << block->getDebugID() << '\n');
    for (auto ppi = std::next(pi); ppi != pe; ++ppi) {
      auto *bb = *ppi;
      LLVM_DEBUG(llvm::dbgs() << "        Computing smallest type for bb"
                              << bb->getDebugID() << '\n');
      LLVM_DEBUG(llvm::dbgs()
                 << "        Recursively loading its available values!\n");
      auto &predAvailableValues = computeAvailableValues(bb);
      for (unsigned i : range(predAvailableValues.size())) {
        if (!smallestTypeAvailable[i].hasValue())
          continue;

        if (!predAvailableValues[i]) {
          smallestTypeAvailable[i] = None;
          continue;
        }

        // Since we assume all types in the type tree for our purposes are
        // absolutely monotonically decreasing in size from their parent (noting
        // the NOTE above), we know that if subElt has a smaller size than our
        // accumulator, then it must be further down the type tree from our
        // accumulator.
        auto offsetSize =
            TypeOffsetSizePair(predAvailableValues[i], getRootValue());
        if (smallestTypeAvailable[i]->first.size > offsetSize.size)
          smallestTypeAvailable[i] = {offsetSize,
                                      predAvailableValues[i]->getType()};
      }
      LLVM_DEBUG(llvm::dbgs() << "        Smallest type available after "
                                 "intersecting with block!\n");
      LLVM_DEBUG(dumpSmallestTypeAvailable(smallestTypeAvailable));
    }
  }

  // At this point, in smallestValueAvailable, we have for each phi slot the
  // smallest size element needed. Now we go through our predecessors again,
  // destructuring available values to match the smallest value needed. If we
  // destructure a larger value, we always update any other available values we
  // are propagating for it using an interval map over the type offsets.
  LLVM_DEBUG(
      llvm::dbgs()
      << "    Destructuring available values in preds to smallest size for bb"
      << block->getDebugID() << '\n');
  auto *fn = block->getFunction();
  IntervalMapAllocator::Map typeSpanToValue(getAllocator());
  for (auto *predBlock : predsSkippingBackEdges) {
    SWIFT_DEFER { typeSpanToValue.clear(); };

    auto &predAvailableValues = computeAvailableValues(predBlock);

    // First go through our available values and initialize our interval map. We
    // should never fail to insert. We want to insert /all/ available values so
    // we can update values that may not be available along other paths if we
    // destructure.
    for (unsigned i : range(predAvailableValues.size())) {
      if (auto value = predAvailableValues[i]) {
        // We check later that we store entire values.
        typeSpanToValue.insert(i, i + 1, value);
      }
    }

    // Now walk through our available values and chop up the contents of our
    // interval map to fit our smallest offset size.
    for (unsigned i : range(predAvailableValues.size())) {
      // If we do not have an offset size for this available value, just
      // continue, we do not need to perform any destructuring.
      //
      // NOTE: If we do not have an available value for this element, then we
      // will already not have a smallest type available due to our earlier
      // work.
      auto smallestOffsetSize = smallestTypeAvailable[i];
      if (!smallestOffsetSize)
        continue;

      // Otherwise, compute the offsetSize for the value associated with this
      // offset in the interval map. If the value is already the correct size,
      // just continue, we do not need to perform any destructuring.
      auto iter = typeSpanToValue.find(i);
      assert(iter != typeSpanToValue.end());
      auto iterValue = iter.value();
      auto iterOffsetSize = TypeOffsetSizePair(iterValue, getRootValue());
      if (smallestOffsetSize->first.size == iterOffsetSize.size) {
        // Our value should already be in the interval map.
        assert(iter.start() == iterOffsetSize.startOffset &&
               iter.stop() == iterOffsetSize.getEndOffset() &&
               "We should always store entire values");
        continue;
      }

      // Otherwise, we need to destructure the value. Our overall plan is that
      // we walk down the type tree, destructuring as we go.
      //
      // NOTE: We do not actually update our available values here since a later
      // smallest offset size could result in further destructuring that an
      // earlier value required. Instead, we do a final loop afterwards using
      // the interval map to update each available value.
      auto iterType = iterValue->getType();
      auto loc = getSafeLoc(predBlock->getTerminator());
      SILBuilderWithScope builder(predBlock->getTerminator());

      while (smallestOffsetSize->first.size < iterOffsetSize.size) {
        TypeOffsetSizePair childOffsetSize;
        SILType childType;

        // We are returned an optional here and should never fail... so use a
        // force unwrap.
        std::tie(childOffsetSize, childType) =
            *iterOffsetSize.walkOneLevelTowardsChild(iterOffsetSize, iterType,
                                                     fn);

        // Before we destructure ourselves, erase our entire value from the
        // map. We do not need to consider the possibility of there being holes
        // in our range since we always store values whole to their entire
        // subelement range. If we lose a single bit of the value, we split it
        // until we again have whole values.
        {
          auto iter = typeSpanToValue.find(i);
          assert(iter.start() == iterOffsetSize.startOffset &&
                 iter.stop() == iterOffsetSize.getEndOffset() &&
                 "We should always store complete values");
          iter.erase();
        }

        // Then perform our destructuring.
        unsigned childOffsetIterator = iterOffsetSize.startOffset;
        builder.emitDestructureValueOperation(
            loc, predAvailableValues[i], [&](unsigned index, SILValue value) {
              // Now, wire up our new value to its span in the interval map.
              TypeSubElementCount childSize(value);
              typeSpanToValue.insert(childOffsetIterator, childSize, value);

              // Update childOffsetIterator so it points at our next child.
              childOffsetIterator += childSize;
            });
      }
    }

    LLVM_DEBUG(llvm::dbgs()
               << "    Updating available values for bb"
               << predBlock->getDebugID() << "\n    Before Update:\n");
    LLVM_DEBUG(predAvailableValues.print(llvm::dbgs(), "        "));

    // Now do one final loop updating our available values using the interval
    // map.
    for (unsigned i : range(predAvailableValues.size())) {
      auto iter = typeSpanToValue.find(i);
      if (iter == typeSpanToValue.end() || iter.start() > i ||
          iter.stop() <= i) {
        predAvailableValues[i] = SILValue();
      } else {
        predAvailableValues[i] = iter.value();
      }
    }
    LLVM_DEBUG(llvm::dbgs() << "    After Update:\n");
    LLVM_DEBUG(predAvailableValues.print(llvm::dbgs(), "        "));
  }

  LLVM_DEBUG(llvm::dbgs() << "    Inserting phis if needed for bb"
                          << block->getDebugID() << '\n');
  // At this point, all of our values should be the "appropriate size". Now we
  // need to perform the actual phi-ing.
  InstructionDeleter deleter;
  for (unsigned i = 0, e = smallestTypeAvailable.size(); i != e; ++i) {
    // If we don't have a smallest value computed for this, this is not a value
    // to phi. Just continue.
    if (!smallestTypeAvailable[i]) {
      continue;
    }

    // Do a quick check to see if all of the values are the same. In such a
    // case, we can quickly update and continue. Otherwise, we need to insert
    // phis.
    SILValue sameValue;
    for (auto *predBlock : predsSkippingBackEdges) {
      auto &predAvailableValues = computeAvailableValues(predBlock);
      if (!sameValue) {
        sameValue = predAvailableValues[i];
      } else if (sameValue != predAvailableValues[i]) {
        sameValue = SILValue();
      }
    }
    if (sameValue) {
      newValues[i] = sameValue;
      continue;
    }

    // Ok, we need to actually construct a phi.
    {
      SILType offsetType = smallestTypeAvailable[i]->second;
      auto *phi = block->createPhiArgument(offsetType, OwnershipKind::Owned);
      newValues[i] = phi;
      interface.createdPhiArguments.push_back(phi);
    }

    for (auto *predBlock : predsSkippingBackEdges) {
      auto &predAvailableValues = computeAvailableValues(predBlock);
      addNewEdgeValueToBranch(predBlock->getTerminator(), block,
                              predAvailableValues[i], deleter);
    }

    // Then walk ahead until we find a type offset size that is not our type
    // offset size. These were all already handled by this value. Each of those
    // we need to assign the same value as in newValues.
    while (i + 1 != e) {
      // If our next available type does not have a smallest type available or
      // that smallest type available is not our smallest type available,
      // break. We will increment and handle this on the next iteration.
      if (!smallestTypeAvailable[i + 1] ||
          *smallestTypeAvailable[i] != *smallestTypeAvailable[i + 1]) {
        break;
      }

      // Otherwise, reuse our phied value for this available value and increment
      // our iterator.
      newValues[i + 1] = newValues[i];
      ++i;
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "    Final available values for bb"
                          << block->getDebugID() << '\n');
  LLVM_DEBUG(newValues.print(llvm::dbgs(), "        "));
  return newValues;
}

#ifndef NDEBUG
static LLVM_ATTRIBUTE_USED void
dumpIntervalMap(IntervalMapAllocator::Map &map) {
  llvm::dbgs() << "Dumping Interval Map!\n";
  for (auto bi = map.begin(), be = map.end(); bi != be; ++bi) {
    llvm::dbgs() << "Entry. Start: " << bi.start() << " End: " << bi.stop()
                 << "\n";
    llvm::dbgs() << "Value: " << *bi.value() << '\n';
  }
}
#endif

void Implementation::rewriteUses(InstructionDeleter *deleter) {
  blocksToUses.setFrozen();

  LLVM_DEBUG(llvm::dbgs()
             << "Performing BorrowToDestructureTransform::rewriteUses()!\n");
  SWIFT_DEFER {
    LLVM_DEBUG(llvm::dbgs() << "Function after rewriting!\n";
               getMarkedValue()->getFunction()->dump());
  };

  llvm::SmallPtrSet<Operand *, 8> seenOperands;
  SmallBitVector bitsNeededInBlock(liveness.getNumSubElements());
  IntervalMapAllocator::Map typeSpanToValue(getAllocator());

  auto *fn = getMarkedValue()->getFunction();
  assert(!initialValue);
  {
    // We are always going to copy our root value.
    auto *next = getRootValue()->getNextInstruction();
    SILBuilderWithScope builder(next);
    initialValue = builder.createCopyValue(getSafeLoc(next), getRootValue());
  }
  assert(initialValue);

  // Walking each block in RPO order.
  for (auto *block : getPostOrderFunctionInfo()->getReversePostOrder(
           getRootValue()->getParentBlock())) {
    SWIFT_DEFER {
      bitsNeededInBlock.reset();
      seenOperands.clear();
    };

    LLVM_DEBUG(llvm::dbgs()
               << "Visiting block bb" << block->getDebugID() << '\n');

    // See if we have any operands that we need to process...
    if (auto operandList = blocksToUses.find(block)) {
      // If we do, gather up the bits that we need.
      for (auto operand : *operandList) {
        auto &liveBits = operand.second.liveBits;
        LLVM_DEBUG(llvm::dbgs() << "    Found need operand "
                                << operand.first->getOperandNumber()
                                << " of inst: " << *operand.first->getUser());
        for (auto bit : liveBits.set_bits()) {
          bitsNeededInBlock.set(bit);
        }
        seenOperands.insert(operand.first);
      }
    }

    // If we do not need any bits... just continue.
    if (bitsNeededInBlock.none()) {
      LLVM_DEBUG(llvm::dbgs() << "    No needed bits! Continuing!\n");
      continue;
    }

    // Ok, we need some bits in this block. Compute our available values in this
    // block.
    LLVM_DEBUG(llvm::dbgs()
               << "    Found needed bits! Propagating available values!\n");
    auto &availableValues = computeAvailableValues(block);
    LLVM_DEBUG(llvm::dbgs() << "    Computed available values for block bb"
                            << block->getDebugID() << '\n';
               availableValues.print(llvm::dbgs(), "        "));
    // Then walk from the top to the bottom of the block rewriting as we go.
    for (auto ii = block->begin(), ie = block->end(); ii != ie;) {
      auto *inst = &*ii;
      ++ii;

      for (auto &operand : inst->getAllOperands()) {
        if (!seenOperands.count(&operand))
          continue;

        auto span = *TypeTreeLeafTypeRange::get(operand.get(), getRootValue());

        // All available values in our span should have the same value
        // associated with it.
        SILValue first = availableValues[span.startEltOffset];
        assert(llvm::all_of(
            range(span.startEltOffset + 1, span.endEltOffset),
            [&](unsigned index) { return first == availableValues[index]; }));

        LLVM_DEBUG(llvm::dbgs()
                   << "    Rewriting Operand: " << operand.getOperandNumber()
                   << " of inst: " << *operand.getUser()
                   << "    Type Span: " << span << '\n'
                   << "    AvailableValue: " << *first);

        // Then see if first at a type level is equal to our operand's value
        // type. If so, we can just reuse it.
        if (first->getType().removingMoveOnlyWrapper() ==
            operand.get()->getType().removingMoveOnlyWrapper()) {
          LLVM_DEBUG(llvm::dbgs() << "    Found a value that completely covers "
                                     "the operand!\n    Value: "
                                  << *first);
          // If we have:
          //
          // 1. A consuming use.
          // 2. A value that is /not/ move only wrapped and an operand that is
          // non-consuming but can accept an owned value.
          //
          // Just use the owned value. In the case of 2, we need to use a borrow
          // so we can insert the moveonlywrapper_to_copyable [guaranteed] for
          // the use.
          if (operand.isConsuming() ||
              (operand.canAcceptKind(OwnershipKind::Owned) &&
               (first->getType().isMoveOnlyWrapped() ==
                operand.get()->getType().isMoveOnlyWrapped()))) {
            // If we get to this point and have a move only wrapped type and our
            // operand is not a move only wrapped type, then we need to insert
            // an owned moveonlywrapper_to_copyable. We know it must be owned
            // since we can only reach this point if we are consuming.
            if (first->getType().isMoveOnlyWrapped() &&
                !operand.get()->getType().isMoveOnlyWrapped()) {
              SILBuilderWithScope builder(inst);
              first = builder.createOwnedMoveOnlyWrapperToCopyableValue(
                  getSafeLoc(inst), first);
            }
            // NOTE: oldInst may be nullptr if our operand is a SILArgument
            // which can happen with switch_enum.
            SILInstruction *oldInst = operand.get()->getDefiningInstruction();
            operand.set(first);
            if (oldInst && deleter)
              deleter->forceTrackAsDead(oldInst);
            continue;
          }

          // Otherwise, we need to insert a borrow.
          SILBuilderWithScope borrowBuilder(inst);
          SILValue borrow =
              borrowBuilder.createBeginBorrow(getSafeLoc(inst), first);
          SILValue innerValue = borrow;
          if (innerValue->getType().isMoveOnlyWrapped()) {
            innerValue =
                borrowBuilder.createGuaranteedMoveOnlyWrapperToCopyableValue(
                    getSafeLoc(inst), innerValue);
          }

          if (auto op = InteriorPointerOperand::get(&operand)) {
            op.visitBaseValueScopeEndingUses([&](Operand *endScope) -> bool {
              auto *endScopeInst = endScope->getUser();
              SILBuilderWithScope endBuilder(endScopeInst);
              endBuilder.createEndBorrow(getSafeLoc(endScopeInst), borrow);
              return true;
            });
          } else {
            auto *nextInst = inst->getNextInstruction();
            SILBuilderWithScope endBuilder(nextInst);
            endBuilder.createEndBorrow(getSafeLoc(nextInst), borrow);
          }

          // NOTE: This needs to be /after/the interior pointer operand usage
          // above so that we can use the end scope of our interior pointer base
          // value.
          // NOTE: oldInst may be nullptr if our operand is a SILArgument
          // which can happen with switch_enum.
          SILInstruction *oldInst = operand.get()->getDefiningInstruction();
          operand.set(innerValue);
          if (oldInst && deleter)
            deleter->forceTrackAsDead(oldInst);
          continue;
        }

        // Compute the location in the type of first's type and operand.get()'s
        // type.
        TypeOffsetSizePair firstValueOffsetSize(first, getRootValue());
        TypeOffsetSizePair useOffsetSize(operand.get(), getRootValue());

        LLVM_DEBUG(llvm::dbgs() << "    FirstValueTypeOffsetSize: "
                                << firstValueOffsetSize << '\n');
        LLVM_DEBUG(llvm::dbgs()
                   << "    UseOffsetSize: " << useOffsetSize << '\n');

        // Make sure that useOffsetSize is within firstOffsetSize. If it isn't,
        // then lets emit an early error than erroring within our iteration
        // below.
        assert((firstValueOffsetSize.startOffset <= useOffsetSize.startOffset &&
                useOffsetSize.getEndOffset() <=
                    firstValueOffsetSize.getEndOffset()) &&
               "useOffsetSize not within firstOffsetSize?! "
               "operand.get()->getType() isn't a child type of "
               "first->getType()?!");

        // Otherwise, if we have a non-consuming use, we need to create a new
        // borrow scope and extract out the value. Our value should always be
        // fully available.
        if (!operand.isConsuming()) {
          LLVM_DEBUG(
              llvm::dbgs()
              << "    Non Consuming Operand! Extracting using borrows!\n");

          SILBuilderWithScope borrowBuilder(inst);
          auto loc = getSafeLoc(inst);
          auto *borrow = borrowBuilder.createBeginBorrow(loc, first);
          SILValue value = borrow;

          // First walk until we find the same size use as our element and our
          // type that also equals our type. The second part of the check allows
          // us to skip through single level types.
          SILType operandUnwrappedType =
              operand.get()->getType().removingMoveOnlyWrapper();
          while (operandUnwrappedType !=
                 value->getType().removingMoveOnlyWrapper()) {
            std::tie(firstValueOffsetSize, value) =
                *useOffsetSize.walkOneLevelTowardsChild(
                    borrowBuilder, loc, firstValueOffsetSize, value);
          }

          // At this point, we know we have a type of the same size and the same
          // type (modulo moveonlywrapped). If we need to wrap our gepped value,
          // do so now and then set operand to take this new value.
          if (!operand.get()->getType().isMoveOnlyWrapped() &&
              value->getType().isMoveOnlyWrapped()) {
            value =
                borrowBuilder.createGuaranteedMoveOnlyWrapperToCopyableValue(
                    loc, value);
          }
          // NOTE: oldInst may be nullptr if our operand is a SILArgument
          // which can happen with switch_enum.
          auto *oldInst = operand.get()->getDefiningInstruction();
          operand.set(value);
          if (oldInst && deleter)
            deleter->forceTrackAsDead(oldInst);

          // If we have a terminator that is a trivial use (e.x.: we
          // struct_extract a trivial value). Just put the end_borrow before the
          // terminator.
          if (auto *ti = dyn_cast<TermInst>(inst)) {
            if (ti->isFunctionExiting() &&
                operand.getOperandOwnership() == OperandOwnership::TrivialUse) {
              SILBuilderWithScope endBuilder(inst);
              endBuilder.createEndBorrow(getSafeLoc(inst), borrow);
              continue;
            } else {
              // Otherwise, put the end_borrow.
              for (auto *succBlock : ti->getSuccessorBlocks()) {
                auto *nextInst = &succBlock->front();
                SILBuilderWithScope endBuilder(nextInst);
                endBuilder.createEndBorrow(getSafeLoc(nextInst), borrow);
              }
              continue;
            }
          }

          auto *nextInst = inst->getNextInstruction();
          SILBuilderWithScope endBuilder(nextInst);
          endBuilder.createEndBorrow(getSafeLoc(nextInst), borrow);
          continue;
        }

        // If we do a consuming use though, we need to destructure and then
        // update our available value array.
        LLVM_DEBUG(
            llvm::dbgs()
            << "    Consuming Operand! Extracting using destructures!\n");
        SILBuilderWithScope consumeBuilder(inst,
                                           &interface.createdDestructures);
        auto loc = getSafeLoc(inst);
        auto iterOffsetSize = firstValueOffsetSize;
        SILValue iterValue = first;
        SILType iterType = iterValue->getType();
        SWIFT_DEFER { typeSpanToValue.clear(); };

        SILType unwrappedOperandType =
            operand.get()->getType().removingMoveOnlyWrapper();
        while (unwrappedOperandType != iterType.removingMoveOnlyWrapper()) {
          // NOTE: We purposely do not erase our parent offset from the
          // typeSpanToValue. We never insert any element along our walk path
          // (including the initial value) into the interval map.
          auto parentOffsetSize = iterOffsetSize;

          // Then walk one level towards our target type.
          std::tie(iterOffsetSize, iterType) =
              *useOffsetSize.walkOneLevelTowardsChild(parentOffsetSize,
                                                      iterType, fn);

          unsigned start = parentOffsetSize.startOffset;
          consumeBuilder.emitDestructureValueOperation(
              loc, iterValue, [&](unsigned index, SILValue value) {
                unsigned childSize = TypeSubElementCount(value);
                // If we found our value, then stash it into iter value. We are
                // going to consume it directly here.
                if (start == iterOffsetSize.startOffset) {
                  iterValue = value;
                } else {
                  // Otherwise, add it to the type span to value array so we can
                  // update our available values as appropriate.
                  typeSpanToValue.insert(start, start + childSize, value);
                }
                start += childSize;
              });
        }

        // Now that we have finished destructuring, set operand to our iter
        // value... unwrapping iterValue if we need to do so.
        if (iterValue->getType().isMoveOnlyWrapped() &&
            !operand.get()->getType().isMoveOnlyWrapped()) {
          iterValue = consumeBuilder.createOwnedMoveOnlyWrapperToCopyableValue(
              loc, iterValue);
        }
        // NOTE: oldInst may be nullptr if our operand is a SILArgument
        // which can happen with switch_enum.
        auto *oldInst = operand.get()->getDefiningInstruction();
        operand.set(iterValue);
        if (oldInst && deleter)
          deleter->forceTrackAsDead(oldInst);

        // Then go through our available values and use the interval map to
        // update them with the destructured values if we have one for it.
        for (unsigned i : firstValueOffsetSize.getRange()) {
          // NOTE: IntervalMap.find returns the first interval that ends /after/
          // i. This means we need to treat a found iterator that doesn't
          // contain i to be a fail.
          auto iter = typeSpanToValue.find(i);
          if (iter == typeSpanToValue.end() || iter.start() > i ||
              iter.stop() <= i)
            availableValues[i] = SILValue();
          else
            availableValues[i] = iter.value();
        }
        LLVM_DEBUG(llvm::dbgs()
                       << "    Available values after destructuring:\n";
                   availableValues.print(llvm::dbgs(), "        "));
      }
    }

    LLVM_DEBUG(llvm::dbgs() << "Finished visiting/rewriting uses for block bb"
                            << block->getDebugID() << '\n');
  }
}

void Implementation::cleanup() {
  // Then add destroys for any destructure elements that we inserted that we did
  // not actually completely consume.
  auto *fn = getMarkedValue()->getFunction();
  SmallVector<SILBasicBlock *, 8> discoveredBlocks;
  PrunedLivenessBoundary boundary;
  while (!interface.createdDestructures.empty()) {
    auto *inst = interface.createdDestructures.pop_back_val();
    assert(isa<DestructureStructInst>(inst) || isa<DestructureTupleInst>(inst));
    for (auto result : inst->getResults()) {
      if (result->getType().isTrivial(*fn))
        continue;
      SSAPrunedLiveness liveness(fn, &discoveredBlocks);
      SWIFT_DEFER {
        discoveredBlocks.clear();
        boundary.clear();
      };
      addCompensatingDestroys(liveness, boundary, result);
    }
  }

  // Then do this for our inserted phis.
  while (!interface.createdPhiArguments.empty()) {
    auto *arg = interface.createdPhiArguments.pop_back_val();

    // If we have a trivial argument, we do not ened to add any compensating
    // destroys.
    if (arg->getType().isTrivial(*fn))
      continue;

    SSAPrunedLiveness liveness(fn, &discoveredBlocks);
    SWIFT_DEFER {
      discoveredBlocks.clear();
      boundary.clear();
    };
    addCompensatingDestroys(liveness, boundary, arg);
  }

  // And finally do the same thing for our initial copy_value.
  SSAPrunedLiveness liveness(fn, &discoveredBlocks);
  addCompensatingDestroys(liveness, boundary, initialValue);
}

//===----------------------------------------------------------------------===//
//                   MARK: Borrow and SwitchEnum Gathering
//===----------------------------------------------------------------------===//

/// Visit all of the uses of \p mmci and find all begin_borrows.
///
/// Returns false if we found an escape and thus cannot process. It is assumed
/// that the caller will fail in such a case.
static bool gatherBorrows(SILValue rootValue,
                          StackList<BeginBorrowInst *> &borrowWorklist) {
  // If we have a no implicit copy mark_must_check, we do not run the borrow to
  // destructure transform since:
  //
  // 1. If we have a move only type, we should have emitted an earlier error
  //    saying that move only types should not be marked as no implicit copy.
  //
  // 2. If we do not have a move only type, then we know that all fields that we
  //    access directly and would cause a need to destructure must be copyable,
  //    so no transformation/error is needed.
  if (rootValue->getType().isMoveOnlyWrapped()) {
    LLVM_DEBUG(llvm::dbgs()
               << "Skipping move only wrapped inst: " << *rootValue);
    return true;
  }

  LLVM_DEBUG(llvm::dbgs() << "Searching for borrows for inst: " << *rootValue);

  StackList<Operand *> worklist(rootValue->getFunction());
  for (auto *op : rootValue->getUses())
    worklist.push_back(op);

  while (!worklist.empty()) {
    auto *use = worklist.pop_back_val();
    switch (use->getOperandOwnership()) {
    case OperandOwnership::NonUse:
    case OperandOwnership::TrivialUse:
      continue;

    // Conservatively treat a conversion to an unowned value as a pointer
    // escape. Is it legal to canonicalize ForwardingUnowned?
    case OperandOwnership::ForwardingUnowned:
    case OperandOwnership::PointerEscape:
      return false;

    case OperandOwnership::InstantaneousUse:
    case OperandOwnership::UnownedInstantaneousUse:
    case OperandOwnership::BitwiseEscape:
      // We don't care about these types of uses.
      continue;

    case OperandOwnership::ForwardingConsume:
      // Skip if our type is not move only.
      if (!use->get()->getType().isMoveOnly())
        continue;

      // Do not look through apply sites.
      if (ApplySite::isa(use->getUser()))
        continue;

      // Search through forwarding consumes.
      //
      // TODO: Can this just not return a forwarded value for ApplySites?
      ForwardingOperand(use).visitForwardedValues([&](SILValue value) -> bool {
        for (auto *use : value->getUses())
          worklist.push_back(use);
        return true;
      });
      continue;
    case OperandOwnership::DestroyingConsume:
      // We don't care about destroying consume.
      continue;
    case OperandOwnership::Borrow:
      if (auto *bbi = dyn_cast<BeginBorrowInst>(use->getUser())) {
        LLVM_DEBUG(llvm::dbgs() << "    Found borrow: " << *bbi);
        borrowWorklist.push_back(bbi);
      }
      continue;
    case OperandOwnership::InteriorPointer:
      // We don't care about these.
      continue;
    case OperandOwnership::GuaranteedForwarding:
    case OperandOwnership::EndBorrow:
    case OperandOwnership::Reborrow:
      llvm_unreachable("Visiting an owned value!\n");
    }
  }

  return true;
}

//===----------------------------------------------------------------------===//
//                          MARK: Switch Enum Search
//===----------------------------------------------------------------------===//

static bool
gatherSwitchEnum(SILValue value,
                 SmallVectorImpl<SwitchEnumInst *> &switchEnumWorklist) {
  LLVM_DEBUG(llvm::dbgs() << "Gathering switch enums for value: " << *value);

  auto *fn = value->getFunction();
  StackList<Operand *> useWorklist(fn);
  for (auto *use : value->getUses()) {
    useWorklist.push_back(use);
  }

  // Grab the start of our switch enum worklist, so that after we visit the
  // switch_enums that are users of this value, we can recursively visit those
  // values.
  unsigned start = switchEnumWorklist.size();

  while (!useWorklist.empty()) {
    auto *nextUse = useWorklist.pop_back_val();
    LLVM_DEBUG(llvm::dbgs() << "    NextUse: " << *nextUse->getUser());
    switch (nextUse->getOperandOwnership()) {
    case OperandOwnership::NonUse:
      continue;

    // Conservatively treat a conversion to an unowned value as a pointer
    // escape. If we see this in the SIL, fail and return false so we emit a
    // "compiler doesn't understand error".
    case OperandOwnership::ForwardingUnowned:
    case OperandOwnership::PointerEscape:
      LLVM_DEBUG(llvm::dbgs()
                 << "        Found forwarding unowned or pointer escape!\n");
      return false;

    // These might be uses that we need to perform a destructure or insert
    // struct_extracts for.
    case OperandOwnership::TrivialUse:
    case OperandOwnership::InstantaneousUse:
    case OperandOwnership::UnownedInstantaneousUse:
    case OperandOwnership::InteriorPointer:
    case OperandOwnership::BitwiseEscape: {
      // Look through copy_value of a move only value. We treat copy_value of
      // copyable values as normal uses.
      if (auto *cvi = dyn_cast<CopyValueInst>(nextUse->getUser())) {
        if (cvi->getOperand()->getType().isMoveOnly()) {
          LLVM_DEBUG(llvm::dbgs() << "        Found copy value of move only "
                                     "field... looking through!\n");
          for (auto *use : cvi->getUses())
            useWorklist.push_back(use);
          continue;
        }

        // If we don't have a copy of a move only type, we just treat this as a
        // normal use, so we just continue.
      }
      continue;
    }

    case OperandOwnership::ForwardingConsume:
    case OperandOwnership::DestroyingConsume:
      // We don't care about forwarding consumes or destroying consumes.
      continue;

    case OperandOwnership::GuaranteedForwarding:
      // Look through guaranteed forwarding unless we have a switch enum. If we
      // have a switch enum, we add it to the list.
      if (auto *switchEnum = dyn_cast<SwitchEnumInst>(nextUse->getUser())) {
        LLVM_DEBUG(llvm::dbgs()
                   << "        Found switch enum: " << *nextUse->getUser());
        switchEnumWorklist.push_back(switchEnum);
        continue;
      }

      ForwardingOperand(nextUse).visitForwardedValues([&](SILValue value) {
        for (auto *use : value->getUses()) {
          useWorklist.push_back(use);
        }
        return true;
      });
      continue;

    case OperandOwnership::Borrow:
      // Look through borrows.
      if (auto *bbi = dyn_cast<BeginBorrowInst>(nextUse->getUser())) {
        LLVM_DEBUG(llvm::dbgs() << "        Found recursive borrow!\n");
        for (auto *use : bbi->getUses()) {
          useWorklist.push_back(use);
        }
      }
      continue;
    case OperandOwnership::EndBorrow:
      LLVM_DEBUG(llvm::dbgs() << "        Found end borrow!\n");
      continue;
    case OperandOwnership::Reborrow:
      llvm_unreachable("Unsupported for now?!");
    }
  }

  unsigned end = switchEnumWorklist.size();
  if (start == end)
    return true;

  for (unsigned i : range(start, end)) {
    auto *s = switchEnumWorklist[i];
    for (auto argList : s->getSuccessorBlockArgumentLists()) {
      for (SILValue value : argList) {
        if (value->getType().isTrivial(*fn))
          continue;

        if (!gatherSwitchEnum(value, switchEnumWorklist))
          return false;
      }
    }
  }

  return true;
}

//===----------------------------------------------------------------------===//
//                        MARK: Top Level Entrypoint
//===----------------------------------------------------------------------===//

bool BorrowToDestructureTransform::transform() {
  LLVM_DEBUG(llvm::dbgs() << "Performing Borrow To Destructure Tranform!\n");
  auto *fn = mmci->getFunction();
  StackList<BeginBorrowInst *> borrowWorklist(mmci->getFunction());

  // If we failed to gather borrows due to the transform not understanding part
  // of the SIL, fail and return false.
  if (!gatherBorrows(rootValue, borrowWorklist)) {
    diagnosticEmitter.emitCheckerDoesntUnderstandDiagnostic(mmci);
    return false;
  }

  // If we do not have any borrows to process, return true early to show we
  // succeeded in processing.
  if (borrowWorklist.empty()) {
    LLVM_DEBUG(llvm::dbgs() << "No borrows found!\n");
    return true;
  }

  // Then go through our borrows and attempt to gather up guaranteed
  // switch_enums. If we see any of them, we need to transform them into owned
  // switch_enums.
  SmallVector<SwitchEnumInst *, 8> switchEnumWorklist;
  for (auto *borrow : borrowWorklist) {
    // Attempt to gather the switch enums and if we fail, return false.
    if (!gatherSwitchEnum(borrow, switchEnumWorklist)) {
      diagnosticEmitter.emitCheckerDoesntUnderstandDiagnostic(mmci);
      return false;
    }
  }

  // Now perform the checking of our switch_enum, working in stack order.
  {
    SmallVector<CopyValueInst *, 8> switchEnumArgCopyValueToDelete;
    InstructionDeleter deleter;
    while (!switchEnumWorklist.empty()) {
      auto *s = switchEnumWorklist.pop_back_val();
      for (auto argList : s->getSuccessorBlockArgumentLists()) {
        for (SILValue arg : argList) {
          // Skip trivial or copyable values. If we have a copyable value, we
          // will handle it as part of the cleanup phase at the end when we
          // convert the actual switch_enum to be an owned switch_enum.
          if (arg->getType().isTrivial(*fn) || !arg->getType().isMoveOnly())
            continue;

          SmallVector<SILBasicBlock *, 8> discoveredBlocks;
          Implementation impl(*this, discoveredBlocks);
          impl.init(arg);
          if (!impl.gatherUses(arg)) {
            diagnosticEmitter.emitCheckerDoesntUnderstandDiagnostic(mmci);
            continue;
          }

          // Next make sure that any destructure needing instructions are on the
          // boundary in a per bit field sensitive manner.
          unsigned diagnosticCount = diagnosticEmitter.getDiagnosticCount();
          impl.checkDestructureUsesOnBoundary();

          // If we emitted any diagnostic, break out. We return true since we
          // actually succeeded in our processing by finding the error. We only
          // return false if we want to tell the rest of the checker that there
          // was an internal compiler error that we need to emit a "compiler
          // doesn't understand error".
          if (diagnosticCount != diagnosticEmitter.getDiagnosticCount())
            return true;

          // Then check if we had two consuming uses on the same instruction or
          // a consuming/non-consuming use on the same isntruction.
          impl.checkForErrorsOnSameInstruction();

          // If we emitted any diagnostic, break out. We return true since we
          // actually succeeded in our processing by finding the error. We only
          // return false if we want to tell the rest of the checker that there
          // was an internal compiler error that we need to emit a "compiler
          // doesn't understand error".
          if (diagnosticCount != diagnosticEmitter.getDiagnosticCount())
            return true;

          // At this point, we know that all of our destructure requiring uses
          // are on the boundary of our live range. Now we need to do the
          // rewriting.
          impl.blockToAvailableValues.emplace(impl.liveness);
          impl.rewriteUses(&deleter);

          // Now that we have done our rewritting, we need to do a few cleanups
          // starting by inserting compensating destroys for all of our inserted
          // phis/destructures/initial value copy.
          impl.cleanup();

          // Now grab our initialValue which will be a copy_value from our
          // argument and RAUW it. We are going to convert the argument
          // later. We left it in to ensure that as we recreated instructions,
          // OSSA invariants were satisfied locally (albeit the actual IR was
          // not in a consistent state).
          auto *cvi = cast<CopyValueInst>(impl.initialValue);
          switchEnumArgCopyValueToDelete.push_back(cvi);
        }
      }

      // Now that we have processed all of the arguments for this switch_enum,
      // cleanup any dead instructions.
      deleter.cleanupDeadInstructions();

      // Now that we have processed the switch_enum, we need to convert the
      // switch_enum to be owned. We do this by introducing a copy on the
      // switch_enum argument and then insert a destroy_value after the single
      // copy_value in each destination block that we originally inserted.
      {
        SmallVector<SILBasicBlock *, 8> discoveredBlocks;
        PrunedLivenessBoundary boundary;

        SILBuilderWithScope builder(s);
        SILValue newOperand =
            builder.createCopyValue(getSafeLoc(s), s->getOperand());
        s->setOperand(0, newOperand);
        s->setForwardingOwnershipKind(OwnershipKind::Owned);
        for (auto argList : s->getSuccessorBlockArgumentLists()) {
          for (SILArgument *arg : argList) {
            if (arg->getType().isTrivial(*fn))
              continue;
            arg->setOwnershipKind(OwnershipKind::Owned);

            if (arg->getType().isMoveOnly())
              continue;

            // If we have a copyable type, we need to insert compensating
            // destroys.
            SSAPrunedLiveness liveness(fn, &discoveredBlocks);
            SWIFT_DEFER {
              discoveredBlocks.clear();
              boundary.clear();
            };
            addCompensatingDestroys(liveness, boundary, arg);
          }
        }
      }

      // Now eliminate our unneeded copyvalues from earlier than we inserted to
      // satisfy OSSA invariants.
      while (!switchEnumArgCopyValueToDelete.empty()) {
        auto *cvi = switchEnumArgCopyValueToDelete.pop_back_val();
        cvi->replaceAllUsesWith(cvi->getOperand());
        cvi->eraseFromParent();
      }
    }
  }

  // At this point, we have correct OSSA SIL for our switch_enums. Check if for
  // any of our switch_enum we emitted a we don't understand diagnostic... in
  // such a case, exit before we do further work.
  if (diagnosticEmitter.didEmitCheckerDoesntUnderstandDiagnostic())
    return false;

  // Now that we have handled our switch_enum we need to handle our
  // borrows... begin by gathering uses. Return false if we saw something that
  // we did not understand.
  SmallVector<SILBasicBlock *, 8> discoveredBlocks;
  Implementation impl(*this, discoveredBlocks);
  impl.init(rootValue);
  for (auto *bbi : borrowWorklist) {
    if (!impl.gatherUses(bbi)) {
      diagnosticEmitter.emitCheckerDoesntUnderstandDiagnostic(mmci);
      return false;
    }
  }

  // Next make sure that any destructure needing instructions are on the
  // boundary in a per bit field sensitive manner.
  unsigned diagnosticCount = diagnosticEmitter.getDiagnosticCount();
  impl.checkDestructureUsesOnBoundary();

  // If we emitted any diagnostic, break out. We return true since we actually
  // succeeded in our processing by finding the error. We only return false if
  // we want to tell the rest of the checker that there was an internal
  // compiler error that we need to emit a "compiler doesn't understand
  // error".
  if (diagnosticCount != diagnosticEmitter.getDiagnosticCount())
    return true;

  // Then check if we had two consuming uses on the same instruction or a
  // consuming/non-consuming use on the same isntruction.
  impl.checkForErrorsOnSameInstruction();

  // If we emitted any diagnostic, break out. We return true since we actually
  // succeeded in our processing by finding the error. We only return false if
  // we want to tell the rest of the checker that there was an internal
  // compiler error that we need to emit a "compiler doesn't understand
  // error".
  if (diagnosticCount != diagnosticEmitter.getDiagnosticCount())
    return true;

  // At this point, we know that all of our destructure requiring uses are on
  // the boundary of our live range. Now we need to do the rewriting.
  impl.blockToAvailableValues.emplace(impl.liveness);
  impl.rewriteUses();

  // Now that we have done our rewritting, we need to do a few cleanups starting
  // by inserting compensating destroys for all of our inserted
  // phis/destructures/initial value copy.
  impl.cleanup();

  // Then clean up all of our borrows/copies/struct_extracts which no longer
  // have any uses...
  {
    InstructionDeleter deleter;
    while (!borrowWorklist.empty()) {
      deleter.recursivelyForceDeleteUsersAndFixLifetimes(
          borrowWorklist.pop_back_val());
    }
  }

  return true;
}
