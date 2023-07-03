//===--- LSLocationPrinter.cpp - Dump all memory locations in program -----===//
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
// This pass tests type expansion, memlocation expansion and memlocation
// reduction.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-memlocation-dumper"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/LoadStoreOptUtils.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {

enum class MLKind : unsigned {
  OnlyExpansion = 0,
  OnlyReduction = 1,
  OnlyTypeExpansion = 2,
  All = 3,
};

} // end anonymous namespace

static llvm::cl::opt<MLKind> LSLocationKinds(
    "ml", llvm::cl::desc("LSLocation Kinds:"), llvm::cl::init(MLKind::All),
    llvm::cl::values(
        clEnumValN(MLKind::OnlyExpansion, "only-expansion", "only-expansion"),
        clEnumValN(MLKind::OnlyReduction, "only-reduction", "only-reduction"),
        clEnumValN(MLKind::OnlyTypeExpansion, "only-type-expansion",
                   "only-type-expansion"),
        clEnumValN(MLKind::All, "all", "all")));

static llvm::cl::opt<bool> UseProjection("lslocation-dump-use-new-projection",
                                            llvm::cl::init(false));

namespace {

class LSLocationPrinter : public SILModuleTransform {
  /// Type expansion analysis.
  TypeExpansionAnalysis *TE;

public:
  /// Dumps the expansions of SILType accessed in the function.
  /// This tests the expandTypeIntoLeafProjectionPaths function, which is
  /// a function used extensively in expand and reduce functions.
  ///
  /// We test it to catch any suspicious things in the earliest point.
  ///
  void printTypeExpansion(SILFunction &Fn) {
    SILModule *M = &Fn.getModule();
    ProjectionPathList PPList;
    unsigned Counter = 0;
    for (auto &BB : Fn) {
      for (auto &II : BB) {
        if (auto *LI = dyn_cast<LoadInst>(&II)) {
          SILValue V = LI->getOperand();
          // This is an address type, take it object type.
          SILType Ty = V->getType().getObjectType();
          ProjectionPath::expandTypeIntoLeafProjectionPaths(
              Ty, M, TypeExpansionContext(Fn), PPList);
        } else if (auto *SI = dyn_cast<StoreInst>(&II)) {
          SILValue V = SI->getDest();
          // This is an address type, take it object type.
          SILType Ty = V->getType().getObjectType();
          ProjectionPath::expandTypeIntoLeafProjectionPaths(
              Ty, M, TypeExpansionContext(Fn), PPList);
        } else {
          // Not interested in these instructions yet.
          continue;
        }

        llvm::outs() << "#" << Counter++ << II;
        for (auto &T : PPList) {
          T.value().print(llvm::outs(), *M, TypeExpansionContext(Fn));
        }
        PPList.clear();
      }
    }
    llvm::outs() << "\n";
  }

  void printTypeExpansionWithProjection(SILFunction &Fn) {
    SILModule *M = &Fn.getModule();
    llvm::SmallVector<llvm::Optional<ProjectionPath>, 8> PPList;
    unsigned Counter = 0;
    for (auto &BB : Fn) {
      for (auto &II : BB) {
        SILValue V;
        SILType Ty;
        if (auto *LI = dyn_cast<LoadInst>(&II)) {
          V = LI->getOperand();
          // This is an address type, take it object type.
          Ty = V->getType().getObjectType();
          ProjectionPath::expandTypeIntoLeafProjectionPaths(
              Ty, M, TypeExpansionContext(Fn), PPList);
        } else if (auto *SI = dyn_cast<StoreInst>(&II)) {
          V = SI->getDest();
          // This is an address type, take it object type.
          Ty = V->getType().getObjectType();
          ProjectionPath::expandTypeIntoLeafProjectionPaths(
              Ty, M, TypeExpansionContext(Fn), PPList);
        } else {
          // Not interested in these instructions yet.
          continue;
        }

        llvm::outs() << "#" << Counter++ << II;
        for (auto &T : PPList) {
          T.value().print(llvm::outs(), *M, TypeExpansionContext(Fn));
        }
        PPList.clear();
      }
    }
    llvm::outs() << "\n";
  }

  /// Dumps the expansions of memory locations accessed in the function.
  /// This tests the expand function in LSLocation class.
  ///
  /// We test it to catch any suspicious things when memory location is
  /// expanded, i.e. base is traced back and aggregate is expanded
  /// properly.
  void printMemExpansion(SILFunction &Fn) {
    LSLocation L;
    LSLocationList Locs;
    unsigned Counter = 0;
    for (auto &BB : Fn) {
      for (auto &II : BB) {
        if (auto *LI = dyn_cast<LoadInst>(&II)) {
          SILValue Mem = LI->getOperand();
          SILValue UO = getUnderlyingObject(Mem);
          L.init(UO, ProjectionPath::getProjectionPath(UO, Mem));
          if (!L.isValid())
            continue;
          LSLocation::expand(L, &Fn.getModule(), TypeExpansionContext(Fn), Locs,
                             TE);
        } else if (auto *SI = dyn_cast<StoreInst>(&II)) {
          SILValue Mem = SI->getDest();
          SILValue UO = getUnderlyingObject(Mem);
          L.init(UO, ProjectionPath::getProjectionPath(UO, Mem));
          if (!L.isValid())
            continue;
          LSLocation::expand(L, &Fn.getModule(), TypeExpansionContext(Fn), Locs,
                             TE);
        } else {
          // Not interested in these instructions yet.
          continue;
        }

        llvm::outs() << "#" << Counter++ << II;
        for (auto &Loc : Locs) {
          Loc.print(llvm::outs());
        }
        Locs.clear();
      }
    }
    llvm::outs() << "\n";
  }

  /// Dumps the reductions of set of memory locations.
  ///
  /// This function first calls expand on a memory location. It then calls
  /// reduce, in hope to get the original memory location back.
  ///
  void printMemReduction(SILFunction &Fn) {
    LSLocation L;
    LSLocationList Locs;
    LSLocationList SLocs;
    unsigned Counter = 0;
    for (auto &BB : Fn) {
      for (auto &II : BB) {
   
        // Expand it first.
        //
        if (auto *LI = dyn_cast<LoadInst>(&II)) {
          SILValue Mem = LI->getOperand();
          SILValue UO = getUnderlyingObject(Mem);
          L.init(UO, ProjectionPath::getProjectionPath(UO, Mem));
          if (!L.isValid())
            continue;
          LSLocation::expand(L, &Fn.getModule(), TypeExpansionContext(Fn), Locs,
                             TE);
        } else if (auto *SI = dyn_cast<StoreInst>(&II)) {
          SILValue Mem = SI->getDest();
          SILValue UO = getUnderlyingObject(Mem);
          L.init(UO, ProjectionPath::getProjectionPath(UO, Mem));
          if (!L.isValid())
            continue;
          LSLocation::expand(L, &Fn.getModule(), TypeExpansionContext(Fn), Locs,
                             TE);
        } else {
          // Not interested in these instructions yet.
          continue;
        }

        // Try to reduce it.
        //
        // Reduction should not care about the order of the memory locations in
        // the set.
        for (auto I = Locs.begin(); I != Locs.end(); ++I) {
          SLocs.push_back(*I);
        }

        // This should get the original (unexpanded) location back.
        LSLocation::reduce(L, &Fn.getModule(), TypeExpansionContext(Fn), SLocs);
        llvm::outs() << "#" << Counter++ << II;
        for (auto &Loc : SLocs) {
          Loc.print(llvm::outs());
        }
        L.reset();
        Locs.clear();
        SLocs.clear();
      }
    }
    llvm::outs() << "\n";
  }

  void run() override {
    for (auto &Fn : *getModule()) {
      if (Fn.isExternalDeclaration()) continue;

      // Initialize the type expansion analysis.
      TE = PM->getAnalysis<TypeExpansionAnalysis>();

      llvm::outs() << "@" << Fn.getName() << "\n";
      switch (LSLocationKinds) {
        case MLKind::OnlyTypeExpansion:
          printTypeExpansionWithProjection(Fn);
          break;
        case MLKind::OnlyExpansion:
          printMemExpansion(Fn);
          break;
        case MLKind::OnlyReduction:
          printMemReduction(Fn);
          break;
        default:
          break;
      }
    }
  }

};

} // end anonymous namespace

SILTransform *swift::createLSLocationPrinter() {
  return new LSLocationPrinter();
}
