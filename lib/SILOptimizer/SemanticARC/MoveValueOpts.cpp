//===--- MoveValueOpts.cpp ------------------------------------------------===//
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

#include "Context.h"
#include "SemanticARCOptVisitor.h"

using namespace swift;
using namespace swift::semanticarc;

bool SemanticARCOptVisitor::visitMoveValueInst(MoveValueInst *mvi) {
  if (!ctx.shouldPerform(ARCTransformKind::MoveValuePeepholes))
    return false;

  // If our move_value is used by a copy_value, we are ending the copy_value's
  // lifetime at this point in the code so we should be able to just RAUW.
  if (auto *cvi = dyn_cast<CopyValueInst>(mvi->getOperand())) {
    eraseAndRAUWSingleValueInstruction(mvi, cvi);
  }

  return false;
}
