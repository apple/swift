//===--- OwnershipPhiOperand.h --------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_SEMANTICARC_OWNERSHIPPHIOPERAND_H
#define SWIFT_SILOPTIMIZER_SEMANTICARC_OWNERSHIPPHIOPERAND_H

#include "swift/Basic/STLExtras.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/SILValue.h"

namespace swift {
namespace semanticarc {

/// The operand of a "phi" in the induced ownership graph of a def-use graph.
///
/// Some examples: br, struct, tuple.
class LLVM_LIBRARY_VISIBILITY OwnershipPhiOperand {
public:
  enum Kind {
    Branch,
    Struct,
    Tuple,
  };

private:
  Operand *op;

  OwnershipPhiOperand(Operand *op) : op(op) {}

public:
  static llvm::Optional<OwnershipPhiOperand> get(const Operand *op) {
    switch (op->getUser()->getKind()) {
    case SILInstructionKind::BranchInst:
    case SILInstructionKind::StructInst:
    case SILInstructionKind::TupleInst:
      return {{const_cast<Operand *>(op)}};
    default:
      return llvm::None;
    }
  }

  Kind getKind() const {
    switch (op->getUser()->getKind()) {
    case SILInstructionKind::BranchInst:
      return Kind::Branch;
    case SILInstructionKind::StructInst:
      return Kind::Struct;
    case SILInstructionKind::TupleInst:
      return Kind::Tuple;
    default:
      llvm_unreachable("unhandled case?!");
    }
  }

  operator const Operand *() const { return op; }
  operator Operand *() { return op; }

  Operand *getOperand() const { return op; }
  SILValue getValue() const { return op->get(); }
  SILType getType() const { return op->get()->getType(); }

  unsigned getOperandNumber() const { return op->getOperandNumber(); }

  void markUndef() & {
    op->set(SILUndef::get(getType(), *op->getUser()->getFunction()));
  }

  SILInstruction *getInst() const { return op->getUser(); }

  /// Return true if this phi consumes a borrow.
  ///
  /// If so, we may need to insert an extra begin_borrow to balance the +1 when
  /// converting owned ownership phis to guaranteed ownership phis.
  bool isGuaranteedConsuming() const {
    switch (getKind()) {
    case Kind::Branch:
      return true;
    case Kind::Tuple:
    case Kind::Struct:
      return false;
    }
    llvm_unreachable("unhandled operand kind!");
  }

  bool operator<(const OwnershipPhiOperand &other) const {
    return op < other.op;
  }

  bool operator==(const OwnershipPhiOperand &other) const {
    return op == other.op;
  }

  bool visitResults(function_ref<bool(SILValue)> visitor) const {
    switch (getKind()) {
    case Kind::Struct:
      return visitor(cast<StructInst>(getInst()));
    case Kind::Tuple:
      return visitor(cast<TupleInst>(getInst()));
    case Kind::Branch: {
      auto *br = cast<BranchInst>(getInst());
      unsigned opNum = getOperandNumber();
      return llvm::all_of(
          br->getSuccessorBlocks(), [&](SILBasicBlock *succBlock) {
            return visitor(succBlock->getSILPhiArguments()[opNum]);
          });
    }
    }
    llvm_unreachable("unhandled operand kind!");
  }
};

} // namespace semanticarc
} // namespace swift

#endif // SWIFT_SILOPTIMIZER_SEMANTICARC_OWNERSHIPPHIOPERAND_H
