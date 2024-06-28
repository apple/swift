//===--- LoopInfoPrinter.cpp - Print SIL Loop Info ------------------------===//
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

#include "swift/Basic/Assertions.h"
#include "swift/SILOptimizer/Analysis/LoopAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILVisitor.h"
#include "llvm/ADT/Statistic.h"

using namespace swift;

namespace {

class LoopInfoPrinter : public SILModuleTransform {

  /// The entry point to the transformation.
  void run() override {
    SILLoopAnalysis *LA = PM->getAnalysis<SILLoopAnalysis>();
    assert(LA && "Invalid LoopAnalysis");
    for (auto &Fn : *getModule()) {
      if (Fn.isExternalDeclaration()) continue;

      SILLoopInfo *LI = LA->get(&Fn);
      assert(LI && "Invalid loop info for function");


      if (LI->empty()) {
        llvm::errs() << "No loops in " << Fn.getName() << "\n";
        return;
      }

      llvm::errs() << "Loops in " << Fn.getName() << "\n";
      for (auto *LoopIt : *LI) {
        LoopIt->dump();
      }
    }
  }
};

} // end anonymous namespace


SILTransform *swift::createLoopInfoPrinter() {
  return new LoopInfoPrinter();
}

