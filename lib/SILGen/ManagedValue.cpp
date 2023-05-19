//===--- ManagedValue.cpp - Value with cleanup ----------------------------===//
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
//
// A storage structure for holding a destructured rvalue with an optional
// cleanup(s).
// Ownership of the rvalue can be "forwarded" to disable the associated
// cleanup(s).
//
//===----------------------------------------------------------------------===//

#include "ManagedValue.h"
#include "SILGenFunction.h"
using namespace swift;
using namespace Lowering;

ManagedValue ManagedValue::forForwardedRValue(SILGenFunction &SGF,
                                              SILValue value) {
  if (!value)
    return ManagedValue();

  switch (value->getOwnershipKind()) {
  case OwnershipKind::Any:
    llvm_unreachable("Invalid ownership for value");

  case OwnershipKind::Owned:
    return ManagedValue(value, SGF.enterDestroyCleanup(value));

  case OwnershipKind::Guaranteed:
  case OwnershipKind::None:
  case OwnershipKind::Unowned:
    return ManagedValue::forUnmanaged(value);
  }
}

/// Emit a copy of this value with independent ownership.
ManagedValue ManagedValue::copy(SILGenFunction &SGF, SILLocation loc) const {
  auto &lowering = SGF.getTypeLowering(getType());
  if (lowering.isTrivial())
    return *this;

  if (getType().isObject()) {
    return SGF.B.createCopyValue(loc, *this, lowering);
  }

  SILValue buf = SGF.emitTemporaryAllocation(loc, getType());
  SGF.B.createCopyAddr(loc, getValue(), buf, IsNotTake, IsInitialization);
  return SGF.emitManagedRValueWithCleanup(buf, lowering);
}

// Emit an unmanaged copy of this value
// WARNING: Callers of this API should manage the cleanup of this value!
SILValue ManagedValue::unmanagedCopy(SILGenFunction &SGF,
                                         SILLocation loc) const {
  auto &lowering = SGF.getTypeLowering(getType());
  if (lowering.isTrivial())
    return getValue();

  if (getType().isObject()) {
    auto copy = SGF.B.emitCopyValueOperation(loc, getValue());
    return copy;
  }

  SILValue buf = SGF.emitTemporaryAllocation(loc, getType());
  SGF.B.createCopyAddr(loc, getValue(), buf, IsNotTake, IsInitialization);
  return buf;
}

/// Emit a copy of this value with independent ownership.
ManagedValue ManagedValue::formalAccessCopy(SILGenFunction &SGF,
                                            SILLocation loc) {
  assert(SGF.isInFormalEvaluationScope() &&
         "Can only perform a formal access copy in a formal evaluation scope");
  auto &lowering = SGF.getTypeLowering(getType());
  if (lowering.isTrivial())
    return *this;

  if (getType().isObject()) {
    return SGF.B.createFormalAccessCopyValue(loc, *this);
  }

  SILValue buf = SGF.emitTemporaryAllocation(loc, getType());
  return SGF.B.createFormalAccessCopyAddr(loc, *this, buf, IsNotTake,
                                          IsInitialization);
}

/// Store a copy of this value with independent ownership into the given
/// uninitialized address.
void ManagedValue::copyInto(SILGenFunction &SGF, SILLocation loc,
                            SILValue dest) {
  auto &lowering = SGF.getTypeLowering(getType());
  if (lowering.isAddressOnly() && SGF.silConv.useLoweredAddresses()) {
    SGF.B.createCopyAddr(loc, getValue(), dest, IsNotTake, IsInitialization);
    return;
  }

  SILValue copy = lowering.emitCopyValue(SGF.B, loc, getValue());
  lowering.emitStoreOfCopy(SGF.B, loc, copy, dest, IsInitialization);
}

void ManagedValue::copyInto(SILGenFunction &SGF, SILLocation loc,
                            Initialization *dest) {
  dest->copyOrInitValueInto(SGF, loc, *this, /*isInit*/ false);
  dest->finishInitialization(SGF);
}

/// This is the same operation as 'copy', but works on +0 values that don't
/// have cleanups.  It returns a +1 value with one.
ManagedValue ManagedValue::copyUnmanaged(SILGenFunction &SGF, SILLocation loc) {
  if (getType().isObject()) {
    return SGF.B.createCopyValue(loc, *this);
  }

  SILValue result = SGF.emitTemporaryAllocation(loc, getType());
  SGF.B.createCopyAddr(loc, getValue(), result, IsNotTake, IsInitialization);
  return SGF.emitManagedRValueWithCleanup(result);
}

/// This is the same operation as 'copy', but works on +0 values that don't
/// have cleanups.  It returns a +1 value with one.
ManagedValue ManagedValue::formalAccessCopyUnmanaged(SILGenFunction &SGF,
                                                     SILLocation loc) {
  assert(SGF.isInFormalEvaluationScope());

  if (getType().isObject()) {
    return SGF.B.createFormalAccessCopyValue(loc, *this);
  }

  SILValue result = SGF.emitTemporaryAllocation(loc, getType());
  return SGF.B.createFormalAccessCopyAddr(loc, *this, result, IsNotTake,
                                          IsInitialization);
}

/// Disable the cleanup for this value.
void ManagedValue::forwardCleanup(SILGenFunction &SGF) const {
  assert(hasCleanup() && "value doesn't have cleanup!");
  SGF.Cleanups.forwardCleanup(getCleanup());
}

/// Forward this value, deactivating the cleanup and returning the
/// underlying value.
SILValue ManagedValue::forward(SILGenFunction &SGF) const {
  if (hasCleanup())
    forwardCleanup(SGF);
  return getValue();
}

void ManagedValue::forwardInto(SILGenFunction &SGF, SILLocation loc,
                               SILValue address) {
  assert(isPlusOneOrTrivial(SGF));
  auto &addrTL = SGF.getTypeLowering(address->getType());
  SGF.emitSemanticStore(loc, forward(SGF), address, addrTL, IsInitialization);
}

void ManagedValue::assignInto(SILGenFunction &SGF, SILLocation loc,
                              SILValue address) {
  assert(isPlusOneOrTrivial(SGF));
  auto &addrTL = SGF.getTypeLowering(address->getType());
  SGF.emitSemanticStore(loc, forward(SGF), address, addrTL,
                        IsNotInitialization);
}

void ManagedValue::forwardInto(SILGenFunction &SGF, SILLocation loc,
                               Initialization *dest) {
  assert(isPlusOneOrTrivial(SGF));
  dest->copyOrInitValueInto(SGF, loc, *this, /*isInit*/ true);
  dest->finishInitialization(SGF);
}

ManagedValue ManagedValue::borrow(SILGenFunction &SGF, SILLocation loc) const {
  assert(getValue() && "cannot borrow an invalid or in-context value");
  if (isLValue())
    return *this;
  if (getType().isAddress())
    return ManagedValue::forUnmanaged(getValue());
  return SGF.emitManagedBeginBorrow(loc, getValue());
}

ManagedValue ManagedValue::formalAccessBorrow(SILGenFunction &SGF,
                                              SILLocation loc) const {
  assert(SGF.isInFormalEvaluationScope());
  assert(getValue() && "cannot borrow an invalid or in-context value");
  if (isLValue())
    return *this;
  if (getType().isAddress())
    return ManagedValue::forUnmanaged(getValue());
  return SGF.emitFormalEvaluationManagedBeginBorrow(loc, getValue());
}

ManagedValue ManagedValue::materialize(SILGenFunction &SGF,
                                       SILLocation loc) const {
  auto temporary = SGF.emitTemporaryAllocation(loc, getType());
  bool hadCleanup = hasCleanup();

  if (hadCleanup) {
    SGF.B.emitStoreValueOperation(loc, forward(SGF), temporary,
                                  StoreOwnershipQualifier::Init);

    // SEMANTIC SIL TODO: This should really be called a temporary LValue.
    return ManagedValue::forOwnedAddressRValue(
        temporary, SGF.enterDestroyCleanup(temporary));
  }
  auto &lowering = SGF.getTypeLowering(getType());
  if (lowering.isAddressOnly()) {
    assert(!SGF.silConv.useLoweredAddresses());
    auto copy = SGF.B.createCopyValue(loc, getValue());
    SGF.B.emitStoreValueOperation(loc, copy, temporary,
                                  StoreOwnershipQualifier::Init);
    return ManagedValue::forOwnedAddressRValue(
        temporary, SGF.enterDestroyCleanup(temporary));
  }
  // The temporary memory is +0 if the value was.
  auto object = SGF.emitManagedBeginBorrow(loc, getValue());
  auto borrowedAddr =
      SGF.emitManagedStoreBorrow(loc, object.getValue(), temporary);
  return ManagedValue::forBorrowedAddressRValue(borrowedAddr.getValue());
}

ManagedValue ManagedValue::formallyMaterialize(SILGenFunction &SGF,
                                               SILLocation loc) const {
  auto temporary = SGF.emitTemporaryAllocation(loc, getType());
  bool hadCleanup = hasCleanup();
  auto &lowering = SGF.getTypeLowering(getType());

  if (hadCleanup) {
    SGF.B.emitStoreValueOperation(loc, forward(SGF), temporary,
                                  StoreOwnershipQualifier::Init);

    return ManagedValue::forOwnedAddressRValue(
        temporary, SGF.enterDestroyCleanup(temporary));
  }
  if (lowering.isAddressOnly()) {
    assert(!SGF.silConv.useLoweredAddresses());
    auto copy = SGF.B.createCopyValue(loc, getValue());
    SGF.B.emitStoreValueOperation(loc, copy, temporary,
                                  StoreOwnershipQualifier::Init);
    return ManagedValue::forOwnedAddressRValue(
        temporary, SGF.enterDestroyCleanup(temporary));
  }
  auto object = SGF.emitFormalEvaluationManagedBeginBorrow(loc, getValue());
  return SGF.emitFormalEvaluationManagedStoreBorrow(loc, object.getValue(),
                                                    temporary);
}

void ManagedValue::print(raw_ostream &os) const {
  if (SILValue v = getValue()) {
    v->print(os);
  }
}

void ManagedValue::dump() const {
  dump(llvm::errs());
}

void ManagedValue::dump(raw_ostream &os, unsigned indent) const {
  os.indent(indent);
  if (isInContext()) {
    os << "InContext\n";
    return;
  }
  if (isLValue()) os << "[lvalue] ";
  if (hasCleanup()) os << "[cleanup] ";
  if (SILValue v = getValue()) {
    v->print(os);
  } else {
    os << "<null>\n";
  }
}

ManagedValue ManagedValue::ensurePlusOne(SILGenFunction &SGF,
                                         SILLocation loc) const {
  // Undef can pair with any type of ownership, so it is effectively a +1 value.
  if (isa<SILUndef>(getValue()))
    return *this;

  if (!isPlusOneOrTrivial(SGF)) {
    return copy(SGF, loc);
  }
  return *this;
}

bool ManagedValue::isPlusOne(SILGenFunction &SGF) const {
  // If this value is SILUndef, return true. SILUndef can always be passed to +1
  // APIs.
  if (isa<SILUndef>(getValue()))
    return true;

  // A value without ownership can always be passed to +1 APIs.
  //
  // This is not true for address types because deinitializing an in-memory
  // value invalidates the storage.
  if (getType().isObject() && getOwnershipKind() == OwnershipKind::None)
    return true;

  return hasCleanup();
}

bool ManagedValue::isPlusOneOrTrivial(SILGenFunction &SGF) const {
  return getType().isTrivial(SGF.F) || isPlusOne(SGF);
}

bool ManagedValue::isPlusZero() const {
  // SILUndef can always be passed to +0 APIs.
  if (isa<SILUndef>(getValue()))
    return true;

  // Otherwise, just check if we have a cleanup.
  return !hasCleanup();
}
