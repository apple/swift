//===--- BasicCalleeAnalysis.cpp - Determine callees per call site --------===//
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

#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"

#include "swift/AST/Decl.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/Basic/Statistic.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/SILBridging.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/Test.h"
#include "swift/SILOptimizer/OptimizerBridging.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/Support/Compiler.h"

#include <algorithm>

#define DEBUG_TYPE "BasicCalleeAnalysis"

using namespace swift;

void BasicCalleeAnalysis::dump() const {
  print(llvm::errs());
}

void BasicCalleeAnalysis::print(llvm::raw_ostream &os) const {
  if (!Cache) {
    os << "<no cache>\n";
  }
  llvm::DenseSet<SILDeclRef> printed;
  for (auto &VTable : M.getVTables()) {
    for (const SILVTable::Entry &entry : VTable->getEntries()) {
      if (printed.insert(entry.getMethod()).second) {
        os << "callees for " << entry.getMethod() << ":\n";
        Cache->getCalleeList(entry.getMethod()).print(os);
      }
    }
  }
}

//===----------------------------------------------------------------------===//
//                            Swift Bridging
//===----------------------------------------------------------------------===//

static BridgedCalleeAnalysis::IsDeinitBarrierFn instructionIsDeinitBarrierFunction;
static BridgedCalleeAnalysis::GetMemBehvaiorFn getMemBehvaiorFunction = nullptr;

void BridgedCalleeAnalysis::registerAnalysis(IsDeinitBarrierFn instructionIsDeinitBarrierFn,
                                             GetMemBehvaiorFn getMemBehvaiorFn) {
  instructionIsDeinitBarrierFunction = instructionIsDeinitBarrierFn;
  getMemBehvaiorFunction = getMemBehvaiorFn;
}

MemoryBehavior BasicCalleeAnalysis::
getMemoryBehavior(ApplySite as, bool observeRetains) {
  if (getMemBehvaiorFunction) {
    auto b = getMemBehvaiorFunction({as.getInstruction()->asSILNode()},
                                    observeRetains,
                                    {this});
    return (MemoryBehavior)b;
  }
  return MemoryBehavior::MayHaveSideEffects;
}

bool swift::isDeinitBarrier(SILInstruction *const instruction,
                            BasicCalleeAnalysis *bca) {
  if (!instructionIsDeinitBarrierFunction) {
    return mayBeDeinitBarrierNotConsideringSideEffects(instruction);
  }
  BridgedInstruction inst = {
      cast<SILNode>(const_cast<SILInstruction *>(instruction))};
  BridgedCalleeAnalysis analysis = {bca};
  return instructionIsDeinitBarrierFunction(inst, analysis);
}

namespace swift::test {
// Arguments:
// - instruction
// Dumps:
// - instruction
// - whether it's a deinit barrier
static FunctionTest IsDeinitBarrierTest("is-deinit-barrier", [](auto &function,
                                                                auto &arguments,
                                                                auto &test) {
  auto *instruction = arguments.takeInstruction();
  auto *analysis = test.template getAnalysis<BasicCalleeAnalysis>();
  auto isBarrier = isDeinitBarrier(instruction, analysis);
  instruction->print(llvm::outs());
  auto *boolString = isBarrier ? "true" : "false";
  llvm::outs() << boolString << "\n";
});
} // namespace swift::test
