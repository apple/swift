//===--- SILValue.cpp - Implementation for SILValue -----------------------===//
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

#include "swift/SIL/SILValue.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuiltinVisitor.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILVisitor.h"
#include "llvm/ADT/StringSwitch.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                       Check SILNode Type Properties
//===----------------------------------------------------------------------===//

/// These are just for performance and verification. If one needs to make
/// changes that cause the asserts the fire, please update them. The purpose is
/// to prevent these predicates from changing values by mistake.

//===----------------------------------------------------------------------===//
//                       Check SILValue Type Properties
//===----------------------------------------------------------------------===//

/// These are just for performance and verification. If one needs to make
/// changes that cause the asserts the fire, please update them. The purpose is
/// to prevent these predicates from changing values by mistake.
static_assert(std::is_standard_layout<SILValue>::value,
              "Expected SILValue to be standard layout");
static_assert(sizeof(SILValue) == sizeof(uintptr_t),
              "SILValue should be pointer sized");

//===----------------------------------------------------------------------===//
//                              Utility Methods
//===----------------------------------------------------------------------===//

void ValueBase::replaceAllUsesWith(ValueBase *RHS) {
  assert(this != RHS && "Cannot RAUW a value with itself");
  while (!use_empty()) {
    Operand *Op = *use_begin();
    Op->set(RHS);
  }
}

void ValueBase::replaceAllUsesWithUndef() {
  auto *F = getFunction();
  if (!F) {
    llvm_unreachable("replaceAllUsesWithUndef can only be used on ValueBase "
                     "that have access to the parent function.");
  }
  while (!use_empty()) {
    Operand *Op = *use_begin();
    Op->set(SILUndef::get(Op->get()->getType(), *F));
  }
}

void ValueBase::replaceAllTypeDependentUsesWith(ValueBase *RHS) {
  SmallVector<Operand *, 4> typeUses(getTypeDependentUses());
  for (Operand *use : typeUses) {
    use->set(RHS);
  }
}

SILInstruction *ValueBase::getDefiningInstruction() {
  if (auto *inst = dyn_cast<SingleValueInstruction>(this))
    return inst;
  if (auto *result = dyn_cast<MultipleValueInstructionResult>(this))
    return result->getParent();
  return nullptr;
}

SILInstruction *ValueBase::getDefiningInstructionOrTerminator() {
  if (auto *inst = dyn_cast<SingleValueInstruction>(this))
    return inst;
  if (auto *result = dyn_cast<MultipleValueInstructionResult>(this))
    return result->getParent();
  if (auto *result = SILArgument::isTerminatorResult(this))
    return result->getSingleTerminator();
  return nullptr;
}

SILInstruction *ValueBase::getDefiningInsertionPoint() {
  if (auto *inst = getDefiningInstruction())
    return inst;
  if (auto *arg = dyn_cast<SILArgument>(this))
    return &*arg->getParentBlock()->begin();
  return nullptr;
}

SILInstruction *ValueBase::getNextInstruction() {
  if (auto *inst = getDefiningInstruction())
    return &*std::next(inst->getIterator());
  if (auto *arg = dyn_cast<SILArgument>(this))
    return &*arg->getParentBlock()->begin();
  return nullptr;
}

Optional<ValueBase::DefiningInstructionResult>
ValueBase::getDefiningInstructionResult() {
  if (auto *inst = dyn_cast<SingleValueInstruction>(this))
    return DefiningInstructionResult{inst, 0};
  if (auto *result = dyn_cast<MultipleValueInstructionResult>(this))
    return DefiningInstructionResult{result->getParent(), result->getIndex()};
  return None;
}

bool SILPhiArgument::isLexical() const {
  if (!isPhi())
    return false;

  // FIXME: Cache this on the node.

  // Does there exist an incoming value which is lexical?
  //
  // Invert the condition to "is every incoming value non-lexical?" in order to
  // stop visiting incoming values once one lexical value is
  // found--visitTransitiveIncomingPhiOperands stops once false is returned
  // from it.
  auto isEveryIncomingValueNonLexical =
      visitTransitiveIncomingPhiOperands([&](auto *, auto *operand) {
        auto value = operand->get();
        SILPhiArgument *phi = dyn_cast<SILPhiArgument>(value);
        if (phi && phi->isPhi()) {
          return true;
        }
        // If this non-phi incoming value is lexical, then there is one at least
        // one lexical value incoming to this phi, to it's lexical.
        return !value->isLexical();
      });
  return !isEveryIncomingValueNonLexical;
}

bool ValueBase::isLexical() const {
  if (auto *argument = dyn_cast<SILFunctionArgument>(this))
    return argument->getLifetime().isLexical();
  auto *phi = dyn_cast<SILPhiArgument>(this);
  if (phi && phi->isPhi())
    return phi->isLexical();
  if (auto *bbi = dyn_cast<BeginBorrowInst>(this))
    return bbi->isLexical();
  if (auto *mvi = dyn_cast<MoveValueInst>(this))
    return mvi->isLexical();
  return false;
}

bool ValueBase::isGuaranteedForwarding() const {
  if (getOwnershipKind() != OwnershipKind::Guaranteed) {
    return false;
  }
  // NOTE: canOpcodeForwardInnerGuaranteedValues returns true for transformation
  // terminator results.
  if (canOpcodeForwardInnerGuaranteedValues(this) ||
      isa<SILFunctionArgument>(this)) {
    return true;
  }
  // If not a phi, return false
  auto *phi = dyn_cast<SILPhiArgument>(this);
  if (!phi || !phi->isPhi()) {
    return false;
  }

  return phi->isGuaranteedForwarding();
}

bool ValueBase::hasDebugTrace() const {
  for (auto *op : getUses()) {
    if (auto *debugValue = dyn_cast<DebugValueInst>(op->getUser())) {
      if (debugValue->hasTrace())
        return true;
    }
  }
  return false;
}

SILBasicBlock *SILNode::getParentBlock() const {
  if (auto *Inst = dyn_cast<SILInstruction>(this))
    return Inst->getParent();
  if (auto *Arg = dyn_cast<SILArgument>(this))
    return Arg->getParent();
  if (auto *MVR = dyn_cast<MultipleValueInstructionResult>(this)) {
    return MVR->getParent()->getParent();
  }
  return nullptr;
}

SILFunction *SILNode::getFunction() const {
  if (auto *parentBlock = getParentBlock())
    return parentBlock->getParent();
  return nullptr;
}

SILModule *SILNode::getModule() const {
  if (SILFunction *func = getFunction())
    return &func->getModule();
  return nullptr;
}

/// Get a location for this value.
SILLocation SILValue::getLoc() const {
  if (auto *instr = Value->getDefiningInstruction())
    return instr->getLoc();

  if (auto *arg = dyn_cast<SILArgument>(*this)) {
    if (arg->getDecl())
      return RegularLocation(const_cast<ValueDecl *>(arg->getDecl()));
  }
  // TODO: bbargs should probably use one of their operand locations.
  return Value->getFunction()->getLocation();
}

void SILValue::dump() const {
  Value->dump();
}

//===----------------------------------------------------------------------===//
//                               OwnershipKind
//===----------------------------------------------------------------------===//

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &os,
                                     const OwnershipKind &kind) {
  return os << kind.asString();
}

StringRef OwnershipKind::asString() const {
  switch (value) {
  case OwnershipKind::Any:
    return "any";
  case OwnershipKind::Unowned:
    return "unowned";
  case OwnershipKind::Owned:
    return "owned";
  case OwnershipKind::Guaranteed:
    return "guaranteed";
  case OwnershipKind::None:
    return "none";
  }
  llvm_unreachable("covered switch");
}

//===----------------------------------------------------------------------===//
//                             ValueOwnershipKind
//===----------------------------------------------------------------------===//

ValueOwnershipKind::ValueOwnershipKind(const SILFunction &F, SILType Type,
                                       SILArgumentConvention Convention)
    : ValueOwnershipKind(F, Type, Convention,
                         SILModuleConventions(F.getModule())) {}

ValueOwnershipKind::ValueOwnershipKind(const SILFunction &F, SILType Type,
                                       SILArgumentConvention Convention,
                                       SILModuleConventions moduleConventions)
    : value(OwnershipKind::Any) {
  // Trivial types can be passed using a variety of conventions. They always
  // have trivial ownership.
  if (Type.isTrivial(F)) {
    value = OwnershipKind::None;
    return;
  }

  switch (Convention) {
  case SILArgumentConvention::Indirect_In:
    value = moduleConventions.useLoweredAddresses() ? OwnershipKind::None
                                                    : OwnershipKind::Owned;
    break;
  case SILArgumentConvention::Indirect_In_Guaranteed:
    value = moduleConventions.useLoweredAddresses() ? OwnershipKind::None
                                                    : OwnershipKind::Guaranteed;
    break;
  case SILArgumentConvention::Indirect_Inout:
  case SILArgumentConvention::Indirect_InoutAliasable:
  case SILArgumentConvention::Indirect_Out:
  case SILArgumentConvention::Pack_Inout:
  case SILArgumentConvention::Pack_Out:
  case SILArgumentConvention::Pack_Owned:
  case SILArgumentConvention::Pack_Guaranteed:
    value = OwnershipKind::None;
    return;
  case SILArgumentConvention::Direct_Owned:
    value = OwnershipKind::Owned;
    return;
  case SILArgumentConvention::Direct_Unowned:
    value = OwnershipKind::Unowned;
    return;
  case SILArgumentConvention::Direct_Guaranteed:
    value = OwnershipKind::Guaranteed;
    return;
  }
}

StringRef ValueOwnershipKind::asString() const {
  return value.asString();
}

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &os,
                                     ValueOwnershipKind kind) {
  return os << kind.asString();
}

ValueOwnershipKind::ValueOwnershipKind(StringRef S)
    : value(OwnershipKind::Any) {
  auto Result = llvm::StringSwitch<Optional<OwnershipKind::innerty>>(S)
                    .Case("unowned", OwnershipKind::Unowned)
                    .Case("owned", OwnershipKind::Owned)
                    .Case("guaranteed", OwnershipKind::Guaranteed)
                    .Case("none", OwnershipKind::None)
                    .Default(None);
  if (!Result.has_value())
    llvm_unreachable("Invalid string representation of ValueOwnershipKind");
  value = Result.value();
}

ValueOwnershipKind
ValueOwnershipKind::getProjectedOwnershipKind(const SILFunction &F,
                                              SILType Proj) const {
  if (Proj.isTrivial(F))
    return OwnershipKind::None;
  return *this;
}

#if 0
/// Map a SILValue mnemonic name to its ValueKind.
ValueKind swift::getSILValueKind(StringRef Name) {
#define SINGLE_VALUE_INST(Id, TextualName, Parent, MemoryBehavior,             \
                          ReleasingBehavior)                                   \
  if (Name == #TextualName)                                                    \
    return ValueKind::Id;

#define VALUE(Id, Parent)                                                      \
  if (Name == #Id)                                                             \
    return ValueKind::Id;

#include "swift/SIL/SILNodes.def"

#ifdef NDEBUG
  llvm::errs()
    << "Unknown SILValue name\n";
  abort();
#endif
  llvm_unreachable("Unknown SILValue name");
}

/// Map ValueKind to a corresponding mnemonic name.
StringRef swift::getSILValueName(ValueKind Kind) {
  switch (Kind) {
#define SINGLE_VALUE_INST(Id, TextualName, Parent, MemoryBehavior,             \
                          ReleasingBehavior)                                   \
  case ValueKind::Id:                                                          \
    return #TextualName;

#define VALUE(Id, Parent)                                                      \
  case ValueKind::Id:                                                          \
    return #Id;

#include "swift/SIL/SILNodes.def"
  }
}
#endif

//===----------------------------------------------------------------------===//
//                           UseLifetimeConstraint
//===----------------------------------------------------------------------===//

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &os,
                                     UseLifetimeConstraint constraint) {
  switch (constraint) {
  case UseLifetimeConstraint::NonLifetimeEnding:
    os << "NonLifetimeEnding";
    break;
  case UseLifetimeConstraint::LifetimeEnding:
    os << "LifetimeEnding";
    break;
  }
  return os;
}

//===----------------------------------------------------------------------===//
//                                  Operand
//===----------------------------------------------------------------------===//

SILBasicBlock *Operand::getParentBlock() const {
  auto *self = const_cast<Operand *>(this);
  return self->getUser()->getParent();
}

SILFunction *Operand::getParentFunction() const {
  auto *self = const_cast<Operand *>(this);
  return self->getUser()->getFunction();
}

bool Operand::canAcceptKind(ValueOwnershipKind kind,
                            SILModuleConventions *silConv) const {
  auto operandOwnership = getOperandOwnership(silConv);
  auto constraint = operandOwnership.getOwnershipConstraint();
  if (constraint.satisfiesConstraint(kind)) {
    // Constraints aren't precise enough to enforce Unowned value uses.
    if (kind == OwnershipKind::Unowned) {
      return canAcceptUnownedValue(operandOwnership);
    }
    return true;
  }

  return false;
}

bool Operand::satisfiesConstraints(SILModuleConventions *silConv) const {
  return canAcceptKind(get()->getOwnershipKind(), silConv);
}

bool Operand::isLifetimeEnding() const {
  auto constraint = getOwnershipConstraint();

  // If our use lifetime constraint is NonLifetimeEnding, just return false.
  if (!constraint.isLifetimeEnding())
    return false;

  // Otherwise, we may have a lifetime ending use. We consider two cases here:
  // the case where our value has OwnershipKind::None and one where it has some
  // other OwnershipKind. Note that values with OwnershipKind::None ownership
  // can not have their lifetime ended since they are outside of the ownership
  // system. Given such a case, if we have such a value we return
  // isLifetimeEnding() as false even if the constraint itself has a constraint
  // that says a value is LifetimeEnding. If we have a value that has a
  // non-OwnershipKind::None ownership then we just return true as expected.
  return get()->getOwnershipKind() != OwnershipKind::None;
}

bool Operand::isConsuming() const {
  if (!getOwnershipConstraint().isConsuming())
    return false;

  return get()->getOwnershipKind() != OwnershipKind::None;
}

void Operand::dump() const { print(llvm::dbgs()); }

void Operand::print(llvm::raw_ostream &os) const {
  os << "Operand.\n"
        "Owner: "
     << *Owner << "Value: " << get() << "Operand Number: " << getOperandNumber()
     << '\n'
     << "Is Type Dependent: " << (isTypeDependent() ? "yes" : "no") << '\n';
}

//===----------------------------------------------------------------------===//
//                             OperandConstraint
//===----------------------------------------------------------------------===//

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &os,
                                     OwnershipConstraint constraint) {
  return os << "<Constraint "
               "Kind:" << constraint.getPreferredKind()
            << " LifetimeConstraint:" << constraint.getLifetimeConstraint()
            << ">";
}

StringRef OperandOwnership::asString() const {
  switch (value) {
  case OperandOwnership::NonUse:
    return "non-use";
  case OperandOwnership::TrivialUse:
    return "trivial-use";
  case OperandOwnership::InstantaneousUse:
    return "instantaneous";
  case OperandOwnership::UnownedInstantaneousUse:
    return "unowned-instantaneous";
  case OperandOwnership::ForwardingUnowned:
    return "forwarding-unowned";
  case OperandOwnership::PointerEscape:
    return "pointer-escape";
  case OperandOwnership::BitwiseEscape:
    return "bitwise-escape";
  case OperandOwnership::Borrow:
    return "borrow";
  case OperandOwnership::DestroyingConsume:
    return "destroying-consume";
  case OperandOwnership::ForwardingConsume:
    return "forwarding-consume";
  case OperandOwnership::InteriorPointer:
    return "interior-pointer";
  case OperandOwnership::GuaranteedForwarding:
    return "guaranteed-forwarding";
  case OperandOwnership::EndBorrow:
    return "end-borrow";
  case OperandOwnership::Reborrow:
    return "reborrow";
  }
  llvm_unreachable("covered switch");
}

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &os,
                                     const OperandOwnership &operandOwnership) {
  return os << operandOwnership.asString();
}

//===----------------------------------------------------------------------===//
//                              PlaceholderValue
//===----------------------------------------------------------------------===//

int PlaceholderValue::numPlaceholderValuesAlive = 0;

PlaceholderValue::PlaceholderValue(SILType type)
      : ValueBase(ValueKind::PlaceholderValue, type) {
  numPlaceholderValuesAlive++;
}

PlaceholderValue::~PlaceholderValue() {
  numPlaceholderValuesAlive--;
}

