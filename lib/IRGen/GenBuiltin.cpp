//===--- GenBuiltin.cpp - IR Generation for calls to builtin functions ----===//
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
//
//  This file implements IR generation for the assorted operations that
//  are performed by builtin functions.
//
//===----------------------------------------------------------------------===//

#include "GenBuiltin.h"

#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/StringSwitch.h"
#include "swift/AST/Builtins.h"
#include "swift/AST/Types.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "clang/AST/ASTContext.h"

#include "Explosion.h"
#include "GenCall.h"
#include "GenCast.h"
#include "GenConcurrency.h"
#include "GenDistributed.h"
#include "GenPointerAuth.h"
#include "GenIntegerLiteral.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "LoadableTypeInfo.h"

using namespace swift;
using namespace irgen;

static void emitCastBuiltin(IRGenFunction &IGF, SILType destType,
                            Explosion &result,
                            Explosion &args,
                            llvm::Instruction::CastOps opcode) {
  llvm::Value *input = args.claimNext();
  assert(args.empty() && "wrong operands to cast operation");

  llvm::Type *destTy = IGF.IGM.getStorageType(destType);
  llvm::Value *output = IGF.Builder.CreateCast(opcode, input, destTy);
  result.add(output);
}

static void emitCastOrBitCastBuiltin(IRGenFunction &IGF,
                                     SILType destType,
                                     Explosion &result,
                                     Explosion &args,
                                     BuiltinValueKind BV) {
  llvm::Value *input = args.claimNext();
  assert(args.empty() && "wrong operands to cast operation");

  llvm::Type *destTy = IGF.IGM.getStorageType(destType);
  llvm::Value *output;
  switch (BV) {
  default: llvm_unreachable("Not a cast-or-bitcast operation");
  case BuiltinValueKind::TruncOrBitCast:
    output = IGF.Builder.CreateTruncOrBitCast(input, destTy); break;
  case BuiltinValueKind::ZExtOrBitCast:
    output = IGF.Builder.CreateZExtOrBitCast(input, destTy); break;
  case BuiltinValueKind::SExtOrBitCast:
    output = IGF.Builder.CreateSExtOrBitCast(input, destTy); break;
  }
  result.add(output);
}

static void emitCompareBuiltin(IRGenFunction &IGF, Explosion &result,
                               Explosion &args, llvm::CmpInst::Predicate pred) {
  llvm::Value *lhs = args.claimNext();
  llvm::Value *rhs = args.claimNext();
  
  llvm::Value *v;
  if (lhs->getType()->isFPOrFPVectorTy())
    v = IGF.Builder.CreateFCmp(pred, lhs, rhs);
  else
    v = IGF.Builder.CreateICmp(pred, lhs, rhs);
  
  result.add(v);
}

static void emitTypeTraitBuiltin(IRGenFunction &IGF,
                                 Explosion &out,
                                 Explosion &args,
                                 SubstitutionMap substitutions,
                                 TypeTraitResult (TypeBase::*trait)()) {
  assert(substitutions.getReplacementTypes().size() == 1
         && "type trait should have gotten single type parameter");
  args.claimNext();
  
  // Lower away the trait to a tristate 0 = no, 1 = yes, 2 = maybe.
  unsigned result;
  switch ((substitutions.getReplacementTypes()[0].getPointer()->*trait)()) {
  case TypeTraitResult::IsNot:
    result = 0;
    break;
  case TypeTraitResult::Is:
    result = 1;
    break;
  case TypeTraitResult::CanBe:
    result = 2;
    break;
  }

  out.add(llvm::ConstantInt::get(IGF.IGM.Int8Ty, result));
}

static std::pair<SILType, const TypeInfo &>
getLoweredTypeAndTypeInfo(IRGenModule &IGM, Type unloweredType) {
  auto lowered = IGM.getLoweredType(unloweredType);
  return {lowered, IGM.getTypeInfo(lowered)};
}

static std::pair<SILType, const TypeInfo &>
getMaximallyAbstractedLoweredTypeAndTypeInfo(IRGenModule &IGM, Type unloweredType) {
  auto lowered = IGM.getLoweredType(AbstractionPattern::getOpaque(), unloweredType);
  return {lowered, IGM.getTypeInfo(lowered)};
}

/// emitBuiltinCall - Emit a call to a builtin function.
void irgen::emitBuiltinCall(IRGenFunction &IGF, const BuiltinInfo &Builtin,
                            BuiltinInst *Inst, ArrayRef<SILType> argTypes,
                            Explosion &args, Explosion &out) {
  Identifier FnId = Inst->getName();
  SILType resultType = Inst->getType();
  SubstitutionMap substitutions = Inst->getSubstitutions();

  if (Builtin.ID == BuiltinValueKind::COWBufferForReading) {
    // Just forward the incoming argument.
    assert(args.size() == 1 && "Expecting one incoming argument");
    out = std::move(args);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::OnFastPath) {
    // The onFastPath builtin has only an effect on SIL level, so we lower it
    // to a no-op.
    return;
  }

  // These builtins don't care about their argument:
  if (Builtin.ID == BuiltinValueKind::Sizeof) {
    (void)args.claimAll();
    auto valueTy = getMaximallyAbstractedLoweredTypeAndTypeInfo(IGF.IGM,
                                             substitutions.getReplacementTypes()[0]);
    out.add(valueTy.second.getSize(IGF, valueTy.first));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::Strideof) {
    (void)args.claimAll();
    auto valueTy = getMaximallyAbstractedLoweredTypeAndTypeInfo(IGF.IGM,
                                             substitutions.getReplacementTypes()[0]);
    out.add(valueTy.second.getStride(IGF, valueTy.first));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::Alignof) {
    (void)args.claimAll();
    auto valueTy = getMaximallyAbstractedLoweredTypeAndTypeInfo(IGF.IGM,
                                             substitutions.getReplacementTypes()[0]);
    // The alignof value is one greater than the alignment mask.
    out.add(IGF.Builder.CreateAdd(
                           valueTy.second.getAlignmentMask(IGF, valueTy.first),
                           IGF.IGM.getSize(Size(1))));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::IsPOD) {
    (void)args.claimAll();
    auto valueTy = getLoweredTypeAndTypeInfo(IGF.IGM,
                                             substitutions.getReplacementTypes()[0]);
    out.add(valueTy.second.getIsTriviallyDestroyable(IGF, valueTy.first));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::IsConcrete) {
    (void)args.claimAll();
    auto isConcrete = !substitutions.getReplacementTypes()[0]->hasArchetype();
    out.add(llvm::ConstantInt::get(IGF.IGM.Int1Ty, isConcrete));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::IsBitwiseTakable) {
    (void)args.claimAll();
    auto valueTy = getLoweredTypeAndTypeInfo(IGF.IGM,
                                             substitutions.getReplacementTypes()[0]);
    out.add(valueTy.second.getIsBitwiseTakable(IGF, valueTy.first));
    return;
  }

  // addressof expects an lvalue argument.
  if (Builtin.ID == BuiltinValueKind::AddressOf) {
    llvm::Value *address = args.claimNext();
    llvm::Value *value = IGF.Builder.CreateBitCast(address,
                                                   IGF.IGM.Int8PtrTy);
    out.add(value);
    return;
  }

  // getCurrentAsyncTask has no arguments.
  if (Builtin.ID == BuiltinValueKind::GetCurrentAsyncTask) {
    auto task = IGF.getAsyncTask();
    if (!task->getType()->isPointerTy()) {
      out.add(IGF.Builder.CreateIntToPtr(task, IGF.IGM.RefCountedPtrTy));
    } else {
      out.add(IGF.Builder.CreateBitCast(task, IGF.IGM.RefCountedPtrTy));
    }
    return;
  }

  // emitGetCurrentExecutor has no arguments.
  if (Builtin.ID == BuiltinValueKind::GetCurrentExecutor) {
    emitGetCurrentExecutor(IGF, out);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::StartAsyncLet) {
    auto taskOptions = args.claimNext();
    auto taskFunction = args.claimNext();
    auto taskContext = args.claimNext();

    auto asyncLet = emitBuiltinStartAsyncLet(
        IGF,
        taskOptions,
        taskFunction,
        taskContext,
        nullptr,
        substitutions
        );

    out.add(asyncLet);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::StartAsyncLetWithLocalBuffer) {
    auto taskOptions = args.claimNext();
    auto taskFunction = args.claimNext();
    auto taskContext = args.claimNext();
    auto localBuffer = args.claimNext();

    auto asyncLet = emitBuiltinStartAsyncLet(
        IGF,
        taskOptions,
        taskFunction,
        taskContext,
        localBuffer,
        substitutions
        );

    out.add(asyncLet);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::EndAsyncLet) {
    emitEndAsyncLet(IGF, args.claimNext());
    // Ignore a second operand which is inserted by ClosureLifetimeFixup and
    // only used for dependency tracking.
    (void)args.claimAll();
    return;
  }

  if (Builtin.ID == BuiltinValueKind::EndAsyncLetLifetime) {
    IGF.Builder.CreateLifetimeEnd(args.claimNext());
    // Ignore a second operand which is inserted by ClosureLifetimeFixup and
    // only used for dependency tracking.
    (void)args.claimAll();
    return;
  }

  if (Builtin.ID == BuiltinValueKind::TaskRunInline) {
    auto result = args.claimNext();
    auto closure = args.claimNext();
    auto closureContext = args.claimNext();

    emitTaskRunInline(IGF, substitutions, result, closure, closureContext);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::CreateTaskGroup) {
    llvm::Value *groupFlags = nullptr;
    // Claim metadata pointer.
    (void)args.claimAll();
    out.add(emitCreateTaskGroup(IGF, substitutions, groupFlags));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::CreateTaskGroupWithFlags) {
    auto groupFlags = args.claimNext();
    // Claim the remaining metadata pointer.
    if (args.size() == 1) {
      (void)args.claimNext();
    } else if (args.size() > 1) {
      llvm_unreachable("createTaskGroupWithFlags expects 1 or 2 arguments");
    }
    out.add(emitCreateTaskGroup(IGF, substitutions, groupFlags));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::DestroyTaskGroup) {
    emitDestroyTaskGroup(IGF, args.claimNext());
    return;
  }

  // Everything else cares about the (rvalue) argument.

  if (Builtin.ID == BuiltinValueKind::CancelAsyncTask) {
    emitTaskCancel(IGF, args.claimNext());
    return;
  }

  if (Builtin.ID == BuiltinValueKind::CreateAsyncTask ||
      Builtin.ID == BuiltinValueKind::CreateAsyncTaskInGroup) {

    auto flags = args.claimNext();
    auto taskGroup =
        (Builtin.ID == BuiltinValueKind::CreateAsyncTaskInGroup)
        ? args.claimNext()
        : nullptr;
    auto futureResultType = args.claimNext();
    auto taskFunction = args.claimNext();
    auto taskContext = args.claimNext();

    auto newTaskAndContext = emitTaskCreate(
        IGF,
        flags,
        taskGroup,
        futureResultType,
        taskFunction, taskContext,
        substitutions);

    // Cast back to NativeObject/RawPointer.
    auto newTask = IGF.Builder.CreateExtractValue(newTaskAndContext, { 0 });
    newTask = IGF.Builder.CreateBitCast(newTask, IGF.IGM.RefCountedPtrTy);
    auto newContext = IGF.Builder.CreateExtractValue(newTaskAndContext, { 1 });
    newContext = IGF.Builder.CreateBitCast(newContext, IGF.IGM.Int8PtrTy);
    out.add(newTask);
    out.add(newContext);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::ConvertTaskToJob) {
    auto task = args.claimNext();
    // The job object starts at the beginning of the task.
    auto job = IGF.Builder.CreateBitCast(task, IGF.IGM.SwiftJobPtrTy);
    out.add(job);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::InitializeDefaultActor ||
      Builtin.ID == BuiltinValueKind::InitializeNonDefaultDistributedActor ||
      Builtin.ID == BuiltinValueKind::DestroyDefaultActor) {
    irgen::FunctionPointer fn;
    switch (Builtin.ID) {
      case BuiltinValueKind::InitializeDefaultActor:
        fn = IGF.IGM.getDefaultActorInitializeFunctionPointer();
        break;
      case BuiltinValueKind::InitializeNonDefaultDistributedActor:
        fn = IGF.IGM.getNonDefaultDistributedActorInitializeFunctionPointer();
        break;
      case BuiltinValueKind::DestroyDefaultActor:
        fn = IGF.IGM.getDefaultActorDestroyFunctionPointer();
        break;
      default:
        llvm_unreachable("unhandled builtin id!");
    }
    auto actor = args.claimNext();
    actor = IGF.Builder.CreateBitCast(actor, IGF.IGM.RefCountedPtrTy);
    auto call = IGF.Builder.CreateCall(fn, {actor});
    call->setCallingConv(IGF.IGM.SwiftCC);
    call->setDoesNotThrow();
    return;
  }

  if (Builtin.ID == BuiltinValueKind::ResumeThrowingContinuationReturning ||
      Builtin.ID == BuiltinValueKind::ResumeNonThrowingContinuationReturning) {
    auto continuation = args.claimNext();
    auto valueTy = argTypes[1];
    auto valuePtr = args.claimNext();
    bool throwing =
      (Builtin.ID == BuiltinValueKind::ResumeThrowingContinuationReturning);
    IGF.emitResumeAsyncContinuationReturning(continuation, valuePtr, valueTy,
                                             throwing);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::ResumeThrowingContinuationThrowing) {
    auto continuation = args.claimNext();
    auto error = args.claimNext();
    IGF.emitResumeAsyncContinuationThrowing(continuation, error);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::BuildMainActorExecutorRef) {
    emitBuildMainActorExecutorRef(IGF, out);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::BuildDefaultActorExecutorRef) {
    auto actor = args.claimNext();
    emitBuildDefaultActorExecutorRef(IGF, actor, out);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::BuildOrdinarySerialExecutorRef) {
    auto actor = args.claimNext();
    auto type = substitutions.getReplacementTypes()[0]->getCanonicalType();
    auto conf = substitutions.getConformances()[0];
    emitBuildOrdinarySerialExecutorRef(IGF, actor, type, conf, out);
    return;
  }
  if (Builtin.ID == BuiltinValueKind::BuildComplexEqualitySerialExecutorRef) {
    auto actor = args.claimNext();
    auto type = substitutions.getReplacementTypes()[0]->getCanonicalType();
    auto conf = substitutions.getConformances()[0];
    emitBuildComplexEqualitySerialExecutorRef(IGF, actor, type, conf, out);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::InitializeDistributedRemoteActor) {
    auto actorMetatype = args.claimNext();
    emitDistributedActorInitializeRemote(IGF, resultType, actorMetatype, out);
    return;
  }

  // If this is an LLVM IR intrinsic, lower it to an intrinsic call.
  const IntrinsicInfo &IInfo = IGF.getSILModule().getIntrinsicInfo(FnId);
  llvm::Intrinsic::ID IID = IInfo.ID;

  // Emit non-mergeable traps only.
  if (IGF.Builder.isTrapIntrinsic(IID)) {
    IGF.Builder.CreateNonMergeableTrap(IGF.IGM, StringRef());
    return;
  }

  // Implement the ptrauth builtins as no-ops when the Clang
  // intrinsics are disabled.
  if ((IID == llvm::Intrinsic::ptrauth_sign ||
       IID == llvm::Intrinsic::ptrauth_auth ||
       IID == llvm::Intrinsic::ptrauth_resign ||
       IID == llvm::Intrinsic::ptrauth_strip) &&
      !IGF.IGM.getClangASTContext().getLangOpts().PointerAuthIntrinsics) {
    out.add(args.claimNext()); // Return the input pointer.
    (void) args.claimNext();   // Ignore the key.
    if (IID != llvm::Intrinsic::ptrauth_strip) {
      (void) args.claimNext(); // Ignore the discriminator.
    }
    if (IID == llvm::Intrinsic::ptrauth_resign) {
      (void) args.claimNext(); // Ignore the new key.
      (void) args.claimNext(); // Ignore the new discriminator.
    }
    return;
  }

  if (IID != llvm::Intrinsic::not_intrinsic) {
    SmallVector<llvm::Type*, 4> ArgTys;
    for (auto T : IInfo.Types)
      ArgTys.push_back(IGF.IGM.getStorageTypeForLowered(T->getCanonicalType()));
      
    auto F = llvm::Intrinsic::getDeclaration(&IGF.IGM.Module,
                                             (llvm::Intrinsic::ID)IID, ArgTys);
    llvm::FunctionType *FT = F->getFunctionType();
    SmallVector<llvm::Value*, 8> IRArgs;
    for (unsigned i = 0, e = FT->getNumParams(); i != e; ++i)
      IRArgs.push_back(args.claimNext());
    llvm::Value *TheCall = IGF.Builder.CreateIntrinsicCall(
        (llvm::Intrinsic::ID)IID, ArgTys, IRArgs);

    if (!TheCall->getType()->isVoidTy())
      extractScalarResults(IGF, TheCall->getType(), TheCall, out);

    return;
  }

  if (Builtin.ID == BuiltinValueKind::StringObjectOr) {
    llvm::Value *lhs = args.claimNext();
    llvm::Value *rhs = args.claimNext();
    llvm::Value *v = IGF.Builder.CreateOr(lhs, rhs);
    return out.add(v);
  }

    // TODO: A linear series of ifs is suboptimal.
#define BUILTIN_SIL_OPERATION(id, name, overload) \
  if (Builtin.ID == BuiltinValueKind::id) \
    llvm_unreachable(name " builtin should be lowered away by SILGen!");

#define BUILTIN_CAST_OPERATION(id, name, attrs) \
  if (Builtin.ID == BuiltinValueKind::id) \
    return emitCastBuiltin(IGF, resultType, out, args, \
                           llvm::Instruction::id);

#define BUILTIN_CAST_OR_BITCAST_OPERATION(id, name, attrs) \
  if (Builtin.ID == BuiltinValueKind::id) \
    return emitCastOrBitCastBuiltin(IGF, resultType, out, args, \
                                    BuiltinValueKind::id);

#define BUILTIN_BINARY_OPERATION_OVERLOADED_STATIC(id, name, attrs, overload)  \
  if (Builtin.ID == BuiltinValueKind::id) {                                    \
    llvm::Value *lhs = args.claimNext();                                       \
    llvm::Value *rhs = args.claimNext();                                       \
    llvm::Value *v = IGF.Builder.Create##id(lhs, rhs);                         \
    return out.add(v);                                                         \
  }
#define BUILTIN_BINARY_OPERATION_POLYMORPHIC(id, name)                         \
  if (Builtin.ID == BuiltinValueKind::id) {                                    \
    /* This builtin must be guarded so that dynamically it is never called. */ \
    IGF.emitTrap("invalid use of polymorphic builtin", /*Unreachable*/ false); \
    auto returnValue = llvm::UndefValue::get(IGF.IGM.Int8PtrTy);               \
    /* Consume the arguments of the builtin. */                                \
    (void)args.claimAll();                                                     \
    return out.add(returnValue);                                               \
  }

#define BUILTIN_RUNTIME_CALL(id, name, attrs)                                  \
  if (Builtin.ID == BuiltinValueKind::id) {                                    \
    auto fn = IGF.IGM.get##id##FunctionPointer();                              \
    llvm::CallInst *call = IGF.Builder.CreateCall(fn, args.claimNext());       \
    return out.add(call);                                                      \
  }

#define BUILTIN_BINARY_OPERATION_WITH_OVERFLOW(id, name, uncheckedID, attrs,   \
                                               overload)                       \
  if (Builtin.ID == BuiltinValueKind::id) {                                    \
    SmallVector<llvm::Type *, 2> ArgTys;                                       \
    auto opType = Builtin.Types[0]->getCanonicalType();                        \
    ArgTys.push_back(IGF.IGM.getStorageTypeForLowered(opType));                \
    auto F = llvm::Intrinsic::getDeclaration(                                  \
        &IGF.IGM.Module, getLLVMIntrinsicIDForBuiltinWithOverflow(Builtin.ID), \
        ArgTys);                                                               \
    SmallVector<llvm::Value *, 2> IRArgs;                                      \
    IRArgs.push_back(args.claimNext());                                        \
    IRArgs.push_back(args.claimNext());                                        \
    args.claimNext();                                                          \
    llvm::Value *TheCall = IGF.Builder.CreateCall(                             \
        cast<llvm::FunctionType>(F->getValueType()), F, IRArgs);               \
    extractScalarResults(IGF, TheCall->getType(), TheCall, out);               \
    return;                                                                    \
  }
  // FIXME: We could generate the code to dynamically report the overflow if the
  // third argument is true. Now, we just ignore it.

#define BUILTIN_BINARY_PREDICATE(id, name, attrs, overload) \
  if (Builtin.ID == BuiltinValueKind::id) \
    return emitCompareBuiltin(IGF, out, args, llvm::CmpInst::id);
  
#define BUILTIN_TYPE_TRAIT_OPERATION(id, name) \
  if (Builtin.ID == BuiltinValueKind::id) \
    return emitTypeTraitBuiltin(IGF, out, args, substitutions, &TypeBase::name);
  
#define BUILTIN(ID, Name, Attrs)  // Ignore the rest.
#include "swift/AST/Builtins.def"

  if (Builtin.ID == BuiltinValueKind::GlobalStringTablePointer) {
    // This builtin should be used only on strings constructed from a
    // string literal. If we ever get to the point of executing this builtin
    // at run time, it implies an incorrect use of the builtin and must result
    // in a trap.
    IGF.emitTrap("invalid use of globalStringTablePointer",
                 /*Unreachable=*/false);
    auto returnValue = llvm::UndefValue::get(IGF.IGM.Int8PtrTy);
    // Consume the arguments of the builtin.
    (void)args.claimAll();
    return out.add(returnValue);
  }

  if (Builtin.ID == BuiltinValueKind::WillThrow) {
    // willThrow is emitted like a Swift function call with the error in
    // the error return register. We also have to pass a fake context
    // argument due to how swiftcc works in clang.

    auto fn = IGF.IGM.getWillThrowFunctionPointer();
    auto error = args.claimNext();
    auto errorTy = IGF.IGM.Context.getErrorExistentialType();
    auto errorBuffer = IGF.getCalleeErrorResultSlot(
        SILType::getPrimitiveObjectType(errorTy));
    IGF.Builder.CreateStore(error, errorBuffer);
    
    auto context = llvm::UndefValue::get(IGF.IGM.Int8PtrTy);

    llvm::CallInst *call = IGF.Builder.CreateCall(fn,
                                        {context, errorBuffer.getAddress()});
    call->setCallingConv(IGF.IGM.SwiftCC);
    call->addFnAttr(llvm::Attribute::NoUnwind);
    call->addParamAttr(1, llvm::Attribute::ReadOnly);

    auto attrs = call->getAttributes();
    IGF.IGM.addSwiftSelfAttributes(attrs, 0);
    IGF.IGM.addSwiftErrorAttributes(attrs, 1);
    call->setAttributes(attrs);

    IGF.Builder.CreateStore(llvm::ConstantPointerNull::get(IGF.IGM.ErrorPtrTy),
                            errorBuffer);

    return out.add(call);
  }
  
  if (Builtin.ID == BuiltinValueKind::FNeg) {
    llvm::Value *rhs = args.claimNext();
    llvm::Value *lhs = llvm::ConstantFP::get(rhs->getType(), "-0.0");
    llvm::Value *v = IGF.Builder.CreateFSub(lhs, rhs);
    return out.add(v);
  }
  if (Builtin.ID == BuiltinValueKind::AssumeTrue) {
    llvm::Value *v = args.claimNext();
    if (v->getType() == IGF.IGM.Int1Ty) {
      IGF.Builder.CreateIntrinsicCall(llvm::Intrinsic::assume, v);
    }
    return;
  }
  if (Builtin.ID == BuiltinValueKind::AssumeNonNegative) {
    llvm::Value *v = args.claimNext();
    // Set a value range on the load instruction, which must be the argument of
    // the builtin.
    if (isa<llvm::LoadInst>(v) || isa<llvm::CallInst>(v)) {
      // The load must be post-dominated by the builtin. Otherwise we would get
      // a wrong assumption in the else-branch in this example:
      //    x = f()
      //    if condition {
      //      y = assumeNonNegative(x)
      //    } else {
      //      // x might be negative here!
      //    }
      // For simplicity we just enforce that both the load and the builtin must
      // be in the same block.
      llvm::Instruction *I = static_cast<llvm::Instruction *>(v);
      if (I->getParent() == IGF.Builder.GetInsertBlock()) {
        llvm::LLVMContext &ctx = IGF.IGM.Module.getContext();
        auto *intType = dyn_cast<llvm::IntegerType>(v->getType());
        llvm::Metadata *rangeElems[] = {
          llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(intType, 0)),
          llvm::ConstantAsMetadata::get(
              llvm::ConstantInt::get(intType,
                  APInt::getSignedMaxValue(intType->getBitWidth())))
        };
        llvm::MDNode *range = llvm::MDNode::get(ctx, rangeElems);
        I->setMetadata(llvm::LLVMContext::MD_range, range);
      }
    }
    // Don't generate any code for the builtin.
    return out.add(v);
  }
  
  if (Builtin.ID == BuiltinValueKind::AllocRaw) {
    auto size = args.claimNext();
    auto align = args.claimNext();
    // Translate the alignment to a mask.
    auto alignMask = IGF.Builder.CreateSub(align, IGF.IGM.getSize(Size(1)));
    auto alloc = IGF.emitAllocRawCall(size, alignMask, "builtin-allocRaw");
    out.add(alloc);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::DeallocRaw) {
    auto pointer = args.claimNext();
    auto size = args.claimNext();
    auto align = args.claimNext();
    // Translate the alignment to a mask.
    auto alignMask = IGF.Builder.CreateSub(align, IGF.IGM.getSize(Size(1)));
    IGF.emitDeallocRawCall(pointer, size, alignMask);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::Fence) {
    SmallVector<Type, 4> Types;
    StringRef BuiltinName =
      getBuiltinBaseName(IGF.IGM.Context, FnId.str(), Types);
    BuiltinName = BuiltinName.drop_front(strlen("fence_"));
    // Decode the ordering argument, which is required.
    auto underscore = BuiltinName.find('_');
    auto ordering = decodeLLVMAtomicOrdering(BuiltinName.substr(0, underscore));
    assert(ordering != llvm::AtomicOrdering::NotAtomic);
    BuiltinName = BuiltinName.substr(underscore);
    
    // Accept singlethread if present.
    bool isSingleThread = BuiltinName.startswith("_singlethread");
    if (isSingleThread)
      BuiltinName = BuiltinName.drop_front(strlen("_singlethread"));
    assert(BuiltinName.empty() && "Mismatch with sema");
    
    IGF.Builder.CreateFence(ordering, isSingleThread
                                          ? llvm::SyncScope::SingleThread
                                          : llvm::SyncScope::System);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::Ifdef) {
    // Ifdef not constant folded, which means it was not @_alwaysEmitIntoClient
    IGF.IGM.error(
        Inst->getLoc().getSourceLoc(),
        "Builtin.ifdef can only be used in @_alwaysEmitIntoClient functions");
    out.add(IGF.Builder.getInt32(0));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::CmpXChg) {
    SmallVector<Type, 4> Types;
    StringRef BuiltinName =
      getBuiltinBaseName(IGF.IGM.Context, FnId.str(), Types);
    BuiltinName = BuiltinName.drop_front(strlen("cmpxchg_"));

    // Decode the success- and failure-ordering arguments, which are required.
    SmallVector<StringRef, 4> Parts;
    BuiltinName.split(Parts, "_");
    assert(Parts.size() >= 2 && "Mismatch with sema");
    auto successOrdering = decodeLLVMAtomicOrdering(Parts[0]);
    auto failureOrdering = decodeLLVMAtomicOrdering(Parts[1]);
    assert(successOrdering != llvm::AtomicOrdering::NotAtomic);
    assert(failureOrdering != llvm::AtomicOrdering::NotAtomic);
    auto NextPart = Parts.begin() + 2;

    // Accept weak, volatile, and singlethread if present.
    bool isWeak = false, isVolatile = false, isSingleThread = false;
    if (NextPart != Parts.end() && *NextPart == "weak") {
      isWeak = true;
      ++NextPart;
    }
    if (NextPart != Parts.end() && *NextPart == "volatile") {
      isVolatile = true;
      ++NextPart;
    }
    if (NextPart != Parts.end() && *NextPart == "singlethread") {
      isSingleThread = true;
      ++NextPart;
    }
    assert(NextPart == Parts.end() && "Mismatch with sema");

    auto pointer = args.claimNext();
    auto cmp = args.claimNext();
    auto newval = args.claimNext();

    llvm::Type *origTy = cmp->getType();
    if (origTy->isPointerTy()) {
      cmp = IGF.Builder.CreatePtrToInt(cmp, IGF.IGM.IntPtrTy);
      newval = IGF.Builder.CreatePtrToInt(newval, IGF.IGM.IntPtrTy);
    }

    pointer = IGF.Builder.CreateBitCast(pointer,
                                  llvm::PointerType::getUnqual(cmp->getType()));
    llvm::Value *value = IGF.Builder.CreateAtomicCmpXchg(
        pointer, cmp, newval, llvm::MaybeAlign(),
        successOrdering, failureOrdering,
        isSingleThread ? llvm::SyncScope::SingleThread
                       : llvm::SyncScope::System);
    cast<llvm::AtomicCmpXchgInst>(value)->setVolatile(isVolatile);
    cast<llvm::AtomicCmpXchgInst>(value)->setWeak(isWeak);

    auto valueLoaded = IGF.Builder.CreateExtractValue(value, {0});
    auto loadSuccessful = IGF.Builder.CreateExtractValue(value, {1});

    if (origTy->isPointerTy())
      valueLoaded = IGF.Builder.CreateIntToPtr(valueLoaded, origTy);

    out.add(valueLoaded);
    out.add(loadSuccessful);

    return;
  }
  
  if (Builtin.ID == BuiltinValueKind::AtomicRMW) {
    using namespace llvm;

    SmallVector<Type, 4> Types;
    StringRef BuiltinName = getBuiltinBaseName(IGF.IGM.Context,
                                               FnId.str(), Types);
    BuiltinName = BuiltinName.drop_front(strlen("atomicrmw_"));
    auto underscore = BuiltinName.find('_');
    StringRef SubOp = BuiltinName.substr(0, underscore);
    
    AtomicRMWInst::BinOp SubOpcode = StringSwitch<AtomicRMWInst::BinOp>(SubOp)
      .Case("xchg", AtomicRMWInst::Xchg)
      .Case("add",  AtomicRMWInst::Add)
      .Case("sub",  AtomicRMWInst::Sub)
      .Case("and",  AtomicRMWInst::And)
      .Case("nand", AtomicRMWInst::Nand)
      .Case("or",   AtomicRMWInst::Or)
      .Case("xor",  AtomicRMWInst::Xor)
      .Case("max",  AtomicRMWInst::Max)
      .Case("min",  AtomicRMWInst::Min)
      .Case("umax", AtomicRMWInst::UMax)
      .Case("umin", AtomicRMWInst::UMin);
    BuiltinName = BuiltinName.drop_front(underscore+1);
    
    // Decode the ordering argument, which is required.
    underscore = BuiltinName.find('_');
    auto ordering = decodeLLVMAtomicOrdering(BuiltinName.substr(0, underscore));
    assert(ordering != llvm::AtomicOrdering::NotAtomic);
    BuiltinName = BuiltinName.substr(underscore);
    
    // Accept volatile and singlethread if present.
    bool isVolatile = BuiltinName.startswith("_volatile");
    if (isVolatile) BuiltinName = BuiltinName.drop_front(strlen("_volatile"));
    
    bool isSingleThread = BuiltinName.startswith("_singlethread");
    if (isSingleThread)
      BuiltinName = BuiltinName.drop_front(strlen("_singlethread"));
    assert(BuiltinName.empty() && "Mismatch with sema");
    
    auto pointer = args.claimNext();
    auto val = args.claimNext();

    // Handle atomic ops on pointers by casting to intptr_t.
    llvm::Type *origTy = val->getType();
    if (origTy->isPointerTy())
      val = IGF.Builder.CreatePtrToInt(val, IGF.IGM.IntPtrTy);

    pointer = IGF.Builder.CreateBitCast(pointer,
                                  llvm::PointerType::getUnqual(val->getType()));
    llvm::Value *value = IGF.Builder.CreateAtomicRMW(
        SubOpcode, pointer, val, llvm::MaybeAlign(), ordering,
        isSingleThread ? llvm::SyncScope::SingleThread
                       : llvm::SyncScope::System);
    cast<AtomicRMWInst>(value)->setVolatile(isVolatile);

    if (origTy->isPointerTy())
      value = IGF.Builder.CreateIntToPtr(value, origTy);

    out.add(value);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::AtomicLoad
      || Builtin.ID == BuiltinValueKind::AtomicStore) {
    using namespace llvm;

    SmallVector<Type, 4> Types;
    StringRef BuiltinName = getBuiltinBaseName(IGF.IGM.Context,
                                               FnId.str(), Types);
    auto underscore = BuiltinName.find('_');
    BuiltinName = BuiltinName.substr(underscore+1);

    underscore = BuiltinName.find('_');
    auto ordering = decodeLLVMAtomicOrdering(BuiltinName.substr(0, underscore));
    assert(ordering != llvm::AtomicOrdering::NotAtomic);
    BuiltinName = BuiltinName.substr(underscore);

    // Accept volatile and singlethread if present.
    bool isVolatile = BuiltinName.startswith("_volatile");
    if (isVolatile) BuiltinName = BuiltinName.drop_front(strlen("_volatile"));

    bool isSingleThread = BuiltinName.startswith("_singlethread");
    if (isSingleThread)
      BuiltinName = BuiltinName.drop_front(strlen("_singlethread"));
    assert(BuiltinName.empty() && "Mismatch with sema");

    auto pointer = args.claimNext();
    auto &valueTI = IGF.getTypeInfoForUnlowered(Types[0]);
    auto schema = valueTI.getSchema();
    assert(schema.size() == 1 && "not a scalar type?!");
    auto origValueTy = schema[0].getScalarType();

    // If the type is floating-point, then we need to bitcast to integer.
    auto valueTy = origValueTy;
    if (valueTy->isFloatingPointTy()) {
      valueTy = llvm::IntegerType::get(IGF.IGM.getLLVMContext(),
                                       valueTy->getPrimitiveSizeInBits());
    }

    pointer = IGF.Builder.CreateBitCast(pointer, valueTy->getPointerTo());

    if (Builtin.ID == BuiltinValueKind::AtomicLoad) {
      auto load = IGF.Builder.CreateLoad(pointer, valueTy,
                                         valueTI.getBestKnownAlignment());
      load->setAtomic(ordering, isSingleThread ? llvm::SyncScope::SingleThread
                                               : llvm::SyncScope::System);
      load->setVolatile(isVolatile);

      llvm::Value *value = load;
      if (valueTy != origValueTy)
        value = IGF.Builder.CreateBitCast(value, origValueTy);
      out.add(value);
      return;
    } else if (Builtin.ID == BuiltinValueKind::AtomicStore) {
      llvm::Value *value = args.claimNext();
      if (valueTy != origValueTy)
        value = IGF.Builder.CreateBitCast(value, valueTy);
      auto store = IGF.Builder.CreateStore(value, pointer,
                                           valueTI.getBestKnownAlignment());
      store->setAtomic(ordering, isSingleThread ? llvm::SyncScope::SingleThread
                                                : llvm::SyncScope::System);
      store->setVolatile(isVolatile);
      return;
    } else {
      llvm_unreachable("out of sync with outer conditional");
    }
  }

  if (Builtin.ID == BuiltinValueKind::ExtractElement) {
    using namespace llvm;

    auto vector = args.claimNext();
    auto index = args.claimNext();
    out.add(IGF.Builder.CreateExtractElement(vector, index));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::InsertElement) {
    using namespace llvm;

    auto vector = args.claimNext();
    auto newValue = args.claimNext();
    auto index = args.claimNext();
    out.add(IGF.Builder.CreateInsertElement(vector, newValue, index));
    return;
  }
  
  if (Builtin.ID == BuiltinValueKind::ShuffleVector) {
    using namespace llvm;

    auto dict0 = args.claimNext();
    auto dict1 = args.claimNext();
    auto index = args.claimNext();
    out.add(IGF.Builder.CreateShuffleVector(dict0, dict1, index));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::SToSCheckedTrunc ||
      Builtin.ID == BuiltinValueKind::UToUCheckedTrunc ||
      Builtin.ID == BuiltinValueKind::SToUCheckedTrunc) {
    bool Signed = (Builtin.ID == BuiltinValueKind::SToSCheckedTrunc);

    auto FromType = Builtin.Types[0]->getCanonicalType();
    auto ToTy = cast<llvm::IntegerType>(
      IGF.IGM.getStorageTypeForLowered(Builtin.Types[1]->getCanonicalType()));

    auto FromTy = IGF.IGM.getStorageTypeForLowered(FromType);

    // Handle the arbitrary-precision truncate specially.
    if (isa<BuiltinIntegerLiteralType>(FromType)) {
      emitIntegerLiteralCheckedTrunc(IGF, args, IGF.IGM.SizeTy, ToTy, Signed,
                                     out);
      return;
    }

    // Compute the result for SToSCheckedTrunc_IntFrom_IntTo(Arg):
    //   Res = trunc_IntTo(Arg)
    //   Ext = sext_IntFrom(Res)
    //   OverflowFlag = (Arg == Ext) ? 0 : 1
    //   return (resultVal, OverflowFlag)
    //
    // Compute the result for UToUCheckedTrunc_IntFrom_IntTo(Arg)
    // and SToUCheckedTrunc_IntFrom_IntTo(Arg):
    //   Res = trunc_IntTo(Arg)
    //   Ext = zext_IntFrom(Res)
    //   OverflowFlag = (Arg == Ext) ? 0 : 1
    //   return (Res, OverflowFlag)
    llvm::Value *Arg = args.claimNext();
    llvm::Value *Res = IGF.Builder.CreateTrunc(Arg, ToTy);
    llvm::Value *Ext = Signed ? IGF.Builder.CreateSExt(Res, FromTy) :
                                IGF.Builder.CreateZExt(Res, FromTy);
    llvm::Value *OverflowCond = IGF.Builder.CreateICmpEQ(Arg, Ext);
    llvm::Value *OverflowFlag = IGF.Builder.CreateSelect(OverflowCond,
                                  llvm::ConstantInt::get(IGF.IGM.Int1Ty, 0),
                                  llvm::ConstantInt::get(IGF.IGM.Int1Ty, 1));
    // Return the tuple - the result + the overflow flag.
    out.add(Res);
    return out.add(OverflowFlag);
  }

  if (Builtin.ID == BuiltinValueKind::UToSCheckedTrunc) {
    auto FromTy =
      IGF.IGM.getStorageTypeForLowered(Builtin.Types[0]->getCanonicalType());
    auto ToTy =
      IGF.IGM.getStorageTypeForLowered(Builtin.Types[1]->getCanonicalType());
    llvm::Type *ToMinusOneTy =
      llvm::Type::getIntNTy(ToTy->getContext(), ToTy->getIntegerBitWidth() - 1);

    // Compute the result for UToSCheckedTrunc_IntFrom_IntTo(Arg):
    //   Res = trunc_IntTo(Arg)
    //   Trunc = trunc_'IntTo-1bit'(Arg)
    //   Ext = zext_IntFrom(Trunc)
    //   OverflowFlag = (Arg == Ext) ? 0 : 1
    //   return (Res, OverflowFlag)
    llvm::Value *Arg = args.claimNext();
    llvm::Value *Res = IGF.Builder.CreateTrunc(Arg, ToTy);
    llvm::Value *Trunc = IGF.Builder.CreateTrunc(Arg, ToMinusOneTy);
    llvm::Value *Ext = IGF.Builder.CreateZExt(Trunc, FromTy);
    llvm::Value *OverflowCond = IGF.Builder.CreateICmpEQ(Arg, Ext);
    llvm::Value *OverflowFlag = IGF.Builder.CreateSelect(OverflowCond,
                                  llvm::ConstantInt::get(IGF.IGM.Int1Ty, 0),
                                  llvm::ConstantInt::get(IGF.IGM.Int1Ty, 1));
    // Return the tuple: (the result, the overflow flag).
    out.add(Res);
    return out.add(OverflowFlag);
  }

  // We are currently emitting code for '_convertFromBuiltinIntegerLiteral',
  // which will call the builtin and pass it a non-compile-time-const parameter.
  if (Builtin.ID == BuiltinValueKind::IntToFPWithOverflow) {
    assert(Builtin.Types[0]->is<BuiltinIntegerLiteralType>());
    auto toType =
      IGF.IGM.getStorageTypeForLowered(Builtin.Types[1]->getCanonicalType());
    auto result = emitIntegerLiteralToFP(IGF, args, toType);
    out.add(result);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::BitWidth) {
    assert(Builtin.Types[0]->is<BuiltinIntegerLiteralType>());
    out.add(emitIntLiteralBitWidth(IGF, args));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::IsNegative) {
    assert(Builtin.Types[0]->is<BuiltinIntegerLiteralType>());
    out.add(emitIntLiteralIsNegative(IGF, args));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::WordAtIndex) {
    assert(Builtin.Types[0]->is<BuiltinIntegerLiteralType>());
    out.add(emitIntLiteralWordAtIndex(IGF, args));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::Once
      || Builtin.ID == BuiltinValueKind::OnceWithContext) {
    // The input type is statically (Builtin.RawPointer, @convention(thin) () -> ()).
    llvm::Value *PredPtr = args.claimNext();
    // Cast the predicate to a OnceTy pointer.
    PredPtr = IGF.Builder.CreateBitCast(PredPtr, IGF.IGM.OnceTy->getPointerTo());
    llvm::Value *FnCode = args.claimNext();
    // Get the context if any.
    llvm::Value *Context;
    if (Builtin.ID == BuiltinValueKind::OnceWithContext) {
      Context = args.claimNext();
    } else {
      Context = llvm::UndefValue::get(IGF.IGM.Int8PtrTy);
    }
    
    // If we know the platform runtime's "done" value, emit the check inline.
    llvm::BasicBlock *doneBB = nullptr;
    
    llvm::BasicBlock *beforeBB = IGF.Builder.GetInsertBlock();

    if (auto ExpectedPred = IGF.IGM.TargetInfo.OnceDonePredicateValue) {
      auto PredValue = IGF.Builder.CreateLoad(PredPtr, IGF.IGM.OnceTy,
                                              IGF.IGM.getPointerAlignment());
      auto ExpectedPredValue = llvm::ConstantInt::getSigned(IGF.IGM.OnceTy,
                                                            *ExpectedPred);
      auto PredIsDone = IGF.Builder.CreateICmpEQ(PredValue, ExpectedPredValue);
      PredIsDone = IGF.Builder.CreateExpect(PredIsDone,
                                     llvm::ConstantInt::get(IGF.IGM.Int1Ty, 1));
      
      auto notDoneBB = IGF.createBasicBlock("once_not_done");
      doneBB = IGF.createBasicBlock("once_done");
      
      IGF.Builder.CreateCondBr(PredIsDone, doneBB, notDoneBB);
      
      IGF.Builder.SetInsertPoint(&IGF.CurFn->back());
      IGF.Builder.emitBlock(notDoneBB);
    }
    
    // Emit the runtime "once" call.
    auto call = IGF.Builder.CreateCall(IGF.IGM.getOnceFunctionPointer(),
                                       {PredPtr, FnCode, Context});
    call->setCallingConv(IGF.IGM.DefaultCC);
    
    // If we emitted the "done" check inline, join the branches.
    if (auto ExpectedPred = IGF.IGM.TargetInfo.OnceDonePredicateValue) {
      IGF.Builder.CreateBr(doneBB);
      IGF.Builder.SetInsertPoint(beforeBB);
      IGF.Builder.emitBlock(doneBB);
      // We can assume the once predicate is in the "done" state now.
      auto PredValue = IGF.Builder.CreateLoad(PredPtr, IGF.IGM.OnceTy,
                                              IGF.IGM.getPointerAlignment());
      auto ExpectedPredValue = llvm::ConstantInt::getSigned(IGF.IGM.OnceTy,
                                                            *ExpectedPred);
      auto PredIsDone = IGF.Builder.CreateICmpEQ(PredValue, ExpectedPredValue);

      IGF.Builder.CreateAssumption(PredIsDone);
    }
    
    // No return value.
    return;
  }

  if (Builtin.ID == BuiltinValueKind::AssertConf) {
    // Replace the call to assert_configuration by the Debug configuration
    // value.
    // TODO: assert(IGF.IGM.getOptions().AssertConfig ==
    //              SILOptions::DisableReplacement);
    // Make sure this only happens in a mode where we build a library dylib.

    llvm::Value *DebugAssert = IGF.Builder.getInt32(SILOptions::Debug);
    out.add(DebugAssert);
    return;
  }
  
  if (Builtin.ID == BuiltinValueKind::DestroyArray) {
    // The input type is (T.Type, Builtin.RawPointer, Builtin.Word).
    /* metatype (which may be thin) */
    if (args.size() == 3)
      args.claimNext();
    llvm::Value *ptr = args.claimNext();
    llvm::Value *count = args.claimNext();
    
    auto valueTy = getLoweredTypeAndTypeInfo(IGF.IGM,
                                             substitutions.getReplacementTypes()[0]);
    
    ptr = IGF.Builder.CreateBitCast(ptr,
                              valueTy.second.getStorageType()->getPointerTo());
    Address array = valueTy.second.getAddressForPointer(ptr);
    valueTy.second.destroyArray(IGF, array, count, valueTy.first);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::CopyArray ||
      Builtin.ID == BuiltinValueKind::TakeArrayNoAlias ||
      Builtin.ID == BuiltinValueKind::TakeArrayFrontToBack ||
      Builtin.ID == BuiltinValueKind::TakeArrayBackToFront ||
      Builtin.ID == BuiltinValueKind::AssignCopyArrayNoAlias ||
      Builtin.ID == BuiltinValueKind::AssignCopyArrayFrontToBack ||
      Builtin.ID == BuiltinValueKind::AssignCopyArrayBackToFront ||
      Builtin.ID == BuiltinValueKind::AssignTakeArray) {
    // The input type is (T.Type, Builtin.RawPointer, Builtin.RawPointer, Builtin.Word).
    /* metatype (which may be thin) */
    if (args.size() == 4)
      args.claimNext();
    llvm::Value *dest = args.claimNext();
    llvm::Value *src = args.claimNext();
    llvm::Value *count = args.claimNext();
    
    auto valueTy = getLoweredTypeAndTypeInfo(IGF.IGM,
                                             substitutions.getReplacementTypes()[0]);
    
    dest = IGF.Builder.CreateBitCast(dest,
                               valueTy.second.getStorageType()->getPointerTo());
    src = IGF.Builder.CreateBitCast(src,
                               valueTy.second.getStorageType()->getPointerTo());
    Address destArray = valueTy.second.getAddressForPointer(dest);
    Address srcArray = valueTy.second.getAddressForPointer(src);
    
    switch (Builtin.ID) {
    case BuiltinValueKind::CopyArray:
      valueTy.second.initializeArrayWithCopy(IGF, destArray, srcArray, count,
                                             valueTy.first);
      break;
    case BuiltinValueKind::TakeArrayNoAlias:
      valueTy.second.initializeArrayWithTakeNoAlias(IGF, destArray, srcArray,
                                                    count, valueTy.first);
      break;
    case BuiltinValueKind::TakeArrayFrontToBack:
      valueTy.second.initializeArrayWithTakeFrontToBack(IGF, destArray, srcArray,
                                                        count, valueTy.first);
      break;
    case BuiltinValueKind::TakeArrayBackToFront:
      valueTy.second.initializeArrayWithTakeBackToFront(IGF, destArray, srcArray,
                                                        count, valueTy.first);
      break;
    case BuiltinValueKind::AssignCopyArrayNoAlias:
      valueTy.second.assignArrayWithCopyNoAlias(IGF, destArray, srcArray, count,
                                                valueTy.first);
      break;
    case BuiltinValueKind::AssignCopyArrayFrontToBack:
      valueTy.second.assignArrayWithCopyFrontToBack(IGF, destArray, srcArray,
                                                    count, valueTy.first);
      break;
    case BuiltinValueKind::AssignCopyArrayBackToFront:
      valueTy.second.assignArrayWithCopyBackToFront(IGF, destArray, srcArray,
                                                    count, valueTy.first);
      break;
    case BuiltinValueKind::AssignTakeArray:
      valueTy.second.assignArrayWithTake(IGF, destArray, srcArray, count,
                                         valueTy.first);
      break;
    default:
      llvm_unreachable("out of sync with if condition");
    }    
    return;
  }
  
  if (Builtin.ID == BuiltinValueKind::CondUnreachable) {
    // conditionallyUnreachable is a no-op by itself. Since it's noreturn, there
    // should be a true unreachable terminator right after.
    return;
  }
  
  if (Builtin.ID == BuiltinValueKind::ZeroInitializer) {
    // Build a zero initializer of the result type.
    auto valueTy = getLoweredTypeAndTypeInfo(IGF.IGM,
                                             substitutions.getReplacementTypes()[0]);
    
    if (args.size() > 0) {
      // `memset` the memory addressed by the argument.
      auto address = args.claimNext();
      IGF.Builder.CreateMemSet(valueTy.second.getAddressForPointer(address),
                               llvm::ConstantInt::get(IGF.IGM.Int8Ty, 0),
                               valueTy.second.getSize(IGF, argTypes[0]));
    } else {
      auto schema = valueTy.second.getSchema();
      for (auto &elt : schema) {
        out.add(llvm::Constant::getNullValue(elt.getScalarType()));
      }
    }
    return;
  }
  
  if (Builtin.ID == BuiltinValueKind::GetObjCTypeEncoding) {
    (void)args.claimAll();
    Type valueTy = substitutions.getReplacementTypes()[0];
    // Get the type encoding for the associated clang type.
    auto clangTy = IGF.IGM.getClangType(valueTy->getCanonicalType());
    std::string encoding;
    IGF.IGM.getClangASTContext().getObjCEncodingForType(clangTy, encoding);
    
    auto globalString = IGF.IGM.getAddrOfGlobalString(encoding);
    out.add(globalString);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::TSanInoutAccess) {
    auto address = args.claimNext();

    // The tsanInoutAccess builtin takes a single argument, the address
    // of the accessed storage
    SILType accessedType = argTypes[0];

    // Empty types (such as structs without stored properties) have a
    // meaningless value for their address. We not should call into the
    // TSan runtime to check for data races on accesses on such addresses.
    if (!IGF.IGM.getTypeInfo(accessedType)
        .isKnownEmpty(ResilienceExpansion::Maximal)) {
      IGF.emitTSanInoutAccessCall(address);
    }
    return;
  }

  if (Builtin.ID == BuiltinValueKind::TargetOSVersionAtLeast) {
    auto major = args.claimNext();
    auto minor = args.claimNext();
    auto patch = args.claimNext();
    auto result = IGF.emitTargetOSVersionAtLeastCall(major, minor, patch);
    out.add(result);
    return;
  }

  if (Builtin.ID == BuiltinValueKind::Swift3ImplicitObjCEntrypoint) {
    llvm::Value *entrypointArgs[7];
    auto argIter = IGF.CurFn->arg_begin();

    // self
    entrypointArgs[0] = &*argIter++;
    if (entrypointArgs[0]->getType() != IGF.IGM.ObjCPtrTy)
      entrypointArgs[0] = IGF.Builder.CreateBitCast(entrypointArgs[0], IGF.IGM.ObjCPtrTy);

    // _cmd
    entrypointArgs[1] = &*argIter;
    if (entrypointArgs[1]->getType() != IGF.IGM.ObjCSELTy)
      entrypointArgs[1] = IGF.Builder.CreateBitCast(entrypointArgs[1], IGF.IGM.ObjCSELTy);
    
    // Filename pointer
    entrypointArgs[2] = args.claimNext();
    // Filename length
    entrypointArgs[3] = args.claimNext();
    // Line
    entrypointArgs[4] = args.claimNext();
    // Column
    entrypointArgs[5] = args.claimNext();
    
    // Create a flag variable so that this invocation logs only once.
    auto flagStorageTy = llvm::ArrayType::get(IGF.IGM.Int8Ty,
                                        IGF.IGM.getAtomicBoolSize().getValue());
    auto flag = new llvm::GlobalVariable(IGF.IGM.Module, flagStorageTy,
                               /*constant*/ false,
                               llvm::GlobalValue::PrivateLinkage,
                               llvm::ConstantAggregateZero::get(flagStorageTy));
    flag->setAlignment(
        llvm::MaybeAlign(IGF.IGM.getAtomicBoolAlignment().getValue()));
    entrypointArgs[6] = llvm::ConstantExpr::getBitCast(flag, IGF.IGM.Int8PtrTy);

    IGF.Builder.CreateCall(
        IGF.IGM.getSwift3ImplicitObjCEntrypointFunctionPointer(),
        entrypointArgs);
    return;
  }
  
  if (Builtin.ID == BuiltinValueKind::TypePtrAuthDiscriminator) {
    (void)args.claimAll();
    Type valueTy = substitutions.getReplacementTypes()[0];
    
    // The type should lower statically to a SILFunctionType.
    auto loweredTy = IGF.IGM.getLoweredType(valueTy).castTo<SILFunctionType>();
    
    out.add(PointerAuthEntity(loweredTy).getTypeDiscriminator(IGF.IGM));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::IsSameMetatype) {
    auto metatypeLHS = args.claimNext();
    auto metatypeRHS = args.claimNext();
    (void)args.claimAll();
    llvm::Value *metatypeLHSCasted =
        IGF.Builder.CreateBitCast(metatypeLHS, IGF.IGM.Int8PtrTy);
    llvm::Value *metatypeRHSCasted =
        IGF.Builder.CreateBitCast(metatypeRHS, IGF.IGM.Int8PtrTy);

    out.add(IGF.Builder.CreateICmpEQ(metatypeLHSCasted, metatypeRHSCasted));
    return;
  }

  if (Builtin.ID == BuiltinValueKind::AutoDiffCreateLinearMapContext) {
    auto topLevelSubcontextSize = args.claimNext();
    out.add(emitAutoDiffCreateLinearMapContext(IGF, topLevelSubcontextSize)
                .getAddress());
    return;
  }

  if (Builtin.ID == BuiltinValueKind::AutoDiffProjectTopLevelSubcontext) {
    Address allocatorAddr(args.claimNext(), IGF.IGM.RefCountedStructTy,
                          IGF.IGM.getPointerAlignment());
    out.add(
        emitAutoDiffProjectTopLevelSubcontext(IGF, allocatorAddr).getAddress());
    return;
  }

  if (Builtin.ID == BuiltinValueKind::AutoDiffAllocateSubcontext) {
    Address allocatorAddr(args.claimNext(), IGF.IGM.RefCountedStructTy,
                          IGF.IGM.getPointerAlignment());
    auto size = args.claimNext();
    out.add(
        emitAutoDiffAllocateSubcontext(IGF, allocatorAddr, size).getAddress());
    return;
  }

  if (Builtin.ID == BuiltinValueKind::Copy) {
    auto input = args.claimNext();
    auto result = args.claimNext();
    SILType addrTy = argTypes[0];
    const TypeInfo &addrTI = IGF.getTypeInfo(addrTy);
    Address inputAttr = addrTI.getAddressForPointer(input);
    Address resultAttr = addrTI.getAddressForPointer(result);
    addrTI.initializeWithCopy(IGF, resultAttr, inputAttr, addrTy, false);
    return;
  }
  if (Builtin.ID == BuiltinValueKind::AssumeAlignment) {
    // A no-op pointer cast that passes on its first value. Common occurrences of
    // this builtin should already be removed with the alignment guarantee moved
    // to the subsequent load or store.
    //
    // TODO: Consider lowering to an LLVM intrinsic if there is any benefit:
    // 'call void @llvm.assume(i1 true) ["align"(i32* %arg0, i32 %arg1)]'
    auto pointerSrc = args.claimNext();
    (void)args.claimAll();
    out.add(pointerSrc);
    return;
  }

  llvm_unreachable("IRGen unimplemented for this builtin!");
}
