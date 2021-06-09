//===--- SILBridging.h - header for the swift SILBridging module ----------===//
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

#ifndef SWIFT_SIL_SILBRIDGING_H
#define SWIFT_SIL_SILBRIDGING_H

#include "BridgedSwiftObject.h"
#include <stddef.h>

typedef struct {
  const unsigned char * _Nullable data;
  size_t length;
} BridgedStringRef;

typedef struct {
  const unsigned char * _Nonnull data;
  size_t numOperands;
} BridgedOperandArray;

enum {
  BridgedOperandSize = 4 * sizeof(uintptr_t)
};

typedef struct {
  void * _Nullable data;
} BridgedSlab;

enum {
  BridgedSlabCapacity = 64 * sizeof(uintptr_t)
};

enum ChangeNotificationKind {
  instructionsChanged,
  callsChanged,
  branchesChanged
};

typedef struct {
  const void * _Nonnull opaqueCtxt;
} BridgedPassContext;

typedef struct {
  void * _Null_unspecified word0;
  void * _Null_unspecified word1;
  void * _Null_unspecified word2;
} BridgedLocation;

typedef struct {
  void * _Nullable typePtr;
} BridgedType;

typedef struct {
  const void * _Nullable data;
  size_t count;
} BridgedValueArray;

typedef struct {
  const void * _Nonnull op;
} BridgedOperand;

typedef struct {
  const void * _Nullable op;
} OptionalBridgedOperand;

typedef struct {
  SwiftObject obj;
} BridgedFunction;

typedef struct {
  SwiftObject obj;
} BridgedGlobalVar;

typedef struct {
  SwiftObject obj;
} BridgedBasicBlock;

typedef struct {
  OptionalSwiftObject obj;
} OptionalBridgedBasicBlock;

typedef struct {
  SwiftObject obj;
} BridgedArgument;

typedef struct {
  OptionalSwiftObject obj;
} OptionalBridgedArgument;

typedef struct {
  SwiftObject obj;
} BridgedNode;

typedef struct {
  SwiftObject obj;
} BridgedValue;

typedef struct {
  SwiftObject obj;
} BridgedInstruction;

typedef struct {
  OptionalSwiftObject obj;
} OptionalBridgedInstruction;

typedef struct {
  SwiftObject obj;
} BridgedMultiValueResult;

typedef long SwiftInt;

void registerBridgedClass(BridgedStringRef className, SwiftMetatype metatype);

void freeBridgedStringRef(BridgedStringRef str);

void PassContext_notifyChanges(BridgedPassContext passContext,
                               enum ChangeNotificationKind changeKind);
void PassContext_eraseInstruction(BridgedPassContext passContext,
                                  BridgedInstruction inst);
BridgedSlab PassContext_getNextSlab(BridgedSlab slab);
BridgedSlab PassContext_allocSlab(BridgedPassContext passContext,
                                  BridgedSlab afterSlab);
BridgedSlab PassContext_freeSlab(BridgedPassContext passContext,
                                 BridgedSlab slab);

BridgedStringRef SILFunction_getName(BridgedFunction function);
BridgedStringRef SILFunction_debugDescription(BridgedFunction function);
OptionalBridgedBasicBlock SILFunction_firstBlock(BridgedFunction function);
OptionalBridgedBasicBlock SILFunction_lastBlock(BridgedFunction function);

BridgedStringRef SILGlobalVariable_getName(BridgedGlobalVar global);
BridgedStringRef SILGlobalVariable_debugDescription(BridgedGlobalVar global);

OptionalBridgedBasicBlock SILBasicBlock_next(BridgedBasicBlock block);
OptionalBridgedBasicBlock SILBasicBlock_previous(BridgedBasicBlock block);
BridgedStringRef SILBasicBlock_debugDescription(BridgedBasicBlock block);
OptionalBridgedInstruction SILBasicBlock_firstInst(BridgedBasicBlock block);
OptionalBridgedInstruction SILBasicBlock_lastInst(BridgedBasicBlock block);
SwiftInt SILBasicBlock_getNumArguments(BridgedBasicBlock block);
BridgedArgument SILBasicBlock_getArgument(BridgedBasicBlock block, SwiftInt index);

BridgedValue Operand_getValue(BridgedOperand);
OptionalBridgedOperand Operand_nextUse(BridgedOperand);
BridgedInstruction Operand_getUser(BridgedOperand);

BridgedStringRef SILNode_debugDescription(BridgedNode node);
OptionalBridgedOperand SILValue_firstUse(BridgedValue value);
BridgedType SILValue_getType(BridgedValue value);

BridgedBasicBlock SILArgument_getParent(BridgedArgument argument);

OptionalBridgedInstruction SILInstruction_next(BridgedInstruction inst);
OptionalBridgedInstruction SILInstruction_previous(BridgedInstruction inst);
BridgedBasicBlock SILInstruction_getParent(BridgedInstruction inst);
BridgedOperandArray SILInstruction_getOperands(BridgedInstruction inst);
BridgedLocation SILInstruction_getLocation(BridgedInstruction inst);
int SILInstruction_mayHaveSideEffects(BridgedInstruction inst);
int SILInstruction_mayReadFromMemory(BridgedInstruction inst);
int SILInstruction_mayWriteToMemory(BridgedInstruction inst);
int SILInstruction_mayReadOrWriteMemory(BridgedInstruction inst);

BridgedInstruction MultiValueInstResult_getParent(BridgedMultiValueResult result);
SwiftInt MultipleValueInstruction_getNumResults(BridgedInstruction inst);
BridgedMultiValueResult
  MultipleValueInstruction_getResult(BridgedInstruction inst, SwiftInt index);

BridgedStringRef CondFailInst_getMessage(BridgedInstruction cfi);
BridgedGlobalVar GlobalAccessInst_getGlobal(BridgedInstruction globalInst);

BridgedInstruction SILBuilder_createBuiltinBinaryFunction(
          BridgedInstruction insertionPoint,
          BridgedLocation loc, BridgedStringRef name,
          BridgedType operandType, BridgedType resultType, BridgedValueArray arguments);
BridgedInstruction SILBuilder_createCondFail(BridgedInstruction insertionPoint,
          BridgedLocation loc, BridgedValue condition, BridgedStringRef messge);

#endif
