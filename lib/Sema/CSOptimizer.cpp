//===--- CSOptimizer.cpp - Constraint Optimizer ---------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements disjunction and other constraint optimizations.
//
//===----------------------------------------------------------------------===//

#include "swift/Sema/ConstraintSystem.h"
#include "llvm/ADT/SmallVector.h"

using namespace swift;
using namespace constraints;

void ConstraintSystem::optimizeDisjunctions(
    SmallVectorImpl<Constraint *> &disjunctions) {
}
