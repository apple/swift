//===--- ExitableFullExpr.h - An exitable full-expression -------*- C++ -*-===//
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
// This file defines ExitableFullExpr, a cleanup scope RAII object
// that conveniently creates a continuation block.
//
//===----------------------------------------------------------------------===//

#ifndef EXITABLE_FULL_EXPR_H
#define EXITABLE_FULL_EXPR_H

#include "JumpDest.h"
#include "Scope.h"
#include "swift/Basic/Assertions.h"

namespace swift {
namespace Lowering {

/// A cleanup scope RAII object, like FullExpr, that comes with a
/// JumpDest for a continuation block.
///
/// You *must* call exit() at some point.
///
/// This scope is also exposed to the debug info.
class LLVM_LIBRARY_VISIBILITY ExitableFullExpr {
  SILGenFunction &SGF;
  FullExpr Scope;
  JumpDest ExitDest;
public:
  explicit ExitableFullExpr(SILGenFunction &SGF, CleanupLocation loc)
    : SGF(SGF), Scope(SGF.Cleanups, loc),
      ExitDest(SGF.B.splitBlockForFallthrough(),
               SGF.Cleanups.getCleanupsDepth(), loc) {
    SGF.enterDebugScope(loc);
  }
  ~ExitableFullExpr() {
    SGF.leaveDebugScope();
  }


  JumpDest getExitDest() const { return ExitDest; }

  SILBasicBlock *exit() {
    assert(!SGF.B.hasValidInsertionPoint());
    Scope.pop();
    SGF.B.setInsertionPoint(ExitDest.getBlock());
    return ExitDest.getBlock();
  }
};

} // end namespace Lowering
} // end namespace swift

#endif
