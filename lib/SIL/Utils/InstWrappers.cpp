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
  case SILInstructionKind::DestructureTupleInst:
  case SILInstructionKind::DestructureStructInst:
  case SILInstructionKind::InitExistentialRefInst:
  case SILInstructionKind::ObjectInst:
  case SILInstructionKind::OpenExistentialBoxValueInst:
  case SILInstructionKind::OpenExistentialRefInst:
  case SILInstructionKind::OpenExistentialValueInst:
  case SILInstructionKind::MarkMustCheckInst:
  case SILInstructionKind::MarkUninitializedInst:
  case SILInstructionKind::SelectEnumInst:
  case SILInstructionKind::StructExtractInst:
  case SILInstructionKind::TupleExtractInst:
    return true;
  }
}

bool ForwardingOperation::isAddressOnly() const {
  if (canForwardAllOperands()) {
    // All ForwardingInstructions that forward all operands are currently a
    // single value instruction.
    auto *aggregate =
        cast<OwnershipForwardingSingleValueInstruction>(forwardingInst);
    // If any of the operands are address-only, then the aggregate must be.
    return aggregate->getType().isAddressOnly(*forwardingInst->getFunction());
  }
  return forwardingInst->getOperand(0)->getType().isAddressOnly(
      *forwardingInst->getFunction());
}
