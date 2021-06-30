//===--- ArgumentSource.cpp - Latent value representation -----------------===//
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
// A structure for holding a r-value or l-value
//
//===----------------------------------------------------------------------===//

#include "ArgumentSource.h"
#include "Conversion.h"
#include "Initialization.h"

using namespace swift;
using namespace Lowering;

RValue &ArgumentSource::peekRValue() & {
  assert(isRValue() && "Undefined behavior to call this method without the "
         "ArgumentSource actually being an RValue");
  return Storage.get<RValueStorage>(StoredKind).Value;
}

RValue ArgumentSource::getAsRValue(SILGenFunction &SGF, SGFContext C) && {
  switch (StoredKind) {
  case Kind::Invalid:
    llvm_unreachable("argument source is invalid");
  case Kind::LValue:
    llvm_unreachable("cannot get l-value as r-value");
  case Kind::RValue:
    return std::move(*this).asKnownRValue(SGF);
  case Kind::Expr:
    return SGF.emitRValue(std::move(*this).asKnownExpr(), C);
  }
  llvm_unreachable("bad kind");
}

ManagedValue ArgumentSource::getAsSingleValue(SILGenFunction &SGF,
                                              SGFContext C) && {
  switch (StoredKind) {
  case Kind::Invalid:
    llvm_unreachable("argument source is invalid");
  case Kind::LValue: {
    auto loc = getKnownLValueLocation();
    LValue &&lv = std::move(*this).asKnownLValue();
    return SGF.emitAddressOfLValue(loc, std::move(lv));
  }
  case Kind::RValue: {
    auto loc = getKnownRValueLocation();
    if (auto init = C.getEmitInto()) {
      std::move(*this).asKnownRValue(SGF)
                      .ensurePlusOne(SGF, loc)
                      .forwardInto(SGF, loc, init);
      return ManagedValue::forInContext();
    } else {
      return std::move(*this).asKnownRValue(SGF).getAsSingleValue(SGF, loc);
    }
  }
  case Kind::Expr: {
    auto e = std::move(*this).asKnownExpr();
    if (e->isSemanticallyInOutExpr()) {
      auto lv = SGF.emitLValue(e, SGFAccessKind::ReadWrite);
      return SGF.emitAddressOfLValue(e, std::move(lv));
    } else {
      return SGF.emitRValueAsSingleValue(e, C);
    }
  }
  }
  llvm_unreachable("bad kind");
}


ManagedValue ArgumentSource::getAsSingleValue(SILGenFunction &SGF,
                                              AbstractionPattern origFormalType,
                                              SILType loweredTy,
                                              SGFContext C) && {
  auto substFormalType = getSubstRValueType();
  auto conversion =
    Conversion::getSubstToOrig(origFormalType, substFormalType, loweredTy);
  return std::move(*this).getConverted(SGF, conversion, C);
}

ManagedValue ArgumentSource::getConverted(SILGenFunction &SGF,
                                          const Conversion &conversion,
                                          SGFContext C) && {
  switch (StoredKind) {
  case Kind::Invalid:
    llvm_unreachable("argument source is invalid");
  case Kind::LValue:
    llvm_unreachable("cannot get converted l-value");
  case Kind::RValue:
  case Kind::Expr:
    return SGF.emitConvertedRValue(getLocation(), conversion, C,
                [&](SILGenFunction &SGF, SILLocation loc, SGFContext C) {
      return std::move(*this).getAsSingleValue(SGF, C);
    });
  }
  llvm_unreachable("bad kind");
}

void ArgumentSource::forwardInto(SILGenFunction &SGF, Initialization *dest) && {
  switch (StoredKind) {
  case Kind::Invalid:
    llvm_unreachable("argument source is invalid");
  case Kind::LValue:
    llvm_unreachable("cannot forward an l-value");
  case Kind::RValue: {
    auto loc = getKnownRValueLocation();
    std::move(*this).asKnownRValue(SGF).ensurePlusOne(SGF, loc).forwardInto(SGF, loc, dest);
    return;
  }
  case Kind::Expr: {
    auto e = std::move(*this).asKnownExpr();
    SGF.emitExprInto(e, dest);
    return;
  }
  }
  llvm_unreachable("bad kind");
}

// FIXME: Once uncurrying is removed, get rid of this constructor.
ArgumentSource::ArgumentSource(SILLocation loc, RValue &&rv, Kind kind)
    : Storage(), StoredKind(kind) {
  Storage.emplaceAggregate<RValueStorage>(StoredKind, std::move(rv), loc);
}

ArgumentSource ArgumentSource::borrow(SILGenFunction &SGF) const & {
  switch (StoredKind) {
  case Kind::Invalid:
    llvm_unreachable("argument source is invalid");
  case Kind::LValue:
    llvm_unreachable("cannot borrow an l-value");
  case Kind::RValue: {
    auto loc = getKnownRValueLocation();
    return ArgumentSource(loc, asKnownRValue().borrow(SGF, loc));
  }
  case Kind::Expr: {
    llvm_unreachable("cannot borrow an expression");
  }
  }
  llvm_unreachable("bad kind");
}

ManagedValue ArgumentSource::materialize(SILGenFunction &SGF) && {
  if (isRValue()) {
    auto loc = getKnownRValueLocation();
    return std::move(*this).asKnownRValue(SGF).materialize(SGF, loc);
  }

  auto loc = getLocation();
  auto temp = SGF.emitTemporary(loc, SGF.getTypeLowering(getSubstRValueType()));
  std::move(*this).forwardInto(SGF, temp.get());
  return temp->getManagedAddress();
}

ManagedValue ArgumentSource::materialize(SILGenFunction &SGF,
                                         AbstractionPattern origFormalType,
                                         SILType destType) && {
  auto substFormalType = getSubstRValueType();
  assert(!destType || destType.getObjectType() ==
               SGF.getLoweredType(origFormalType,
                                  substFormalType).getObjectType());

  // Fast path: if the types match exactly, no abstraction difference
  // is possible and we can just materialize as normal.
  if (origFormalType.isExactType(substFormalType))
    return std::move(*this).materialize(SGF);

  auto &destTL =
    (destType ? SGF.getTypeLowering(destType)
              : SGF.getTypeLowering(origFormalType, substFormalType));
  if (!destType) destType = destTL.getLoweredType();

  // If there's no abstraction difference, we can just materialize as normal.
  if (destTL.getLoweredType() == SGF.getLoweredType(substFormalType)) {
    return std::move(*this).materialize(SGF);
  }

  // Emit a temporary at the given address.
  auto temp = SGF.emitTemporary(getLocation(), destTL);

  // Forward into it.
  std::move(*this).forwardInto(SGF, origFormalType, temp.get(), destTL);

  return temp->getManagedAddress();
}

void ArgumentSource::forwardInto(SILGenFunction &SGF,
                                 AbstractionPattern origFormalType,
                                 Initialization *dest,
                                 const TypeLowering &destTL) && {
  auto substFormalType = getSubstRValueType();
  assert(destTL.getLoweredType() ==
                        SGF.getLoweredType(origFormalType, substFormalType));

  // If there are no abstraction changes, we can just forward
  // normally.
  if (origFormalType.isExactType(substFormalType) ||
      destTL.getLoweredType() == SGF.getLoweredType(substFormalType)) {
    std::move(*this).forwardInto(SGF, dest);
    return;
  }

  // Otherwise, emit as a single independent value.
  SILLocation loc = getLocation();
  ManagedValue outputValue =
      std::move(*this).getAsSingleValue(SGF, origFormalType,
                                        destTL.getLoweredType(),
                                        SGFContext(dest));

  if (outputValue.isInContext()) return;

  // Use RValue's forward-into-initialization code.  We have to lie to
  // RValue about the formal type (by using the lowered type) because
  // we're emitting into an abstracted value, which RValue doesn't
  // really handle.
  auto substLoweredType = destTL.getLoweredType().getASTType();
  RValue(SGF, loc, substLoweredType, outputValue).forwardInto(SGF, loc, dest);
}

void ArgumentSource::dump() const {
  dump(llvm::errs());
}

void ArgumentSource::dump(raw_ostream &out, unsigned indent) const {
  out.indent(indent) << "ArgumentSource::";
  switch (StoredKind) {
  case Kind::Invalid:
    out << "Invalid\n";
    return;
  case Kind::LValue:
    out << "LValue\n";
    Storage.get<LValueStorage>(StoredKind).Value.dump(out, indent + 2);
    return;
  case Kind::RValue:
    out << "RValue\n";
    Storage.get<RValueStorage>(StoredKind).Value.dump(out, indent + 2);
    return;
  case Kind::Expr:
    out << "Expr\n";
    Storage.get<Expr*>(StoredKind)->dump(out); // FIXME: indent
    out << "\n";
    return;
  }
  llvm_unreachable("bad kind");
}

PreparedArguments::PreparedArguments(ArrayRef<AnyFunctionType::Param> params,
                                     ArgumentList *argList)
    : PreparedArguments(params) {
  for (auto &arg : *argList)
    addArbitrary(arg.getExpr());
}

PreparedArguments
PreparedArguments::copy(SILGenFunction &SGF, SILLocation loc) const {
  if (isNull()) return PreparedArguments();

  assert(isValid());
  PreparedArguments result(getParams());
  for (auto &elt : Arguments) {
    assert(elt.isRValue());
    result.add(elt.getKnownRValueLocation(),
               elt.asKnownRValue().copy(SGF, loc));
  }
  assert(isValid());
  return result;
}

bool PreparedArguments::isObviouslyEqual(const PreparedArguments &other) const {
  if (isNull() != other.isNull())
    return false;
  if (isNull())
    return true;

  assert(isValid() && other.isValid());
  if (Arguments.size() != other.Arguments.size())
    return false;
  for (auto i : indices(Arguments)) {
    if (!Arguments[i].isObviouslyEqual(other.Arguments[i]))
      return false;
  }
  return true;
}

bool ArgumentSource::isObviouslyEqual(const ArgumentSource &other) const {
  if (StoredKind != other.StoredKind)
    return false;

  switch (StoredKind) {
  case Kind::Invalid:
    llvm_unreachable("argument source is invalid");
  case Kind::RValue:
    return asKnownRValue().isObviouslyEqual(other.asKnownRValue());
  case Kind::LValue:
    return false; // TODO?
  case Kind::Expr:
    return false; // TODO?
  }
  llvm_unreachable("bad kind");
}

PreparedArguments PreparedArguments::copyForDiagnostics() const {
  if (isNull())
    return PreparedArguments();

  assert(isValid());
  PreparedArguments result(getParams());
  for (auto &arg : Arguments) {
    result.Arguments.push_back(arg.copyForDiagnostics());
  }
  return result;
}

ArgumentSource ArgumentSource::copyForDiagnostics() const {
  switch (StoredKind) {
  case Kind::Invalid:
    return ArgumentSource();
  case Kind::LValue:
    // We have no way to copy an l-value for diagnostics.
    return {getKnownLValueLocation(), LValue()};
  case Kind::RValue:
    return {getKnownRValueLocation(), asKnownRValue().copyForDiagnostics()};
  case Kind::Expr:
    return asKnownExpr();
  }
  llvm_unreachable("bad kind");
}
