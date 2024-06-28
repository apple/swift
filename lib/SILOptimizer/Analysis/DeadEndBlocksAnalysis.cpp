//===--- DeadEndBlocksAnalysis.cpp ----------------------------------------===//
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

#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/OptimizerBridging.h"
#include "swift/AST/Decl.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/SILFunction.h"

using namespace swift;

void DeadEndBlocksAnalysis::verify(DeadEndBlocks *deBlocks) const {
  // If the passed in deBlocks has not computed, there is nothing to check.
  if (!deBlocks->isComputed())
    return;

  // Then create our new dead end blocks instance so we can check the internal
  // state of our input against it.
  auto *fn = deBlocks->getFunction();
  DeadEndBlocks newBlocks(fn);

  // Make sure that all values that deBlocks thinks is unreachable are
  // actually unreachable.
  //
  // NOTE: We verify like this b/c DeadEndBlocks looks up state lazily so we
  // can only check the work we have done so far.
  for (auto &block : *fn) {
    if (deBlocks->isDeadEnd(&block)) {
      if (!newBlocks.isDeadEnd(&block)) {
        llvm::errs() << "DeadEndBlocksAnalysis Error! Found dead end block "
                        "that is no longer a dead end block?!";
        llvm_unreachable("standard error assertion");
      }
    } else {
      if (newBlocks.isDeadEnd(&block)) {
        llvm::errs() << "DeadEndBlocksAnalysis Error! Found reachable block "
                        "that is no longer reachable?!";
        llvm_unreachable("standard error assertion");
      }
    }
  }
}

//===----------------------------------------------------------------------===//
//                              Main Entry Point
//===----------------------------------------------------------------------===//

SILAnalysis *swift::createDeadEndBlocksAnalysis(SILModule *) {
  return new DeadEndBlocksAnalysis();
}
