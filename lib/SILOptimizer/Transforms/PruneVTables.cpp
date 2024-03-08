//===--- PruneVTables.cpp - Prune unnecessary vtable entries --------------===//
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
// Mark sil_vtable entries as [nonoverridden] when possible, so that we know
// at IRGen time they can be elided from runtime vtables.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "prune-vtables"

#include "swift/SIL/CalleeCache.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"

STATISTIC(NumNonoverriddenVTableEntries,
          "# of vtable entries marked non-overridden");

using namespace swift;

namespace {
class PruneVTables : public SILModuleTransform {
  void runOnVTable(SILModule *M, SILVTable *vtable) {
    LLVM_DEBUG(llvm::dbgs() << "PruneVTables inspecting table:\n";
               vtable->print(llvm::dbgs()));
    if (!M->isWholeModule() &&
        vtable->getClass()->getEffectiveAccess() > AccessLevel::FilePrivate) {
      LLVM_DEBUG(llvm::dbgs() << "Ignoring visible table: ";
                 vtable->print(llvm::dbgs()));
      return;
    }

    for (auto &entry : vtable->getMutableEntries()) {
      
      // We don't need to worry about entries that are overridden,
      // or have already been found to have no overrides.
      if (entry.isNonOverridden()) {
        LLVM_DEBUG(llvm::dbgs() << "-- entry for ";
                   entry.getMethod().print(llvm::dbgs());
                   llvm::dbgs() << " is already nonoverridden\n");
        continue;
      }
      
      switch (entry.getKind()) {
      case SILVTable::Entry::Normal:
      case SILVTable::Entry::Inherited:
        break;
          
      case SILVTable::Entry::Override:
        LLVM_DEBUG(llvm::dbgs() << "-- entry for ";
                   entry.getMethod().print(llvm::dbgs());
                   llvm::dbgs() << " is an override\n");
        continue;
      }

      // The destructor entry must remain.
      if (entry.getMethod().kind == SILDeclRef::Kind::Deallocator) {
        LLVM_DEBUG(llvm::dbgs() << "-- entry for ";
                   entry.getMethod().print(llvm::dbgs());
                   llvm::dbgs() << " is a destructor\n");
        continue;
      }

      auto methodDecl = entry.getMethod().getAbstractFunctionDecl();
      if (!methodDecl) {
        LLVM_DEBUG(llvm::dbgs() << "-- entry for ";
                   entry.getMethod().print(llvm::dbgs());
                   llvm::dbgs() << " is not a function decl\n");
        continue;
      }

      // Is the method declared final?
      if (!methodDecl->isFinal()) {
        // Are callees of this entry statically knowable?
        if (!calleesAreStaticallyKnowable(*M, entry.getMethod())) {
          LLVM_DEBUG(llvm::dbgs() << "-- entry for ";
                     entry.getMethod().print(llvm::dbgs());
                     llvm::dbgs() << " does not have statically-knowable callees\n");
          continue;
        }
        
        // Does the method have any overrides in this module?
        if (methodDecl->isOverridden()) {
          LLVM_DEBUG(llvm::dbgs() << "-- entry for ";
                     entry.getMethod().print(llvm::dbgs());
                     llvm::dbgs() << " has overrides\n");
          continue;
        }
      }
      LLVM_DEBUG(llvm::dbgs() << "++ entry for ";
                 entry.getMethod().print(llvm::dbgs());
                 llvm::dbgs() << " can be marked non-overridden!\n");
      ++NumNonoverriddenVTableEntries;
      entry.setNonOverridden(true);
      vtable->updateVTableCache(entry);
    }
  }
  
  void run() override {
    SILModule *M = getModule();
    
    for (auto &vtable : M->getVTables()) {
      runOnVTable(M, vtable);
    }
  }
};
}

SILTransform *swift::createPruneVTables() {
  return new PruneVTables();
}
