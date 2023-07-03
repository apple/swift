//===--- IRGenFunction.cpp - Swift Per-Function IR Generation -------------===//
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
//  This file implements basic setup and teardown for the class which
//  performs IR generation for function bodies.
//
//===----------------------------------------------------------------------===//
#include "swift/ABI/MetadataValues.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/IRGen/Linking.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/BinaryFormat/MachO.h"

#include "Callee.h"
#include "Explosion.h"
#include "GenPointerAuth.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "LoadableTypeInfo.h"

using namespace swift;
using namespace irgen;

static llvm::cl::opt<bool> EnableTrapDebugInfo(
    "enable-trap-debug-info", llvm::cl::init(true), llvm::cl::Hidden,
    llvm::cl::desc("Generate failure-message functions in the debug info"));

IRGenFunction::IRGenFunction(IRGenModule &IGM, llvm::Function *Fn,
                             OptimizationMode OptMode,
                             const SILDebugScope *DbgScope,
                             llvm::Optional<SILLocation> DbgLoc)
    : IGM(IGM), Builder(IGM.getLLVMContext(),
                        IGM.DebugInfo && !IGM.Context.LangOpts.DebuggerSupport),
      OptMode(OptMode), CurFn(Fn), DbgScope(DbgScope) {

  // Make sure the instructions in this function are attached its debug scope.
  if (IGM.DebugInfo) {
    // Functions, especially artificial thunks and closures, are often
    // generated on-the-fly while we are in the middle of another
    // function. Be nice and preserve the current debug location until
    // after we're done with this function.
    IGM.DebugInfo->pushLoc();
  }

  emitPrologue();
}

IRGenFunction::~IRGenFunction() {
  emitEpilogue();

  // Restore the debug location.
  if (IGM.DebugInfo) IGM.DebugInfo->popLoc();

  // Tear down any side-table data structures.
  if (LocalTypeData) destroyLocalTypeData();

  // All dynamically allocated metadata should have been cleaned up.
}

OptimizationMode IRGenFunction::getEffectiveOptimizationMode() const {
  if (OptMode != OptimizationMode::NotSet)
    return OptMode;

  return IGM.getOptions().OptMode;
}

bool IRGenFunction::canStackPromotePackMetadata() const {
  return IGM.getSILModule().getOptions().EnablePackMetadataStackPromotion &&
         !packMetadataStackPromotionDisabled;
}

ModuleDecl *IRGenFunction::getSwiftModule() const {
  return IGM.getSwiftModule();
}

SILModule &IRGenFunction::getSILModule() const {
  return IGM.getSILModule();
}

Lowering::TypeConverter &IRGenFunction::getSILTypes() const {
  return IGM.getSILTypes();
}

const IRGenOptions &IRGenFunction::getOptions() const {
  return IGM.getOptions();
}

// Returns the default atomicity of the module.
Atomicity IRGenFunction::getDefaultAtomicity() {
  return getSILModule().isDefaultAtomic() ? Atomicity::Atomic : Atomicity::NonAtomic;
}

/// Call the llvm.memcpy intrinsic.  The arguments need not already
/// be of i8* type.
void IRGenFunction::emitMemCpy(llvm::Value *dest, llvm::Value *src,
                               Size size, Alignment align) {
  emitMemCpy(dest, src, IGM.getSize(size), align);
}

void IRGenFunction::emitMemCpy(llvm::Value *dest, llvm::Value *src,
                               llvm::Value *size, Alignment align) {
  Builder.CreateMemCpy(dest, llvm::MaybeAlign(align.getValue()), src,
                       llvm::MaybeAlign(align.getValue()), size);
}

void IRGenFunction::emitMemCpy(Address dest, Address src, Size size) {
  emitMemCpy(dest, src, IGM.getSize(size));
}

void IRGenFunction::emitMemCpy(Address dest, Address src, llvm::Value *size) {
  // Map over to the inferior design of the LLVM intrinsic.
  emitMemCpy(dest.getAddress(), src.getAddress(), size,
             std::min(dest.getAlignment(), src.getAlignment()));
}

static llvm::Value *emitAllocatingCall(IRGenFunction &IGF, FunctionPointer fn,
                                       ArrayRef<llvm::Value *> args,
                                       const llvm::Twine &name) {
  auto allocAttrs = IGF.IGM.getAllocAttrs();
  llvm::CallInst *call =
    IGF.Builder.CreateCall(fn, makeArrayRef(args.begin(), args.size()));
  call->setAttributes(allocAttrs);
  return call;
}

/// Emit a 'raw' allocation, which has no heap pointer and is
/// not guaranteed to be zero-initialized.
llvm::Value *IRGenFunction::emitAllocRawCall(llvm::Value *size,
                                             llvm::Value *alignMask,
                                             const llvm::Twine &name) {
  // For now, all we have is swift_slowAlloc.
  return emitAllocatingCall(*this, IGM.getSlowAllocFunctionPointer(),
                            {size, alignMask}, name);
}

/// Emit a heap allocation.
llvm::Value *IRGenFunction::emitAllocObjectCall(llvm::Value *metadata,
                                                llvm::Value *size,
                                                llvm::Value *alignMask,
                                                const llvm::Twine &name) {
  // For now, all we have is swift_allocObject.
  return emitAllocatingCall(*this, IGM.getAllocObjectFunctionPointer(),
                            {metadata, size, alignMask}, name);
}

llvm::Value *IRGenFunction::emitInitStackObjectCall(llvm::Value *metadata,
                                                    llvm::Value *object,
                                                    const llvm::Twine &name) {
  llvm::CallInst *call = Builder.CreateCall(
      IGM.getInitStackObjectFunctionPointer(), {metadata, object}, name);
  call->setDoesNotThrow();
  return call;
}

llvm::Value *IRGenFunction::emitInitStaticObjectCall(llvm::Value *metadata,
                                                     llvm::Value *object,
                                                     const llvm::Twine &name) {
  llvm::CallInst *call = Builder.CreateCall(
      IGM.getInitStaticObjectFunctionPointer(), {metadata, object}, name);
  call->setDoesNotThrow();
  return call;
}

llvm::Value *IRGenFunction::emitVerifyEndOfLifetimeCall(llvm::Value *object,
                                                      const llvm::Twine &name) {
  llvm::CallInst *call = Builder.CreateCall(
      IGM.getVerifyEndOfLifetimeFunctionPointer(), {object}, name);
  call->setDoesNotThrow();
  return call;
}

void IRGenFunction::emitAllocBoxCall(llvm::Value *typeMetadata,
                                      llvm::Value *&box,
                                      llvm::Value *&valueAddress) {
  llvm::CallInst *call =
      Builder.CreateCall(IGM.getAllocBoxFunctionPointer(), typeMetadata);
  call->addFnAttr(llvm::Attribute::NoUnwind);

  box = Builder.CreateExtractValue(call, 0);
  valueAddress = Builder.CreateExtractValue(call, 1);
}

void IRGenFunction::emitMakeBoxUniqueCall(llvm::Value *box,
                                          llvm::Value *typeMetadata,
                                          llvm::Value *alignMask,
                                          llvm::Value *&outBox,
                                          llvm::Value *&outValueAddress) {
  llvm::CallInst *call = Builder.CreateCall(
      IGM.getMakeBoxUniqueFunctionPointer(), {box, typeMetadata, alignMask});
  call->addFnAttr(llvm::Attribute::NoUnwind);

  outBox = Builder.CreateExtractValue(call, 0);
  outValueAddress = Builder.CreateExtractValue(call, 1);
}


void IRGenFunction::emitDeallocBoxCall(llvm::Value *box,
                                        llvm::Value *typeMetadata) {
  llvm::CallInst *call =
      Builder.CreateCall(IGM.getDeallocBoxFunctionPointer(), box);
  call->setCallingConv(IGM.DefaultCC);
  call->addFnAttr(llvm::Attribute::NoUnwind);
}

llvm::Value *IRGenFunction::emitProjectBoxCall(llvm::Value *box,
                                                llvm::Value *typeMetadata) {
  llvm::CallInst *call =
      Builder.CreateCall(IGM.getProjectBoxFunctionPointer(), box);
  call->setCallingConv(IGM.DefaultCC);
  call->addFnAttr(llvm::Attribute::NoUnwind);
  return call;
}

llvm::Value *IRGenFunction::emitAllocEmptyBoxCall() {
  llvm::CallInst *call =
      Builder.CreateCall(IGM.getAllocEmptyBoxFunctionPointer(), {});
  call->setCallingConv(IGM.DefaultCC);
  call->addFnAttr(llvm::Attribute::NoUnwind);
  return call;
}

static void emitDeallocatingCall(IRGenFunction &IGF, FunctionPointer fn,
                                 std::initializer_list<llvm::Value *> args) {
  llvm::CallInst *call =
      IGF.Builder.CreateCall(fn, makeArrayRef(args.begin(), args.size()));
  call->setDoesNotThrow();
}

/// Emit a 'raw' deallocation, which has no heap pointer and is not
/// guaranteed to be zero-initialized.
void IRGenFunction::emitDeallocRawCall(llvm::Value *pointer,
                                       llvm::Value *size,
                                       llvm::Value *alignMask) {
  // For now, all we have is swift_slowDealloc.
  return emitDeallocatingCall(*this, IGM.getSlowDeallocFunctionPointer(),
                              {pointer, size, alignMask});
}

void IRGenFunction::emitTSanInoutAccessCall(llvm::Value *address) {
  auto fn = IGM.getTSanInoutAccessFunctionPointer();

  llvm::Value *castAddress = Builder.CreateBitCast(address, IGM.Int8PtrTy);

  // Passing 0 as the caller PC causes compiler-rt to get our PC.
  llvm::Value *callerPC = llvm::ConstantPointerNull::get(IGM.Int8PtrTy);

  // A magic number agreed upon with compiler-rt to indicate a modifying
  // access.
  const unsigned kExternalTagSwiftModifyingAccess = 0x1;
  llvm::Value *tagValue =
      llvm::ConstantInt::get(IGM.SizeTy, kExternalTagSwiftModifyingAccess);
  llvm::Value *castTag = Builder.CreateIntToPtr(tagValue, IGM.Int8PtrTy);

  Builder.CreateCall(fn, {castAddress, callerPC, castTag});
}

// This is shamelessly copied from clang's codegen. We need to get the clang
// functionality into a shared header so that platforms only
// needs to be updated in one place.
static unsigned getBaseMachOPlatformID(const llvm::Triple &TT) {
  switch (TT.getOS()) {
  case llvm::Triple::Darwin:
  case llvm::Triple::MacOSX:
    return llvm::MachO::PLATFORM_MACOS;
  case llvm::Triple::IOS:
    return llvm::MachO::PLATFORM_IOS;
  case llvm::Triple::TvOS:
    return llvm::MachO::PLATFORM_TVOS;
  case llvm::Triple::WatchOS:
    return llvm::MachO::PLATFORM_WATCHOS;
  default:
    return /*Unknown platform*/ 0;
  }
}

llvm::Value *
IRGenFunction::emitTargetOSVersionAtLeastCall(llvm::Value *major,
                                              llvm::Value *minor,
                                              llvm::Value *patch) {
  auto fn = IGM.getPlatformVersionAtLeastFunctionPointer();

  llvm::Value *platformID =
    llvm::ConstantInt::get(IGM.Int32Ty, getBaseMachOPlatformID(IGM.Triple));
  return Builder.CreateCall(fn, {platformID, major, minor, patch});
}

/// Initialize a relative indirectable pointer to the given value.
/// This always leaves the value in the direct state; if it's not a
/// far reference, it's the caller's responsibility to ensure that the
/// pointer ranges are sufficient.
void IRGenFunction::emitStoreOfRelativeIndirectablePointer(llvm::Value *value,
                                                           Address addr,
                                                           bool isFar) {
  value = Builder.CreatePtrToInt(value, IGM.IntPtrTy);
  auto addrAsInt =
    Builder.CreatePtrToInt(addr.getAddress(), IGM.IntPtrTy);

  auto difference = Builder.CreateSub(value, addrAsInt);
  if (!isFar) {
    difference = Builder.CreateTrunc(difference, IGM.RelativeAddressTy);
  }

  Builder.CreateStore(difference, addr);
}

llvm::Value *
IRGenFunction::emitLoadOfRelativePointer(Address addr, bool isFar,
                                         llvm::Type *expectedPointedToType,
                                         const llvm::Twine &name) {
  llvm::Value *value = Builder.CreateLoad(addr);
  assert(value->getType() ==
         (isFar ? IGM.FarRelativeAddressTy : IGM.RelativeAddressTy));
  if (!isFar) {
    value = Builder.CreateSExt(value, IGM.IntPtrTy);
  }
  auto *addrInt = Builder.CreatePtrToInt(addr.getAddress(), IGM.IntPtrTy);
  auto *uncastPointerInt = Builder.CreateAdd(addrInt, value);
  auto *uncastPointer = Builder.CreateIntToPtr(uncastPointerInt, IGM.Int8PtrTy);
  auto uncastPointerAddress =
      Address(uncastPointer, IGM.Int8Ty, IGM.getPointerAlignment());
  auto pointer =
      Builder.CreateElementBitCast(uncastPointerAddress, expectedPointedToType);
  return pointer.getAddress();
}

llvm::Value *IRGenFunction::emitLoadOfCompactFunctionPointer(
    Address addr, bool isFar, llvm::Type *expectedPointedToType,
    const llvm::Twine &name) {
  if (IGM.getOptions().CompactAbsoluteFunctionPointer) {
    llvm::Value *value = Builder.CreateLoad(addr);
    auto *uncastPointer = Builder.CreateIntToPtr(value, IGM.Int8PtrTy);
    auto pointer = Builder.CreateElementBitCast(
        Address(uncastPointer, IGM.Int8Ty, IGM.getPointerAlignment()),
        expectedPointedToType);
    return pointer.getAddress();
  } else {
    return emitLoadOfRelativePointer(addr, isFar, expectedPointedToType, name);
  }
}

void IRGenFunction::emitFakeExplosion(const TypeInfo &type,
                                      Explosion &explosion) {
  if (!isa<LoadableTypeInfo>(type)) {
    explosion.add(llvm::UndefValue::get(type.getStorageType()->getPointerTo()));
    return;
  }

  ExplosionSchema schema = cast<LoadableTypeInfo>(type).getSchema();
  for (auto &element : schema) {
    llvm::Type *elementType;
    if (element.isAggregate()) {
      elementType = element.getAggregateType()->getPointerTo();
    } else {
      elementType = element.getScalarType();
    }
    
    explosion.add(llvm::UndefValue::get(elementType));
  }
}

void IRGenFunction::unimplemented(SourceLoc Loc, StringRef Message) {
  return IGM.unimplemented(Loc, Message);
}

// Debug output for Explosions.

void Explosion::print(llvm::raw_ostream &OS) {
  for (auto value : makeArrayRef(Values).slice(NextValue)) {
    value->print(OS);
    OS << '\n';
  }
}

void Explosion::dump() {
  print(llvm::errs());
}

llvm::Value *Offset::getAsValue(IRGenFunction &IGF) const {
  if (isStatic()) {
    return IGF.IGM.getSize(getStatic());
  } else {
    return getDynamic();
  }
}

Offset Offset::offsetBy(IRGenFunction &IGF, Size other) const {
  if (isStatic()) {
    return Offset(getStatic() + other);
  }
  auto otherVal = llvm::ConstantInt::get(IGF.IGM.SizeTy, other.getValue());
  return Offset(IGF.Builder.CreateAdd(getDynamic(), otherVal));
}

Address IRGenFunction::emitAddressAtOffset(llvm::Value *base, Offset offset,
                                           llvm::Type *objectTy,
                                           Alignment objectAlignment,
                                           const llvm::Twine &name) {
  // Use a slightly more obvious IR pattern if it's a multiple of the type
  // size.  I'll confess that this is partly just to avoid updating tests.
  if (offset.isStatic()) {
    auto byteOffset = offset.getStatic();
    Size objectSize(IGM.DataLayout.getTypeAllocSize(objectTy));
    if (byteOffset.isMultipleOf(objectSize)) {
      // Cast to T*.
      auto objectPtrTy = objectTy->getPointerTo();
      base = Builder.CreateBitCast(base, objectPtrTy);

      // GEP to the slot, computing the index as a signed number.
      auto scaledIndex =
        int64_t(byteOffset.getValue()) / int64_t(objectSize.getValue());
      auto indexValue = IGM.getSize(Size(scaledIndex));
      auto slotPtr = Builder.CreateInBoundsGEP(objectTy, base, indexValue);

      return Address(slotPtr, objectTy, objectAlignment);
    }
  }

  // GEP to the slot.
  auto offsetValue = offset.getAsValue(*this);
  auto slotPtr = emitByteOffsetGEP(base, offsetValue, objectTy);
  return Address(slotPtr, objectTy, objectAlignment);
}

llvm::CallInst *IRBuilder::CreateNonMergeableTrap(IRGenModule &IGM,
                                                  StringRef failureMsg) {
  if (IGM.IRGen.Opts.shouldOptimize()) {
    // Emit unique side-effecting inline asm calls in order to eliminate
    // the possibility that an LLVM optimization or code generation pass
    // will merge these blocks back together again. We emit an empty asm
    // string with the side-effect flag set, and with a unique integer
    // argument for each cond_fail we see in the function.
    llvm::IntegerType *asmArgTy = IGM.Int32Ty;
    llvm::Type *argTys = {asmArgTy};
    llvm::FunctionType *asmFnTy =
        llvm::FunctionType::get(IGM.VoidTy, argTys, false /* = isVarArg */);
    llvm::InlineAsm *inlineAsm =
        llvm::InlineAsm::get(asmFnTy, "", "n", true /* = SideEffects */);
    CreateAsmCall(inlineAsm,
                  llvm::ConstantInt::get(asmArgTy, NumTrapBarriers++));
  }

  // Emit the trap instruction.
  llvm::Function *trapIntrinsic =
      llvm::Intrinsic::getDeclaration(&IGM.Module, llvm::Intrinsic::trap);
  if (EnableTrapDebugInfo && IGM.DebugInfo && !failureMsg.empty()) {
    IGM.DebugInfo->addFailureMessageToCurrentLoc(*this, failureMsg);
  }
  auto Call = IRBuilderBase::CreateCall(trapIntrinsic, {});
  setCallingConvUsingCallee(Call);

  if (!IGM.IRGen.Opts.TrapFuncName.empty()) {
    auto A = llvm::Attribute::get(getContext(), "trap-func-name",
                                  IGM.IRGen.Opts.TrapFuncName);
    Call->addFnAttr(A);
  }

  return Call;
}

void IRGenFunction::emitTrap(StringRef failureMessage, bool EmitUnreachable) {
  Builder.CreateNonMergeableTrap(IGM, failureMessage);
  if (EmitUnreachable)
    Builder.CreateUnreachable();
}

Address IRGenFunction::emitTaskAlloc(llvm::Value *size, Alignment alignment) {
  auto *call = Builder.CreateCall(IGM.getTaskAllocFunctionPointer(), {size});
  call->setDoesNotThrow();
  call->setCallingConv(IGM.SwiftCC);
  auto address = Address(call, IGM.Int8Ty, alignment);
  return address;
}

void IRGenFunction::emitTaskDealloc(Address address) {
  auto *call = Builder.CreateCall(IGM.getTaskDeallocFunctionPointer(),
                                  {address.getAddress()});
  call->setDoesNotThrow();
  call->setCallingConv(IGM.SwiftCC);
}

llvm::Value *IRGenFunction::alignUpToMaximumAlignment(llvm::Type *sizeTy, llvm::Value *val) {
  auto *alignMask = llvm::ConstantInt::get(sizeTy, MaximumAlignment - 1);
  auto *invertedMask = Builder.CreateNot(alignMask);
  return Builder.CreateAnd(Builder.CreateAdd(val, alignMask), invertedMask);
}

/// Returns the current task \p currTask as a Builtin.RawUnsafeContinuation at +1.
static llvm::Value *unsafeContinuationFromTask(IRGenFunction &IGF,
                                               llvm::Value *currTask) {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;

  auto &rawPointerTI = IGM.getRawUnsafeContinuationTypeInfo();
  return Builder.CreateBitOrPointerCast(currTask, rawPointerTI.getStorageType());
}

static llvm::Value *emitLoadOfResumeContextFromTask(IRGenFunction &IGF,
                                                    llvm::Value *task) {
  // Task.ResumeContext is at field index 8 within SwiftTaskTy. The offset comes
  // from 7 pointers (two within the single RefCountedStructTy) and 2 Int32
  // fields.
  const unsigned taskResumeContextIndex = 8;
  const Size taskResumeContextOffset = (7 * IGF.IGM.getPointerSize()) + Size(8);

  auto addr = Address(task, IGF.IGM.SwiftTaskTy, IGF.IGM.getPointerAlignment());
  auto resumeContextAddr = IGF.Builder.CreateStructGEP(
    addr, taskResumeContextIndex, taskResumeContextOffset);
  llvm::Value *resumeContext = IGF.Builder.CreateLoad(resumeContextAddr);
  if (auto &schema = IGF.getOptions().PointerAuth.TaskResumeContext) {
    auto info = PointerAuthInfo::emit(IGF, schema,
                                      resumeContextAddr.getAddress(),
                                      PointerAuthEntity());
    resumeContext = emitPointerAuthAuth(IGF, resumeContext, info);
  }
  return resumeContext;
}

static Address emitLoadOfContinuationContext(IRGenFunction &IGF,
                                             llvm::Value *continuation) {
  auto ptr = emitLoadOfResumeContextFromTask(IGF, continuation);
  ptr = IGF.Builder.CreateBitCast(ptr, IGF.IGM.ContinuationAsyncContextPtrTy);
  return Address(ptr, IGF.IGM.ContinuationAsyncContextTy,
                 IGF.IGM.getAsyncContextAlignment());
}

static Address emitAddrOfContinuationNormalResultPointer(IRGenFunction &IGF,
                                                         Address context) {
  assert(context.getType() == IGF.IGM.ContinuationAsyncContextPtrTy);
  auto offset = 5 * IGF.IGM.getPointerSize();
  return IGF.Builder.CreateStructGEP(context, 4, offset);
}

static Address emitAddrOfContinuationErrorResultPointer(IRGenFunction &IGF,
                                                        Address context) {
  assert(context.getType() == IGF.IGM.ContinuationAsyncContextPtrTy);
  auto offset = 4 * IGF.IGM.getPointerSize();
  return IGF.Builder.CreateStructGEP(context, 3, offset);
}

void IRGenFunction::emitGetAsyncContinuation(SILType resumeTy,
                                             StackAddress resultAddr,
                                             Explosion &out,
                                             bool canThrow) {
  // A continuation is just a reference to the current async task,
  // parked with a special context:
  //
  // struct ContinuationAsyncContext : AsyncContext {
  //   std::atomic<size_t> awaitSynchronization;
  //   SwiftError *errResult;
  //   Result *result;
  //   ExecutorRef resumeExecutor;
  // };
  //
  // We need fill out this context essentially as if we were calling
  // something.

  // Create and setup the continuation context.
  auto continuationContext =
    createAlloca(IGM.ContinuationAsyncContextTy,
                 IGM.getAsyncContextAlignment());
  AsyncCoroutineCurrentContinuationContext = continuationContext.getAddress();
  // TODO: add lifetime with matching lifetime in await_async_continuation

  // We're required to initialize three fields in the continuation
  // context before calling swift_continuation_init:

  // - Parent, the parent context pointer, which we initialize to
  //   the current context.
  auto contextBase = Builder.CreateStructGEP(continuationContext, 0, Size(0));
  auto parentContextAddr = Builder.CreateStructGEP(contextBase, 0, Size(0));
  llvm::Value *asyncContextValue =
    Builder.CreateBitCast(getAsyncContext(), IGM.SwiftContextPtrTy);
  if (auto schema = IGM.getOptions().PointerAuth.AsyncContextParent) {
    auto authInfo = PointerAuthInfo::emit(*this, schema,
                                          parentContextAddr.getAddress(),
                                          PointerAuthEntity());
    asyncContextValue = emitPointerAuthSign(*this, asyncContextValue, authInfo);
  }
  Builder.CreateStore(asyncContextValue, parentContextAddr);

  // - NormalResult, the pointer to the normal result, which we initialize
  //   to the result address that we were given, or else a temporary slot.
  //   TODO: emit lifetime.start for this temporary, paired with a
  //   lifetime.end within the await after we take from the slot.
  auto normalResultAddr =
    emitAddrOfContinuationNormalResultPointer(*this, continuationContext);
  if (!resultAddr.getAddress().isValid()) {
    auto &resumeTI = getTypeInfo(resumeTy);
    resultAddr =
      resumeTI.allocateStack(*this, resumeTy, "async.continuation.result");
  }
  Builder.CreateStore(Builder.CreateBitOrPointerCast(
                            resultAddr.getAddress().getAddress(),
                            IGM.OpaquePtrTy),
                      normalResultAddr);

  // - ResumeParent, the continuation function pointer, which we initialize
  //   with the result of a new call to @llvm.coro.async.resume; we'll pair
  //   this with a suspend point when we emit the corresponding
  //   await_async_continuation.
  auto coroResume =
    Builder.CreateIntrinsicCall(llvm::Intrinsic::coro_async_resume, {});
  auto resumeFunctionAddr =
    Builder.CreateStructGEP(contextBase, 1, IGM.getPointerSize());
  llvm::Value *coroResumeValue =
    Builder.CreateBitOrPointerCast(coroResume,
                                   IGM.TaskContinuationFunctionPtrTy);
  if (auto schema = IGM.getOptions().PointerAuth.AsyncContextResume) {
    auto authInfo = PointerAuthInfo::emit(*this, schema,
                                          resumeFunctionAddr.getAddress(),
                                          PointerAuthEntity());
    coroResumeValue = emitPointerAuthSign(*this, coroResumeValue, authInfo);
  }
  Builder.CreateStore(coroResumeValue, resumeFunctionAddr);

  // Save the resume intrinsic call for await_async_continuation.
  assert(AsyncCoroutineCurrentResume == nullptr &&
         "Don't support nested get_async_continuation");
  AsyncCoroutineCurrentResume = coroResume;

  AsyncContinuationFlags flags;
  if (canThrow) flags.setCanThrow(true);

  // Call the swift_continuation_init runtime function to initialize
  // the rest of the continuation and return the task pointer back to us.
  auto task = Builder.CreateCall(IGM.getContinuationInitFunctionPointer(),
                                 {continuationContext.getAddress(),
                                  IGM.getSize(Size(flags.getOpaqueValue()))});
  task->setCallingConv(IGM.SwiftCC);

  // TODO: if we have a better idea of what executor to return to than
  // the current executor, overwrite the ResumeToExecutor field.

  auto unsafeContinuation = unsafeContinuationFromTask(*this, task);
  out.add(unsafeContinuation);
}

static bool shouldUseContinuationAwait(IRGenModule &IGM) {
  auto &ctx = IGM.Context;
  auto module = ctx.getLoadedModule(ctx.Id_Concurrency);
  assert(module && "building async code without concurrency library");
  SmallVector<ValueDecl *, 1> results;
  module->lookupValue(ctx.getIdentifier("_abiEnableAwaitContinuation"),
                      NLKind::UnqualifiedLookup, results);
  assert(results.size() <= 1);
  return !results.empty();
}

void IRGenFunction::emitAwaitAsyncContinuation(
    SILType resumeTy, bool isIndirectResult,
    Explosion &outDirectResult, llvm::BasicBlock *&normalBB,
    llvm::PHINode *&optionalErrorResult, llvm::BasicBlock *&optionalErrorBB) {
  assert(AsyncCoroutineCurrentContinuationContext && "no active continuation");
  Address continuationContext(AsyncCoroutineCurrentContinuationContext,
                              IGM.ContinuationAsyncContextTy,
                              IGM.getAsyncContextAlignment());

  // Call swift_continuation_await to check whether the continuation
  // has already been resumed.
  bool useContinuationAwait = shouldUseContinuationAwait(IGM);

  // As a temporary hack for compatibility with SDKs that don't provide
  // swift_continuation_await, emit the old inline sequence.  This can
  // be removed as soon as we're sure that such SDKs don't exist.
  if (!useContinuationAwait) {
    auto contAwaitSyncAddr =
      Builder.CreateStructGEP(continuationContext, 2,
                              3 * IGM.getPointerSize()).getAddress();

    auto pendingV = llvm::ConstantInt::get(
        IGM.SizeTy, unsigned(ContinuationStatus::Pending));
    auto awaitedV = llvm::ConstantInt::get(
        IGM.SizeTy, unsigned(ContinuationStatus::Awaited));
    auto results = Builder.CreateAtomicCmpXchg(
        contAwaitSyncAddr, pendingV, awaitedV, llvm::MaybeAlign(),
        llvm::AtomicOrdering::Release /*success ordering*/,
        llvm::AtomicOrdering::Acquire /* failure ordering */,
        llvm::SyncScope::System);
    auto firstAtAwait = Builder.CreateExtractValue(results, 1);
    auto contBB = createBasicBlock("await.async.resume");
    auto abortBB = createBasicBlock("await.async.abort");
    Builder.CreateCondBr(firstAtAwait, abortBB, contBB);
    Builder.emitBlock(abortBB);
    {
      // We were the first to the sync point. "Abort" (return from the
      // coroutine partial function, without making a tail call to anything)
      // because the continuation result is not available yet. When the
      // continuation is later resumed, the task will get scheduled
      // starting from the suspension point.
      emitCoroutineOrAsyncExit();
    }

    Builder.emitBlock(contBB);
  }

  {
    // Set up the suspend point.
    SmallVector<llvm::Value *, 8> arguments;
    unsigned swiftAsyncContextIndex = 0;
    arguments.push_back(IGM.getInt32(swiftAsyncContextIndex)); // context index
    arguments.push_back(AsyncCoroutineCurrentResume);
    auto resumeProjFn = getOrCreateResumePrjFn();
    arguments.push_back(
        Builder.CreateBitOrPointerCast(resumeProjFn, IGM.Int8PtrTy));

    llvm::Constant *awaitFnPtr;
    if (useContinuationAwait) {
      awaitFnPtr = IGM.getAwaitAsyncContinuationFn();
    } else {
      auto resumeFnPtr =
        getFunctionPointerForResumeIntrinsic(AsyncCoroutineCurrentResume);
      awaitFnPtr = createAsyncDispatchFn(resumeFnPtr, {IGM.Int8PtrTy});
    }
    arguments.push_back(
        Builder.CreateBitOrPointerCast(awaitFnPtr, IGM.Int8PtrTy));

    if (useContinuationAwait) {
      arguments.push_back(continuationContext.getAddress());
    } else {
      arguments.push_back(AsyncCoroutineCurrentResume);
      arguments.push_back(Builder.CreateBitOrPointerCast(
        continuationContext.getAddress(), IGM.Int8PtrTy));
    }

    auto resultTy =
        llvm::StructType::get(IGM.getLLVMContext(), {IGM.Int8PtrTy}, false /*packed*/);
    emitSuspendAsyncCall(swiftAsyncContextIndex, resultTy, arguments);
  }

  // If there's an error destination (i.e. if the continuation is throwing),
  // load the error value out and check whether it's null.  If so, branch
  // to the error destination.
  if (optionalErrorBB) {
    auto normalContBB = createBasicBlock("await.async.normal");
    auto contErrResultAddr =
        emitAddrOfContinuationErrorResultPointer(*this, continuationContext);
    auto errorRes = Builder.CreateLoad(contErrResultAddr);
    auto nullError = llvm::Constant::getNullValue(errorRes->getType());
    auto hasError = Builder.CreateICmpNE(errorRes, nullError);
    optionalErrorResult->addIncoming(errorRes, Builder.GetInsertBlock());
    Builder.CreateCondBr(hasError, optionalErrorBB, normalContBB);
    Builder.emitBlock(normalContBB);
  }

  // We're now on the normal-result path.  If we didn't have an indirect
  // result slot, load from the temporary we created during
  // get_async_continuation.
  if (!isIndirectResult) {
    auto contResultAddrAddr =
        emitAddrOfContinuationNormalResultPointer(*this, continuationContext);
    auto resultAddrVal =
        Builder.CreateLoad(contResultAddrAddr);
    // Take the result.
    auto &resumeTI = cast<LoadableTypeInfo>(getTypeInfo(resumeTy));
    auto resultStorageTy = resumeTI.getStorageType();
    auto resultAddr =
        Address(Builder.CreateBitOrPointerCast(resultAddrVal,
                                               resultStorageTy->getPointerTo()),
                resultStorageTy, resumeTI.getFixedAlignment());
    resumeTI.loadAsTake(*this, resultAddr, outDirectResult);
  }

  Builder.CreateBr(normalBB);
  AsyncCoroutineCurrentResume = nullptr;
  AsyncCoroutineCurrentContinuationContext = nullptr;
}

void IRGenFunction::emitResumeAsyncContinuationReturning(
                        llvm::Value *continuation, llvm::Value *srcPtr,
                        SILType valueTy, bool throwing) {
  continuation = Builder.CreateBitCast(continuation, IGM.SwiftTaskPtrTy);
  auto &valueTI = getTypeInfo(valueTy);
  Address srcAddr = valueTI.getAddressForPointer(srcPtr);

  // Extract the destination value pointer and cast it from an opaque
  // pointer type.
  Address context = emitLoadOfContinuationContext(*this, continuation);
  auto destPtrAddr = emitAddrOfContinuationNormalResultPointer(*this, context);
  auto destPtr = Builder.CreateBitCast(Builder.CreateLoad(destPtrAddr),
                                     valueTI.getStorageType()->getPointerTo());
  Address destAddr = valueTI.getAddressForPointer(destPtr);

  valueTI.initializeWithTake(*this, destAddr, srcAddr, valueTy,
                             /*outlined*/ false);

  auto call = Builder.CreateCall(
      throwing ? IGM.getContinuationThrowingResumeFunctionPointer()
               : IGM.getContinuationResumeFunctionPointer(),
      {continuation});
  call->setCallingConv(IGM.SwiftCC);
}

void IRGenFunction::emitResumeAsyncContinuationThrowing(
                        llvm::Value *continuation, llvm::Value *error) {
  continuation = Builder.CreateBitCast(continuation, IGM.SwiftTaskPtrTy);
  auto call = Builder.CreateCall(
      IGM.getContinuationThrowingResumeWithErrorFunctionPointer(),
      {continuation, error});
  call->setCallingConv(IGM.SwiftCC);
}
