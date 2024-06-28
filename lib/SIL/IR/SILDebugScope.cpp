//===--- SILDebugScope.cpp - DebugScopes for SIL code ---------------------===//
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
///
/// \file
///
/// This file defines a container for scope information used to
/// generate debug info.
///
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILFunction.h"

using namespace swift;

SILDebugScope::SILDebugScope(SILLocation Loc, SILFunction *SILFn,
                             const SILDebugScope *ParentScope,
                             const SILDebugScope *InlinedCallSite)
    : Loc(Loc), InlinedCallSite(InlinedCallSite) {
  if (ParentScope)
    Parent = ParentScope;
  else {
    assert(SILFn && "no parent provided");
    Parent = SILFn;
  }
}

SILDebugScope::SILDebugScope(SILLocation Loc)
    : Loc(Loc), InlinedCallSite(nullptr) {}

SILFunction *SILDebugScope::getInlinedFunction() const {
  if (Parent.isNull())
    return nullptr;

  const SILDebugScope *Scope = this;
  while (Scope->Parent.is<const SILDebugScope *>())
    Scope = Scope->Parent.get<const SILDebugScope *>();
  assert(Scope->Parent.is<SILFunction *>() && "orphaned scope");
  return Scope->Parent.get<SILFunction *>();
}

SILFunction *SILDebugScope::getParentFunction() const {
  if (InlinedCallSite)
    return InlinedCallSite->getParentFunction();
  if (auto *ParentScope = Parent.dyn_cast<const SILDebugScope *>())
    return ParentScope->getParentFunction();
  return Parent.get<SILFunction *>();
}

/// Determine whether an instruction may not have a SILDebugScope.
bool swift::maybeScopeless(const SILInstruction &inst) {
  if (inst.getFunction()->isBare())
    return true;
  return !isa<DebugValueInst>(inst);
}
