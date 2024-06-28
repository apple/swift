//===--- GenConcurrency.h - IRGen for concurrency features ------*- C++ -*-===//
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
// This file defines interfaces for emitting code for various concurrency
// features.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_GENCONCURRENCY_H
#define SWIFT_IRGEN_GENCONCURRENCY_H

#include "swift/AST/Types.h"
#include "swift/Basic/LLVM.h"
#include "swift/SIL/ApplySite.h"
#include "llvm/IR/CallingConv.h"

#include "Callee.h"
#include "GenHeap.h"
#include "IRGenModule.h"

namespace llvm {
class Value;
}

namespace swift {
class CanType;
class ProtocolConformanceRef;
class SILType;

namespace irgen {
class Explosion;
class OptionalExplosion;
class IRGenFunction;

/// Emit the buildMainActorExecutorRef builtin.
void emitBuildMainActorExecutorRef(IRGenFunction &IGF, Explosion &out);

/// Emit the buildDefaultActorExecutorRef builtin.
void emitBuildDefaultActorExecutorRef(IRGenFunction &IGF, llvm::Value *actor,
                                      Explosion &out);

/// Emit the buildOrdinaryTaskExecutorRef builtin.
void emitBuildOrdinaryTaskExecutorRef(
    IRGenFunction &IGF, llvm::Value *executor, CanType executorType,
    ProtocolConformanceRef executorConformance, Explosion &out);

/// Emit the buildOrdinarySerialExecutorRef builtin.
void emitBuildOrdinarySerialExecutorRef(IRGenFunction &IGF,
                                        llvm::Value *executor,
                                        CanType executorType,
                                        ProtocolConformanceRef executorConformance,
                                        Explosion &out);

/// Emit the buildComplexEqualitySerialExecutorRef builtin.
void emitBuildComplexEqualitySerialExecutorRef(IRGenFunction &IGF,
                                        llvm::Value *executor,
                                        CanType executorType,
                                        ProtocolConformanceRef executorConformance,
                                        Explosion &out);

/// Emit the getCurrentExecutor builtin.
void emitGetCurrentExecutor(IRGenFunction &IGF, Explosion &out);

/// Emit the createAsyncLet builtin.
llvm::Value *emitBuiltinStartAsyncLet(IRGenFunction &IGF,
                                      llvm::Value *taskOptions,
                                      llvm::Value *taskFunction,
                                      llvm::Value *localContextInfo,
                                      llvm::Value *resultBuffer,
                                      SubstitutionMap subs);

/// Emit the endAsyncLet builtin.
void emitEndAsyncLet(IRGenFunction &IGF, llvm::Value *alet);

/// Emit the createTaskGroup builtin.
llvm::Value *emitCreateTaskGroup(IRGenFunction &IGF, SubstitutionMap subs,
                                 llvm::Value *groupFlags);

/// Emit the destroyTaskGroup builtin.
void emitDestroyTaskGroup(IRGenFunction &IGF, llvm::Value *group);

void emitTaskRunInline(IRGenFunction &IGF, SubstitutionMap subs,
                       llvm::Value *result, llvm::Value *closure,
                       llvm::Value *closureContext);

void emitTaskCancel(IRGenFunction &IGF, llvm::Value *task);

llvm::Value *maybeAddEmbeddedSwiftResultTypeInfo(IRGenFunction &IGF,
                                                 llvm::Value *taskOptions,
                                                 CanType formalResultType);

/// Emit a call to swift_task_create[_f] with the given flags, options, and
/// task function.
std::pair<llvm::Value *, llvm::Value *>
emitTaskCreate(IRGenFunction &IGF, llvm::Value *flags,
               OptionalExplosion &initialExecutor,
               OptionalExplosion &taskGroup,
               OptionalExplosion &taskExecutorUnowned,
               OptionalExplosion &taskExecutorExistential,
               Explosion &taskFunction,
               SubstitutionMap subs);

} // end namespace irgen
} // end namespace swift

#endif
