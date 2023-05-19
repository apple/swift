//===--- SILArgument.cpp - Arguments for high-level SIL code --------------===//
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

#include "swift/SIL/SILArgument.h"
#include "swift/Basic/GraphNodeWorklist.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "llvm/ADT/STLExtras.h"

using namespace swift;

//===----------------------------------------------------------------------===//
// SILArgument Implementation
//===----------------------------------------------------------------------===//

SILArgument::SILArgument(ValueKind subClassKind,
                         SILBasicBlock *inputParentBlock, SILType type,
                         ValueOwnershipKind ownershipKind,
                         const ValueDecl *inputDecl, bool reborrow,
                         bool escaping)
    : ValueBase(subClassKind, type), parentBlock(inputParentBlock),
      decl(inputDecl) {
  sharedUInt8().SILArgument.valueOwnershipKind = uint8_t(ownershipKind);
  sharedUInt8().SILArgument.reborrow = reborrow;
  sharedUInt8().SILArgument.escaping = escaping;
  inputParentBlock->insertArgument(inputParentBlock->args_end(), this);
}

SILFunction *SILArgument::getFunction() {
  return getParent()->getParent();
}

const SILFunction *SILArgument::getFunction() const {
  return getParent()->getParent();
}

SILModule &SILArgument::getModule() const {
  return getFunction()->getModule();
}

unsigned SILArgument::getIndex() const {
  for (auto p : llvm::enumerate(getParent()->getArguments())) {
    if (p.value() == this) {
      return p.index();
    }
  }
  llvm_unreachable("SILArgument not argument of its parent BB");
}

bool SILFunctionArgument::isIndirectResult() const {
  auto numIndirectResults =
      getFunction()->getConventions().getNumIndirectSILResults();
  return getIndex() < numIndirectResults;
}

SILArgumentConvention SILFunctionArgument::getArgumentConvention() const {
  return getFunction()->getConventions().getSILArgumentConvention(getIndex());
}

/// Given that this is an entry block argument, and given that it does
/// not correspond to an indirect result, return the corresponding
/// SILParameterInfo.
SILParameterInfo SILFunctionArgument::getKnownParameterInfo() const {
  return getFunction()->getConventions().getParamInfoForSILArg(getIndex());
}


//===----------------------------------------------------------------------===//
//                              SILBlockArgument
//===----------------------------------------------------------------------===//

// FIXME: SILPhiArgument should only refer to phis (values merged from
// BranchInst operands). Phis are directly substitutable with their incoming
// values modulo control flow. They usually need to be distinguished from
// projections and casts. It is needlessly expensive to call this helper instead
// of simply specifying phis with an opcode. It results in repeated CFG
// traversals and repeated, unnecessary switching over terminator opcodes.
bool SILPhiArgument::isPhi() const {
  // No predecessors indicates an unreachable block. Treat this like a
  // degenerate phi so we don't consider it a terminator result.
  if (getParent()->pred_empty())
    return true;

  // Multiple predecessors require phis.
  auto *predBlock = getParent()->getSinglePredecessorBlock();
  if (!predBlock)
    return true;

  auto *termInst = predBlock->getTerminator();
  return isa<BranchInst>(termInst) || isa<CondBranchInst>(termInst);
}

static Operand *getIncomingPhiOperandForPred(const SILBasicBlock *parentBlock,
                                             const SILBasicBlock *predBlock,
                                             unsigned argIndex) {
  auto *predBlockTermInst = predBlock->getTerminator();
  if (auto *bi = dyn_cast<BranchInst>(predBlockTermInst)) {
    return &const_cast<BranchInst *>(bi)->getAllOperands()[argIndex];
  }

  // FIXME: Disallowing critical edges in SIL would enormously simplify phi and
  // branch handling and reduce expensive analysis invalidation. If that is
  // done, then only BranchInst will participate in phi operands, eliminating
  // the need to search for the appropriate CondBranchInst operand.
  return cast<CondBranchInst>(predBlockTermInst)
      ->getOperandForDestBB(parentBlock, argIndex);
}

static SILValue getIncomingPhiValueForPred(const SILBasicBlock *parentBlock,
                                           const SILBasicBlock *predBlock,
                                           unsigned argIndex) {
  const auto *predBlockTermInst = predBlock->getTerminator();
  if (auto *bi = dyn_cast<BranchInst>(predBlockTermInst))
    return bi->getArg(argIndex);

  // FIXME: Disallowing critical edges in SIL would enormously simplify phi and
  // branch handling and reduce expensive analysis invalidation. If that is
  // done, then only BranchInst will participate in phi operands, eliminating
  // the need to search for the appropriate CondBranchInst operand.
  return cast<CondBranchInst>(predBlockTermInst)
      ->getArgForDestBB(parentBlock, argIndex);
}

SILValue SILPhiArgument::getIncomingPhiValue(SILBasicBlock *predBlock) const {
  if (!isPhi())
    return SILValue();

  const auto *parentBlock = getParent();
  assert(!parentBlock->pred_empty());

  unsigned argIndex = getIndex();

  assert(parentBlock->pred_end() != std::find(parentBlock->pred_begin(),
                                              parentBlock->pred_end(),
                                              predBlock));

  return getIncomingPhiValueForPred(parentBlock, predBlock, argIndex);
}

bool SILPhiArgument::getIncomingPhiValues(
    SmallVectorImpl<SILValue> &returnedPhiValues) const {
  if (!isPhi())
    return false;

  const auto *parentBlock = getParent();
  assert(!parentBlock->pred_empty());

  unsigned argIndex = getIndex();
  for (auto *predBlock : getParent()->getPredecessorBlocks()) {
    SILValue incomingValue =
        getIncomingPhiValueForPred(parentBlock, predBlock, argIndex);
    assert(incomingValue);
    returnedPhiValues.push_back(incomingValue);
  }
  return true;
}

Operand *SILPhiArgument::getIncomingPhiOperand(SILBasicBlock *predBlock) const {
  if (!isPhi())
    return nullptr;
  return getIncomingPhiOperandForPred(getParent(), predBlock, getIndex());
}

bool SILPhiArgument::getIncomingPhiOperands(
    SmallVectorImpl<Operand *> &returnedPhiOperands) const {
  if (!isPhi())
    return false;

  const auto *parentBlock = getParent();

  unsigned argIndex = getIndex();
  for (auto *predBlock : getParent()->getPredecessorBlocks()) {
    Operand *incomingOperand =
        getIncomingPhiOperandForPred(parentBlock, predBlock, argIndex);
    assert(incomingOperand);
    returnedPhiOperands.push_back(incomingOperand);
  }
  return true;
}

bool SILPhiArgument::visitIncomingPhiOperands(
    function_ref<bool(Operand *)> visitor) const {
  if (!isPhi())
    return false;

  const auto *parentBlock = getParent();
  assert(!parentBlock->pred_empty());

  unsigned argIndex = getIndex();
  for (auto *predBlock : getParent()->getPredecessorBlocks()) {
    Operand *incomingOperand =
        getIncomingPhiOperandForPred(parentBlock, predBlock, argIndex);
    assert(incomingOperand);

    // Call the visitor, bailing if the callee signals error.
    if (!visitor(incomingOperand)) {
      return false;
    }
  }
  return true;
}

bool SILPhiArgument::getIncomingPhiValues(
    SmallVectorImpl<std::pair<SILBasicBlock *, SILValue>>
        &returnedPredBBAndPhiValuePairs) const {
  if (!isPhi())
    return false;

  const auto *parentBlock = getParent();

  unsigned argIndex = getIndex();
  for (auto *predBlock : getParent()->getPredecessorBlocks()) {
    SILValue incomingValue =
        getIncomingPhiValueForPred(parentBlock, predBlock, argIndex);
    assert(incomingValue);
    returnedPredBBAndPhiValuePairs.push_back({predBlock, incomingValue});
  }
  return true;
}

bool SILPhiArgument::visitTransitiveIncomingPhiOperands(
    function_ref<bool(SILPhiArgument *, Operand *)> visitor) const {
  if (!isPhi())
    return false;

  GraphNodeWorklist<SILPhiArgument *, 4> worklist;
  worklist.insert(const_cast<SILPhiArgument *>(this));

  while (auto *argument = worklist.pop()) {
    SmallVector<Operand *> operands;
    argument->getIncomingPhiOperands(operands);

    for (auto *operand : operands) {
      SILPhiArgument *forwarded = dyn_cast<SILPhiArgument>(operand->get());
      if (forwarded && forwarded->isPhi()) {
        worklist.insert(forwarded);
      }
      if (!visitor(argument, operand))
        return false;
    }
  }
  return true;
}

static SILValue
getSingleTerminatorOperandForPred(const SILBasicBlock *parentBlock,
                                  const SILBasicBlock *predBlock,
                                  unsigned argIndex) {
  const auto *predTermInst = predBlock->getTerminator();

  switch (predTermInst->getTermKind()) {
  case TermKind::UnreachableInst:
  case TermKind::ReturnInst:
  case TermKind::ThrowInst:
  case TermKind::UnwindInst:
    llvm_unreachable("Have terminator that implies no successors?!");
  case TermKind::TryApplyInst:
  case TermKind::SwitchValueInst:
  case TermKind::SwitchEnumAddrInst:
  case TermKind::CheckedCastAddrBranchInst:
  case TermKind::DynamicMethodBranchInst:
  case TermKind::YieldInst:
  case TermKind::AwaitAsyncContinuationInst:
    return SILValue();
  case TermKind::BranchInst:
    return cast<const BranchInst>(predTermInst)->getArg(argIndex);
  case TermKind::CondBranchInst:
    return cast<const CondBranchInst>(predTermInst)
        ->getArgForDestBB(parentBlock, argIndex);
  case TermKind::CheckedCastBranchInst:
    return cast<const CheckedCastBranchInst>(predTermInst)->getOperand();
  case TermKind::SwitchEnumInst:
    return cast<const SwitchEnumInst>(predTermInst)->getOperand();
  }
  llvm_unreachable("Unhandled TermKind?!");
}

bool SILPhiArgument::getSingleTerminatorOperands(
    SmallVectorImpl<SILValue> &returnedSingleTermOperands) const {
  const auto *parentBlock = getParent();

  if (parentBlock->pred_empty())
    return false;

  unsigned argIndex = getIndex();
  for (auto *predBlock : getParent()->getPredecessorBlocks()) {
    SILValue incomingValue =
        getSingleTerminatorOperandForPred(parentBlock, predBlock, argIndex);
    if (!incomingValue)
      return false;
    returnedSingleTermOperands.push_back(incomingValue);
  }

  return true;
}

bool SILPhiArgument::getSingleTerminatorOperands(
    SmallVectorImpl<std::pair<SILBasicBlock *, SILValue>>
        &returnedSingleTermOperands) const {
  const auto *parentBlock = getParent();

  if (parentBlock->pred_empty())
    return false;

  unsigned argIndex = getIndex();
  for (auto *predBlock : getParent()->getPredecessorBlocks()) {
    SILValue incomingValue =
        getSingleTerminatorOperandForPred(parentBlock, predBlock, argIndex);
    if (!incomingValue)
      return false;
    returnedSingleTermOperands.push_back({predBlock, incomingValue});
  }

  return true;
}

TermInst *SILPhiArgument::getSingleTerminator() const {
  auto *parentBlock = getParent();
  auto *predBlock = parentBlock->getSinglePredecessorBlock();
  if (!predBlock)
    return nullptr;
  return const_cast<SILBasicBlock *>(predBlock)->getTerminator();
}

TermInst *SILPhiArgument::getTerminatorForResult() const {
  if (auto *termInst = getSingleTerminator()) {
    if (!swift::isa<BranchInst>(termInst)
        && !swift::isa<CondBranchInst>(termInst)) {
      return termInst;
    }
  }
  return nullptr;
}

Operand *SILArgument::forwardedTerminatorResultOperand() const {
  assert(isTerminatorResult() && "API is invalid for phis");
  return getSingleTerminator()->forwardedOperand();
}

SILPhiArgument *BranchInst::getArgForOperand(const Operand *oper) {
  assert(oper->getUser() == this);
  return cast<SILPhiArgument>(
      getDestBB()->getArgument(oper->getOperandNumber()));
}

const SILPhiArgument *
CondBranchInst::getArgForOperand(const Operand *oper) const {
  assert(oper->getUser() == this);

  unsigned operIdx = oper->getOperandNumber();
  if (isTrueOperandIndex(operIdx)) {
    return cast<SILPhiArgument>(getTrueBB()->getArgument(
        operIdx - getTrueOperands().front().getOperandNumber()));
  }
  if (isFalseOperandIndex(operIdx)) {
    return cast<SILPhiArgument>(getFalseBB()->getArgument(
        operIdx - getFalseOperands().front().getOperandNumber()));
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
//                            SILFunctionArgument
//===----------------------------------------------------------------------===//

bool SILFunctionArgument::isSelf() const {
  // Return true if we are the last argument of our BB and that our parent
  // function has a call signature with self.
  return getFunction()->hasSelfParam() &&
         getParent()->getArguments().back() == this;
}
