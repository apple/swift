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

enum SerializedKind_t : uint8_t;
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
  unsigned serialized : 2;

  SILMoveOnlyDeinit()
      : nominalDecl(nullptr), funcImpl(nullptr), serialized(unsigned(IsNotSerialized)) {}

  SILMoveOnlyDeinit(NominalTypeDecl *nominaldecl, SILFunction *implementation,
                    unsigned serialized);
  ~SILMoveOnlyDeinit();

public:
  static SILMoveOnlyDeinit *create(SILModule &mod, NominalTypeDecl *nominalDecl,
                                   SerializedKind_t serialized,
                                   SILFunction *funcImpl);

  NominalTypeDecl *getNominalDecl() const { return nominalDecl; }

  SILFunction *getImplementation() const {
    assert(funcImpl);
    return funcImpl;
  }

  bool isAnySerialized() const {
    return SerializedKind_t(serialized) == IsSerialized ||
           SerializedKind_t(serialized) == IsSerializedForPackage;
  }
  SerializedKind_t getSerializedKind() const {
    return SerializedKind_t(serialized);
  }
  void setSerializedKind(SerializedKind_t inputSerialized) {
    serialized = unsigned(inputSerialized);
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
