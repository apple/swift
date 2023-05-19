//===--- SILSSAUpdater.cpp - Unstructured SSA Update Tool -----------------===//
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

#include "swift/SILOptimizer/Utils/SILSSAUpdater.h"
#include "swift/Basic/Malloc.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/SSAUpdaterImpl.h"

using namespace swift;

void *SILSSAUpdater::allocate(unsigned size, unsigned align) const {
  return AlignedAlloc(size, align);
}

void SILSSAUpdater::deallocateSentinel(SILUndef *undef) {
  AlignedFree(undef);
}

SILSSAUpdater::SILSSAUpdater(SmallVectorImpl<SILPhiArgument *> *phis)
    : blockToAvailableValueMap(nullptr), ownershipKind(OwnershipKind::None),
      phiSentinel(nullptr, deallocateSentinel), insertedPhis(phis) {}

SILSSAUpdater::~SILSSAUpdater() = default;

void SILSSAUpdater::initialize(SILType inputType, ValueOwnershipKind kind) {
  type = inputType;
  ownershipKind = kind;

  phiSentinel = std::unique_ptr<SILUndef, void (*)(SILUndef *)>(
      SILUndef::getSentinelValue(inputType, this),
      SILSSAUpdater::deallocateSentinel);

  if (!blockToAvailableValueMap)
    blockToAvailableValueMap.reset(new AvailableValsTy());
  else
    blockToAvailableValueMap->clear();
}

bool SILSSAUpdater::hasValueForBlock(SILBasicBlock *block) const {
  return blockToAvailableValueMap->count(block);
}

/// Indicate that a rewritten value is available in the specified block with the
/// specified value.
void SILSSAUpdater::addAvailableValue(SILBasicBlock *block, SILValue value) {
  assert(value->getOwnershipKind().isCompatibleWith(ownershipKind));
  (*blockToAvailableValueMap)[block] = value;
}

/// Construct SSA form, materializing a value that is live at the end of the
/// specified block.
SILValue SILSSAUpdater::getValueAtEndOfBlock(SILBasicBlock *block) {
  return getValueAtEndOfBlockInternal(block);
}

/// Are all available values identicalTo each other.
static bool
areIdentical(llvm::DenseMap<SILBasicBlock *, SILValue> &availableValues) {
  if (auto *firstInst =
          dyn_cast<SingleValueInstruction>(availableValues.begin()->second)) {
    for (auto value : availableValues) {
      auto *svi = dyn_cast<SingleValueInstruction>(value.second);
      if (!svi)
        return false;
      if (!svi->isIdenticalTo(firstInst))
        return false;
    }
    return true;
  }

  auto *mvir =
      dyn_cast<MultipleValueInstructionResult>(availableValues.begin()->second);
  if (!mvir)
    return false;

  for (auto value : availableValues) {
    auto *result = dyn_cast<MultipleValueInstructionResult>(value.second);
    if (!result)
      return false;
    if (!result->getParent()->isIdenticalTo(mvir->getParent()) ||
        result->getIndex() != mvir->getIndex()) {
      return false;
    }
  }
  return true;
}

/// This should be called in top-down order of each def that needs its uses
/// rewritten. The order that we visit uses for a given def is irrelevant.
void SILSSAUpdater::rewriteUse(Operand &use) {
  // Replicate function_refs to their uses. SILGen can't build phi nodes for
  // them and it would not make much sense anyways.
  if (auto *fri = dyn_cast<FunctionRefInst>(use.get())) {
    assert(areIdentical(*blockToAvailableValueMap) &&
           "The function_refs need to have the same value");
    SILInstruction *user = use.getUser();
    use.set(cast<FunctionRefInst>(fri->clone(user)));
    return;
  } else if (auto *pdfri =
                 dyn_cast<PreviousDynamicFunctionRefInst>(use.get())) {
    assert(areIdentical(*blockToAvailableValueMap) &&
           "The function_refs need to have the same value");
    SILInstruction *user = use.getUser();
    use.set(cast<PreviousDynamicFunctionRefInst>(pdfri->clone(user)));
    return;
  } else if (auto *dfri = dyn_cast<DynamicFunctionRefInst>(use.get())) {
    assert(areIdentical(*blockToAvailableValueMap) &&
           "The function_refs need to have the same value");
    SILInstruction *user = use.getUser();
    use.set(cast<DynamicFunctionRefInst>(dfri->clone(user)));
    return;
  } else if (auto *ili = dyn_cast<IntegerLiteralInst>(use.get()))
    if (areIdentical(*blockToAvailableValueMap)) {
      // Some llvm intrinsics don't like phi nodes as their constant inputs (e.g
      // ctlz).
      SILInstruction *user = use.getUser();
      use.set(cast<IntegerLiteralInst>(ili->clone(user)));
      return;
    }

  // Again we need to be careful here, because ssa construction (with the
  // existing representation) can change the operand from under us.
  UseWrapper useWrapper(&use);

  SILInstruction *user = use.getUser();
  SILValue newVal = getValueInMiddleOfBlock(user->getParent());
  assert(newVal && "Need a valid value");
  static_cast<Operand *>(useWrapper)->set(newVal);
}

/// Get the edge values from the terminator to the destination basic block.
static OperandValueArrayRef getEdgeValuesForTerminator(TermInst *ti,
                                                       SILBasicBlock *toBlock) {
  if (auto *br = dyn_cast<BranchInst>(ti)) {
    assert(br->getDestBB() == toBlock &&
           "Incoming edge block and phi block mismatch");
    return br->getArgs();
  }
  if (auto *cbi = dyn_cast<CondBranchInst>(ti)) {
    bool isTrueEdge = cbi->getTrueBB() == toBlock;
    assert(((isTrueEdge && cbi->getTrueBB() == toBlock) ||
            cbi->getFalseBB() == toBlock) &&
           "Incoming edge block and phi block mismatch");
    return isTrueEdge ? cbi->getTrueArgs() : cbi->getFalseArgs();
  }

  // We need a predecessor who is capable of holding outgoing branch
  // arguments.
  llvm_unreachable("Unrecognized terminator leading to phi block");
}

/// Check that the argument has the same incoming edge values as the value
/// map.
static bool
isEquivalentPHI(SILPhiArgument *phi,
                llvm::SmallDenseMap<SILBasicBlock *, SILValue, 8> &valueMap) {
  SILBasicBlock *phiBlock = phi->getParent();
  size_t phiArgEdgeIndex = phi->getIndex();
  for (auto *predBlock : phiBlock->getPredecessorBlocks()) {
    auto desiredVal = valueMap[predBlock];
    OperandValueArrayRef edgeValues =
        getEdgeValuesForTerminator(predBlock->getTerminator(), phiBlock);
    if (edgeValues[phiArgEdgeIndex] != desiredVal)
      return false;
  }
  return true;
}

SILValue SILSSAUpdater::getValueInMiddleOfBlock(SILBasicBlock *block) {
  // If this basic block does not define a value we can just use the value
  // live at the end of the block.
  if (!hasValueForBlock(block))
    return getValueAtEndOfBlock(block);

  /// Otherwise, we have to build SSA for the value defined in this block and
  /// this block's predecessors.
  SILValue singularValue;
  SmallVector<std::pair<SILBasicBlock *, SILValue>, 4> predVals;
  bool firstPred = true;

  // SSAUpdater can modify TerminatorInst and therefore invalidate the
  // predecessor iterator. Find all the predecessors before the SSA update.
  SmallVector<SILBasicBlock *, 4> preds;
  for (auto *predBlock : block->getPredecessorBlocks()) {
    preds.push_back(predBlock);
  }

  for (auto *predBlock : preds) {
    SILValue predVal = getValueAtEndOfBlock(predBlock);
    predVals.push_back(std::make_pair(predBlock, predVal));
    if (firstPred) {
      singularValue = predVal;
      firstPred = false;
    } else if (singularValue != predVal)
      singularValue = SILValue();
  }

  // Return undef for blocks without predecessor.
  if (predVals.empty())
    return SILUndef::get(type, *block->getParent());

  if (singularValue)
    return singularValue;

  // Check if we already have an equivalent phi.
  if (!block->getArguments().empty()) {
    llvm::SmallDenseMap<SILBasicBlock *, SILValue, 8> valueMap(predVals.begin(),
                                                               predVals.end());
    for (auto *arg : block->getSILPhiArguments())
      if (isEquivalentPHI(arg, valueMap))
        return arg;
  }

  // Create a new phi node.
  SILPhiArgument *phiArg = block->createPhiArgument(type, ownershipKind);
  for (auto &pair : predVals) {
    addNewEdgeValueToBranch(pair.first->getTerminator(), block, pair.second,
                            deleter);
  }
  // Set the reborrow flag on the newly created phi.
  phiArg->setReborrow(computeIsReborrow(phiArg));

  if (insertedPhis)
    insertedPhis->push_back(phiArg);

  return phiArg;
}

namespace llvm {

/// Traits for the SSAUpdaterImpl specialized for SIL and the SILSSAUpdater.
template <>
class SSAUpdaterTraits<SILSSAUpdater> {
public:
  using BlkT = SILBasicBlock;
  using ValT = SILValue;
  using PhiT = SILPhiArgument;

  using BlkSucc_iterator = SILBasicBlock::succ_iterator;
  static BlkSucc_iterator BlkSucc_begin(BlkT *block) {
    return block->succ_begin();
  }
  static BlkSucc_iterator BlkSucc_end(BlkT *block) { return block->succ_end(); }

  /// Iterator for PHI operands.
  class PHI_iterator {
  private:
    SILBasicBlock::pred_iterator predBlockIter;
    SILBasicBlock *phiBlock;
    size_t phiArgEdgeIndex;

  public:
    explicit PHI_iterator(SILPhiArgument *phiArg) // begin iterator
        : predBlockIter(phiArg->getParent()->pred_begin()),
          phiBlock(phiArg->getParent()), phiArgEdgeIndex(phiArg->getIndex()) {}
    PHI_iterator(SILPhiArgument *phiArg, bool) // end iterator
        : predBlockIter(phiArg->getParent()->pred_end()),
          phiBlock(phiArg->getParent()), phiArgEdgeIndex(phiArg->getIndex()) {}

    PHI_iterator &operator++() {
      ++predBlockIter;
      return *this;
    }

    bool operator==(const PHI_iterator &x) const {
      return predBlockIter == x.predBlockIter;
    }

    bool operator!=(const PHI_iterator& x) const { return !operator==(x); }

    SILValue getValueForBlock(size_t inputArgIndex, SILBasicBlock *block,
                              TermInst *ti) {
      OperandValueArrayRef args = getEdgeValuesForTerminator(ti, block);
      assert(inputArgIndex < args.size() &&
             "Not enough values on incoming edge");
      return args[inputArgIndex];
    }

    SILValue getIncomingValue() {
      return getValueForBlock(phiArgEdgeIndex, phiBlock,
                              (*predBlockIter)->getTerminator());
    }

    SILBasicBlock *getIncomingBlock() { return *predBlockIter; }
  };

  static inline PHI_iterator PHI_begin(PhiT *phi) { return PHI_iterator(phi); }
  static inline PHI_iterator PHI_end(PhiT *phi) {
    return PHI_iterator(phi, true);
  }

  /// Put the predecessors of BB into the Preds vector.
  static void
  FindPredecessorBlocks(SILBasicBlock *block,
                        SmallVectorImpl<SILBasicBlock *> *predBlocks) {
    llvm::copy(block->getPredecessorBlocks(), std::back_inserter(*predBlocks));
  }

  static SILValue GetUndefVal(SILBasicBlock *block, SILSSAUpdater *ssaUpdater) {
    return SILUndef::get(ssaUpdater->type, *block->getParent());
  }

  /// Add an Argument to the basic block.
  static SILValue CreateEmptyPHI(SILBasicBlock *block, unsigned numPreds,
                                 SILSSAUpdater *ssaUpdater) {
    // Add the argument to the block.
    SILValue phi(
        block->createPhiArgument(ssaUpdater->type, ssaUpdater->ownershipKind));

    // Mark all predecessor blocks with the sentinel undef value.
    SmallVector<SILBasicBlock *, 4> predBlockList(
        block->getPredecessorBlocks());

    for (auto *predBlock : predBlockList) {
      TermInst *ti = predBlock->getTerminator();
      addNewEdgeValueToBranch(ti, block, ssaUpdater->phiSentinel.get(),
                              ssaUpdater->deleter);
    }

    return phi;
  }

  /// Add \p value as an operand of the phi argument \p phi for the specified
  /// predecessor block \p predBlock.
  static void AddPHIOperand(SILPhiArgument *phi, SILValue value,
                            SILBasicBlock *predBlock) {
    auto *phiBlock = phi->getParent();
    size_t phiArgIndex = phi->getIndex();
    auto *ti = predBlock->getTerminator();

    changeEdgeValue(ti, phiBlock, phiArgIndex, value);

    // Set the reborrow flag.
    phi->setReborrow(computeIsReborrow(phi));
  }

  /// Check if an instruction is a PHI.
  static SILPhiArgument *InstrIsPHI(ValueBase *valueBase) {
    return dyn_cast<SILPhiArgument>(valueBase);
  }

  /// Check if the instruction that defines the specified SILValue is a PHI
  /// instruction.
  static SILPhiArgument *ValueIsPHI(SILValue value, SILSSAUpdater *) {
    return InstrIsPHI(value);
  }

  /// Like ValueIsPHI but also check if the PHI has no source
  /// operands, i.e., it was just added.
  static SILPhiArgument *ValueIsNewPHI(SILValue value,
                                       SILSSAUpdater *ssaUpdater) {
    SILPhiArgument *phiArg = ValueIsPHI(value, ssaUpdater);
    if (!phiArg) {
      return nullptr;
    }

    auto *phiBlock = phiArg->getParent();
    size_t phiArgEdgeIndex = phiArg->getIndex();

    // If all predecessor edges are 'not set' this is a new phi.
    for (auto *predBlock : phiBlock->getPredecessorBlocks()) {
      OperandValueArrayRef edgeValues =
          getEdgeValuesForTerminator(predBlock->getTerminator(), phiBlock);

      assert(phiArgEdgeIndex < edgeValues.size() && "Not enough edges!");

      SILValue edgeValue = edgeValues[phiArgEdgeIndex];
      // Check for the 'not set' sentinel.
      if (edgeValue != ssaUpdater->phiSentinel.get())
        return nullptr;
    }
    return phiArg;
  }

  static SILValue GetPHIValue(SILPhiArgument *phi) { return phi; }
};

} // namespace llvm

/// Check to see if AvailableVals has an entry for the specified BB and if so,
/// return it.  If not, construct SSA form by first calculating the required
/// placement of PHIs and then inserting new PHIs where needed.
SILValue SILSSAUpdater::getValueAtEndOfBlockInternal(SILBasicBlock *block) {
  AvailableValsTy &availableValues = *blockToAvailableValueMap;
  auto iter = availableValues.find(block);
  if (iter != availableValues.end())
    return iter->second;

  llvm::SSAUpdaterImpl<SILSSAUpdater> impl(this, &availableValues,
                                           insertedPhis);
  return impl.GetValue(block);
}

/// Construct a use wrapper. For branches we store information so that we
/// can reconstruct the use after the branch has been modified.
///
/// When a branch is modified existing pointers to the operand
/// (ValueUseIterator) become invalid as they point to freed operands.  Instead
/// we store the branch's parent and the idx so that we can reconstruct the use.
UseWrapper::UseWrapper(Operand *inputUse) {
  wrappedUse = nullptr;
  type = kRegularUse;

  SILInstruction *user = inputUse->getUser();

  // Direct branch user.
  if (auto *br = dyn_cast<BranchInst>(user)) {
    for (auto pair : llvm::enumerate(user->getAllOperands())) {
      if (inputUse == &pair.value()) {
        index = pair.index();
        type = kBranchUse;
        parent = br->getParent();
        return;
      }
    }
  }

  // Conditional branch user.
  if (auto *cbi = dyn_cast<CondBranchInst>(user)) {
    auto operands = user->getAllOperands();
    auto numTrueArgs = cbi->getTrueArgs().size();
    for (auto pair : llvm::enumerate(operands)) {
      if (inputUse == &pair.value()) {
        unsigned i = pair.index();
        // We treat the condition as part of the true args.
        if (i < numTrueArgs + 1) {
          index = i;
          type = kCondBranchUseTrue;
        } else {
          index = i - numTrueArgs - 1;
          type = kCondBranchUseFalse;
        }
        parent = cbi->getParent();
        return;
      }
    }
  }

  wrappedUse = inputUse;
}

/// Return the operand we wrap. Reconstructing branch operands.
Operand *UseWrapper::getOperand() {
  switch (type) {
  case kRegularUse:
    return wrappedUse;

  case kBranchUse: {
    auto *br = cast<BranchInst>(parent->getTerminator());
    assert(index < br->getNumArgs());
    return &br->getAllOperands()[index];
  }

  case kCondBranchUseTrue:
  case kCondBranchUseFalse: {
    auto *cbi = cast<CondBranchInst>(parent->getTerminator());
    auto indexToUse = [&]() -> unsigned {
      if (type == kCondBranchUseTrue)
        return index;
      return cbi->getTrueArgs().size() + 1 + index;
    }();
    assert(indexToUse < cbi->getAllOperands().size());
    return &cbi->getAllOperands()[indexToUse];
  }
  }

  llvm_unreachable("uninitialize use type");
}

/// At least one value feeding the specified SILArgument is a Struct. Attempt to
/// replace the Argument with a new Struct in the same block.
///
/// When we handle more types of casts, this can become a template.
///
/// ArgValues are the values feeding the specified Argument from each
/// predecessor. They must be listed in order of Arg->getParent()->getPreds().
static StructInst *
replaceBBArgWithStruct(SILPhiArgument *phiArg,
                       SmallVectorImpl<SILValue> &argValues) {

  SILBasicBlock *phiBlock = phiArg->getParent();
  auto *firstSI = dyn_cast<StructInst>(argValues[0]);
  if (!firstSI)
    return nullptr;

  // Collect the BBArg index of each struct oper.
  // e.g.
  //   struct(A, B)
  //   br (B, A)
  // : ArgIdxForOper => {1, 0}
  SmallVector<unsigned, 4> argIdxForOper;
  for (unsigned operIdx : indices(firstSI->getElements())) {
    bool foundMatchingArgIdx = false;
    for (unsigned argIdx : indices(phiBlock->getArguments())) {
      auto avIter = argValues.begin();
      bool tryNextArgIdx = false;
      for (SILBasicBlock *predBlock : phiBlock->getPredecessorBlocks()) {
        // All argument values must be StructInst.
        auto *predSI = dyn_cast<StructInst>(*avIter++);
        if (!predSI)
          return nullptr;
        OperandValueArrayRef edgeValues =
            getEdgeValuesForTerminator(predBlock->getTerminator(), phiBlock);
        if (edgeValues[argIdx] != predSI->getElements()[operIdx]) {
          tryNextArgIdx = true;
          break;
        }
      }
      if (!tryNextArgIdx) {
        assert(avIter == argValues.end() &&
               "# ArgValues does not match # BB preds");
        foundMatchingArgIdx = true;
        argIdxForOper.push_back(argIdx);
        break;
      }
    }
    if (!foundMatchingArgIdx)
      return nullptr;
  }

  SmallVector<SILValue, 4> structArgs;
  for (auto argIdx : argIdxForOper)
    structArgs.push_back(phiBlock->getArgument(argIdx));

  // TODO: We probably want to use a SILBuilderWithScope here. What should we
  // use?
  SILBuilder builder(phiBlock, phiBlock->begin());
  return builder.createStruct(cast<StructInst>(argValues[0])->getLoc(),
                              phiArg->getType(), structArgs);
}

/// Canonicalize BB arguments, replacing argument-of-casts with
/// cast-of-arguments. This only eliminates existing arguments, replacing them
/// with casts. No new arguments are created. This allows downstream pattern
/// detection like induction variable analysis to succeed.
///
/// If Arg is replaced, return the cast instruction. Otherwise return nullptr.
SILValue swift::replaceBBArgWithCast(SILPhiArgument *arg) {
  SmallVector<SILValue, 4> argValues;
  arg->getIncomingPhiValues(argValues);
  if (isa<StructInst>(argValues[0]))
    return replaceBBArgWithStruct(arg, argValues);
  return nullptr;
}
