//===--- ConstantFolding.h - Utilities for SIL constant folding -*- C++ -*-===//
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
// This file defines utility functions for constant folding.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_CONSTANTFOLDING_H
#define SWIFT_SIL_CONSTANTFOLDING_H

#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "llvm/ADT/SetVector.h"
#include <functional>

namespace swift {

class SILOptFunctionBuilder;

/// Evaluates the constant result of a binary bit-operation.
///
/// The \p ID must be the ID of a binary bit-operation builtin.
APInt constantFoldBitOperation(APInt lhs, APInt rhs, BuiltinValueKind ID);

/// Evaluates the constant result of an integer comparison.
///
/// The \p ID must be the ID of an integer builtin operation.
APInt constantFoldComparison(APInt lhs, APInt rhs, BuiltinValueKind ID);

/// Evaluates the constant result of a binary operation with overflow.
///
/// The \p ID must be the ID of a binary operation with overflow.
APInt constantFoldBinaryWithOverflow(APInt lhs, APInt rhs, bool &Overflow,
                                     llvm::Intrinsic::ID ID);

/// Evaluates the constant result of a division operation.
///
/// The \p ID must be the ID of a division operation.
APInt constantFoldDiv(APInt lhs, APInt rhs, bool &Overflow, BuiltinValueKind ID);

  /// Evaluates the constant result of an integer cast operation.
  ///
  /// The \p ID must be the ID of a trunc/sext/zext builtin.
APInt constantFoldCast(APInt val, const BuiltinInfo &BI);

/// If `ResultsInError` is not none than errors are diagnosed and
/// `ResultsInError` is set to true in case of an error.
SILValue constantFoldBuiltin(BuiltinInst *BI,
                             llvm::Optional<bool> &ResultsInError);

/// A utility class to do constant folding.
class ConstantFolder {
private:
  SILOptFunctionBuilder &FuncBuilder;

  /// The worklist of the constants that could be folded into their users.
  llvm::SetVector<SILInstruction *> WorkList;

  /// The assert configuration of SILOptions.
  unsigned AssertConfiguration;

  /// Print diagnostics as part of mandatory constant propagation.
  bool EnableDiagnostics;

  /// Called for each constant folded instruction.
  std::function<void (SILInstruction *)> Callback;

public:
  /// The constructor.
  ///
  /// \param AssertConfiguration The assert configuration of SILOptions.
  /// \param EnableDiagnostics Print diagnostics as part of mandatory constant
  ///                          propagation.
  /// \param Callback Called for each constant folded instruction.
  ConstantFolder(SILOptFunctionBuilder &FuncBuilder,
                 unsigned AssertConfiguration,
                 bool EnableDiagnostics = false,
                 std::function<void (SILInstruction *)> Callback =
                 [](SILInstruction *){}) :
    FuncBuilder(FuncBuilder),
    AssertConfiguration(AssertConfiguration),
    EnableDiagnostics(EnableDiagnostics),
    Callback(Callback) { }

  /// Initialize the worklist with all instructions of the function \p F.
  void initializeWorklist(SILFunction &F);

  /// When asserts are enabled, dumps the worklist for diagnostic
  /// purposes. Without asserts this is a no-op.
  void dumpWorklist() const;

  /// Initialize the worklist with a single instruction \p I.
  void addToWorklist(SILInstruction *I) {
    WorkList.insert(I);
  }

  /// Constant fold everything in the worklist and transitively all uses of
  /// folded instructions.
  SILAnalysis::InvalidationKind processWorkList();
};

} // end namespace swift

#endif
