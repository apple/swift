//===--- MoveOnlyUtils.h --------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_MANDATORY_MOVEONLYUTILS_H
#define SWIFT_SILOPTIMIZER_MANDATORY_MOVEONLYUTILS_H

namespace swift {

class SILFunction;
class MarkMustCheckInst;
class Operand;

namespace siloptimizer {

class DiagnosticEmitter;

bool cleanupNonCopyableCopiesAfterEmittingDiagnostic(SILFunction *fn);

/// Emit an error if we missed any copies when running markers. To check if a
/// diagnostic was emitted, use \p diagnosticEmitter.getDiagnosticCount().
void emitCheckerMissedCopyOfNonCopyableTypeErrors(
    SILFunction *fn, DiagnosticEmitter &diagnosticEmitter);

bool eliminateTemporaryAllocationsFromLet(MarkMustCheckInst *markedInst);

namespace noncopyable {

bool memInstMustConsume(Operand *memOper);
bool memInstMustReinitialize(Operand *memOper);
bool memInstMustInitialize(Operand *memOper);

} // namespace noncopyable

} // namespace siloptimizer

} // namespace swift

#endif
