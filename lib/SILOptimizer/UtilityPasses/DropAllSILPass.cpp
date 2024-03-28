//===--- DropAllSILPass.cpp -----------------------------------------------===//
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

#define DEBUG_TYPE "drop-all-sil"
#include "swift/SIL/SILFunction.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;

namespace {

class DropAllSILPass : public SILModuleTransform {
public:
  DropAllSILPass() {}
  
  void run() override {
    auto &M = *getModule();

    llvm::SmallVector<SILFunction *, 16> FunctionsToErase;
    llvm::SmallVector<SILGlobalVariable *, 16> GlobalsToErase;

    for (SILFunction &F : M) {
      F.dropAllReferences();
      FunctionsToErase.push_back(&F);
    }

    for (SILGlobalVariable &G : M.getSILGlobals()) {
      G.dropAllReferences();
      GlobalsToErase.push_back(&G);
    }

    M.eraseAllVTables();

    for (SILFunction *F : FunctionsToErase) {
      notifyWillDeleteFunction(F);
      M.eraseFunction(F);
    }

    for (SILGlobalVariable *G : GlobalsToErase) {
      M.eraseGlobalVariable(G);
    }
    
    invalidateFunctionTables();
  }
};

}

SILTransform *swift::createDropAllSILPass() {
  return new DropAllSILPass();
}
