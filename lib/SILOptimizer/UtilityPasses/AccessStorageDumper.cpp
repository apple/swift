//===--- AccessStorageDumper.cpp - Dump accessed storage ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-accessed-storage-dumper"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;

static llvm::cl::opt<bool> EnableDumpUses(
    "enable-access-storage-dump-uses", llvm::cl::init(false),
    llvm::cl::desc("With --sil-access-storage-dumper, dump all uses"));

namespace {

/// Dumps sorage information for each access.
class AccessStorageDumper : public SILModuleTransform {
  llvm::SmallVector<Operand *, 32> uses;

  void dumpAccessStorage(Operand *operand) {
    SILFunction *function = operand->getParentFunction();
    // Print storage itself first, for comparison against AccessPath. They can
    // differ in rare cases of unidentified storage with phis.
    AccessStorage::compute(operand->get()).print(llvm::outs());
    // Now print the access path and base.
    auto pathAndBase = AccessPathWithBase::compute(operand->get());
    pathAndBase.print(llvm::outs());
    // If enable-accessed-storage-dump-uses is set, dump all types of uses.
    auto accessPath = pathAndBase.accessPath;
    if (!accessPath.isValid() || !EnableDumpUses)
      return;

    uses.clear();
    accessPath.collectUses(uses, AccessUseType::Exact, function);
    llvm::outs() << "Exact Uses {\n";
    for (auto *useOperand : uses) {
      llvm::outs() << *useOperand->getUser() << "  ";
      auto usePath = AccessPath::compute(useOperand->get());
      usePath.printPath(llvm::outs());
      assert(accessPath == usePath
             && "access path does not match use access path");
    }
    llvm::outs() << "}\n";
    uses.clear();
    accessPath.collectUses(uses, AccessUseType::Inner, function);
    llvm::outs() << "Inner Uses {\n";
    for (auto *useOperand : uses) {
      llvm::outs() << *useOperand->getUser() << "  ";
      auto usePath = AccessPath::compute(useOperand->get());
      usePath.printPath(llvm::outs());
      assert(accessPath.contains(usePath)
             && "access path does not contain use access path");
    }
    llvm::outs() << "}\n";
    uses.clear();
    accessPath.collectUses(uses, AccessUseType::Overlapping, function);
    llvm::outs() << "Overlapping Uses {\n";
    for (auto *useOperand : uses) {
      llvm::outs() << *useOperand->getUser() << "  ";
      auto usePath = AccessPath::compute(useOperand->get());
      usePath.printPath(llvm::outs());
      assert(accessPath.mayOverlap(usePath)
             && "access path does not overlap with use access path");
    }
    llvm::outs() << "}\n";
  }

  void run() override {
    for (auto &fn : *getModule()) {
      llvm::outs() << "@" << fn.getName() << "\n";
      if (fn.empty()) {
        llvm::outs() << "<unknown>\n";
        continue;
      }
      PrettyStackTraceSILFunction functionDumper("...", &fn);
      for (auto &bb : fn) {
        for (auto &inst : bb) {
          if (inst.mayReadOrWriteMemory()) {
            llvm::outs() << "###For MemOp: " << inst;
            visitAccessedAddress(&inst, [this](Operand *operand) {
              dumpAccessStorage(operand);
            });
          }
        }
      }
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createAccessStorageDumper() {
  return new AccessStorageDumper();
}
