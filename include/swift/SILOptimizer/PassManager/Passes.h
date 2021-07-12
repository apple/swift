//===--- Passes.h - Swift Compiler SIL Pass Entrypoints ---------*- C++ -*-===//
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
//  This file declares the main entrypoints to SIL passes.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_PASSMANAGER_PASSES_H
#define SWIFT_SILOPTIMIZER_PASSMANAGER_PASSES_H

#include "swift/SIL/SILModule.h"

namespace swift {
  class SILOptions;
  class SILTransform;
  class SILModuleTransform;

  namespace irgen {
    class IRGenModule;
  }

  /// Run all the SIL diagnostic passes on \p M.
  ///
  /// \returns true if the diagnostic passes produced an error
  bool runSILDiagnosticPasses(SILModule &M);

  /// Run all the SIL performance optimization passes on \p M.
  void runSILOptimizationPasses(SILModule &M);

  /// Run all SIL passes for -Onone on module \p M.
  void runSILPassesForOnone(SILModule &M);

  /// Run the SIL lower hop-to-actor pass on \p M.
  bool runSILLowerHopToActorPass(SILModule &M);

/// Run the SIL ownership eliminator pass on \p M.
  bool runSILOwnershipEliminatorPass(SILModule &M);

  void runSILOptimizationPassesWithFileSpecification(SILModule &Module,
                                                     StringRef FileName);

  /// Detect and remove unreachable code. Diagnose provably unreachable
  /// user code.
  void performSILDiagnoseUnreachable(SILModule *M);

  /// Remove dead functions from \p M.
  void performSILDeadFunctionElimination(SILModule *M);

  /// Convert SIL to a lowered form suitable for IRGen.
  void runSILLoweringPasses(SILModule &M);

  /// Perform SIL Inst Count on M if needed.
  void performSILInstCountIfNeeded(SILModule *M);

  /// Identifiers for all passes. Used to procedurally create passes from
  /// lists of passes.
  enum class PassKind {
#define PASS(ID, TAG, NAME) ID,
#define PASS_RANGE(ID, START, END) ID##_First = START, ID##_Last = END,
#include "Passes.def"
    invalidPassKind
  };

  PassKind PassKindFromString(StringRef ID);
  StringRef PassKindID(PassKind Kind);
  StringRef PassKindTag(PassKind Kind);

#define PASS(ID, TAG, NAME) SILTransform *create##ID();
#define IRGEN_PASS(ID, TAG, NAME)
#include "Passes.def"

} // end namespace swift

#endif
