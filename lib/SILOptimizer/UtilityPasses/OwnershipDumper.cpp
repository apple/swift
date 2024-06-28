//===--- OwnershipDumper.cpp ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// This is a simple utility pass that dumps the ValueOwnershipKind of all
/// SILValue in a module. It is meant to trigger assertions and verification of
/// these values.
///
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                               Implementation
//===----------------------------------------------------------------------===//

static void dumpInstruction(SILInstruction &ii) {
  llvm::outs() << "Visiting: " << ii;

  auto ops = ii.getAllOperands();
  if (!ops.empty()) {
    llvm::outs() << "Ownership Constraint:\n";
    for (const auto &op : ops) {
      llvm::outs() << "Op #: " << op.getOperandNumber() << "\n"
                      "Constraint: " << op.getOwnershipConstraint() << "\n";
    }
  }

  // If the instruction doesn't have any results, bail.
  auto results = ii.getResults();
  if (!results.empty()) {
    llvm::outs() << "Results Ownership Kinds:\n";
    for (auto v : results) {
      auto kind = v->getOwnershipKind();
      llvm::outs() << "Result: " << v;
      llvm::outs() << "Kind: " << kind << "\n";
    }
  }
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class OwnershipDumper : public SILFunctionTransform {
  void run() override {
    SILFunction *f = getFunction();
    llvm::outs() << "*** Dumping Function: '" << f->getName() << "'\n";
    for (auto &bb : *f) {
      // We only dump instructions right now.
      for (auto &ii : bb) {
        dumpInstruction(ii);
      }
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createOwnershipDumper() { return new OwnershipDumper(); }
