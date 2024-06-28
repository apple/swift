//===--- InstWrappers.cpp ------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/SIL/InstWrappers.h"
#include "swift/SIL/SILFunction.h"

using namespace swift;

ForwardingOperation::ForwardingOperation(SILInstruction *inst) {
  if (ForwardingInstruction::isa(inst)) {
    this->forwardingInst = inst;
    return;
  }
}

ValueOwnershipKind ForwardingOperation::getForwardingOwnershipKind() {
  return ForwardingInstruction::get(forwardingInst)
      ->getForwardingOwnershipKind();
}

bool ForwardingOperation::preservesOwnership() {
  return ForwardingInstruction::get(forwardingInst)->preservesOwnership();
}

// See ForwardingInstruction.swift preservesRepresentation().
bool ForwardingOperation::hasSameRepresentation() const {
  switch (forwardingInst->getKind()) {
  // Explicitly list instructions which definitely involve a representation
  // change.
  case SILInstructionKind::SwitchEnumInst:
  default:
    // Conservatively assume that a conversion changes representation.
    // Operations can be added as needed to participate in SIL opaque values.
    return false;

  case SILInstructionKind::ConvertFunctionInst:
  case SILInstructionKind::CopyableToMoveOnlyWrapperValueInst:
  case SILInstructionKind::DestructureTupleInst:
  case SILInstructionKind::DestructureStructInst:
  case SILInstructionKind::InitExistentialRefInst:
  case SILInstructionKind::ObjectInst:
  case SILInstructionKind::OpenExistentialBoxValueInst:
  case SILInstructionKind::OpenExistentialRefInst:
  case SILInstructionKind::OpenExistentialValueInst:
  case SILInstructionKind::MarkUnresolvedNonCopyableValueInst:
  case SILInstructionKind::MoveOnlyWrapperToCopyableValueInst:
  case SILInstructionKind::MarkUninitializedInst:
  case SILInstructionKind::StructExtractInst:
  case SILInstructionKind::TupleExtractInst:
  case SILInstructionKind::TuplePackExtractInst:
    return true;
  }
}

bool ForwardingOperation::isAddressOnly() const {
  if (auto *singleForwardingOp = getSingleForwardingOperand()) {
    return singleForwardingOp->get()->getType().isAddressOnly(
        *forwardingInst->getFunction());
  }
  // All ForwardingInstructions that forward all operands or no operands are
  // currently a single value instruction.
  auto *aggregate =
      cast<OwnershipForwardingSingleValueInstruction>(forwardingInst);
  // If any of the operands are address-only, then the aggregate must be.
  return aggregate->getType().isAddressOnly(*forwardingInst->getFunction());
}

bool ForwardingOperation::visitForwardedValues(
    function_ref<bool(SILValue)> visitor) {
  if (auto *svi = dyn_cast<SingleValueInstruction>(forwardingInst)) {
    return visitor(svi);
  }
  if (auto *mvri = dyn_cast<MultipleValueInstruction>(forwardingInst)) {
    return llvm::all_of(mvri->getResults(), [&](SILValue value) {
      if (value->getOwnershipKind() == OwnershipKind::None)
        return true;
      return visitor(value);
    });
  }
  auto *ti = cast<TermInst>(forwardingInst);
  assert(ti->mayHaveTerminatorResult());
  return llvm::all_of(ti->getSuccessorBlocks(), [&](SILBasicBlock *succBlock) {
    // If we do not have any arguments, then continue.
    if (succBlock->args_empty())
      return true;

    auto args = succBlock->getSILPhiArguments();
    assert(args.size() == 1 && "Transforming terminator with multiple args?!");
    return visitor(args[0]);
  });
}
