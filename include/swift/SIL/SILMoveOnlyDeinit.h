//===--- SILMoveOnlyDeinit.h ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// This file defines the SILMoveOnlyDeinit class which is used to map a
/// non-class move only nominal type to the concrete implementation of its
/// deinit. This function is called in the destroy witness by IRGen and is
/// called directly by the move checker for concrete classes.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILMOVEONLYDEINIT_H
#define SWIFT_SIL_SILMOVEONLYDEINIT_H

#include "swift/AST/Decl.h"
#include "swift/SIL/SILAllocated.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILFunction.h"

namespace swift {

enum IsSerialized_t : unsigned char;
class SILFunction;
class SILModule;

class SILMoveOnlyDeinit final : public SILAllocated<SILMoveOnlyDeinit> {
  friend SILModule;

  /// The nominal decl that is mapped to this move only deinit table.
  NominalTypeDecl *nominalDecl;

  /// The SILFunction that implements this deinit.
  SILFunction *funcImpl;

  /// Whether or not this deinit table is serialized. If a deinit is not
  /// serialized, then other modules can not consume directly a move only type
  /// since the deinit can not be called directly.
  bool serialized : 1;

  SILMoveOnlyDeinit()
      : nominalDecl(nullptr), funcImpl(nullptr), serialized(false) {}

  SILMoveOnlyDeinit(NominalTypeDecl *nominaldecl, SILFunction *implementation,
                    bool serialized);
  ~SILMoveOnlyDeinit();

public:
  static SILMoveOnlyDeinit *create(SILModule &mod, NominalTypeDecl *nominalDecl,
                                   IsSerialized_t serialized,
                                   SILFunction *funcImpl);

  NominalTypeDecl *getNominalDecl() const { return nominalDecl; }

  SILFunction *getImplementation() const {
    assert(funcImpl);
    return funcImpl;
  }

  IsSerialized_t isSerialized() const {
    return serialized ? IsSerialized : IsNotSerialized;
  }
  void setSerialized(IsSerialized_t inputSerialized) {
    serialized = inputSerialized ? 1 : 0;
  }

  void print(llvm::raw_ostream &os, bool verbose) const;
  void dump() const;

  bool operator==(const SILMoveOnlyDeinit &e) const {
    return funcImpl == e.funcImpl && serialized == e.serialized &&
           nominalDecl == e.nominalDecl;
  }

  bool operator!=(const SILMoveOnlyDeinit &e) const { return !(*this == e); }
};

} // namespace swift

#endif
