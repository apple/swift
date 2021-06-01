//===--- OptimizerBridging.h - header for the OptimizerBridging module ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_OPTIMIZERBRIDGING_H
#define SWIFT_SILOPTIMIZER_OPTIMIZERBRIDGING_H

#include "../SIL/SILBridging.h"

typedef struct {
  BridgedFunction function;
  BridgedPassContext passContext;
} BridgedFunctionPassCtxt;

typedef struct {
  BridgedInstruction instruction;
  BridgedPassContext passContext;
} BridgedInstructionPassCtxt;

typedef void (*BridgedFunctionPassRunFn)(BridgedFunctionPassCtxt);
typedef void (*BridgedInstructionPassRunFn)(BridgedInstructionPassCtxt);

void SILPassManager_registerFunctionPass(BridgedStringRef name,
                                         BridgedFunctionPassRunFn runFn);

void SILCombine_registerInstructionPass(BridgedStringRef name,
                                        BridgedInstructionPassRunFn runFn);

#endif
