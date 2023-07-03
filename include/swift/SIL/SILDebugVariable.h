//===--- SILDebugVariable.h -----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILDEBUGVARIABLE_H
#define SWIFT_SIL_SILDEBUGVARIABLE_H

#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILDebugInfoExpression.h"
#include "swift/SIL/SILLocation.h"
#include "swift/SIL/SILType.h"
#include "llvm/ADT/StringRef.h"

namespace swift {

class AllocationInst;

/// Holds common debug information about local variables and function
/// arguments that are needed by DebugValueInst, AllocStackInst,
/// and AllocBoxInst.
struct SILDebugVariable;
inline llvm::hash_code hash_value(const SILDebugVariable &P);

/// Holds common debug information about local variables and function
/// arguments that are needed by DebugValueInst, AllocStackInst,
/// and AllocBoxInst.
struct SILDebugVariable {
  friend llvm::hash_code hash_value(const SILDebugVariable &P);

  StringRef Name;
  unsigned ArgNo : 16;
  unsigned Constant : 1;
  unsigned Implicit : 1;
  unsigned isDenseMapSingleton : 2;
  llvm::Optional<SILType> Type;
  llvm::Optional<SILLocation> Loc;
  const SILDebugScope *Scope;
  SILDebugInfoExpression DIExpr;

  // Use vanilla copy ctor / operator
  SILDebugVariable(const SILDebugVariable &) = default;
  SILDebugVariable &operator=(const SILDebugVariable &) = default;

  enum class IsDenseMapSingleton { No, IsEmpty, IsTombstone };
  SILDebugVariable(IsDenseMapSingleton inputIsDenseMapSingleton)
      : SILDebugVariable() {
    assert(inputIsDenseMapSingleton != IsDenseMapSingleton::No &&
           "Should only pass IsEmpty or IsTombstone");
    isDenseMapSingleton = unsigned(inputIsDenseMapSingleton);
  }

  SILDebugVariable()
      : ArgNo(0), Constant(false), Implicit(false), isDenseMapSingleton(0),
        Scope(nullptr) {}
  SILDebugVariable(bool Constant, uint16_t ArgNo)
      : ArgNo(ArgNo), Constant(Constant), Implicit(false),
        isDenseMapSingleton(0), Scope(nullptr) {}
  SILDebugVariable(StringRef Name, bool Constant, unsigned ArgNo,
                   bool IsImplicit = false,
                   llvm::Optional<SILType> AuxType = {},
                   llvm::Optional<SILLocation> DeclLoc = {},
                   const SILDebugScope *DeclScope = nullptr,
                   llvm::ArrayRef<SILDIExprElement> ExprElements = {})
      : Name(Name), ArgNo(ArgNo), Constant(Constant), Implicit(IsImplicit),
        isDenseMapSingleton(0), Type(AuxType), Loc(DeclLoc), Scope(DeclScope),
        DIExpr(ExprElements) {}

  /// Created from either AllocStack or AllocBox instruction
  static llvm::Optional<SILDebugVariable>
  createFromAllocation(const AllocationInst *AI);

  // We're not comparing DIExpr here because strictly speaking,
  // DIExpr is not part of the debug variable. We simply piggyback
  // it in this class so that's it's easier to carry DIExpr around.
  bool operator==(const SILDebugVariable &V) const {
    return ArgNo == V.ArgNo && Constant == V.Constant && Name == V.Name &&
           Implicit == V.Implicit && Type == V.Type && Loc == V.Loc &&
           Scope == V.Scope && isDenseMapSingleton == V.isDenseMapSingleton &&
           DIExpr == V.DIExpr;
  }

  SILDebugVariable withoutDIExpr() const {
    auto result = *this;
    result.DIExpr = {};
    return result;
  }

  bool isLet() const { return Name.size() && Constant; }

  bool isVar() const { return Name.size() && !Constant; }
};

/// Returns the hashcode for the new projection path.
inline llvm::hash_code hash_value(const SILDebugVariable &P) {
  return llvm::hash_combine(P.ArgNo, P.Constant, P.Name, P.Implicit,
                            P.isDenseMapSingleton, P.Type, P.Loc, P.Scope,
                            P.DIExpr);
}

} // namespace swift

#endif
