//===--- InstructionUtils.h - Utilities for SIL instructions ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_WRAPPERTYPES_H
#define SWIFT_SIL_WRAPPERTYPES_H

#include "swift/SIL/SILInstruction.h"

namespace swift {
/// An abstraction over LoadInst/LoadBorrowInst so one can handle both types of
/// load using common code.
struct LoadOperation {
  llvm::PointerUnion<LoadInst *, LoadBorrowInst *> value;

  LoadOperation() : value() {}
  LoadOperation(SILInstruction *input) : value(nullptr) {
    if (auto *li = dyn_cast<LoadInst>(input)) {
      value = li;
      return;
    }

    if (auto *lbi = dyn_cast<LoadBorrowInst>(input)) {
      value = lbi;
      return;
    }
  }

  explicit operator bool() const { return !value.isNull(); }

  SingleValueInstruction *getLoadInst() const {
    if (value.is<LoadInst *>())
      return value.get<LoadInst *>();
    return value.get<LoadBorrowInst *>();
  }

  SingleValueInstruction *operator*() const { return getLoadInst(); }

  const SingleValueInstruction *operator->() const { return getLoadInst(); }

  SingleValueInstruction *operator->() { return getLoadInst(); }

  SILValue getOperand() const {
    if (value.is<LoadInst *>())
      return value.get<LoadInst *>()->getOperand();
    return value.get<LoadBorrowInst *>()->getOperand();
  }

  /// Return the ownership qualifier of the underlying load if we have a load or
  /// None if we have a load_borrow.
  ///
  /// TODO: Rather than use an optional here, we should include an invalid
  /// representation in LoadOwnershipQualifier.
  llvm::Optional<LoadOwnershipQualifier> getOwnershipQualifier() const {
    if (auto *lbi = value.dyn_cast<LoadBorrowInst *>()) {
      return llvm::None;
    }

    return value.get<LoadInst *>()->getOwnershipQualifier();
  }
};

/// A wrapper type for writing generic code against conversion instructions.
struct ConversionOperation {
  SingleValueInstruction *inst = nullptr;

  ConversionOperation() = default;

  explicit ConversionOperation(SILInstruction *inst) {
    auto *svi = dyn_cast<SingleValueInstruction>(inst);
    if (!svi) {
      return;
    }
    if (!ConversionOperation::isa(svi)) {
      return;
    }
    this->inst = svi;
  }

  explicit ConversionOperation(SILValue value) {
    auto *inst = value->getDefiningInstruction();
    if (!inst) {
      return;
    }
    auto *svi = dyn_cast<SingleValueInstruction>(inst);
    if (!svi) {
      return;
    }
    if (!ConversionOperation::isa(svi)) {
      return;
    }
    this->inst = svi;
  }

  operator bool() const { return inst != nullptr; }

  SingleValueInstruction *operator->() { return inst; }
  SingleValueInstruction *operator->() const { return inst; }
  SingleValueInstruction *operator*() { return inst; }
  SingleValueInstruction *operator*() const { return inst; }

  static bool isa(SILInstruction *inst) {
    switch (inst->getKind()) {
    case SILInstructionKind::ConvertFunctionInst:
    case SILInstructionKind::UpcastInst:
    case SILInstructionKind::AddressToPointerInst:
    case SILInstructionKind::UncheckedTrivialBitCastInst:
    case SILInstructionKind::UncheckedAddrCastInst:
    case SILInstructionKind::UncheckedBitwiseCastInst:
    case SILInstructionKind::RefToRawPointerInst:
    case SILInstructionKind::RawPointerToRefInst:
    case SILInstructionKind::ConvertEscapeToNoEscapeInst:
    case SILInstructionKind::RefToBridgeObjectInst:
    case SILInstructionKind::BridgeObjectToRefInst:
    case SILInstructionKind::BridgeObjectToWordInst:
    case SILInstructionKind::ThinToThickFunctionInst:
    case SILInstructionKind::ThickToObjCMetatypeInst:
    case SILInstructionKind::ObjCToThickMetatypeInst:
    case SILInstructionKind::ObjCMetatypeToObjectInst:
    case SILInstructionKind::ObjCExistentialMetatypeToObjectInst:
    case SILInstructionKind::UnconditionalCheckedCastInst:
    case SILInstructionKind::UncheckedRefCastInst:
    case SILInstructionKind::UncheckedValueCastInst:
    case SILInstructionKind::RefToUnmanagedInst:
    case SILInstructionKind::RefToUnownedInst:
    case SILInstructionKind::UnmanagedToRefInst:
    case SILInstructionKind::UnownedToRefInst:
      return true;
    default:
      return false;
    }
  }

  SILValue getConverted() { return inst->getOperand(0); }
};

/// A wrapper type for writing generic code against SelectEnumAddrInst and
/// SelectEnumInst.
///
/// We use this instead of SelectEnumInstBase in order to avoid the need for
/// templating SelectEnumInstBase.
class SelectEnumOperation {
  PointerUnion<SelectEnumAddrInst *, SelectEnumInst *> value;

public:
  SelectEnumOperation(SelectEnumAddrInst *seai) : value(seai) {}
  SelectEnumOperation(SelectEnumInst *seai) : value(seai) {}
  SelectEnumOperation(SILInstruction *i) : value(nullptr) {
    if (auto *seai = dyn_cast<SelectEnumAddrInst>(i)) {
      value = seai;
      return;
    }

    if (auto *sei = dyn_cast<SelectEnumInst>(i)) {
      value = sei;
      return;
    }
  }

  SelectEnumOperation(const SILInstruction *i)
      : SelectEnumOperation(const_cast<SILInstruction *>(i)) {}

  operator SingleValueInstruction *() const {
    if (auto *seai = value.dyn_cast<SelectEnumAddrInst *>())
      return seai;
    return value.get<SelectEnumInst *>();
  }

  SingleValueInstruction *operator*() const {
    if (auto *seai = value.dyn_cast<SelectEnumAddrInst *>())
      return seai;
    return value.get<SelectEnumInst *>();
  }

  SingleValueInstruction *operator->() const {
    if (auto *seai = value.dyn_cast<SelectEnumAddrInst *>())
      return seai;
    return value.get<SelectEnumInst *>();
  }

  operator bool() const { return bool(value); }

  SILValue getOperand() {
    if (auto *sei = value.dyn_cast<SelectEnumInst *>())
      return sei->getOperand();
    return value.get<SelectEnumAddrInst *>()->getOperand();
  }

  SILValue getEnumOperand() { return getOperand(); }

  const Operand &getEnumOperandRef() {
    if (auto *sei = value.dyn_cast<SelectEnumInst *>())
      return sei->getEnumOperandRef();
    return value.get<SelectEnumAddrInst *>()->getEnumOperandRef();
  }

  unsigned getNumCases() const {
    if (auto *sei = value.dyn_cast<SelectEnumInst *>())
      return sei->getNumCases();
    return value.get<SelectEnumAddrInst *>()->getNumCases();
  }

  std::pair<EnumElementDecl *, SILValue> getCase(unsigned i) const {
    if (auto *sei = value.dyn_cast<SelectEnumInst *>())
      return sei->getCase(i);
    return value.get<SelectEnumAddrInst *>()->getCase(i);
  }
  /// Return the value that will be used as the result for the specified enum
  /// case.
  SILValue getCaseResult(EnumElementDecl *D) {
    if (auto *sei = value.dyn_cast<SelectEnumInst *>())
      return sei->getCaseResult(D);
    return value.get<SelectEnumAddrInst *>()->getCaseResult(D);
  }

  /// If the default refers to exactly one case decl, return it.
  NullablePtr<EnumElementDecl> getUniqueCaseForDefault();

  bool hasDefault() const {
    if (auto *sei = value.dyn_cast<SelectEnumInst *>())
      return sei->hasDefault();
    return value.get<SelectEnumAddrInst *>()->hasDefault();
  }

  SILValue getDefaultResult() const {
    if (auto *sei = value.dyn_cast<SelectEnumInst *>())
      return sei->getDefaultResult();
    return value.get<SelectEnumAddrInst *>()->getDefaultResult();
  }
};

class ForwardingOperation {
  SILInstruction *forwardingInst = nullptr;

public:
  explicit ForwardingOperation(SILInstruction *inst);

  operator bool() const { return bool(forwardingInst); }
  const SILInstruction *operator->() const { return forwardingInst; }
  SILInstruction *operator->() { return forwardingInst; }
  const SILInstruction *operator*() const { return forwardingInst; }
  SILInstruction *operator*() { return forwardingInst; }

  ValueOwnershipKind getForwardingOwnershipKind();
  bool preservesOwnership();

  // FIXME: Find a better name. Even unary instructions like struct_extract
  // forward "all" operands.
  bool canForwardAllOperands() const {
    switch (forwardingInst->getKind()) {
    case SILInstructionKind::StructInst:
    case SILInstructionKind::TupleInst:
    case SILInstructionKind::LinearFunctionInst:
    case SILInstructionKind::DifferentiableFunctionInst:
      return true;
    default:
      return false;
    }
  }

  // FIXME: Find a better name. Even instructions that forward all operands can
  // forward the first operand.
  bool canForwardFirstOperandOnly() const {
    return !canForwardAllOperands() && forwardingInst->getNumRealOperands() > 0;
  }

  ArrayRef<Operand> getForwardedOperands() const {
    if (canForwardAllOperands()) {
      return forwardingInst->getAllOperands();
    }
    if (canForwardFirstOperandOnly()) {
      return forwardingInst->getOperandRef(0);
    }
    return {};
  }

  MutableArrayRef<Operand> getForwardedOperands() {
    if (canForwardAllOperands()) {
      return forwardingInst->getAllOperands();
    }
    if (canForwardFirstOperandOnly()) {
      return forwardingInst->getOperandRef(0);
    }
    return {};
  }

  bool canForwardOwnedCompatibleValuesOnly() {
    switch (forwardingInst->getKind()) {
    case SILInstructionKind::MarkUninitializedInst:
      return true;
    default:
      return false;
    }
  }

  bool canForwardGuaranteedCompatibleValuesOnly() {
    switch (forwardingInst->getKind()) {
    case SILInstructionKind::TupleExtractInst:
    case SILInstructionKind::StructExtractInst:
    case SILInstructionKind::DifferentiableFunctionExtractInst:
    case SILInstructionKind::LinearFunctionExtractInst:
      return true;
    default:
      return false;
    }
  }

  /// Return true if the forwarded value has the same representation. If true,
  /// then the result can be mapped to the same storage without a move or copy.
  bool hasSameRepresentation() const;

  /// Return true if the forwarded value is address-only either before or after
  /// forwarding.
  bool isAddressOnly() const;
};
} // end namespace swift

#endif
