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
#include "GenCall.h"
#include "GenProto.h"
#include "GenType.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "LoadableTypeInfo.h"
#include "ScalarPairTypeInfo.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/Basic/Assertions.h"

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
                                  align, IsTriviallyDestroyable,
                                  IsCopyable, IsFixedSize) {}

  static Size getFirstElementSize(IRGenModule &IGM) {
    return IGM.getPointerSize();
  }
  static StringRef getFirstElementLabel() {
    return ".identity";
  }

  TypeLayoutEntry
  *buildTypeLayoutEntry(IRGenModule &IGM,
                        SILType T,
                        bool useStructLayouts) const override {
    if (!useStructLayouts) {
      return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
    }
    return IGM.typeLayoutCache.getOrCreateScalarEntry(*this, T,
                                            ScalarKind::TriviallyDestroyable);
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
  auto call = IGF.Builder.CreateCall(
      IGF.IGM.getTaskGetMainExecutorFunctionPointer(), {});
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

void irgen::emitBuildOrdinaryTaskExecutorRef(
    IRGenFunction &IGF, llvm::Value *executor, CanType executorType,
    ProtocolConformanceRef executorConf, Explosion &out) {
  // The implementation word of an "ordinary" executor is
  // just the witness table pointer with no flags set.
  llvm::Value *identity =
      IGF.Builder.CreatePtrToInt(executor, IGF.IGM.ExecutorFirstTy);
  llvm::Value *impl = emitWitnessTableRef(IGF, executorType, executorConf);
  impl = IGF.Builder.CreatePtrToInt(impl, IGF.IGM.ExecutorSecondTy);

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

void irgen::emitBuildComplexEqualitySerialExecutorRef(IRGenFunction &IGF,
                                               llvm::Value *executor,
                                               CanType executorType,
                                               ProtocolConformanceRef executorConf,
                                               Explosion &out) {
  llvm::Value *identity =
    IGF.Builder.CreatePtrToInt(executor, IGF.IGM.ExecutorFirstTy);

  // The implementation word of an "complex equality" serial executor is
  // the witness table pointer with the ExecutorKind::ComplexEquality flag set.
  llvm::Value *impl =
    emitWitnessTableRef(IGF, executorType, executorConf);
  impl = IGF.Builder.CreatePtrToInt(impl, IGF.IGM.ExecutorSecondTy);

  // NOTE: Refer to SerialExecutorRef::ExecutorKind for the flag values.
  llvm::IntegerType *IntPtrTy = IGF.IGM.IntPtrTy;
  auto complexEqualityExecutorKindFlag =
      llvm::Constant::getIntegerValue(IntPtrTy, APInt(IntPtrTy->getBitWidth(),
                                                      0b01));
  impl = IGF.Builder.CreateOr(impl, complexEqualityExecutorKindFlag);

  out.add(identity);
  out.add(impl);
}

void irgen::emitGetCurrentExecutor(IRGenFunction &IGF, Explosion &out) {
  auto *call = IGF.Builder.CreateCall(
      IGF.IGM.getTaskGetCurrentExecutorFunctionPointer(), {});
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);

  IGF.emitAllExtractValues(call, IGF.IGM.SwiftExecutorTy, out);
}

llvm::Value *irgen::emitBuiltinStartAsyncLet(IRGenFunction &IGF,
                                             llvm::Value *taskOptions,
                                             llvm::Value *taskFunction,
                                             llvm::Value *localContextInfo,
                                             llvm::Value *localResultBuffer,
                                             SubstitutionMap subs) {
  localContextInfo = IGF.Builder.CreateBitCast(localContextInfo,
                                               IGF.IGM.OpaquePtrTy);
  
  // stack allocate AsyncLet, and begin lifetime for it (until EndAsyncLet)
  auto ty = llvm::ArrayType::get(IGF.IGM.Int8PtrTy, NumWords_AsyncLet);
  auto address = IGF.createAlloca(ty, Alignment(Alignment_AsyncLet));
  auto alet = IGF.Builder.CreateBitCast(address.getAddress(),
                                        IGF.IGM.Int8PtrTy);
  IGF.Builder.CreateLifetimeStart(alet);

  assert(subs.getReplacementTypes().size() == 1 &&
         "startAsyncLet should have a type substitution");
  auto futureResultType = subs.getReplacementTypes()[0]->getCanonicalType();

  llvm::Value *futureResultTypeMetadata =
      llvm::ConstantPointerNull::get(IGF.IGM.Int8PtrTy);
  if (!IGF.IGM.Context.LangOpts.hasFeature(Feature::Embedded)) {
    futureResultTypeMetadata =
        IGF.emitAbstractTypeMetadataRef(futureResultType);
  }

  // The concurrency runtime for older Apple OSes has a bug in task formation
  // for `async let`s that may manifest when trying to use room in the
  // parent task's preallocated `async let` buffer for the child task's
  // initial task allocator slab. If targeting those older OSes, pad the
  // context size for async let entry points to never fit in the preallocated
  // space, so that we don't run into that bug. We leave a note on the
  // declaration so that coroutine splitting can pad out the final context
  // size after splitting.
  auto deploymentAvailability
    = AvailabilityContext::forDeploymentTarget(IGF.IGM.Context);
  if (!deploymentAvailability.isContainedIn(
                                   IGF.IGM.Context.getSwift57Availability()))
  {
    auto taskAsyncFunctionPointer
                = cast<llvm::GlobalVariable>(taskFunction->stripPointerCasts());

    if (auto taskAsyncID
          = IGF.IGM.getAsyncCoroIDMapping(taskAsyncFunctionPointer)) {
      // If the entry point function has already been emitted, retroactively
      // pad out the initial context size in the async function pointer record
      // and ID intrinsic so that it will never fit in the preallocated space.
      uint64_t origSize = cast<llvm::ConstantInt>(taskAsyncID->getArgOperand(0))
        ->getValue().getLimitedValue();
      
      uint64_t paddedSize = std::max(origSize,
                     (NumWords_AsyncLet * IGF.IGM.getPointerSize()).getValue());
      auto paddedSizeVal = llvm::ConstantInt::get(IGF.IGM.Int32Ty, paddedSize);
      taskAsyncID->setArgOperand(0, paddedSizeVal);
      
      auto origInit = taskAsyncFunctionPointer->getInitializer();
      auto newInit = llvm::ConstantStruct::get(
                                   cast<llvm::StructType>(origInit->getType()),
                                   origInit->getAggregateElement(0u),
                                   paddedSizeVal);
      taskAsyncFunctionPointer->setInitializer(newInit);
    } else {
      // If it hasn't been emitted yet, mark it to get the padding when it does
      // get emitted.
      IGF.IGM.markAsyncFunctionPointerForPadding(taskAsyncFunctionPointer);
    }
  }

  // In embedded Swift, create and pass result type info.
  taskOptions =
    maybeAddEmbeddedSwiftResultTypeInfo(IGF, taskOptions, futureResultType);
  
  llvm::CallInst *call;
  if (localResultBuffer) {
    // This is @_silgen_name("swift_asyncLet_begin")
    call = IGF.Builder.CreateCall(IGF.IGM.getAsyncLetBeginFunctionPointer(),
                                  {alet, taskOptions, futureResultTypeMetadata,
                                   taskFunction, localContextInfo,
                                   localResultBuffer});
  } else {
    // This is @_silgen_name("swift_asyncLet_start")
    call = IGF.Builder.CreateCall(IGF.IGM.getAsyncLetStartFunctionPointer(),
                                  {alet, taskOptions, futureResultTypeMetadata,
                                   taskFunction, localContextInfo});
  }
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);

  return alet;
}

void irgen::emitEndAsyncLet(IRGenFunction &IGF, llvm::Value *alet) {
  auto *call =
      IGF.Builder.CreateCall(IGF.IGM.getEndAsyncLetFunctionPointer(), {alet});
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);

  IGF.Builder.CreateLifetimeEnd(alet);
}

llvm::Value *irgen::emitCreateTaskGroup(IRGenFunction &IGF,
                                        SubstitutionMap subs,
                                        llvm::Value *groupFlags) {
  auto ty = llvm::ArrayType::get(IGF.IGM.Int8PtrTy, NumWords_TaskGroup);
  auto address = IGF.createAlloca(ty, Alignment(Alignment_TaskGroup));
  auto group = IGF.Builder.CreateBitCast(address.getAddress(),
                                         IGF.IGM.Int8PtrTy);
  IGF.Builder.CreateLifetimeStart(group);
  assert(subs.getReplacementTypes().size() == 1 &&
         "createTaskGroup should have a type substitution");
  auto resultType = subs.getReplacementTypes()[0]->getCanonicalType();
  auto resultTypeMetadata = IGF.emitAbstractTypeMetadataRef(resultType);

  llvm::CallInst *call;
  if (groupFlags) {
    call = IGF.Builder.CreateCall(IGF.IGM.getTaskGroupInitializeWithFlagsFunctionPointer(),
                                  {groupFlags, group, resultTypeMetadata});
  } else {
    call = IGF.Builder.CreateCall(IGF.IGM.getTaskGroupInitializeFunctionPointer(),
                                  {group, resultTypeMetadata});
  }
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);

  return group;
}

void irgen::emitDestroyTaskGroup(IRGenFunction &IGF, llvm::Value *group) {
  auto *call = IGF.Builder.CreateCall(
      IGF.IGM.getTaskGroupDestroyFunctionPointer(), {group});
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);

  IGF.Builder.CreateLifetimeEnd(group);
}

llvm::Function *IRGenModule::getAwaitAsyncContinuationFn() {
  StringRef name = "__swift_continuation_await_point";
  if (llvm::GlobalValue *F = Module.getNamedValue(name))
    return cast<llvm::Function>(F);

  // The parameters here match the extra arguments passed to
  // @llvm.coro.suspend.async by emitAwaitAsyncContinuation.
  llvm::Type *argTys[] = { ContinuationAsyncContextPtrTy };
  auto *suspendFnTy =
    llvm::FunctionType::get(VoidTy, argTys, false /*vaargs*/);

  llvm::Function *suspendFn =
      llvm::Function::Create(suspendFnTy, llvm::Function::InternalLinkage,
                             name, &Module);
  suspendFn->setCallingConv(SwiftAsyncCC);
  suspendFn->setDoesNotThrow();
  IRGenFunction suspendIGF(*this, suspendFn);
  if (DebugInfo)
    DebugInfo->emitArtificialFunction(suspendIGF, suspendFn);
  auto &Builder = suspendIGF.Builder;

  llvm::Value *context = suspendFn->getArg(0);
  auto *call =
      Builder.CreateCall(getContinuationAwaitFunctionPointer(), {context});
  call->setCallingConv(SwiftAsyncCC);
  call->setDoesNotThrow();
  call->setTailCallKind(AsyncTailCallKind);

  Builder.CreateRetVoid();
  return suspendFn;
}

void irgen::emitTaskRunInline(IRGenFunction &IGF, SubstitutionMap subs,
                              llvm::Value *result, llvm::Value *closure,
                              llvm::Value *closureContext) {
  assert(subs.getReplacementTypes().size() == 1 &&
         "taskRunInline should have a type substitution");
  auto resultType = subs.getReplacementTypes()[0]->getCanonicalType();
  auto resultTypeMetadata = IGF.emitAbstractTypeMetadataRef(resultType);

  auto *call = IGF.Builder.CreateCall(
      IGF.IGM.getTaskRunInlineFunctionPointer(),
      {result, closure, closureContext, resultTypeMetadata});
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);
}


void irgen::emitTaskCancel(IRGenFunction &IGF, llvm::Value *task) {
  if (task->getType() != IGF.IGM.SwiftTaskPtrTy) {
    task = IGF.Builder.CreateBitCast(task, IGF.IGM.SwiftTaskPtrTy);
  }

  auto *call =
      IGF.Builder.CreateCall(IGF.IGM.getTaskCancelFunctionPointer(), {task});
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);
}

template <class RecordTraits>
static Address allocateOptionRecord(IRGenFunction &IGF,
                                    const RecordTraits &traits) {
  return IGF.createAlloca(RecordTraits::getRecordType(IGF.IGM),
                          IGF.IGM.getPointerAlignment(),
                          traits.getLabel() + "_record");
}

static void initializeOptionRecordHeader(IRGenFunction &IGF,
                                         Address recordAddr,
                                         TaskOptionRecordFlags flags,
                                         llvm::Value *curRecordPointer) {
  auto baseRecordAddr =
    IGF.Builder.CreateStructGEP(recordAddr, 0, Size(0));

  // Flags
  auto flagsValue =
    llvm::ConstantInt::get(IGF.IGM.SizeTy, flags.getOpaqueValue());
  IGF.Builder.CreateStore(flagsValue,
    IGF.Builder.CreateStructGEP(baseRecordAddr, 0, Size(0)));

  // Parent
  IGF.Builder.CreateStore(curRecordPointer,
    IGF.Builder.CreateStructGEP(baseRecordAddr, 1, IGF.IGM.getPointerSize()));
}


template <class RecordTraits, class... Args>
static llvm::Value *initializeOptionRecord(IRGenFunction &IGF,
                                           Address recordAddr,
                                           llvm::Value *curRecordPointer,
                                           const RecordTraits &traits,
                                           Args &&... args) {
  initializeOptionRecordHeader(IGF, recordAddr, traits.getRecordFlags(),
                               curRecordPointer);

  traits.initialize(IGF, recordAddr, std::forward<Args>(args)...);

  llvm::Value *newRecordPointer = IGF.Builder.CreateBitOrPointerCast(
      recordAddr.getAddress(), IGF.IGM.SwiftTaskOptionRecordPtrTy);
  return newRecordPointer;
}

template <class RecordTraits, class... Args>
static llvm::Value *addOptionRecord(IRGenFunction &IGF,
                                    llvm::Value *curRecordPointer,
                                    const RecordTraits &traits,
                                    Args &&... args) {
  auto recordAddr = allocateOptionRecord(IGF, traits);
  return initializeOptionRecord(IGF, recordAddr, curRecordPointer, traits,
                                std::forward<Args>(args)...);
}

/// Add a task option record to the options list if the given value
/// is present.
template <class RecordTraits>
static llvm::Value *maybeAddOptionRecord(IRGenFunction &IGF,
                                         llvm::Value *curRecordPointer,
                                         const RecordTraits &traits,
                                         OptionalExplosion &value) {
  // We can completely avoid doing any work if the value is statically nil.
  if (value.isNone()) return curRecordPointer;

  // Otherwise, allocate the option record.
  auto recordAddr = allocateOptionRecord(IGF, traits);

  // If the value is statically non-nil, we can unconditionally
  // initialize the record and add it to the chain.
  if (value.isSome()) {
    return initializeOptionRecord(IGF, recordAddr, curRecordPointer,
                                  traits, value.getSomeExplosion());
  }

  // Otherwise, we have to check whether the value is nil dynamically.
  llvm::BasicBlock *contBB = IGF.createBasicBlock(traits.getLabel() + ".cont");
  llvm::BasicBlock *someBB = IGF.createBasicBlock(traits.getLabel() + ".some");

  auto &ctx = IGF.IGM.Context;

  SILType optionalType =
    IGF.IGM.getLoweredType(traits.getValueType(ctx).wrapInOptionalType());
  auto &optionalStrategy = getEnumImplStrategy(IGF.IGM, optionalType);

  // Branch based on whether the value is nil.  We're going to use the
  // value twice, so borrow it the first time.
  value.getOptionalExplosion().borrowing([&](Explosion &borrowedValue) {
    optionalStrategy.emitValueSwitch(IGF, borrowedValue,
                                     {{ctx.getOptionalSomeDecl(), someBB},
                                      {ctx.getOptionalNoneDecl(), contBB}},
                                     /*default*/ nullptr);
  });
  auto noneOriginBB = IGF.Builder.GetInsertBlock();

  // Enter the block for the case where the value is non-nil.
  IGF.Builder.emitBlock(someBB);

  // Project the payload from the optional value.
  Explosion objectValue;
  optionalStrategy.emitValueProject(IGF, value.getOptionalExplosion(),
                                    ctx.getOptionalSomeDecl(),
                                    objectValue);

  // Initialize the record.
  llvm::Value *someRecordPointer =
    initializeOptionRecord(IGF, recordAddr, curRecordPointer,
                           traits, objectValue);

  auto someOriginBB = IGF.Builder.GetInsertBlock();
  IGF.Builder.CreateBr(contBB);

  // Enter the continuation block and create a phi to merge the two cases.
  IGF.Builder.emitBlock(contBB);
  auto recordPointerPHI =
    IGF.Builder.CreatePHI(IGF.IGM.SwiftTaskOptionRecordPtrTy, /*num cases*/ 2);
  recordPointerPHI->addIncoming(curRecordPointer, noneOriginBB);
  recordPointerPHI->addIncoming(someRecordPointer, someOriginBB);

  return recordPointerPHI;
}

namespace {
struct EmbeddedSwiftResultTypeOptionRecordTraits {
  CanType formalResultType;

  static StringRef getLabel() {
    return "result_type_info";
  }
  static llvm::StructType *getRecordType(IRGenModule &IGM) {
    return IGM.SwiftResultTypeInfoTaskOptionRecordTy;
  }
  static TaskOptionRecordFlags getRecordFlags() {
    return TaskOptionRecordFlags(TaskOptionRecordKind::ResultTypeInfo);
  }

  void initialize(IRGenFunction &IGF, Address optionsRecord) const {
    SILType lowered = IGF.IGM.getLoweredType(formalResultType);
    const TypeInfo &TI = IGF.IGM.getTypeInfo(lowered);
    CanType canType = lowered.getASTType();
    FixedPacking packing = TI.getFixedPacking(IGF.IGM);

    // Size
    IGF.Builder.CreateStore(
        TI.getStaticSize(IGF.IGM),
        IGF.Builder.CreateStructGEP(optionsRecord, 1, Size()));
    // Align mask
    IGF.Builder.CreateStore(
        TI.getStaticAlignmentMask(IGF.IGM),
        IGF.Builder.CreateStructGEP(optionsRecord, 2, Size()));
    // initializeWithCopy witness
    IGF.Builder.CreateStore(
        IGF.IGM.getOrCreateValueWitnessFunction(
            ValueWitness::InitializeWithCopy, packing, canType, lowered, TI),
        IGF.Builder.CreateStructGEP(optionsRecord, 3, Size()));
    // storeEnumTagSinglePayload witness
    IGF.Builder.CreateStore(
        IGF.IGM.getOrCreateValueWitnessFunction(
            ValueWitness::StoreEnumTagSinglePayload, packing, canType, lowered,
            TI),
        IGF.Builder.CreateStructGEP(optionsRecord, 4, Size()));
    // destroy witness
    IGF.Builder.CreateStore(
        IGF.IGM.getOrCreateValueWitnessFunction(ValueWitness::Destroy, packing,
                                                canType, lowered, TI),
        IGF.Builder.CreateStructGEP(optionsRecord, 5, Size()));
  }
};
} // end anonymous namespace

llvm::Value *irgen::maybeAddEmbeddedSwiftResultTypeInfo(IRGenFunction &IGF,
                                                        llvm::Value *taskOptions,
                                                        CanType formalResultType) {
  if (!IGF.IGM.Context.LangOpts.hasFeature(Feature::Embedded))
    return taskOptions;

  EmbeddedSwiftResultTypeOptionRecordTraits traits{formalResultType};
  return addOptionRecord(IGF, taskOptions, traits);
}

namespace {

struct InitialSerialExecutorRecordTraits {
  static StringRef getLabel() {
    return "initial_serial_executor";
  }
  static llvm::StructType *getRecordType(IRGenModule &IGM) {
    return IGM.SwiftInitialSerialExecutorTaskOptionRecordTy;
  }
  static TaskOptionRecordFlags getRecordFlags() {
    return TaskOptionRecordFlags(TaskOptionRecordKind::InitialSerialExecutor);
  }
  static CanType getValueType(ASTContext &ctx) {
    return ctx.TheExecutorType;
  }

  void initialize(IRGenFunction &IGF, Address recordAddr,
                  Explosion &serialExecutor) const {
    auto executorRecord =
      IGF.Builder.CreateStructGEP(recordAddr, 1, 2 * IGF.IGM.getPointerSize());
    IGF.Builder.CreateStore(serialExecutor.claimNext(),
      IGF.Builder.CreateStructGEP(executorRecord, 0, Size()));
    IGF.Builder.CreateStore(serialExecutor.claimNext(),
      IGF.Builder.CreateStructGEP(executorRecord, 1, Size()));
  }
};

struct TaskGroupRecordTraits {
  static StringRef getLabel() {
    return "task_group";
  }
  static llvm::StructType *getRecordType(IRGenModule &IGM) {
    return IGM.SwiftTaskGroupTaskOptionRecordTy;
  }
  static TaskOptionRecordFlags getRecordFlags() {
    return TaskOptionRecordFlags(TaskOptionRecordKind::TaskGroup);
  }
  static CanType getValueType(ASTContext &ctx) {
    return ctx.TheRawPointerType;
  }

  void initialize(IRGenFunction &IGF, Address recordAddr,
                  Explosion &taskGroup) const {
    IGF.Builder.CreateStore(
        taskGroup.claimNext(),
        IGF.Builder.CreateStructGEP(recordAddr, 1, 2 * IGF.IGM.getPointerSize()));
  }
};

struct InitialTaskExecutorUnownedRecordTraits {
  static StringRef getLabel() {
    return "task_executor_unowned";
  }
  static llvm::StructType *getRecordType(IRGenModule &IGM) {
    return IGM.SwiftInitialTaskExecutorUnownedPreferenceTaskOptionRecordTy;
  }
  static TaskOptionRecordFlags getRecordFlags() {
    return TaskOptionRecordFlags(TaskOptionRecordKind::InitialTaskExecutorUnowned);
  }
  static CanType getValueType(ASTContext &ctx) {
    return ctx.TheExecutorType;
  }

  void initialize(IRGenFunction &IGF, Address recordAddr,
                  Explosion &taskExecutor) const {
    auto executorRecord =
      IGF.Builder.CreateStructGEP(recordAddr, 1, 2 * IGF.IGM.getPointerSize());
    IGF.Builder.CreateStore(taskExecutor.claimNext(),
      IGF.Builder.CreateStructGEP(executorRecord, 0, Size()));
    IGF.Builder.CreateStore(taskExecutor.claimNext(),
      IGF.Builder.CreateStructGEP(executorRecord, 1, Size()));
  }
};

struct InitialTaskExecutorOwnedRecordTraits {
  static StringRef getLabel() {
    return "task_executor_owned";
  }
  static llvm::StructType *getRecordType(IRGenModule &IGM) {
    return IGM.SwiftInitialTaskExecutorOwnedPreferenceTaskOptionRecordTy;
  }
  static TaskOptionRecordFlags getRecordFlags() {
    return TaskOptionRecordFlags(TaskOptionRecordKind::InitialTaskExecutorOwned);
  }
  static CanType getValueType(ASTContext &ctx) {
    return OptionalType::get(ctx.getProtocol(KnownProtocolKind::TaskExecutor)
                                 ->getDeclaredInterfaceType())
        ->getCanonicalType();
  }

  void initialize(IRGenFunction &IGF, Address recordAddr,
                  Explosion &taskExecutor) const {
    auto executorRecord =
      IGF.Builder.CreateStructGEP(recordAddr, 1, 2 * IGF.IGM.getPointerSize());

    // This relies on the fact that the HeapObject is directly followed by a
    // pointer to the witness table.
    IGF.Builder.CreateStore(taskExecutor.claimNext(),
        IGF.Builder.CreateStructGEP(executorRecord, 0, Size()));
    IGF.Builder.CreateStore(taskExecutor.claimNext(),
        IGF.Builder.CreateStructGEP(executorRecord, 1, Size()));
  }
};

} // end anonymous namespace

static llvm::Value *
maybeAddInitialSerialExecutorOptionRecord(IRGenFunction &IGF,
                                          llvm::Value *prevOptions,
                                          OptionalExplosion &serialExecutor) {
  return maybeAddOptionRecord(IGF, prevOptions,
                              InitialSerialExecutorRecordTraits(),
                              serialExecutor);
}

static llvm::Value *
maybeAddTaskGroupOptionRecord(IRGenFunction &IGF, llvm::Value *prevOptions,
                              OptionalExplosion &taskGroup) {
  return maybeAddOptionRecord(IGF, prevOptions, TaskGroupRecordTraits(),
                              taskGroup);
}

static llvm::Value *
maybeAddInitialTaskExecutorOptionRecord(IRGenFunction &IGF,
                                        llvm::Value *prevOptions,
                                        OptionalExplosion &taskExecutor) {
  return maybeAddOptionRecord(IGF, prevOptions,
                              InitialTaskExecutorUnownedRecordTraits(),
                              taskExecutor);
}

static llvm::Value *
maybeAddInitialTaskExecutorOwnedOptionRecord(IRGenFunction &IGF,
                                        llvm::Value *prevOptions,
                                        OptionalExplosion &taskExecutorExistential) {
  return maybeAddOptionRecord(IGF, prevOptions,
                              InitialTaskExecutorOwnedRecordTraits(),
                              taskExecutorExistential);
}

std::pair<llvm::Value *, llvm::Value *>
irgen::emitTaskCreate(IRGenFunction &IGF, llvm::Value *flags,
                      OptionalExplosion &serialExecutor,
                      OptionalExplosion &taskGroup,
                      OptionalExplosion &taskExecutorUnowned,
                      OptionalExplosion &taskExecutorExistential,
                      Explosion &taskFunction,
                      SubstitutionMap subs) {
  llvm::Value *taskOptions =
    llvm::ConstantPointerNull::get(IGF.IGM.SwiftTaskOptionRecordPtrTy);

  CanType resultType;
  if (subs) {
    resultType = subs.getReplacementTypes()[0]->getCanonicalType();
  } else {
    resultType = IGF.IGM.Context.TheEmptyTupleType;
  }

  llvm::Value *resultTypeMetadata;
  if (IGF.IGM.Context.LangOpts.hasFeature(Feature::Embedded)) {
    resultTypeMetadata = llvm::ConstantPointerNull::get(IGF.IGM.Int8PtrTy);
  } else {
    resultTypeMetadata = IGF.emitTypeMetadataRef(resultType);
  }

  // Add an option record for the initial serial executor, if present.
  taskOptions =
    maybeAddInitialSerialExecutorOptionRecord(IGF, taskOptions, serialExecutor);

  // Add an option record for the task group, if present.
  taskOptions = maybeAddTaskGroupOptionRecord(IGF, taskOptions, taskGroup);

  // Add an option record for the initial task executor, if present.
  {
    // Deprecated: This is the UnownedTaskExecutor? which is NOT consuming
    taskOptions = maybeAddInitialTaskExecutorOptionRecord(
        IGF, taskOptions, taskExecutorUnowned);
    // Take an (any TaskExecutor)? which we retain until task has completed
    taskOptions = maybeAddInitialTaskExecutorOwnedOptionRecord(
        IGF, taskOptions, taskExecutorExistential);
  }

  // In embedded Swift, create and pass result type info.
  taskOptions = maybeAddEmbeddedSwiftResultTypeInfo(IGF, taskOptions, resultType);

  auto taskFunctionPtr = taskFunction.claimNext();
  auto taskFunctionContext = taskFunction.claimNext();

  llvm::CallInst *result = IGF.Builder.CreateCall(
      IGF.IGM.getTaskCreateFunctionPointer(),
      {flags, taskOptions, resultTypeMetadata,
       taskFunctionPtr, taskFunctionContext});
  result->setDoesNotThrow();
  result->setCallingConv(IGF.IGM.SwiftCC);

  // Cast back to NativeObject/RawPointer.
  auto newTask = IGF.Builder.CreateExtractValue(result, { 0 });
  newTask = IGF.Builder.CreateBitCast(newTask, IGF.IGM.RefCountedPtrTy);
  auto newContext = IGF.Builder.CreateExtractValue(result, { 1 });
  newContext = IGF.Builder.CreateBitCast(newContext, IGF.IGM.Int8PtrTy);
  return { newTask, newContext };
}
