//===--- ASTBridging.h - header for the swift SILBridging module ----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_ASTBRIDGING_H
#define SWIFT_AST_ASTBRIDGING_H

#include "swift/AST/DiagnosticEngine.h"
#include "swift/Basic/BasicBridging.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/Nullability.h"
#include <stdbool.h>
#include <stddef.h>

SWIFT_BEGIN_NULLABILITY_ANNOTATIONS

//===----------------------------------------------------------------------===//
// Diagnostic Engine
//===----------------------------------------------------------------------===//

// NOTE: This must be the same underlying value as C++ 'swift::DiagID' defined
// in 'DiagnosticList.cpp'.
typedef enum ENUM_EXTENSIBILITY_ATTR(open) BridgedDiagID : uint32_t {
#define DIAG(KIND, ID, Options, Text, Signature) BridgedDiagID_##ID,
#include "swift/AST/DiagnosticsAll.def"
} BridgedDiagID;

typedef struct {
  void * _Nonnull object;
} BridgedDiagnosticEngine;

typedef struct {
  void *_Nullable object;
} BridgedOptionalDiagnosticEngine;

// FIXME: Can we bridge InFlightDiagnostic?
void DiagnosticEngine_diagnose(BridgedDiagnosticEngine, swift::SourceLoc loc,
                               BridgedDiagID diagID, BridgedArrayRef arguments,
                               swift::CharSourceRange highlight,
                               BridgedArrayRef fixIts);

bool DiagnosticEngine_hadAnyError(BridgedDiagnosticEngine);

using ArrayRefOfDiagnosticArgument = llvm::ArrayRef<swift::DiagnosticArgument>;

SWIFT_END_NULLABILITY_ANNOTATIONS

#endif // SWIFT_AST_ASTBRIDGING_H
