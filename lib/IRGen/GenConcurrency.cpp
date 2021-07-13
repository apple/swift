//===--- GenConcurrency.cpp - IRGen for concurrency features --------------===//
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
//  This file implements IR generation for concurrency features (other than
//  basic async function lowering, which is more spread out).
//
//===----------------------------------------------------------------------===//

#include "GenConcurrency.h"

#include "BitPatternBuilder.h"
#include "ExtraInhabitants.h"
#include "GenProto.h"
#include "GenType.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "LoadableTypeInfo.h"
#include "ScalarPairTypeInfo.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/ABI/MetadataValues.h"

using namespace swift;
using namespace irgen;

namespace {

/// A TypeInfo implementation for Builtin.Executor.
class ExecutorTypeInfo :
  public TrivialScalarPairTypeInfo<ExecutorTypeInfo, LoadableTypeInfo> {

public:
  ExecutorTypeInfo(llvm::StructType *storageType,
                   Size size, Alignment align, SpareBitVector &&spareBits)
      : TrivialScalarPairTypeInfo(storageType, size, std::move(spareBits),
                                  align, IsPOD, IsFixedSize) {}

  static Size getFirstElementSize(IRGenModule &IGM) {
    return IGM.getPointerSize();
  }
  static StringRef getFirstElementLabel() {
    return ".identity";
  }

  TypeLayoutEntry *buildTypeLayoutEntry(IRGenModule &IGM,
                                        SILType T) const override {
    return IGM.typeLayoutCache.getOrCreateScalarEntry(*this, T,
                                                      ScalarKind::POD);
  }

  static Size getSecondElementOffset(IRGenModule &IGM) {
    return IGM.getPointerSize();
  }
  static Size getSecondElementSize(IRGenModule &IGM) {
    return IGM.getPointerSize();
  }
  static StringRef getSecondElementLabel() {
    return ".impl";
  }

  // The identity pointer is a heap object reference.
  bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
    return true;
  }
  PointerInfo getPointerInfo(IRGenModule &IGM) const {
    return PointerInfo::forHeapObject(IGM);
  }
  unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
    return getPointerInfo(IGM).getExtraInhabitantCount(IGM);
  }
  APInt getFixedExtraInhabitantValue(IRGenModule &IGM,
                                     unsigned bits,
                                     unsigned index) const override {
    return getPointerInfo(IGM)
          .getFixedExtraInhabitantValue(IGM, bits, index, 0);
  }
  llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF, Address src,
                                       SILType T,
                                       bool isOutlined) const override {
    src = projectFirstElement(IGF, src);
    return getPointerInfo(IGF.IGM).getExtraInhabitantIndex(IGF, src);
  }
  void storeExtraInhabitant(IRGenFunction &IGF, llvm::Value *index,
                            Address dest, SILType T,
                            bool isOutlined) const override {
    // Store the extra-inhabitant value in the first (identity) word.
    auto first = projectFirstElement(IGF, dest);
    getPointerInfo(IGF.IGM).storeExtraInhabitant(IGF, index, first);

    // Zero the second word.
    auto second = projectSecondElement(IGF, dest);
    IGF.Builder.CreateStore(llvm::ConstantInt::get(IGF.IGM.ExecutorSecondTy, 0),
                            second);
  }
};

} // end anonymous namespace

const LoadableTypeInfo &IRGenModule::getExecutorTypeInfo() {
  return Types.getExecutorTypeInfo();
}

const LoadableTypeInfo &TypeConverter::getExecutorTypeInfo() {
  if (ExecutorTI) return *ExecutorTI;

  auto ty = IGM.SwiftExecutorTy;

  SpareBitVector spareBits;
  spareBits.append(IGM.getHeapObjectSpareBits());
  spareBits.appendClearBits(IGM.getPointerSize().getValueInBits());

  ExecutorTI =
    new ExecutorTypeInfo(ty, IGM.getPointerSize() * 2,
                         IGM.getPointerAlignment(),
                         std::move(spareBits));
  ExecutorTI->NextConverted = FirstType;
  FirstType = ExecutorTI;
  return *ExecutorTI;
}

void irgen::emitBuildMainActorExecutorRef(IRGenFunction &IGF,
                                          Explosion &out) {
  auto call = IGF.Builder.CreateCall(IGF.IGM.getTaskGetMainExecutorFn(),
                                     {});
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);

  IGF.emitAllExtractValues(call, IGF.IGM.SwiftExecutorTy, out);
}

void irgen::emitBuildDefaultActorExecutorRef(IRGenFunction &IGF,
                                             llvm::Value *actor,
                                             Explosion &out) {
  // The implementation word of a default actor is just a null pointer.
  llvm::Value *identity =
    IGF.Builder.CreatePtrToInt(actor, IGF.IGM.ExecutorFirstTy);
  llvm::Value *impl = llvm::ConstantInt::get(IGF.IGM.ExecutorSecondTy, 0);

  out.add(identity);
  out.add(impl);
}

void irgen::emitBuildOrdinarySerialExecutorRef(IRGenFunction &IGF,
                                               llvm::Value *executor,
                                               CanType executorType,
                                       ProtocolConformanceRef executorConf,
                                               Explosion &out) {
  // The implementation word of an "ordinary" serial executor is
  // just the witness table pointer with no flags set.
  llvm::Value *identity =
    IGF.Builder.CreatePtrToInt(executor, IGF.IGM.ExecutorFirstTy);
  llvm::Value *impl =
    emitWitnessTableRef(IGF, executorType, executorConf);
  impl = IGF.Builder.CreatePtrToInt(impl, IGF.IGM.ExecutorSecondTy);

  out.add(identity);
  out.add(impl);
}

void irgen::emitGetCurrentExecutor(IRGenFunction &IGF, Explosion &out) {
  auto *call = IGF.Builder.CreateCall(IGF.IGM.getTaskGetCurrentExecutorFn(),
                                      {});
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);

  IGF.emitAllExtractValues(call, IGF.IGM.SwiftExecutorTy, out);
}

llvm::Value *irgen::emitBuiltinStartAsyncLet(IRGenFunction &IGF,
                                             llvm::Value *taskOptions,
                                             llvm::Value *taskFunction,
                                             llvm::Value *localContextInfo,
                                             SubstitutionMap subs) {
  // stack allocate AsyncLet, and begin lifetime for it (until EndAsyncLet)
  auto ty = llvm::ArrayType::get(IGF.IGM.Int8PtrTy, NumWords_AsyncLet);
  auto address = IGF.createAlloca(ty, Alignment(Alignment_AsyncLet));
  auto alet = IGF.Builder.CreateBitCast(address.getAddress(),
                                        IGF.IGM.Int8PtrTy);
  IGF.Builder.CreateLifetimeStart(alet);

  assert(subs.getReplacementTypes().size() == 1 &&
         "startAsyncLet should have a type substitution");
  auto futureResultType = subs.getReplacementTypes()[0]->getCanonicalType();
  auto futureResultTypeMetadata = IGF.emitAbstractTypeMetadataRef(futureResultType);

  // This is @_silgen_name("swift_asyncLet_start")
  auto *call = IGF.Builder.CreateCall(IGF.IGM.getAsyncLetStartFn(),
                                      {alet,
                                       taskOptions,
                                       futureResultTypeMetadata,
                                       taskFunction,
                                       localContextInfo
                                      });
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);

  return alet;
}

void irgen::emitEndAsyncLet(IRGenFunction &IGF, llvm::Value *alet) {
  auto *call = IGF.Builder.CreateCall(IGF.IGM.getEndAsyncLetFn(),
                                      {alet});
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);

  IGF.Builder.CreateLifetimeEnd(alet);
}

llvm::Value *irgen::emitCreateTaskGroup(IRGenFunction &IGF,
                                        SubstitutionMap subs) {
  auto ty = llvm::ArrayType::get(IGF.IGM.Int8PtrTy, NumWords_TaskGroup);
  auto address = IGF.createAlloca(ty, Alignment(Alignment_TaskGroup));
  auto group = IGF.Builder.CreateBitCast(address.getAddress(),
                                         IGF.IGM.Int8PtrTy);
  IGF.Builder.CreateLifetimeStart(group);
  assert(subs.getReplacementTypes().size() == 1 &&
         "createTaskGroup should have a type substitution");
  auto resultType = subs.getReplacementTypes()[0]->getCanonicalType();
  auto resultTypeMetadata = IGF.emitAbstractTypeMetadataRef(resultType);

  auto *call = IGF.Builder.CreateCall(IGF.IGM.getTaskGroupInitializeFn(),
                                      {group, resultTypeMetadata});
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);

  return group;
}

void irgen::emitDestroyTaskGroup(IRGenFunction &IGF, llvm::Value *group) {
  auto *call = IGF.Builder.CreateCall(IGF.IGM.getTaskGroupDestroyFn(),
                                      {group});
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);

  IGF.Builder.CreateLifetimeEnd(group);
}
