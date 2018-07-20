//===--- PassManagerVerifierAnalysis.cpp ----------------------------------===//
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

#define DEBUG_TYPE "sil-passmanager-verifier-analysis"
#include "swift/SILOptimizer/Analysis/PassManagerVerifierAnalysis.h"
#include "swift/SIL/SILModule.h"
#include "llvm/Support/CommandLine.h"

static llvm::cl::opt<bool>
    EnableVerifier("enable-sil-passmanager-verifier-analysis",
                   llvm::cl::desc("Enable verification of the passmanagers "
                                  "function notification infrastructure"),
                   llvm::cl::init(false));

using namespace swift;

PassManagerVerifierAnalysis::PassManagerVerifierAnalysis(SILModule *mod)
    : SILAnalysis(SILAnalysisKind::PassManagerVerifier), mod(*mod) {
  // Necessary to quiet an unused private field warning when we compile without
  // assertions enabled.
  (void)mod;
#ifndef NDEBUG
  if (!EnableVerifier)
    return;
  for (auto &fn : *mod) {
    DEBUG(llvm::dbgs() << "PMVerifierAnalysis. Add: " << fn.getName() << '\n');
    liveFunctions.insert(fn.getName());
  }
#endif
}

/// If a function has not yet been seen start tracking it.
void PassManagerVerifierAnalysis::notifyAddedOrModifiedFunction(
    SILFunction *f) {
#ifndef NDEBUG
  if (!EnableVerifier)
    return;
  DEBUG(llvm::dbgs() << "PMVerifierAnalysis. Add|Mod: " << f->getName()
                     << '\n');
  liveFunctions.insert(f->getName());
#endif
}

/// Stop tracking a function.
void PassManagerVerifierAnalysis::notifyWillDeleteFunction(SILFunction *f) {
#ifndef NDEBUG
  if (!EnableVerifier)
    return;
  DEBUG(llvm::dbgs() << "PMVerifierAnalysis. Delete: " << f->getName() << '\n');
  if (!liveFunctions.count(f->getName())) {
    llvm::errs()
        << "Error! Tried to delete function that analysis was not aware of: "
        << f->getName() << '\n';
    llvm_unreachable("triggering standard assertion failure routine");
  }
  liveFunctions.erase(f->getName());
#endif
}

/// Run the entire verification.
void PassManagerVerifierAnalysis::verify() const {
#ifndef NDEBUG
  if (!EnableVerifier)
    return;

  // We check that liveFunctions is in sync with the module's function list by
  // going through the module's function list and attempting to remove all
  // functions in the module. If we fail to remove fn, then we know that a
  // function was added to the module without an appropriate message being sent
  // by the pass manager.
  bool foundError = false;
  for (auto &fn : mod) {
    if (liveFunctions.count(fn.getName())) {
      continue;
    }
    llvm::errs() << "Found function in module that was not added to verifier: "
                 << fn.getName() << '\n';
    foundError = true;
  }

  // At this point, if we have any liveFunctions left then we know that these
  // functions were deleted, but we were not sent a delete message. Print out
  // those function names.
  for (auto &iter : liveFunctions) {
    llvm::errs() << "Missing delete message for function: " << iter.first()
                 << '\n';
    foundError = true;
  }

  // We assert here so we emit /all/ errors before asserting.
  assert(!foundError && "triggering standard assertion failure routine");
#endif
}

//===----------------------------------------------------------------------===//
//                              Main Entry Point
//===----------------------------------------------------------------------===//

SILAnalysis *swift::createPassManagerVerifierAnalysis(SILModule *m) {
  return new PassManagerVerifierAnalysis(m);
}
