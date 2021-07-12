//===--- TypeLoc.h - Swift Language Type Locations --------------*- C++ -*-===//
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
// This file defines the TypeLoc struct and related structs.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_TYPELOC_H
#define SWIFT_TYPELOC_H

#include "swift/Basic/SourceLoc.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeAlignments.h"
#include "llvm/ADT/PointerIntPair.h"

namespace swift {

class ASTContext;
class TypeRepr;

/// TypeLoc - Provides source location information for a parsed type.
/// A TypeLoc is stored in AST nodes which use an explicitly written type.
class alignas(1 << TypeReprAlignInBits) TypeLoc {
  Type Ty;
  TypeRepr *TyR = nullptr;

public:
  TypeLoc() {}
  TypeLoc(TypeRepr *TyR) : TyR(TyR) {}
  TypeLoc(TypeRepr *TyR, Type Ty) : TyR(TyR) {
    setType(Ty);
  }

  bool wasValidated() const { return !Ty.isNull(); }
  bool isError() const;

  // FIXME: We generally shouldn't need to build TypeLocs without a location.
  static TypeLoc withoutLoc(Type T) {
    TypeLoc result;
    result.Ty = T;
    return result;
  }

  /// Get the representative location of this type, for diagnostic
  /// purposes.
  /// This location is not necessarily the start location of the type repr.
  SourceLoc getLoc() const;
  SourceRange getSourceRange() const;

  bool hasLocation() const { return TyR != nullptr; }
  TypeRepr *getTypeRepr() const { return TyR; }
  Type getType() const { return Ty; }

  bool isNull() const { return getType().isNull() && TyR == nullptr; }

  void setType(Type Ty);

  friend llvm::hash_code hash_value(const TypeLoc &owner) {
    return llvm::hash_combine(owner.Ty.getPointer(), owner.TyR);
  }

  friend bool operator==(const TypeLoc &lhs,
                         const TypeLoc &rhs) {
    return lhs.Ty.getPointer() == rhs.Ty.getPointer()
        && lhs.TyR == rhs.TyR;
  }

  friend bool operator!=(const TypeLoc &lhs,
                         const TypeLoc &rhs) {
    return !(lhs == rhs);
  }
};

} // end namespace llvm

#endif
