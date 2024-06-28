//===--- SILMoveOnlyDeinit.cpp ---------------------------------------===//
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

#include "swift/Basic/Assertions.h"
#include "swift/SIL/SILMoveOnlyDeinit.h"
#include "swift/SIL/SILModule.h"
#include "llvm/Analysis/ValueTracking.h"

using namespace swift;

SILMoveOnlyDeinit *SILMoveOnlyDeinit::create(SILModule &mod,
                                             NominalTypeDecl *nominalDecl,
                                             SerializedKind_t serialized,
                                             SILFunction *funcImpl) {
  auto buf =
      mod.allocate(sizeof(SILMoveOnlyDeinit), alignof(SILMoveOnlyDeinit));
  auto *table =
      ::new (buf) SILMoveOnlyDeinit(nominalDecl, funcImpl, unsigned(serialized));
  mod.moveOnlyDeinits.push_back(table);
  mod.MoveOnlyDeinitMap[nominalDecl] = table;
  return table;
}

SILMoveOnlyDeinit::SILMoveOnlyDeinit(NominalTypeDecl *nominalDecl,
                                     SILFunction *implementation,
                                     unsigned serialized)
    : nominalDecl(nominalDecl), funcImpl(implementation),
      serialized(serialized) {
  assert(funcImpl);
  funcImpl->incrementRefCount();
}

SILMoveOnlyDeinit::~SILMoveOnlyDeinit() {
  if (funcImpl)
    funcImpl->decrementRefCount();
}
