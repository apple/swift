//===--- GenCall.cpp - Swift IR Generation for Function Calls -------------===//
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
//  This file implements IR generation for function signature lowering
//  in Swift.  This includes creating the IR type, collecting IR attributes,
//  performing calls, and supporting prologue and epilogue emission.
//
//===----------------------------------------------------------------------===//

#include "swift/ABI/MetadataValues.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/Runtime/Config.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecordLayout.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "llvm/IR/GlobalPtrAuthInfo.h"
#include "llvm/Support/Compiler.h"

#include "CallEmission.h"
#include "EntryPointArgumentEmission.h"
#include "Explosion.h"
#include "GenCall.h"
#include "GenFunc.h"
#include "GenHeap.h"
#include "GenObjC.h"
#include "GenPointerAuth.h"
#include "GenPoly.h"
#include "GenProto.h"
#include "GenType.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "LoadableTypeInfo.h"
#include "NativeConventionSchema.h"
#include "Signature.h"
#include "StructLayout.h"

using namespace swift;
using namespace irgen;

static Size getYieldOnceCoroutineBufferSize(IRGenModule &IGM) {
  return NumWords_YieldOnceBuffer * IGM.getPointerSize();
}
static Alignment getYieldOnceCoroutineBufferAlignment(IRGenModule &IGM) {
  return IGM.getPointerAlignment();
}

static Size getYieldManyCoroutineBufferSize(IRGenModule &IGM) {
  return NumWords_YieldManyBuffer * IGM.getPointerSize();
}
static Alignment getYieldManyCoroutineBufferAlignment(IRGenModule &IGM) {
  return IGM.getPointerAlignment();
}

static Size getCoroutineContextSize(IRGenModule &IGM,
                                    CanSILFunctionType fnType) {
  switch (fnType->getCoroutineKind()) {
  case SILCoroutineKind::None:
    llvm_unreachable("expand a coroutine");
  case SILCoroutineKind::YieldOnce:
    return getYieldOnceCoroutineBufferSize(IGM);
  case SILCoroutineKind::YieldMany:
    return getYieldManyCoroutineBufferSize(IGM);
  }
  llvm_unreachable("bad kind");
}

AsyncContextLayout irgen::getAsyncContextLayout(IRGenModule &IGM,
                                                SILFunction *function) {
  SubstitutionMap forwardingSubstitutionMap =
      function->getForwardingSubstitutionMap();
  CanSILFunctionType originalType = function->getLoweredFunctionType();
  CanSILFunctionType substitutedType = originalType->substGenericArgs(
      IGM.getSILModule(), forwardingSubstitutionMap,
      IGM.getMaximalTypeExpansionContext());
  auto layout = getAsyncContextLayout(IGM, originalType, substitutedType,
                                      forwardingSubstitutionMap);
  return layout;
}

AsyncContextLayout
irgen::getAsyncContextLayout(IRGenModule &IGM, CanSILFunctionType originalType,
                             CanSILFunctionType substitutedType,
                             SubstitutionMap substitutionMap) {
  SmallVector<const TypeInfo *, 4> typeInfos;
  SmallVector<SILType, 4> valTypes;
  SmallVector<AsyncContextLayout::ArgumentInfo, 4> paramInfos;
  bool isCoroutine = originalType->isCoroutine();
  SmallVector<SILYieldInfo, 4> yieldInfos;
  SmallVector<SILResultInfo, 4> indirectReturnInfos;
  SmallVector<SILResultInfo, 4> directReturnInfos;

  auto parameters = substitutedType->getParameters();
  SILFunctionConventions fnConv(substitutedType, IGM.getSILModule());

  auto addTaskContinuationFunction = [&]() {
    auto ty = SILType();
    auto &ti = IGM.getTaskContinuationFunctionPtrTypeInfo();
    valTypes.push_back(ty);
    typeInfos.push_back(&ti);
  };
  auto addExecutor = [&]() {
    auto ty = SILType();
    auto &ti = IGM.getSwiftExecutorPtrTypeInfo();
    valTypes.push_back(ty);
    typeInfos.push_back(&ti);
  };

  // AsyncContext * __ptrauth_swift_async_context_parent Parent;
  {
    auto ty = SILType();
    auto &ti = IGM.getSwiftContextPtrTypeInfo();
    valTypes.push_back(ty);
    typeInfos.push_back(&ti);
  }

  // TaskContinuationFunction * __ptrauth_swift_async_context_resume
  //     ResumeParent;
  addTaskContinuationFunction();

  // ExecutorRef ResumeParentExecutor;
  addExecutor();

  // AsyncContextFlags Flags;
  {
    auto ty = SILType::getPrimitiveObjectType(
        BuiltinIntegerType::get(32, IGM.IRGen.SIL.getASTContext())
            ->getCanonicalType());
    const auto &ti = IGM.getTypeInfo(ty);
    valTypes.push_back(ty);
    typeInfos.push_back(&ti);
  }

  if (isCoroutine) {
    // SwiftPartialFunction * __ptrauth(...) yieldToCaller?;
    addTaskContinuationFunction();
  }

  //   SwiftError *errorResult;
  auto errorCanType = IGM.Context.getExceptionType();
  auto errorType = SILType::getPrimitiveObjectType(errorCanType);
  auto &errorTypeInfo = IGM.getTypeInfoForLowered(errorCanType);
  typeInfos.push_back(&errorTypeInfo);
  valTypes.push_back(errorType);

  bool canHaveValidError = substitutedType->hasErrorResult();

  //   IndirectResultTypes *indirectResults...;
  auto indirectResults = fnConv.getIndirectSILResults();
  for (auto indirectResult : indirectResults) {
    auto ty =
        fnConv.getSILType(indirectResult, IGM.getMaximalTypeExpansionContext());
    auto retLoweringTy = CanInOutType::get(ty.getASTType());
    auto &ti = IGM.getTypeInfoForLowered(retLoweringTy);
    valTypes.push_back(ty);
    typeInfos.push_back(&ti);
    indirectReturnInfos.push_back(indirectResult);
  }

  // union {
  if (isCoroutine) {
    // SwiftPartialFunction * __ptrauth(...) resumeFromYield?
    addTaskContinuationFunction();
    // SwiftPartialFunction * __ptrauth(...) abortFromYield?
    addTaskContinuationFunction();
    // SwiftActor * __ptrauth(...) calleeActorDuringYield?
    addExecutor();
    // YieldTypes yieldValues...
    for (auto yield : fnConv.getYields()) {
      auto ty = fnConv.getSILType(yield, IGM.getMaximalTypeExpansionContext());
      auto &ti = IGM.getTypeInfoForLowered(ty.getASTType());
      valTypes.push_back(ty);
      typeInfos.push_back(&ti);
      yieldInfos.push_back(yield);
    }
  } else {
    //     ResultTypes directResults...;
    for (auto result : fnConv.getDirectSILResults()) {
      auto ty = fnConv.getSILType(result, IGM.getMaximalTypeExpansionContext());
      auto &ti = IGM.getTypeInfoForLowered(ty.getASTType());
      valTypes.push_back(ty);
      typeInfos.push_back(&ti);
      directReturnInfos.push_back(result);
    }
  }

  //   SelfType self?;
  bool hasSelf = hasSelfContextParameter(substitutedType);
  SILParameterInfo localContextParameter =
      hasSelf ? parameters.back() : SILParameterInfo();
  if (hasSelf) {
    parameters = parameters.drop_back();
  }

  Optional<AsyncContextLayout::ArgumentInfo> localContextInfo = llvm::None;
  if (hasSelf) {
    assert(originalType->getRepresentation() !=
           SILFunctionTypeRepresentation::Thick);
    SILType ty = IGM.silConv.getSILType(localContextParameter, substitutedType,
                                        IGM.getMaximalTypeExpansionContext());
    auto argumentLoweringType =
        getArgumentLoweringType(ty.getASTType(), localContextParameter,
                                /*isNoEscape*/ true);

    auto &ti = IGM.getTypeInfoForLowered(argumentLoweringType);
    valTypes.push_back(ty);
    typeInfos.push_back(&ti);
    localContextInfo = {ty, localContextParameter.getConvention()};
  } else {
    auto &ti = IGM.getNativeObjectTypeInfo();
    SILType ty = SILType::getNativeObjectType(IGM.Context);
    valTypes.push_back(ty);
    typeInfos.push_back(&ti);
    localContextInfo = {ty, substitutedType->getCalleeConvention()};
  }

  //   ArgTypes formalArguments...;
  for (auto parameter : parameters) {
    SILType ty = IGM.silConv.getSILType(parameter, substitutedType,
                                        IGM.getMaximalTypeExpansionContext());

    auto argumentLoweringType =
        getArgumentLoweringType(ty.getASTType(), parameter,
                                /*isNoEscape*/ true);

    auto &ti = IGM.getTypeInfoForLowered(argumentLoweringType);

    valTypes.push_back(ty);
    typeInfos.push_back(&ti);
    paramInfos.push_back({ty, parameter.getConvention()});
  }
  auto bindings = NecessaryBindings::forAsyncFunctionInvocation(
      IGM, originalType, substitutionMap);
  if (!bindings.empty()) {
    auto bindingsSize = bindings.getBufferSize(IGM);
    auto &bindingsTI =
        IGM.getOpaqueStorageTypeInfo(bindingsSize, IGM.getPointerAlignment());
    valTypes.push_back(SILType());
    typeInfos.push_back(&bindingsTI);
  }

  Optional<AsyncContextLayout::TrailingWitnessInfo> trailingWitnessInfo;
  if (originalType->getRepresentation() ==
      SILFunctionTypeRepresentation::WitnessMethod) {
    assert(getTrailingWitnessSignatureLength(IGM, originalType) == 2);

    // First, the Self metadata.
    {
      auto ty = SILType();
      auto &ti = IGM.getTypeMetadataPtrTypeInfo();
      valTypes.push_back(ty);
      typeInfos.push_back(&ti);
    }
    // Then, the Self witness table.
    {
      auto ty = SILType();
      auto &ti = IGM.getWitnessTablePtrTypeInfo();
      valTypes.push_back(ty);
      typeInfos.push_back(&ti);
    }
    trailingWitnessInfo = AsyncContextLayout::TrailingWitnessInfo();
  }

  return AsyncContextLayout(IGM, LayoutStrategy::Optimal, valTypes, typeInfos,
                            originalType, substitutedType, substitutionMap,
                            std::move(bindings), trailingWitnessInfo, errorType,
                            canHaveValidError, paramInfos, isCoroutine,
                            yieldInfos, indirectReturnInfos, directReturnInfos,
                            localContextInfo);
}

AsyncContextLayout::AsyncContextLayout(
    IRGenModule &IGM, LayoutStrategy strategy, ArrayRef<SILType> fieldTypes,
    ArrayRef<const TypeInfo *> fieldTypeInfos, CanSILFunctionType originalType,
    CanSILFunctionType substitutedType, SubstitutionMap substitutionMap,
    NecessaryBindings &&bindings,
    Optional<TrailingWitnessInfo> trailingWitnessInfo, SILType errorType,
    bool canHaveValidError, ArrayRef<ArgumentInfo> argumentInfos,
    bool isCoroutine, ArrayRef<SILYieldInfo> yieldInfos,
    ArrayRef<SILResultInfo> indirectReturnInfos,
    ArrayRef<SILResultInfo> directReturnInfos,
    Optional<AsyncContextLayout::ArgumentInfo> localContextInfo)
    : StructLayout(IGM, /*decl=*/nullptr, LayoutKind::NonHeapObject, strategy,
                   fieldTypeInfos, /*typeToFill*/ nullptr),
      IGM(IGM), originalType(originalType), substitutedType(substitutedType),
      substitutionMap(substitutionMap), errorType(errorType),
      canHaveValidError(canHaveValidError), isCoroutine(isCoroutine),
      yieldInfos(yieldInfos.begin(), yieldInfos.end()),
      directReturnInfos(directReturnInfos.begin(), directReturnInfos.end()),
      indirectReturnInfos(indirectReturnInfos.begin(),
                          indirectReturnInfos.end()),
      localContextInfo(localContextInfo), bindings(std::move(bindings)),
      trailingWitnessInfo(trailingWitnessInfo),
      argumentInfos(argumentInfos.begin(), argumentInfos.end()) {
#ifndef NDEBUG
  assert(fieldTypeInfos.size() == fieldTypes.size() &&
         "type infos don't match types");
  if (isCoroutine) {
    assert(directReturnInfos.empty());
  } else {
    assert(yieldInfos.empty());
  }
  if (!bindings.empty()) {
    assert(fieldTypeInfos.size() >= 2 && "no field for bindings");
    auto fixedBindingsField =
        dyn_cast<FixedTypeInfo>(fieldTypeInfos[getBindingsIndex()]);
    assert(fixedBindingsField && "bindings field is not fixed size");
    assert(fixedBindingsField->getFixedSize() == bindings.getBufferSize(IGM) &&
           fixedBindingsField->getFixedAlignment() ==
               IGM.getPointerAlignment() &&
           "bindings field doesn't fit bindings");
  }
  assert(this->isFixedLayout());
#endif
}

static Alignment getAsyncContextAlignment(IRGenModule &IGM) {
  return IGM.getPointerAlignment();
}

void IRGenFunction::setupAsync() {
  llvm::Value *t = CurFn->getArg((unsigned)AsyncFunctionArgumentIndex::Task);
  asyncTaskLocation = createAlloca(t->getType(), IGM.getPointerAlignment());
  Builder.CreateStore(t, asyncTaskLocation);

  llvm::Value *e = CurFn->getArg((unsigned)AsyncFunctionArgumentIndex::Executor);
  asyncExecutorLocation = createAlloca(e->getType(), IGM.getPointerAlignment());
  Builder.CreateStore(e, asyncExecutorLocation);

  llvm::Value *c = CurFn->getArg((unsigned)AsyncFunctionArgumentIndex::Context);
  asyncContextLocation = createAlloca(c->getType(), IGM.getPointerAlignment());
  Builder.CreateStore(c, asyncContextLocation);
}

llvm::Value *IRGenFunction::getAsyncTask() {
  assert(isAsync());
  return Builder.CreateLoad(asyncTaskLocation);
}

llvm::Value *IRGenFunction::getAsyncExecutor() {
  assert(isAsync());
  return Builder.CreateLoad(asyncExecutorLocation);
}

llvm::Value *IRGenFunction::getAsyncContext() {
  assert(isAsync());
  return Builder.CreateLoad(asyncContextLocation);
}

llvm::CallInst *IRGenFunction::emitSuspendAsyncCall(ArrayRef<llvm::Value *> args) {
  auto *id =
    Builder.CreateIntrinsicCall(llvm::Intrinsic::coro_suspend_async, args);

  // Update the current values of task, executor and context.

  auto *rawTask = Builder.CreateExtractValue(id,
      (unsigned)AsyncFunctionArgumentIndex::Task);
  auto *task = Builder.CreateBitCast(rawTask, IGM.SwiftTaskPtrTy);
  Builder.CreateStore(task, asyncTaskLocation);

  auto *rawExecutor = Builder.CreateExtractValue(id,
      (unsigned)AsyncFunctionArgumentIndex::Executor);
  auto *executor = Builder.CreateBitCast(rawExecutor, IGM.SwiftExecutorPtrTy);
  Builder.CreateStore(executor, asyncExecutorLocation);

  auto *calleeContext = Builder.CreateExtractValue(id,
      (unsigned)AsyncFunctionArgumentIndex::Context);
  llvm::Constant *projectFn = cast<llvm::Constant>(args[1])->stripPointerCasts();
  // Get the caller context from the calle context.
  llvm::Value *context = Builder.CreateCall(projectFn, {calleeContext});
  context = Builder.CreateBitCast(context, IGM.SwiftContextPtrTy);
  Builder.CreateStore(context, asyncContextLocation);

  return id;
}

llvm::Type *ExplosionSchema::getScalarResultType(IRGenModule &IGM) const {
  if (size() == 0) {
    return IGM.VoidTy;
  } else if (size() == 1) {
    return begin()->getScalarType();
  } else {
    SmallVector<llvm::Type*, 16> elts;
    for (auto &elt : *this) elts.push_back(elt.getScalarType());
    return llvm::StructType::get(IGM.getLLVMContext(), elts);
  }
}

static void addDereferenceableAttributeToBuilder(IRGenModule &IGM,
                                                 llvm::AttrBuilder &b,
                                                 const TypeInfo &ti) {
  // The addresses of empty values are undefined, so we can't safely mark them
  // dereferenceable.
  if (ti.isKnownEmpty(ResilienceExpansion::Maximal))
    return;
  
  // If we know the type to have a fixed nonempty size, then the pointer is
  // dereferenceable to at least that size.
  // TODO: Would be nice to have a "getMinimumKnownSize" on TypeInfo for
  // dynamic-layout aggregates.
  if (auto fixedTI = dyn_cast<FixedTypeInfo>(&ti)) {
    b.addAttribute(
      llvm::Attribute::getWithDereferenceableBytes(IGM.getLLVMContext(),
                                         fixedTI->getFixedSize().getValue()));
  }
}

static void addIndirectValueParameterAttributes(IRGenModule &IGM,
                                                llvm::AttributeList &attrs,
                                                const TypeInfo &ti,
                                                unsigned argIndex) {
  llvm::AttrBuilder b;
  // Value parameter pointers can't alias or be captured.
  b.addAttribute(llvm::Attribute::NoAlias);
  b.addAttribute(llvm::Attribute::NoCapture);
  // The parameter must reference dereferenceable memory of the type.
  addDereferenceableAttributeToBuilder(IGM, b, ti);

  attrs = attrs.addAttributes(IGM.getLLVMContext(),
                              argIndex + llvm::AttributeList::FirstArgIndex, b);
}

static void addInoutParameterAttributes(IRGenModule &IGM,
                                        llvm::AttributeList &attrs,
                                        const TypeInfo &ti, unsigned argIndex,
                                        bool aliasable) {
  llvm::AttrBuilder b;
  // Aliasing inouts is unspecified, but we still want aliasing to be memory-
  // safe, so we can't mark inouts as noalias at the LLVM level.
  // They still can't be captured without doing unsafe stuff, though.
  b.addAttribute(llvm::Attribute::NoCapture);
  // The inout must reference dereferenceable memory of the type.
  addDereferenceableAttributeToBuilder(IGM, b, ti);

  attrs = attrs.addAttributes(IGM.getLLVMContext(),
                              argIndex + llvm::AttributeList::FirstArgIndex, b);
}

static llvm::CallingConv::ID getFreestandingConvention(IRGenModule &IGM) {
  // TODO: use a custom CC that returns three scalars efficiently
  return IGM.SwiftCC;
}

/// Expand the requirements of the given abstract calling convention
/// into a "physical" calling convention.
llvm::CallingConv::ID irgen::expandCallingConv(IRGenModule &IGM,
                                    SILFunctionTypeRepresentation convention) {
  switch (convention) {
  case SILFunctionTypeRepresentation::CFunctionPointer:
  case SILFunctionTypeRepresentation::ObjCMethod:
  case SILFunctionTypeRepresentation::CXXMethod:
  case SILFunctionTypeRepresentation::Block:
    return llvm::CallingConv::C;

  case SILFunctionTypeRepresentation::Method:
  case SILFunctionTypeRepresentation::WitnessMethod:
  case SILFunctionTypeRepresentation::Closure:
  case SILFunctionTypeRepresentation::Thin:
  case SILFunctionTypeRepresentation::Thick:
    return getFreestandingConvention(IGM);
  }
  llvm_unreachable("bad calling convention!");
}

static void addIndirectResultAttributes(IRGenModule &IGM,
                                        llvm::AttributeList &attrs,
                                        unsigned paramIndex, bool allowSRet) {
  llvm::AttrBuilder b;
  b.addAttribute(llvm::Attribute::NoAlias);
  b.addAttribute(llvm::Attribute::NoCapture);
  if (allowSRet)
    b.addAttribute(llvm::Attribute::StructRet);
  attrs = attrs.addAttributes(IGM.getLLVMContext(),
                              paramIndex + llvm::AttributeList::FirstArgIndex,
                              b);
}

void IRGenModule::addSwiftSelfAttributes(llvm::AttributeList &attrs,
                                         unsigned argIndex) {
  llvm::AttrBuilder b;
  b.addAttribute(llvm::Attribute::SwiftSelf);
  attrs = attrs.addAttributes(this->getLLVMContext(),
                              argIndex + llvm::AttributeList::FirstArgIndex, b);
}

void IRGenModule::addSwiftErrorAttributes(llvm::AttributeList &attrs,
                                          unsigned argIndex) {
  llvm::AttrBuilder b;
  // Don't add the swifterror attribute on ABIs that don't pass it in a register.
  // We create a shadow stack location of the swifterror parameter for the
  // debugger on such platforms and so we can't mark the parameter with a
  // swifterror attribute.
  if (IsSwiftErrorInRegister)
    b.addAttribute(llvm::Attribute::SwiftError);
  
  // The error result should not be aliased, captured, or pointed at invalid
  // addresses regardless.
  b.addAttribute(llvm::Attribute::NoAlias);
  b.addAttribute(llvm::Attribute::NoCapture);
  b.addDereferenceableAttr(getPointerSize().getValue());
  
  auto attrIndex = argIndex + llvm::AttributeList::FirstArgIndex;
  attrs = attrs.addAttributes(this->getLLVMContext(), attrIndex, b);
}

void irgen::addByvalArgumentAttributes(IRGenModule &IGM,
                                       llvm::AttributeList &attrs,
                                       unsigned argIndex, Alignment align) {
  llvm::AttrBuilder b;
  b.addAttribute(llvm::Attribute::ByVal);
  b.addAttribute(llvm::Attribute::getWithAlignment(
      IGM.getLLVMContext(), llvm::Align(align.getValue())));
  attrs = attrs.addAttributes(IGM.getLLVMContext(),
                              argIndex + llvm::AttributeList::FirstArgIndex, b);
}

void irgen::addExtendAttribute(IRGenModule &IGM, llvm::AttributeList &attrs,
                               unsigned index, bool signExtend) {
  llvm::AttrBuilder b;
  if (signExtend)
    b.addAttribute(llvm::Attribute::SExt);
  else
    b.addAttribute(llvm::Attribute::ZExt);
  attrs = attrs.addAttributes(IGM.getLLVMContext(), index, b);
}

namespace swift {
namespace irgen {
namespace {
  class SignatureExpansion {
    IRGenModule &IGM;
    CanSILFunctionType FnType;
  public:
    SmallVector<llvm::Type*, 8> ParamIRTypes;
    llvm::Type *ResultIRType = nullptr;
    llvm::AttributeList Attrs;
    ForeignFunctionInfo ForeignInfo;
    CoroutineInfo CoroInfo;
    bool CanUseSRet = true;
    bool CanUseError = true;
    bool CanUseSelf = true;

    SignatureExpansion(IRGenModule &IGM, CanSILFunctionType fnType)
      : IGM(IGM), FnType(fnType) {}

    /// Expand the components of the primary entrypoint of the function type.
    void expandFunctionType();

    /// Expand the components of the continuation entrypoint of the
    /// function type.
    void expandCoroutineContinuationType();

    Signature getSignature();

  private:
    void expand(SILParameterInfo param);
    llvm::Type *addIndirectResult();

    SILFunctionConventions getSILFuncConventions() const {
      return SILFunctionConventions(FnType, IGM.getSILModule());
    }

    unsigned getCurParamIndex() {
      return ParamIRTypes.size();
    }

    bool claimSRet() {
      bool result = CanUseSRet;
      CanUseSRet = false;
      return result;
    }

    bool claimSelf() {
      auto Ret = CanUseSelf;
      assert(CanUseSelf && "Multiple self parameters?!");
      CanUseSelf = false;
      return Ret;
    }

    bool claimError() {
      auto Ret = CanUseError;
      assert(CanUseError && "Multiple error parameters?!");
      CanUseError = false;
      return Ret;
    }

    /// Add a pointer to the given type as the next parameter.
    void addPointerParameter(llvm::Type *storageType) {
      ParamIRTypes.push_back(storageType->getPointerTo());
    }

    void addCoroutineContextParameter();
    void addAsyncParameters();

    void expandResult();
    llvm::Type *expandDirectResult();
    void expandParameters();
    void expandExternalSignatureTypes();

    void expandCoroutineResult(bool forContinuation);
    void expandCoroutineContinuationParameters();
  };
} // end anonymous namespace
} // end namespace irgen
} // end namespace swift

llvm::Type *SignatureExpansion::addIndirectResult() {
  auto resultType = getSILFuncConventions().getSILResultType(
      IGM.getMaximalTypeExpansionContext());
  const TypeInfo &resultTI = IGM.getTypeInfo(resultType);
  addIndirectResultAttributes(IGM, Attrs, ParamIRTypes.size(), claimSRet());
  addPointerParameter(resultTI.getStorageType());
  return IGM.VoidTy;
}

/// Expand all of the direct and indirect result types.
void SignatureExpansion::expandResult() {
  if (FnType->isAsync()) {
    // The result will be stored within the SwiftContext that is passed to async
    // functions.
    ResultIRType = IGM.VoidTy;
    return;
  }
  if (FnType->isCoroutine()) {
    // This should be easy enough to support if we need to: use the
    // same algorithm but add the direct results to the results as if
    // they were unioned in.
    return expandCoroutineResult(/*for continuation*/ false);
  }

  auto fnConv = getSILFuncConventions();

  // Disable the use of sret if we have multiple indirect results.
  if (fnConv.getNumIndirectSILResults() > 1)
    CanUseSRet = false;

  // Expand the direct result.
  ResultIRType = expandDirectResult();

  // Expand the indirect results.
  for (auto indirectResultType :
       fnConv.getIndirectSILResultTypes(IGM.getMaximalTypeExpansionContext())) {
    addIndirectResultAttributes(IGM, Attrs, ParamIRTypes.size(), claimSRet());
    addPointerParameter(IGM.getStorageType(indirectResultType));
  }
}

namespace {
  class YieldSchema {
    SILType YieldTy;
    const TypeInfo &YieldTI;
    Optional<NativeConventionSchema> NativeSchema;
    bool IsIndirect;
  public:
    YieldSchema(IRGenModule &IGM, SILFunctionConventions fnConv,
                SILYieldInfo yield)
        : YieldTy(
              fnConv.getSILType(yield, IGM.getMaximalTypeExpansionContext())),
          YieldTI(IGM.getTypeInfo(YieldTy)) {
      if (isFormalIndirect()) {
        IsIndirect = true;
      } else {
        NativeSchema.emplace(IGM, &YieldTI, /*result*/ true);
        IsIndirect = NativeSchema->requiresIndirect();
      }
    }

    SILType getSILType() const {
      return YieldTy;
    }

    const TypeInfo &getTypeInfo() const {
      return YieldTI;
    }

    /// Should the yielded value be yielded as a pointer?
    bool isIndirect() const { return IsIndirect; }

    /// Is the yielded value formally indirect?
    bool isFormalIndirect() const { return YieldTy.isAddress(); }

    llvm::PointerType *getIndirectPointerType() const {
      assert(isIndirect());
      return YieldTI.getStorageType()->getPointerTo();
    }

    const NativeConventionSchema &getDirectSchema() const {
      assert(!isIndirect());
      return *NativeSchema;
    }

    void enumerateDirectComponents(llvm::function_ref<void(llvm::Type*)> fn) {
      getDirectSchema().enumerateComponents([&](clang::CharUnits begin,
                                                clang::CharUnits end,
                                                llvm::Type *componentTy) {
        fn(componentTy);
      });
    }
  };
}

void SignatureExpansion::expandCoroutineResult(bool forContinuation) {
  assert(FnType->getNumResults() == 0 &&
         "having both normal and yield results is currently unsupported");

  // The return type may be different for the ramp function vs. the
  // continuations.
  if (forContinuation) {
    switch (FnType->getCoroutineKind()) {
    case SILCoroutineKind::None:
      llvm_unreachable("should have been filtered out before here");

    // Yield-once coroutines just return void from the continuation.
    case SILCoroutineKind::YieldOnce:
      ResultIRType = IGM.VoidTy;
      return;

    // Yield-many coroutines yield the same types from the continuation
    // as they do from the ramp function.
    case SILCoroutineKind::YieldMany:
      break;
    }
  }

  SmallVector<llvm::Type*, 8> components;

  // The continuation pointer.
  components.push_back(IGM.Int8PtrTy);

  auto fnConv = getSILFuncConventions();
  for (auto yield : FnType->getYields()) {
    YieldSchema schema(IGM, fnConv, yield);

    // If the individual value must be yielded indirectly, add a pointer.
    if (schema.isIndirect()) {
      components.push_back(schema.getIndirectPointerType());
      continue;
    }

    // Otherwise, collect all the component types.
    schema.enumerateDirectComponents([&](llvm::Type *type) {
      components.push_back(type);
    });
  }

  // Find the maximal sequence of the component types that we can
  // convince the ABI to pass directly.
  // When counting components, ignore the continuation pointer.
  unsigned numDirectComponents = components.size() - 1;
  SmallVector<llvm::Type*, 8> overflowTypes;
  while (clang::CodeGen::swiftcall::
                shouldPassIndirectly(IGM.ClangCodeGen->CGM(), components,
                                     /*asReturnValue*/ true)) {
    // If we added a pointer to the end of components, remove it.
    if (!overflowTypes.empty()) components.pop_back();

    // Remove the last component and add it as an overflow type.
    overflowTypes.push_back(components.pop_back_val());
    --numDirectComponents;

    // Add a pointer to the end of components.
    components.push_back(IGM.Int8PtrTy);
  }

  // We'd better have been able to pass at least two pointers.
  assert(components.size() >= 2 || overflowTypes.empty());
  CoroInfo.NumDirectYieldComponents = numDirectComponents;

  // Replace the pointer type we added to components with the real
  // pointer-to-overflow type.
  if (!overflowTypes.empty()) {
    std::reverse(overflowTypes.begin(), overflowTypes.end());

    // TODO: should we use some sort of real layout here instead of
    // trusting LLVM's?
    components.back() =
      llvm::StructType::get(IGM.getLLVMContext(), overflowTypes)
        ->getPointerTo();
  }

  ResultIRType = components.size() == 1
                   ? components.front()
                   : llvm::StructType::get(IGM.getLLVMContext(), components);
}

void SignatureExpansion::expandCoroutineContinuationParameters() {
  // The coroutine context.
  addCoroutineContextParameter();

  // Whether this is an unwind resumption.
  ParamIRTypes.push_back(IGM.Int1Ty);
}

void SignatureExpansion::addAsyncParameters() {
  // using TaskContinuationFunction =
  //   SWIFT_CC(swift)
  //   void (AsyncTask *, ExecutorRef, AsyncContext *);
  ParamIRTypes.push_back(IGM.SwiftTaskPtrTy);
  ParamIRTypes.push_back(IGM.SwiftExecutorPtrTy);
  ParamIRTypes.push_back(IGM.SwiftContextPtrTy);
}

void SignatureExpansion::addCoroutineContextParameter() {
  // Flag that the context is dereferenceable and unaliased.
  auto contextSize = getCoroutineContextSize(IGM, FnType);
  Attrs = Attrs.addDereferenceableParamAttr(IGM.getLLVMContext(),
                                            getCurParamIndex(),
                                            contextSize.getValue());
  Attrs = Attrs.addParamAttribute(IGM.getLLVMContext(),
                                  getCurParamIndex(),
                                  llvm::Attribute::NoAlias);

  ParamIRTypes.push_back(IGM.Int8PtrTy);
}

NativeConventionSchema::NativeConventionSchema(IRGenModule &IGM,
                                               const TypeInfo *ti,
                                               bool IsResult)
    : Lowering(IGM.ClangCodeGen->CGM()) {
  if (auto *loadable = dyn_cast<LoadableTypeInfo>(ti)) {
    // Lower the type according to the Swift ABI.
    loadable->addToAggLowering(IGM, Lowering, Size(0));
    Lowering.finish();
    // Should we pass indirectly according to the ABI?
    RequiresIndirect = Lowering.shouldPassIndirectly(IsResult);
  } else {
    Lowering.finish();
    RequiresIndirect = true;
  }
}

llvm::Type *NativeConventionSchema::getExpandedType(IRGenModule &IGM) const {
  if (empty())
    return IGM.VoidTy;
  SmallVector<llvm::Type *, 8> elts;
  Lowering.enumerateComponents([&](clang::CharUnits offset,
                                   clang::CharUnits end,
                                   llvm::Type *type) { elts.push_back(type); });

  if (elts.size() == 1)
    return elts[0];

  auto &ctx = IGM.getLLVMContext();
  return llvm::StructType::get(ctx, elts, /*packed*/ false);
}

std::pair<llvm::StructType *, llvm::StructType *>
NativeConventionSchema::getCoercionTypes(
    IRGenModule &IGM, SmallVectorImpl<unsigned> &expandedTyIndicesMap) const {
  auto &ctx = IGM.getLLVMContext();

  if (empty()) {
    auto type = llvm::StructType::get(ctx);
    return {type, type};
  }

  clang::CharUnits lastEnd = clang::CharUnits::Zero();
  llvm::SmallSet<unsigned, 8> overlappedWithSuccessor;
  unsigned idx = 0;

  // Mark overlapping ranges.
  Lowering.enumerateComponents(
      [&](clang::CharUnits offset, clang::CharUnits end, llvm::Type *type) {
        if (offset < lastEnd) {
          overlappedWithSuccessor.insert(idx);
        }
        lastEnd = end;
        ++idx;
      });

  // Create the coercion struct with only the integer portion of overlapped
  // components and non-overlapped components.
  idx = 0;
  lastEnd = clang::CharUnits::Zero();
  SmallVector<llvm::Type *, 8> elts;
  bool packed = false;
  Lowering.enumerateComponents(
      [&](clang::CharUnits begin, clang::CharUnits end, llvm::Type *type) {
        bool overlapped = overlappedWithSuccessor.count(idx) ||
                          (idx && overlappedWithSuccessor.count(idx - 1));
        ++idx;
        if (overlapped && !isa<llvm::IntegerType>(type)) {
          // keep the old lastEnd for padding.
          return;
        }
        // Add padding (which may include padding for overlapped non-integer
        // components).
        if (begin != lastEnd) {
          auto paddingSize = begin - lastEnd;
          assert(!paddingSize.isNegative());

          auto padding = llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx),
                                              paddingSize.getQuantity());
          elts.push_back(padding);
        }
        if (!packed &&
            !begin.isMultipleOf(clang::CharUnits::fromQuantity(
                IGM.DataLayout.getABITypeAlignment(type))))
          packed = true;
        elts.push_back(type);
        expandedTyIndicesMap.push_back(idx - 1);
        lastEnd = begin + clang::CharUnits::fromQuantity(
                              IGM.DataLayout.getTypeAllocSize(type));
        assert(end <= lastEnd);
      });

  auto *coercionType = llvm::StructType::get(ctx, elts, packed);
  if (overlappedWithSuccessor.empty())
    return {coercionType, llvm::StructType::get(ctx)};

  // Create the coercion struct with only the non-integer overlapped
  // components.
  idx = 0;
  lastEnd = clang::CharUnits::Zero();
  elts.clear();
  packed = false;
  Lowering.enumerateComponents(
      [&](clang::CharUnits begin, clang::CharUnits end, llvm::Type *type) {
        bool overlapped = overlappedWithSuccessor.count(idx) ||
                          (idx && overlappedWithSuccessor.count(idx - 1));
        ++idx;
        if (!overlapped || (overlapped && isa<llvm::IntegerType>(type))) {
          // Ignore and keep the old lastEnd for padding.
          return;
        }
        // Add padding.
        if (begin != lastEnd) {
          auto paddingSize = begin - lastEnd;
          assert(!paddingSize.isNegative());

          auto padding = llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx),
                                              paddingSize.getQuantity());
          elts.push_back(padding);
        }
        if (!packed &&
            !begin.isMultipleOf(clang::CharUnits::fromQuantity(
                IGM.DataLayout.getABITypeAlignment(type))))
          packed = true;
        elts.push_back(type);
        expandedTyIndicesMap.push_back(idx - 1);
        lastEnd = begin + clang::CharUnits::fromQuantity(
                              IGM.DataLayout.getTypeAllocSize(type));
        assert(end <= lastEnd);
      });
  auto *overlappedCoercionType = llvm::StructType::get(ctx, elts, packed);
  return {coercionType, overlappedCoercionType};
}

// TODO: Direct to Indirect result conversion could be handled in a SIL
// AddressLowering pass.
llvm::Type *SignatureExpansion::expandDirectResult() {
  // Handle the direct result type, checking for supposedly scalar
  // result types that we actually want to return indirectly.
  auto resultType = getSILFuncConventions().getSILResultType(
      IGM.getMaximalTypeExpansionContext());

  // Fast-path the empty tuple type.
  if (auto tuple = resultType.getAs<TupleType>())
    if (tuple->getNumElements() == 0)
      return IGM.VoidTy;

  switch (FnType->getLanguage()) {
  case SILFunctionLanguage::C:
    llvm_unreachable("Expanding C/ObjC parameters in the wrong place!");
    break;
  case SILFunctionLanguage::Swift: {
    auto &ti = IGM.getTypeInfo(resultType);
    auto &native = ti.nativeReturnValueSchema(IGM);
    if (native.requiresIndirect())
      return addIndirectResult();

    // Disable the use of sret if we have a non-trivial direct result.
    if (!native.empty()) CanUseSRet = false;
    return native.getExpandedType(IGM);
  }
  }

  llvm_unreachable("Not a valid SILFunctionLanguage.");
}

static const clang::FieldDecl *
getLargestUnionField(const clang::RecordDecl *record,
                     const clang::ASTContext &ctx) {
  const clang::FieldDecl *largestField = nullptr;
  clang::CharUnits unionSize = clang::CharUnits::Zero();

  for (auto field : record->fields()) {
    assert(!field->isBitField());
    clang::CharUnits fieldSize = ctx.getTypeSizeInChars(field->getType());
    if (unionSize < fieldSize) {
      unionSize = fieldSize;
      largestField = field;
    }
  }
  assert(largestField && "empty union?");
  return largestField;
}

namespace {
  /// A CRTP class for working with Clang's ABIArgInfo::Expand
  /// argument type expansions.
  template <class Impl, class... Args> struct ClangExpand {
    IRGenModule &IGM;
    const clang::ASTContext &Ctx;
    ClangExpand(IRGenModule &IGM) : IGM(IGM), Ctx(IGM.getClangASTContext()) {}

    Impl &asImpl() { return *static_cast<Impl*>(this); }

    void visit(clang::CanQualType type, Args... args) {
      switch (type->getTypeClass()) {
#define TYPE(Class, Base)
#define NON_CANONICAL_TYPE(Class, Base) \
      case clang::Type::Class:
#define DEPENDENT_TYPE(Class, Base) \
      case clang::Type::Class:
#define NON_CANONICAL_UNLESS_DEPENDENT_TYPE(Class, Base) \
      case clang::Type::Class:
#include "clang/AST/TypeNodes.inc"
        llvm_unreachable("canonical or dependent type in ABI lowering");

      // These shouldn't occur in expandable struct types.
      case clang::Type::IncompleteArray:
      case clang::Type::VariableArray:
        llvm_unreachable("variable-sized or incomplete array in ABI lowering");

      // We should only ever get ObjC pointers, not underlying objects.
      case clang::Type::ObjCInterface:
      case clang::Type::ObjCObject:
        llvm_unreachable("ObjC object type in ABI lowering");

      // We should only ever get function pointers.
      case clang::Type::FunctionProto:
      case clang::Type::FunctionNoProto:
        llvm_unreachable("non-pointer function type in ABI lowering");

      // We currently never import C++ code, and we should be able to
      // kill Expand before we do.
      case clang::Type::LValueReference:
      case clang::Type::RValueReference:
      case clang::Type::MemberPointer:
      case clang::Type::Auto:
      case clang::Type::DeducedTemplateSpecialization:
        llvm_unreachable("C++ type in ABI lowering?");

      case clang::Type::Pipe:
        llvm_unreachable("OpenCL type in ABI lowering?");

      case clang::Type::ExtInt:
        llvm_unreachable("ExtInt type in ABI lowering?");

      case clang::Type::ConstantMatrix: {
        llvm_unreachable("ConstantMatrix type in ABI lowering?");
      }

      case clang::Type::ConstantArray: {
        auto array = Ctx.getAsConstantArrayType(type);
        auto elt = Ctx.getCanonicalType(array->getElementType());
        auto &&context = asImpl().beginArrayElements(elt);
        uint64_t n = array->getSize().getZExtValue();
        for (uint64_t i = 0; i != n; ++i) {
          asImpl().visitArrayElement(elt, i, context, args...);
        }
        return;
      }

      case clang::Type::Record: {
        auto record = cast<clang::RecordType>(type)->getDecl();
        if (record->isUnion()) {
          auto largest = getLargestUnionField(record, Ctx);
          asImpl().visitUnionField(record, largest, args...);
        } else {
          auto &&context = asImpl().beginStructFields(record);
          for (auto field : record->fields()) {
            asImpl().visitStructField(record, field, context, args...);
          }
        }
        return;
      }

      case clang::Type::Complex: {
        auto elt = type.castAs<clang::ComplexType>().getElementType();
        asImpl().visitComplexElement(elt, 0, args...);
        asImpl().visitComplexElement(elt, 1, args...);
        return;
      }

      // Just handle this types as opaque integers.
      case clang::Type::Enum:
      case clang::Type::Atomic:
        asImpl().visitScalar(convertTypeAsInteger(type), args...);
        return;

      case clang::Type::Builtin:
        asImpl().visitScalar(
                      convertBuiltinType(type.castAs<clang::BuiltinType>()),
                             args...);
        return;

      case clang::Type::Vector:
      case clang::Type::ExtVector:
        asImpl().visitScalar(
                      convertVectorType(type.castAs<clang::VectorType>()),
                             args...);
        return;

      case clang::Type::Pointer:
      case clang::Type::BlockPointer:
      case clang::Type::ObjCObjectPointer:
        asImpl().visitScalar(IGM.Int8PtrTy, args...);
        return;
      }
      llvm_unreachable("bad type kind");
    }
    
    Size getSizeOfType(clang::QualType type) {
      auto clangSize = Ctx.getTypeSizeInChars(type);
      return Size(clangSize.getQuantity());
    }

  private:
    llvm::Type *convertVectorType(clang::CanQual<clang::VectorType> type) {
      auto eltTy =
        convertBuiltinType(type->getElementType().castAs<clang::BuiltinType>());
      return llvm::FixedVectorType::get(eltTy, type->getNumElements());
    }

    llvm::Type *convertBuiltinType(clang::CanQual<clang::BuiltinType> type) {
      switch (type.getTypePtr()->getKind()) {
#define BUILTIN_TYPE(Id, SingletonId)
#define PLACEHOLDER_TYPE(Id, SingletonId) \
      case clang::BuiltinType::Id:
#include "clang/AST/BuiltinTypes.def"
      case clang::BuiltinType::Dependent:
        llvm_unreachable("placeholder type in ABI lowering");

      // We should never see these unadorned.
      case clang::BuiltinType::ObjCId:
      case clang::BuiltinType::ObjCClass:
      case clang::BuiltinType::ObjCSel:
        llvm_unreachable("bare Objective-C object type in ABI lowering");

      // This should never be the type of an argument or field.
      case clang::BuiltinType::Void:
        llvm_unreachable("bare void type in ABI lowering");

      // We should never see the OpenCL builtin types at all.
      case clang::BuiltinType::OCLImage1dRO:
      case clang::BuiltinType::OCLImage1dRW:
      case clang::BuiltinType::OCLImage1dWO:
      case clang::BuiltinType::OCLImage1dArrayRO:
      case clang::BuiltinType::OCLImage1dArrayRW:
      case clang::BuiltinType::OCLImage1dArrayWO:
      case clang::BuiltinType::OCLImage1dBufferRO:
      case clang::BuiltinType::OCLImage1dBufferRW:
      case clang::BuiltinType::OCLImage1dBufferWO:
      case clang::BuiltinType::OCLImage2dRO:
      case clang::BuiltinType::OCLImage2dRW:
      case clang::BuiltinType::OCLImage2dWO:
      case clang::BuiltinType::OCLImage2dArrayRO:
      case clang::BuiltinType::OCLImage2dArrayRW:
      case clang::BuiltinType::OCLImage2dArrayWO:
      case clang::BuiltinType::OCLImage2dDepthRO:
      case clang::BuiltinType::OCLImage2dDepthRW:
      case clang::BuiltinType::OCLImage2dDepthWO:
      case clang::BuiltinType::OCLImage2dArrayDepthRO:
      case clang::BuiltinType::OCLImage2dArrayDepthRW:
      case clang::BuiltinType::OCLImage2dArrayDepthWO:
      case clang::BuiltinType::OCLImage2dMSAARO:
      case clang::BuiltinType::OCLImage2dMSAARW:
      case clang::BuiltinType::OCLImage2dMSAAWO:
      case clang::BuiltinType::OCLImage2dArrayMSAARO:
      case clang::BuiltinType::OCLImage2dArrayMSAARW:
      case clang::BuiltinType::OCLImage2dArrayMSAAWO:
      case clang::BuiltinType::OCLImage2dMSAADepthRO:
      case clang::BuiltinType::OCLImage2dMSAADepthRW:
      case clang::BuiltinType::OCLImage2dMSAADepthWO:
      case clang::BuiltinType::OCLImage2dArrayMSAADepthRO:
      case clang::BuiltinType::OCLImage2dArrayMSAADepthRW:
      case clang::BuiltinType::OCLImage2dArrayMSAADepthWO:
      case clang::BuiltinType::OCLImage3dRO:
      case clang::BuiltinType::OCLImage3dRW:
      case clang::BuiltinType::OCLImage3dWO:
      case clang::BuiltinType::OCLSampler:
      case clang::BuiltinType::OCLEvent:
      case clang::BuiltinType::OCLClkEvent:
      case clang::BuiltinType::OCLQueue:
      case clang::BuiltinType::OCLReserveID:
      case clang::BuiltinType::OCLIntelSubgroupAVCMcePayload:
      case clang::BuiltinType::OCLIntelSubgroupAVCImePayload:
      case clang::BuiltinType::OCLIntelSubgroupAVCRefPayload:
      case clang::BuiltinType::OCLIntelSubgroupAVCSicPayload:
      case clang::BuiltinType::OCLIntelSubgroupAVCMceResult:
      case clang::BuiltinType::OCLIntelSubgroupAVCImeResult:
      case clang::BuiltinType::OCLIntelSubgroupAVCRefResult:
      case clang::BuiltinType::OCLIntelSubgroupAVCSicResult:
      case clang::BuiltinType::OCLIntelSubgroupAVCImeResultSingleRefStreamout:
      case clang::BuiltinType::OCLIntelSubgroupAVCImeResultDualRefStreamout:
      case clang::BuiltinType::OCLIntelSubgroupAVCImeSingleRefStreamin:
      case clang::BuiltinType::OCLIntelSubgroupAVCImeDualRefStreamin:
        llvm_unreachable("OpenCL type in ABI lowering");

      // We should never see the SVE types at all.
#define SVE_TYPE(Name, Id, ...) \
      case clang::BuiltinType::Id:
#include "clang/Basic/AArch64SVEACLETypes.def"
        llvm_unreachable("SVE type in ABI lowering");

      // Handle all the integer types as opaque values.
#define BUILTIN_TYPE(Id, SingletonId)
#define SIGNED_TYPE(Id, SingletonId) \
      case clang::BuiltinType::Id:
#define UNSIGNED_TYPE(Id, SingletonId) \
      case clang::BuiltinType::Id:
#include "clang/AST/BuiltinTypes.def"
        return convertTypeAsInteger(type);

      // Lower all the floating-point values by their semantics.
      case clang::BuiltinType::Half:
        return convertFloatingType(Ctx.getTargetInfo().getHalfFormat());
      case clang::BuiltinType::Float:
        return convertFloatingType(Ctx.getTargetInfo().getFloatFormat());
      case clang::BuiltinType::Double:
        return convertFloatingType(Ctx.getTargetInfo().getDoubleFormat());
      case clang::BuiltinType::LongDouble:
        return convertFloatingType(Ctx.getTargetInfo().getLongDoubleFormat());
      case clang::BuiltinType::Float16:
        llvm_unreachable("When upstream support is added for Float16 in "
                         "clang::TargetInfo, use the implementation here");
      case clang::BuiltinType::BFloat16:
        return convertFloatingType(Ctx.getTargetInfo().getBFloat16Format());
      case clang::BuiltinType::Float128:
        return convertFloatingType(Ctx.getTargetInfo().getFloat128Format());

      // nullptr_t -> void*
      case clang::BuiltinType::NullPtr:
        return IGM.Int8PtrTy;
      }
      llvm_unreachable("bad builtin type");
    }

    llvm::Type *convertFloatingType(const llvm::fltSemantics &format) {
      if (&format == &llvm::APFloat::IEEEhalf())
        return llvm::Type::getHalfTy(IGM.getLLVMContext());
      if (&format == &llvm::APFloat::IEEEsingle())
        return llvm::Type::getFloatTy(IGM.getLLVMContext());
      if (&format == &llvm::APFloat::IEEEdouble())
        return llvm::Type::getDoubleTy(IGM.getLLVMContext());
      if (&format == &llvm::APFloat::IEEEquad())
        return llvm::Type::getFP128Ty(IGM.getLLVMContext());
      if (&format == &llvm::APFloat::PPCDoubleDouble())
        return llvm::Type::getPPC_FP128Ty(IGM.getLLVMContext());
      if (&format == &llvm::APFloat::x87DoubleExtended())
        return llvm::Type::getX86_FP80Ty(IGM.getLLVMContext());
      llvm_unreachable("bad float format");
    }

    llvm::Type *convertTypeAsInteger(clang::QualType type) {
      auto size = getSizeOfType(type);
      return llvm::IntegerType::get(IGM.getLLVMContext(),
                                    size.getValueInBits());
    }
  };

  /// A CRTP specialization of ClangExpand which projects down to
  /// various aggregate elements of an address.
  ///
  /// Subclasses should only have to define visitScalar.
  template <class Impl>
  class ClangExpandProjection : public ClangExpand<Impl, Address> {
    using super = ClangExpand<Impl, Address>;
    using super::asImpl;
    using super::IGM;
    using super::Ctx;
    using super::getSizeOfType;

  protected:
    IRGenFunction &IGF;
    ClangExpandProjection(IRGenFunction &IGF)
      : super(IGF.IGM), IGF(IGF) {}

  public:
    void visit(clang::CanQualType type, Address addr) {
      assert(addr.getType() == IGM.Int8PtrTy);
      super::visit(type, addr);
    }
    
    Size beginArrayElements(clang::CanQualType element) {
      return getSizeOfType(element);
    }
    void visitArrayElement(clang::CanQualType element, unsigned i,
                           Size elementSize, Address arrayAddr) {
      asImpl().visit(element, createGEPAtOffset(arrayAddr, elementSize * i));
    }

    void visitComplexElement(clang::CanQualType element, unsigned i,
                             Address complexAddr) {
      Address addr = complexAddr;
      if (i) { addr = createGEPAtOffset(complexAddr, getSizeOfType(element)); }
      asImpl().visit(element, addr);
    }

    void visitUnionField(const clang::RecordDecl *record,
                         const clang::FieldDecl *field,
                         Address structAddr) {
      asImpl().visit(Ctx.getCanonicalType(field->getType()), structAddr);
    }

    const clang::ASTRecordLayout &
    beginStructFields(const clang::RecordDecl *record) {
      return Ctx.getASTRecordLayout(record);
    }
    void visitStructField(const clang::RecordDecl *record,
                          const clang::FieldDecl *field,
                          const clang::ASTRecordLayout &layout,
                          Address structAddr) {
      auto fieldIndex = field->getFieldIndex();
      assert(!field->isBitField());
      auto fieldOffset = Size(layout.getFieldOffset(fieldIndex) / 8);
      asImpl().visit(Ctx.getCanonicalType(field->getType()),
                     createGEPAtOffset(structAddr, fieldOffset));
    }

  private:
    Address createGEPAtOffset(Address addr, Size offset) {
      if (offset.isZero()) {
        return addr;
      } else {
        return IGF.Builder.CreateConstByteArrayGEP(addr, offset);
      }
    }
  };

  /// A class for collecting the types of a Clang ABIArgInfo::Expand
  /// argument expansion.
  struct ClangExpandTypeCollector : ClangExpand<ClangExpandTypeCollector> {
    SmallVectorImpl<llvm::Type*> &Types;
    ClangExpandTypeCollector(IRGenModule &IGM,
                             SmallVectorImpl<llvm::Type*> &types)
      : ClangExpand(IGM), Types(types) {}

    bool beginArrayElements(clang::CanQualType element) { return true; }
    void visitArrayElement(clang::CanQualType element, unsigned i, bool _) {
      visit(element);
    }

    void visitComplexElement(clang::CanQualType element, unsigned i) {
      visit(element);
    }

    void visitUnionField(const clang::RecordDecl *record,
                         const clang::FieldDecl *field) {
      visit(Ctx.getCanonicalType(field->getType()));
    }

    bool beginStructFields(const clang::RecordDecl *record) { return true; }
    void visitStructField(const clang::RecordDecl *record,
                          const clang::FieldDecl *field,
                          bool _) {
      visit(Ctx.getCanonicalType(field->getType()));
    }

    void visitScalar(llvm::Type *type) {
      Types.push_back(type);
    }
  };
} // end anonymous namespace

static bool doesClangExpansionMatchSchema(IRGenModule &IGM,
                                          clang::CanQualType type,
                                          const ExplosionSchema &schema) {
  assert(!schema.containsAggregate());
  SmallVector<llvm::Type *, 4> expansion;
  ClangExpandTypeCollector(IGM, expansion).visit(type);

  if (expansion.size() != schema.size())
    return false;

  for (size_t i = 0, e = schema.size(); i != e; ++i) {
    if (schema[i].getScalarType() != expansion[i])
      return false;
  }

  return true;
}

/// Expand the result and parameter types to the appropriate LLVM IR
/// types for C and Objective-C signatures.
void SignatureExpansion::expandExternalSignatureTypes() {
  assert(FnType->getLanguage() == SILFunctionLanguage::C);

  // Convert the SIL result type to a Clang type.
  auto clangResultTy =
    IGM.getClangType(FnType->getFormalCSemanticResult(IGM.getSILModule()));

  // Now convert the parameters to Clang types.
  auto params = FnType->getParameters();

  SmallVector<clang::CanQualType,4> paramTys;
  auto const &clangCtx = IGM.getClangASTContext();

  bool formalIndirectResult = FnType->getNumResults() > 0 &&
                              FnType->getSingleResult().isFormalIndirect();
  if (formalIndirectResult) {
    auto resultType = getSILFuncConventions().getSingleSILResultType(
        IGM.getMaximalTypeExpansionContext());
    auto clangTy =
        IGM.getClangASTContext().getPointerType(IGM.getClangType(resultType));
    paramTys.push_back(clangTy);
  }

  switch (FnType->getRepresentation()) {
  case SILFunctionTypeRepresentation::ObjCMethod: {
    // ObjC methods take their 'self' argument first, followed by an
    // implicit _cmd argument.
    auto &self = params.back();
    auto clangTy = IGM.getClangType(self, FnType);
    paramTys.push_back(clangTy);
    paramTys.push_back(clangCtx.VoidPtrTy);
    params = params.drop_back();
    break;
  }

  case SILFunctionTypeRepresentation::Block:
    // Blocks take their context argument first.
    paramTys.push_back(clangCtx.VoidPtrTy);
    break;

  case SILFunctionTypeRepresentation::CXXMethod:
  case SILFunctionTypeRepresentation::CFunctionPointer:
    // No implicit arguments.
    break;

  case SILFunctionTypeRepresentation::Thin:
  case SILFunctionTypeRepresentation::Thick:
  case SILFunctionTypeRepresentation::Method:
  case SILFunctionTypeRepresentation::WitnessMethod:
  case SILFunctionTypeRepresentation::Closure:
    llvm_unreachable("not a C representation");
  }

  // Given an index within the clang parameters list, what do we need
  // to subtract from it to get to the corresponding index within the
  // Swift parameters list?
  size_t clangToSwiftParamOffset = paramTys.size();

  // Convert each parameter to a Clang type.
  for (auto param : params) {
    auto clangTy = IGM.getClangType(param, FnType);
    paramTys.push_back(clangTy);
  }

  // Generate function info for this signature.
  auto extInfo = clang::FunctionType::ExtInfo();
  auto &FI = clang::CodeGen::arrangeFreeFunctionCall(IGM.ClangCodeGen->CGM(),
                                             clangResultTy, paramTys, extInfo,
                                             clang::CodeGen::RequiredArgs::All);
  ForeignInfo.ClangInfo = &FI;

  assert(FI.arg_size() == paramTys.size() &&
         "Expected one ArgInfo for each parameter type!");

  auto &returnInfo = FI.getReturnInfo();

  // Does the result need an extension attribute?
  if (returnInfo.isExtend()) {
    bool signExt = clangResultTy->hasSignedIntegerRepresentation();
    assert((signExt || clangResultTy->hasUnsignedIntegerRepresentation()) &&
           "Invalid attempt to add extension attribute to argument!");
    addExtendAttribute(IGM, Attrs, llvm::AttributeList::ReturnIndex, signExt);
  }

  // If we return indirectly, that is the first parameter type.
  if (returnInfo.isIndirect()) {
    addIndirectResult();
  }

  size_t firstParamToLowerNormally = 0;

  // Use a special IR type for passing block pointers.
  if (FnType->getRepresentation() == SILFunctionTypeRepresentation::Block) {
    assert(FI.arg_begin()[0].info.isDirect() &&
           "block pointer not passed directly?");
    ParamIRTypes.push_back(IGM.ObjCBlockPtrTy);
    firstParamToLowerNormally = 1;
  }

  for (auto i : indices(paramTys).slice(firstParamToLowerNormally)) {
    auto &AI = FI.arg_begin()[i].info;

    // Add a padding argument if required.
    if (auto *padType = AI.getPaddingType())
      ParamIRTypes.push_back(padType);

    switch (AI.getKind()) {
    case clang::CodeGen::ABIArgInfo::Extend: {
      bool signExt = paramTys[i]->hasSignedIntegerRepresentation();
      assert((signExt || paramTys[i]->hasUnsignedIntegerRepresentation()) &&
             "Invalid attempt to add extension attribute to argument!");
      addExtendAttribute(IGM, Attrs, getCurParamIndex() +
                         llvm::AttributeList::FirstArgIndex, signExt);
      LLVM_FALLTHROUGH;
    }
    case clang::CodeGen::ABIArgInfo::Direct: {
      switch (FI.getExtParameterInfo(i).getABI()) {
      case clang::ParameterABI::Ordinary:
        break;
      case clang::ParameterABI::SwiftContext:
        IGM.addSwiftSelfAttributes(Attrs, getCurParamIndex());
        break;
      case clang::ParameterABI::SwiftErrorResult:
        IGM.addSwiftErrorAttributes(Attrs, getCurParamIndex());
        break;
      case clang::ParameterABI::SwiftIndirectResult:
        addIndirectResultAttributes(IGM, Attrs, getCurParamIndex(),claimSRet());
        break;
      }

      // If the coercion type is a struct which can be flattened, we need to
      // expand it.
      auto *coercedTy = AI.getCoerceToType();
      if (AI.isDirect() && AI.getCanBeFlattened() &&
          isa<llvm::StructType>(coercedTy)) {
        const auto *ST = cast<llvm::StructType>(coercedTy);
        for (unsigned EI : range(ST->getNumElements()))
          ParamIRTypes.push_back(ST->getElementType(EI));
      } else {
        ParamIRTypes.push_back(coercedTy);
      }
      break;
    }
    case clang::CodeGen::ABIArgInfo::CoerceAndExpand: {
      auto types = AI.getCoerceAndExpandTypeSequence();
      ParamIRTypes.append(types.begin(), types.end());
      break;
    }
    case clang::CodeGen::ABIArgInfo::Indirect: {
      assert(i >= clangToSwiftParamOffset &&
             "Unexpected index for indirect byval argument");
      auto &param = params[i - clangToSwiftParamOffset];
      auto paramTy = getSILFuncConventions().getSILType(
          param, IGM.getMaximalTypeExpansionContext());
      auto &paramTI = cast<FixedTypeInfo>(IGM.getTypeInfo(paramTy));
      if (AI.getIndirectByVal())
        addByvalArgumentAttributes(
            IGM, Attrs, getCurParamIndex(),
            Alignment(AI.getIndirectAlign().getQuantity()));
      addPointerParameter(paramTI.getStorageType());
      break;
    }
    case clang::CodeGen::ABIArgInfo::Expand:
      ClangExpandTypeCollector(IGM, ParamIRTypes).visit(paramTys[i]);
      break;
    case clang::CodeGen::ABIArgInfo::Ignore:
      break;
    case clang::CodeGen::ABIArgInfo::InAlloca:
      llvm_unreachable("Need to handle InAlloca during signature expansion");
    }
  }

  if (returnInfo.isIndirect() || returnInfo.isIgnore()) {
    ResultIRType = IGM.VoidTy;
  } else {
    ResultIRType = returnInfo.getCoerceToType();
  }
}

static ArrayRef<llvm::Type *> expandScalarOrStructTypeToArray(llvm::Type *&ty) {
  ArrayRef<llvm::Type*> expandedTys;
  if (auto expansionTy = dyn_cast<llvm::StructType>(ty)) {
    // Is there any good reason this isn't public API of llvm::StructType?
    expandedTys = makeArrayRef(expansionTy->element_begin(),
                               expansionTy->getNumElements());
  } else {
    expandedTys = ty;
  }
  return expandedTys;
}


void SignatureExpansion::expand(SILParameterInfo param) {
  auto paramSILType = getSILFuncConventions().getSILType(
      param, IGM.getMaximalTypeExpansionContext());
  auto &ti = IGM.getTypeInfo(paramSILType);
  switch (auto conv = param.getConvention()) {
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Indirect_In_Constant:
  case ParameterConvention::Indirect_In_Guaranteed:
    addIndirectValueParameterAttributes(IGM, Attrs, ti, ParamIRTypes.size());
    addPointerParameter(IGM.getStorageType(getSILFuncConventions().getSILType(
        param, IGM.getMaximalTypeExpansionContext())));
    return;

  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
    addInoutParameterAttributes(IGM, Attrs, ti, ParamIRTypes.size(),
                          conv == ParameterConvention::Indirect_InoutAliasable);
    addPointerParameter(IGM.getStorageType(getSILFuncConventions().getSILType(
        param, IGM.getMaximalTypeExpansionContext())));
    return;

  case ParameterConvention::Direct_Owned:
  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Direct_Guaranteed:
    switch (FnType->getLanguage()) {
    case SILFunctionLanguage::C: {
      llvm_unreachable("Unexpected C/ObjC method in parameter expansion!");
      return;
    }
    case SILFunctionLanguage::Swift: {
      auto &nativeSchema = ti.nativeParameterValueSchema(IGM);
      if (nativeSchema.requiresIndirect()) {
        addIndirectValueParameterAttributes(IGM, Attrs, ti,
                                            ParamIRTypes.size());
        ParamIRTypes.push_back(ti.getStorageType()->getPointerTo());
        return;
      }
      if (nativeSchema.empty()) {
        assert(ti.getSchema().empty());
        return;
      }
      auto expandedTy = nativeSchema.getExpandedType(IGM);
      auto expandedTysArray = expandScalarOrStructTypeToArray(expandedTy);
      for (auto *Ty : expandedTysArray)
        ParamIRTypes.push_back(Ty);
      return;
    }
    }
    llvm_unreachable("bad abstract CC");
  }
  llvm_unreachable("bad parameter convention");
}

/// Does the given function type have a self parameter that should be
/// given the special treatment for self parameters?
///
/// It's important that this only return true for things that are
/// passed as a single pointer.
bool irgen::hasSelfContextParameter(CanSILFunctionType fnType) {
  if (!fnType->hasSelfParam())
    return false;

  SILParameterInfo param = fnType->getSelfParameter();

  // All the indirect conventions pass a single pointer.
  if (param.isFormalIndirect()) {
    return true;
  }

  // Direct conventions depend on the type.
  CanType type = param.getInterfaceType();

  // Thick or @objc metatypes (but not existential metatypes).
  if (auto metatype = dyn_cast<MetatypeType>(type)) {
    return metatype->getRepresentation() != MetatypeRepresentation::Thin;
  }

  // Classes and class-bounded archetypes or ObjC existentials.
  // No need to apply this to existentials.
  // The direct check for SubstitutableType works because only
  // class-bounded generic types can be passed directly.
  if (type->mayHaveSuperclass() || isa<SubstitutableType>(type) ||
      type->isObjCExistentialType()) {
    return true;
  }

  return false;
}

/// Expand the abstract parameters of a SIL function type into the physical
/// parameters of an LLVM function type (results have already been expanded).
void SignatureExpansion::expandParameters() {
  assert(FnType->getRepresentation() != SILFunctionTypeRepresentation::Block
         && "block with non-C calling conv?!");

  if (FnType->isAsync()) {
    addAsyncParameters();
    // All other parameters will be passed inside the context added by the
    // addAsyncParameters call.
    return;
  }

  // First, if this is a coroutine, add the coroutine-context parameter.
  switch (FnType->getCoroutineKind()) {
  case SILCoroutineKind::None:
    break;

  case SILCoroutineKind::YieldOnce:
  case SILCoroutineKind::YieldMany:
    addCoroutineContextParameter();
    break;
  }

  // Next, the formal parameters.  But 'self' is treated as the
  // context if it has pointer representation.
  auto params = FnType->getParameters();
  bool hasSelfContext = false;
  if (hasSelfContextParameter(FnType)) {
    hasSelfContext = true;
    params = params.drop_back();
  }

  for (auto param : params) {
    expand(param);
  }

  // Next, the generic signature.
  if (hasPolymorphicParameters(FnType))
    expandPolymorphicSignature(IGM, FnType, ParamIRTypes);

  // Context is next.
  if (hasSelfContext) {
    auto curLength = ParamIRTypes.size(); (void) curLength;

    if (claimSelf())
      IGM.addSwiftSelfAttributes(Attrs, curLength);
    expand(FnType->getSelfParameter());
    assert(ParamIRTypes.size() == curLength + 1 &&
           "adding 'self' added unexpected number of parameters");
  } else {
    auto needsContext = [=]() -> bool {
      switch (FnType->getRepresentation()) {
      case SILFunctionType::Representation::Block:
        llvm_unreachable("adding block parameter in Swift CC expansion?");

      // Always leave space for a context argument if we have an error result.
      case SILFunctionType::Representation::CFunctionPointer:
      case SILFunctionType::Representation::Method:
      case SILFunctionType::Representation::WitnessMethod:
      case SILFunctionType::Representation::ObjCMethod:
      case SILFunctionType::Representation::CXXMethod:
      case SILFunctionType::Representation::Thin:
      case SILFunctionType::Representation::Closure:
        return FnType->hasErrorResult();

      case SILFunctionType::Representation::Thick:
        return true;
      }
      llvm_unreachable("bad representation kind");
    };
    if (needsContext()) {
      if (claimSelf())
        IGM.addSwiftSelfAttributes(Attrs, ParamIRTypes.size());
      ParamIRTypes.push_back(IGM.RefCountedPtrTy);
    }
  }

  // Error results are last.  We always pass them as a pointer to the
  // formal error type; LLVM will magically turn this into a non-pointer
  // if we set the right attribute.
  if (FnType->hasErrorResult()) {
    if (claimError())
      IGM.addSwiftErrorAttributes(Attrs, ParamIRTypes.size());
    llvm::Type *errorType =
        IGM.getStorageType(getSILFuncConventions().getSILType(
            FnType->getErrorResult(), IGM.getMaximalTypeExpansionContext()));
    ParamIRTypes.push_back(errorType->getPointerTo());
  }

  // Witness methods have some extra parameter types.
  if (FnType->getRepresentation() ==
        SILFunctionTypeRepresentation::WitnessMethod) {
    expandTrailingWitnessSignature(IGM, FnType, ParamIRTypes);
  }
}

/// Expand the result and parameter types of a SIL function into the
/// physical parameter types of an LLVM function and return the result
/// type.
void SignatureExpansion::expandFunctionType() {
  switch (FnType->getLanguage()) {
  case SILFunctionLanguage::Swift: {
    expandResult();
    expandParameters();
    return;
  }
  case SILFunctionLanguage::C:
    expandExternalSignatureTypes();
    return;
  }
  llvm_unreachable("bad abstract calling convention");
}

void SignatureExpansion::expandCoroutineContinuationType() {
  expandCoroutineResult(/*for continuation*/ true);
  expandCoroutineContinuationParameters();
}

Signature SignatureExpansion::getSignature() {
  // Create the appropriate LLVM type.
  llvm::FunctionType *llvmType =
    llvm::FunctionType::get(ResultIRType, ParamIRTypes, /*variadic*/ false);

  assert((ForeignInfo.ClangInfo != nullptr) ==
           (FnType->getLanguage() == SILFunctionLanguage::C) &&
         "C function type without C function info");

  auto callingConv = expandCallingConv(IGM, FnType->getRepresentation());

  Signature result;
  result.Type = llvmType;
  result.CallingConv = callingConv;
  result.Attributes = Attrs;
  using ExtraData = Signature::ExtraData;
  if (FnType->getLanguage() == SILFunctionLanguage::C) {
    result.ExtraDataKind = ExtraData::kindForMember<ForeignFunctionInfo>();
    result.ExtraDataStorage.emplace<ForeignFunctionInfo>(result.ExtraDataKind,
                                                         ForeignInfo);
  } else if (FnType->isCoroutine()) {
    result.ExtraDataKind = ExtraData::kindForMember<CoroutineInfo>();
    result.ExtraDataStorage.emplace<CoroutineInfo>(result.ExtraDataKind,
                                                   CoroInfo);
  } else {
    result.ExtraDataKind = ExtraData::kindForMember<void>();
  }
  return result;
}

Signature Signature::getUncached(IRGenModule &IGM,
                                 CanSILFunctionType formalType) {
  GenericContextScope scope(IGM, formalType->getInvocationGenericSignature());
  SignatureExpansion expansion(IGM, formalType);
  expansion.expandFunctionType();
  return expansion.getSignature();
}

Signature Signature::forCoroutineContinuation(IRGenModule &IGM,
                                              CanSILFunctionType fnType) {
  assert(fnType->isCoroutine());
  SignatureExpansion expansion(IGM, fnType);
  expansion.expandCoroutineContinuationType();
  return expansion.getSignature();
}

void irgen::extractScalarResults(IRGenFunction &IGF, llvm::Type *bodyType,
                                 llvm::Value *call, Explosion &out) {
  assert(!bodyType->isVoidTy() && "Unexpected void result type!");

  auto *returned = call;
  auto *callType = call->getType();

  // If the type of the result of the call differs from the type used
  // elsewhere in the caller due to ABI type coercion, we need to
  // coerce the result back from the ABI type before extracting the
  // elements.
  if (bodyType != callType)
    returned = IGF.coerceValue(returned, bodyType, IGF.IGM.DataLayout);

  if (auto *structType = dyn_cast<llvm::StructType>(bodyType))
    for (unsigned i = 0, e = structType->getNumElements(); i != e; ++i)
      out.add(IGF.Builder.CreateExtractValue(returned, i));
  else
    out.add(returned);
}

std::pair<llvm::Value *, llvm::Value *> irgen::getAsyncFunctionAndSize(
    IRGenFunction &IGF, SILFunctionTypeRepresentation representation,
    FunctionPointer functionPointer, llvm::Value *thickContext,
    std::pair<bool, bool> values, Size initialContextSize) {
  assert(values.first || values.second);
  assert(functionPointer.getKind() ==
         FunctionPointer::KindTy::AsyncFunctionPointer);
  bool emitFunction = values.first;
  bool emitSize = values.second;
  // TODO: This calculation should be extracted out into standalone functions
  //       emitted on-demand per-module to improve codesize.
  switch (representation) {
  case SILFunctionTypeRepresentation::Thick: {
    assert(!functionPointer.useStaticContextSize());
    // If the called function is thick, the size of the called function's
    // async context is not statically knowable.
    //
    // Specifically, if the thick function was produced by a partial_apply,
    // the function which was originally partially applied determines the
    // size of the needed async context.  That original function isn't known
    // statically.  The dynamic size is available within the context as an
    // i32 at the first index: <{ %swift.refcounted*, /*size*/ i32, ... }>.
    // In this case, the function pointer is actually a pointer to an llvm
    // function.
    //
    // On the other hand, if the thick function was produced by a
    // thin_to_thick_function, then the context will be nullptr.  In that
    // case, the dynamic size of the needed async context is available within
    // the struct, an AsyncFunctionPointer pointed to by the "function" pointer
    // as an i32 at the second index: <{ /*fn rel addr*/ i32, /*size*/ i32 }>.
    //
    // We are currently emitting into some basic block.  To handle these two
    // cases, we need to branch based on whether the context is nullptr; each
    // branch must then determine the size and function pointer in the manner
    // appropriate to it.  Finally, both blocks must join back together to make
    // the call:
    //
    //                      +-------------------------+
    //                      |%cond = %ctx == nullptr  |
    //     +----------------|br %cond, thin, thick    |----------------------+
    //     |                +-------------------------+                      |
    //     |                                                                 |
    //     V                                                                 |
    // +-thin-------------------------------------------+                    |
    // |%afp = bitcast %fp to %swift.async_func_pointer*|                    |
    // |%size_ptr = getelementptr %afp, i32 0, i32 1    |                    |
    // |%size = load %size_ptr                          |                    |
    // |%offset_ptr = getelementptr %afp, i32 0, i32 1  |                    |
    // |%offset = load i32 %offset_ptr                  |                    |
    // |%offset64 = sext %offset to i64                 |                    |
    // |%raw_fp = add %offset64, %offset_ptr            |                    |
    // |br join(%raw_fp, %size)                         |                    |
    // +------------------------------------------------+                    |
    //     |                                                                 |
    //     |                                                                 V
    //     |                +-thick--------------------------------------------+
    //     |                |%layout = bitcast %ctx to <{%swift.context*, i32}>|
    //     |                |%size_addr = getelementptr %layout, i32 0, i32 1  |
    //     |                |%size = load %size_addr                           |
    //     |                |br join(%fp, %size)                               |
    //     |                +---/----------------------------------------------+
    //     |                   /
    //     |                  /
    //     V                 V
    // +-join(%fn, %size)------------------------------------------------------+
    // |%dataAddr = swift_taskAlloc(%task, %size)                              |
    // |%async_context = bitcast %dataAddr to ASYNC_CONTEXT(static_callee_type)|
    // |... // populate the fields %ctx with arguments                         |
    // |call %fn(%async_context, %ctx)                                         |
    // +-----------------------------------------------------------------------+
    auto *thinBlock = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
    auto *thickBlock = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
    auto *joinBlock = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());

    auto hasThickContext =
        IGF.Builder.CreateICmpNE(thickContext, IGF.IGM.RefCountedNull);
    IGF.Builder.CreateCondBr(hasThickContext, thickBlock, thinBlock);

    SmallVector<std::pair<llvm::BasicBlock *, llvm::Value *>, 2> fnPhiValues;
    SmallVector<std::pair<llvm::BasicBlock *, llvm::Value *>, 2> sizePhiValues;
    { // thin
      IGF.Builder.emitBlock(thinBlock);
      auto *ptr = functionPointer.getRawPointer();
      if (auto authInfo = functionPointer.getAuthInfo()) {
        ptr = emitPointerAuthAuth(IGF, ptr, authInfo);
      }
      auto *afpPtr =
          IGF.Builder.CreateBitCast(ptr, IGF.IGM.AsyncFunctionPointerPtrTy);
      if (emitFunction) {
        llvm::Value *addrPtr = IGF.Builder.CreateStructGEP(afpPtr, 0);
        auto *uncastFnPtr = IGF.emitLoadOfRelativePointer(
            Address(addrPtr, IGF.IGM.getPointerAlignment()), /*isFar*/ false,
            /*expectedType*/ functionPointer.getFunctionType()->getPointerTo());
        auto *fnPtr = IGF.Builder.CreateBitCast(uncastFnPtr, IGF.IGM.Int8PtrTy);
        if (auto authInfo = functionPointer.getAuthInfo()) {
          fnPtr = emitPointerAuthSign(IGF, fnPtr, authInfo);
        }
        fnPhiValues.push_back({thinBlock, fnPtr});
      }
      if (emitSize) {
        auto *sizePtr = IGF.Builder.CreateStructGEP(afpPtr, 1);
        auto *size =
            IGF.Builder.CreateLoad(sizePtr, IGF.IGM.getPointerAlignment());
        sizePhiValues.push_back({thinBlock, size});
      }
      IGF.Builder.CreateBr(joinBlock);
    }

    { // thick
      IGF.Builder.emitBlock(thickBlock);
      if (emitFunction) {
        auto *uncastFnPtr = functionPointer.getRawPointer();
        auto *fnPtr = IGF.Builder.CreateBitCast(uncastFnPtr, IGF.IGM.Int8PtrTy);
        fnPhiValues.push_back({thickBlock, fnPtr});
      }
      if (emitSize) {
        SmallVector<const TypeInfo *, 4> argTypeInfos;
        SmallVector<SILType, 4> argValTypes;
        auto int32ASTType =
            BuiltinIntegerType::get(32, IGF.IGM.IRGen.SIL.getASTContext())
                ->getCanonicalType();
        auto int32SILType = SILType::getPrimitiveObjectType(int32ASTType);
        const TypeInfo &int32TI = IGF.IGM.getTypeInfo(int32SILType);
        argValTypes.push_back(int32SILType);
        argTypeInfos.push_back(&int32TI);
        HeapLayout layout(IGF.IGM, LayoutStrategy::Optimal, argValTypes,
                          argTypeInfos,
                          /*typeToFill*/ nullptr, NecessaryBindings());
        auto castThickContext =
            layout.emitCastTo(IGF, thickContext, "context.prefix");
        auto sizeLayout = layout.getElement(0);
        auto sizeAddr = sizeLayout.project(IGF, castThickContext,
                                           /*NonFixedOffsets*/ llvm::None);
        auto *sizeValue = IGF.Builder.CreateLoad(sizeAddr);
        sizePhiValues.push_back({thickBlock, sizeValue});
      }
      IGF.Builder.CreateBr(joinBlock);
    }

    { // join
      IGF.Builder.emitBlock(joinBlock);
      llvm::Value *fn = nullptr;
      llvm::PHINode *fnPhi = nullptr;
      llvm::PHINode *sizePhi = nullptr;
      if (emitFunction) {
        fnPhi = IGF.Builder.CreatePHI(IGF.IGM.Int8PtrTy, fnPhiValues.size());
      }
      if (emitSize) {
        sizePhi = IGF.Builder.CreatePHI(IGF.IGM.Int32Ty, sizePhiValues.size());
      }
      if (emitFunction) {
        assert(fnPhi);
        for (auto &entry : fnPhiValues) {
          fnPhi->addIncoming(entry.second, entry.first);
        }
        fn = IGF.Builder.CreateBitCast(
            fnPhi, functionPointer.getFunctionType()->getPointerTo());
      }
      llvm::Value *size = nullptr;
      if (emitSize) {
        assert(sizePhi);
        for (auto &entry : sizePhiValues) {
          sizePhi->addIncoming(entry.second, entry.first);
        }
        size = sizePhi;
      }
      return {fn, size};
    }
  }
  case SILFunctionTypeRepresentation::Thin:
  case SILFunctionTypeRepresentation::CFunctionPointer:
  case SILFunctionTypeRepresentation::Method:
  case SILFunctionTypeRepresentation::ObjCMethod:
  case SILFunctionTypeRepresentation::CXXMethod:
  case SILFunctionTypeRepresentation::WitnessMethod:
  case SILFunctionTypeRepresentation::Closure:
  case SILFunctionTypeRepresentation::Block: {
    auto *ptr = functionPointer.getRawPointer();
    if (auto authInfo = functionPointer.getAuthInfo()) {
      ptr = emitPointerAuthAuth(IGF, ptr, authInfo);
    }
    auto *afpPtr =
        IGF.Builder.CreateBitCast(ptr, IGF.IGM.AsyncFunctionPointerPtrTy);
    llvm::Value *fn = nullptr;
    if (emitFunction) {
      if (functionPointer.useStaticContextSize()) {
        fn = functionPointer.getRawPointer();
      } else {
        llvm::Value *addrPtr = IGF.Builder.CreateStructGEP(afpPtr, 0);
        fn = IGF.emitLoadOfRelativePointer(
            Address(addrPtr, IGF.IGM.getPointerAlignment()), /*isFar*/ false,
            /*expectedType*/ functionPointer.getFunctionType()->getPointerTo());
      }
      if (auto authInfo = functionPointer.getAuthInfo()) {
        fn = emitPointerAuthSign(IGF, fn, authInfo);
      }
    }
    llvm::Value *size = nullptr;
    if (emitSize) {
      if (functionPointer.useStaticContextSize()) {
        size = llvm::ConstantInt::get(IGF.IGM.Int32Ty,
                                      initialContextSize.getValue());
      } else {
        assert(!functionPointer.useStaticContextSize());
        auto *sizePtr = IGF.Builder.CreateStructGEP(afpPtr, 1);
        size = IGF.Builder.CreateLoad(sizePtr, IGF.IGM.getPointerAlignment());
      }
    }
    return {fn, size};
  }
  }
}

static void externalizeArguments(IRGenFunction &IGF, const Callee &callee,
                                 Explosion &in, Explosion &out,
                                 TemporarySet &temporaries, bool isOutlined);

namespace {

class SyncCallEmission final : public CallEmission {
  using super = CallEmission;

public:
  SyncCallEmission(IRGenFunction &IGF, llvm::Value *selfValue, Callee &&callee)
      : CallEmission(IGF, selfValue, std::move(callee)) {
    setFromCallee();
  }

  FunctionPointer getCalleeFunctionPointer() override {
    return getCallee().getFunctionPointer().getAsFunction(IGF);
  }
  SILType getParameterType(unsigned index) override {
    SILFunctionConventions origConv(getCallee().getOrigFunctionType(),
                                    IGF.getSILModule());
    return origConv.getSILArgumentType(
        index, IGF.IGM.getMaximalTypeExpansionContext());
  }

  llvm::CallInst *createCall(const FunctionPointer &fn,
                             ArrayRef<llvm::Value *> args) override {
    return IGF.Builder.CreateCall(fn, Args);
  }

  void begin() override { super::begin(); }
  void end() override { super::end(); }
  void setFromCallee() override {
    super::setFromCallee();

    auto fnType = CurCallee.getOrigFunctionType();

    if (fnType->getRepresentation() ==
        SILFunctionTypeRepresentation::WitnessMethod) {
      unsigned n = getTrailingWitnessSignatureLength(IGF.IGM, fnType);
      while (n--) {
        Args[--LastArgWritten] = nullptr;
      }
    }

    llvm::Value *contextPtr = CurCallee.getSwiftContext();

    // Add the error result if we have one.
    if (fnType->hasErrorResult()) {
      // The invariant is that this is always zero-initialized, so we
      // don't need to do anything extra here.
      SILFunctionConventions fnConv(fnType, IGF.getSILModule());
      Address errorResultSlot = IGF.getCalleeErrorResultSlot(
          fnConv.getSILErrorType(IGF.IGM.getMaximalTypeExpansionContext()));

      assert(LastArgWritten > 0);
      Args[--LastArgWritten] = errorResultSlot.getAddress();
      addAttribute(LastArgWritten + llvm::AttributeList::FirstArgIndex,
                   llvm::Attribute::NoCapture);
      IGF.IGM.addSwiftErrorAttributes(CurCallee.getMutableAttributes(),
                                      LastArgWritten);

      // Fill in the context pointer if necessary.
      if (!contextPtr) {
        assert(!CurCallee.getOrigFunctionType()->getExtInfo().hasContext() &&
               "Missing context?");
        contextPtr = llvm::UndefValue::get(IGF.IGM.RefCountedPtrTy);
      }
    }

    // Add the data pointer if we have one.
    // (Note that we're emitting backwards, so this correctly goes
    // *before* the error pointer.)
    if (contextPtr) {
      assert(LastArgWritten > 0);
      Args[--LastArgWritten] = contextPtr;
      IGF.IGM.addSwiftSelfAttributes(CurCallee.getMutableAttributes(),
                                     LastArgWritten);
    }
  }
  void setArgs(Explosion &original, bool isOutlined,
               WitnessMetadata *witnessMetadata) override {
    // Convert arguments to a representation appropriate to the calling
    // convention.
    Explosion adjusted;

    auto origCalleeType = CurCallee.getOrigFunctionType();
    SILFunctionConventions fnConv(origCalleeType, IGF.getSILModule());

    // Pass along the indirect result pointers.
    original.transferInto(adjusted, fnConv.getNumIndirectSILResults());

    // Pass along the coroutine buffer.
    switch (origCalleeType->getCoroutineKind()) {
    case SILCoroutineKind::YieldMany:
    case SILCoroutineKind::YieldOnce:
      original.transferInto(adjusted, 1);
      break;

    case SILCoroutineKind::None:
      break;
    }

    // Translate the formal arguments and handle any special arguments.
    switch (getCallee().getRepresentation()) {
    case SILFunctionTypeRepresentation::ObjCMethod:
      adjusted.add(getCallee().getObjCMethodReceiver());
      adjusted.add(getCallee().getObjCMethodSelector());
      externalizeArguments(IGF, getCallee(), original, adjusted, Temporaries,
                           isOutlined);
      break;

    case SILFunctionTypeRepresentation::Block:
    case SILFunctionTypeRepresentation::CXXMethod:
      if (getCallee().getRepresentation() ==
          SILFunctionTypeRepresentation::Block)
        adjusted.add(getCallee().getBlockObject());
      else
        adjusted.add(getCallee().getCXXMethodSelf());
      LLVM_FALLTHROUGH;

    case SILFunctionTypeRepresentation::CFunctionPointer:
      externalizeArguments(IGF, getCallee(), original, adjusted, Temporaries,
                           isOutlined);
      break;

    case SILFunctionTypeRepresentation::WitnessMethod:
      assert(witnessMetadata);
      assert(witnessMetadata->SelfMetadata->getType() ==
             IGF.IGM.TypeMetadataPtrTy);
      assert(witnessMetadata->SelfWitnessTable->getType() ==
             IGF.IGM.WitnessTablePtrTy);
      Args.rbegin()[1] = witnessMetadata->SelfMetadata;
      Args.rbegin()[0] = witnessMetadata->SelfWitnessTable;
      LLVM_FALLTHROUGH;

    case SILFunctionTypeRepresentation::Closure:
    case SILFunctionTypeRepresentation::Method:
    case SILFunctionTypeRepresentation::Thin:
    case SILFunctionTypeRepresentation::Thick: {
      // Check for value arguments that need to be passed indirectly.
      // But don't expect to see 'self' if it's been moved to the context
      // position.
      auto params = origCalleeType->getParameters();
      if (hasSelfContextParameter(origCalleeType)) {
        params = params.drop_back();
      }
      for (auto param : params) {
        addNativeArgument(IGF, original, origCalleeType, param, adjusted,
                          isOutlined);
      }

      // Anything else, just pass along.  This will include things like
      // generic arguments.
      adjusted.add(original.claimAll());

      break;
    }
    }
    super::setArgs(adjusted, isOutlined, witnessMetadata);
  }
  void emitCallToUnmappedExplosion(llvm::CallInst *call, Explosion &out) override {
    // Bail out immediately on a void result.
    llvm::Value *result = call;
    if (result->getType()->isVoidTy())
      return;

    SILFunctionConventions fnConv(getCallee().getOrigFunctionType(),
                                  IGF.getSILModule());

    // If the result was returned autoreleased, implicitly insert the reclaim.
    // This is only allowed on a single direct result.
    if (fnConv.getNumDirectSILResults() == 1
        && (fnConv.getDirectSILResults().begin()->getConvention()
            == ResultConvention::Autoreleased)) {
      result = emitObjCRetainAutoreleasedReturnValue(IGF, result);
    }

    auto origFnType = getCallee().getOrigFunctionType();

    // Specially handle noreturn c function which would return a 'Never' SIL result
    // type.
    if (origFnType->getLanguage() == SILFunctionLanguage::C &&
        origFnType->isNoReturnFunction(
            IGF.getSILModule(), IGF.IGM.getMaximalTypeExpansionContext())) {
      auto clangResultTy = result->getType();
      extractScalarResults(IGF, clangResultTy, result, out);
      return;
    }

    // Get the natural IR type in the body of the function that makes
    // the call. This may be different than the IR type returned by the
    // call itself due to ABI type coercion.
    auto resultType =
        fnConv.getSILResultType(IGF.IGM.getMaximalTypeExpansionContext());
    auto &nativeSchema = IGF.IGM.getTypeInfo(resultType).nativeReturnValueSchema(IGF.IGM);

    // For ABI reasons the result type of the call might not actually match the
    // expected result type.
    //
    // This can happen when calling C functions, or class method dispatch thunks
    // for methods that have covariant ABI-compatible overrides.
    auto expectedNativeResultType = nativeSchema.getExpandedType(IGF.IGM);
    // If the expected result type is void, bail.
    if (expectedNativeResultType->isVoidTy())
      return;
    if (result->getType() != expectedNativeResultType) {
      result =
          IGF.coerceValue(result, expectedNativeResultType, IGF.IGM.DataLayout);
    }

    // Gather the values.
    Explosion nativeExplosion;
    extractScalarResults(IGF, result->getType(), result, nativeExplosion);

    out = nativeSchema.mapFromNative(IGF.IGM, IGF, nativeExplosion, resultType);
  }
  Address getCalleeErrorSlot(SILType errorType) override {
    return IGF.getCalleeErrorResultSlot(errorType);
  };
};

class AsyncCallEmission final : public CallEmission {
  using super = CallEmission;

  Address contextBuffer;
  Address context;
  llvm::Value *calleeFunction = nullptr;
  llvm::Value *currentResumeFn = nullptr;
  llvm::Value *thickContext = nullptr;
  Optional<AsyncContextLayout> asyncContextLayout;

  AsyncContextLayout getAsyncContextLayout() {
    if (!asyncContextLayout) {
      asyncContextLayout.emplace(::getAsyncContextLayout(
          IGF.IGM, getCallee().getOrigFunctionType(),
          getCallee().getSubstFunctionType(), getCallee().getSubstitutions()));
    }
    return *asyncContextLayout;
  }

  void saveValue(ElementLayout layout, Explosion &explosion, bool isOutlined) {
    Address addr = layout.project(IGF, context, /*offsets*/ llvm::None);
    auto &ti = cast<LoadableTypeInfo>(layout.getType());
    ti.initialize(IGF, explosion, addr, isOutlined);
  }
  void loadValue(ElementLayout layout, Explosion &explosion) {
    Address addr = layout.project(IGF, context, /*offsets*/ llvm::None);
    auto &ti = cast<LoadableTypeInfo>(layout.getType());
    ti.loadAsTake(IGF, addr, explosion);
  }

public:
  AsyncCallEmission(IRGenFunction &IGF, llvm::Value *selfValue, Callee &&callee)
      : CallEmission(IGF, selfValue, std::move(callee)) {
    setFromCallee();
  }

  void begin() override {
    super::begin();
    assert(!contextBuffer.isValid());
    assert(!context.isValid());
    auto layout = getAsyncContextLayout();
    // Allocate space for the async arguments.
    llvm::Value *dynamicContextSize32;
    std::tie(calleeFunction, dynamicContextSize32) = getAsyncFunctionAndSize(
        IGF, CurCallee.getOrigFunctionType()->getRepresentation(),
        CurCallee.getFunctionPointer(), thickContext,
        std::make_pair(true, true), layout.getSize());
    auto *dynamicContextSize =
        IGF.Builder.CreateZExt(dynamicContextSize32, IGF.IGM.SizeTy);
    contextBuffer = emitAllocAsyncContext(IGF, dynamicContextSize);
    context = layout.emitCastTo(IGF, contextBuffer.getAddress());
    if (layout.canHaveError()) {
      auto fieldLayout = layout.getErrorLayout();
      auto addr = fieldLayout.project(IGF, context, /*offsets*/ llvm::None);
      auto &ti = fieldLayout.getType();
      auto nullError = llvm::Constant::getNullValue(ti.getStorageType());
      IGF.Builder.CreateStore(nullError, addr);
    }
  }
  void end() override {
    assert(contextBuffer.isValid());
    assert(context.isValid());
    emitDeallocAsyncContext(IGF, contextBuffer);
    super::end();
  }
  void setFromCallee() override {
    super::setFromCallee();
    thickContext = CurCallee.getSwiftContext();
  }
  FunctionPointer getCalleeFunctionPointer() override {
    return FunctionPointer(
        FunctionPointer::KindTy::Function, calleeFunction,
        CurCallee.getFunctionPointer().getAuthInfo(),
        IGF.IGM.getSignature(getCallee().getSubstFunctionType()));
  }
  SILType getParameterType(unsigned index) override {
    return getAsyncContextLayout().getParameterType(index);
  }
  void setArgs(Explosion &llArgs, bool isOutlined,
               WitnessMetadata *witnessMetadata) override {
    Explosion asyncExplosion;
    asyncExplosion.add(IGF.getAsyncTask());
    asyncExplosion.add(IGF.getAsyncExecutor());
    asyncExplosion.add(contextBuffer.getAddress());
    super::setArgs(asyncExplosion, false, witnessMetadata);
    SILFunctionConventions fnConv(getCallee().getSubstFunctionType(),
                                  IGF.getSILModule());
    auto layout = getAsyncContextLayout();

    // Set caller info into the context.
    { // caller context
      Explosion explosion;
      auto fieldLayout = layout.getParentLayout();
      auto *context = IGF.getAsyncContext();
      if (auto schema = IGF.IGM.getOptions().PointerAuth.AsyncContextParent) {
        Address fieldAddr =
            fieldLayout.project(IGF, this->context, /*offsets*/ llvm::None);
        auto authInfo = PointerAuthInfo::emit(
            IGF, schema, fieldAddr.getAddress(), PointerAuthEntity());
        context = emitPointerAuthSign(IGF, context, authInfo);
      }
      explosion.add(context);
      saveValue(fieldLayout, explosion, isOutlined);
    }
    { // Return to caller function.
      auto fieldLayout = layout.getResumeParentLayout();
      currentResumeFn = IGF.Builder.CreateIntrinsicCall(
          llvm::Intrinsic::coro_async_resume, {});
      auto fnVal = currentResumeFn;
      // Sign the pointer.
      if (auto schema = IGF.IGM.getOptions().PointerAuth.AsyncContextResume) {
        Address fieldAddr =
            fieldLayout.project(IGF, this->context, /*offsets*/ llvm::None);
        auto authInfo = PointerAuthInfo::emit(
            IGF, schema, fieldAddr.getAddress(), PointerAuthEntity());
        fnVal = emitPointerAuthSign(IGF, fnVal, authInfo);
      }
      fnVal = IGF.Builder.CreateBitCast(fnVal,
                                        IGF.IGM.TaskContinuationFunctionPtrTy);
      Explosion explosion;
      explosion.add(fnVal);
      saveValue(fieldLayout, explosion, isOutlined);
    }
    { // caller executor
      Explosion explosion;
      explosion.add(IGF.getAsyncExecutor());
      auto fieldLayout = layout.getResumeParentExecutorLayout();
      saveValue(fieldLayout, explosion, isOutlined);
    }
    // Move all the arguments into the context.
    for (unsigned index = 0, count = layout.getIndirectReturnCount();
         index < count; ++index) {
      auto fieldLayout = layout.getIndirectReturnLayout(index);
      saveValue(fieldLayout, llArgs, isOutlined);
    }
    for (unsigned index = 0, count = layout.getArgumentCount(); index < count;
         ++index) {
      auto fieldLayout = layout.getArgumentLayout(index);
      saveValue(fieldLayout, llArgs, isOutlined);
    }
    if (layout.hasBindings()) {
      auto bindingLayout = layout.getBindingsLayout();
      auto bindingsAddr = bindingLayout.project(IGF, context, /*offsets*/ None);
      layout.getBindings().save(IGF, bindingsAddr, llArgs);
    }
    auto isThick =
        getCallee().getRepresentation() == SILFunctionTypeRepresentation::Thick;
    if (selfValue || isThick) {
      Explosion localExplosion;
      if (selfValue) {
        assert(!isThick);
        localExplosion.add(selfValue);
      } else {
        localExplosion.add(getCallee().getSwiftContext());
      }
      auto fieldLayout = layout.getLocalContextLayout();
      saveValue(fieldLayout, localExplosion, isOutlined);
    }
  }
  void emitCallToUnmappedExplosion(llvm::CallInst *call, Explosion &out) override {
    auto layout = getAsyncContextLayout();
    for (unsigned index = 0, count = layout.getDirectReturnCount();
         index < count; ++index) {
      auto fieldLayout = layout.getDirectReturnLayout(index);
      loadValue(fieldLayout, out);
    }
  }
  Address getCalleeErrorSlot(SILType errorType) override {
    auto layout = getAsyncContextLayout();
    auto errorLayout = layout.getErrorLayout();
    auto address = errorLayout.project(IGF, context, /*offsets*/ llvm::None);
    return address;
  };

  llvm::CallInst *createCall(const FunctionPointer &fn,
                             ArrayRef<llvm::Value *> args) override {
    auto &IGM = IGF.IGM;
    auto &Builder = IGF.Builder;
    // Setup the suspend point.
    SmallVector<llvm::Value *, 8> arguments;
    arguments.push_back(currentResumeFn);
    auto resumeProjFn = IGF.getOrCreateResumePrjFn();
    arguments.push_back(
        Builder.CreateBitOrPointerCast(resumeProjFn, IGM.Int8PtrTy));
    auto dispatchFn = IGF.createAsyncDispatchFn(fn, args);
    arguments.push_back(
        Builder.CreateBitOrPointerCast(dispatchFn, IGM.Int8PtrTy));
    arguments.push_back(
        Builder.CreateBitOrPointerCast(fn.getRawPointer(), IGM.Int8PtrTy));
    if (auto authInfo = fn.getAuthInfo()) {
      arguments.push_back(fn.getAuthInfo().getDiscriminator());
    }
    for (auto arg: args)
      arguments.push_back(arg);
    return IGF.emitSuspendAsyncCall(arguments);
  }
};

} // end anonymous namespace

std::unique_ptr<CallEmission> irgen::getCallEmission(IRGenFunction &IGF,
                                                     llvm::Value *selfValue,
                                                     Callee &&callee) {
  if (callee.getOrigFunctionType()->isAsync()) {
    return std::make_unique<AsyncCallEmission>(IGF, selfValue,
                                               std::move(callee));
  } else {
    return std::make_unique<SyncCallEmission>(IGF, selfValue,
                                              std::move(callee));
  }
}

/// Emit the unsubstituted result of this call into the given explosion.
/// The unsubstituted result must be naturally returned directly.
void CallEmission::emitToUnmappedExplosion(Explosion &out) {
  assert(state == State::Emitting);
  assert(LastArgWritten == 0 && "emitting unnaturally to explosion");

  auto call = emitCallSite();

  emitCallToUnmappedExplosion(call, out);
}

/// Emit the unsubstituted result of this call to the given address.
/// The unsubstituted result must be naturally returned indirectly.
void CallEmission::emitToUnmappedMemory(Address result) {
  assert(state == State::Emitting);
  assert(LastArgWritten == 1 && "emitting unnaturally to indirect result");

  Args[0] = result.getAddress();
  SILFunctionConventions FnConv(CurCallee.getSubstFunctionType(),
                                IGF.getSILModule());
  addIndirectResultAttributes(IGF.IGM, CurCallee.getMutableAttributes(),
                              0, FnConv.getNumIndirectSILResults() <= 1);
#ifndef NDEBUG
  LastArgWritten = 0; // appease an assert
#endif
  
  emitCallSite();
}

/// The private routine to ultimately emit a call or invoke instruction.
llvm::CallInst *CallEmission::emitCallSite() {
  assert(state == State::Emitting);
  assert(LastArgWritten == 0);
  assert(!EmittedCall);
  EmittedCall = true;

  // Make the call and clear the arguments array.
  FunctionPointer fn = getCalleeFunctionPointer();
  assert(fn.getKind() == FunctionPointer::KindTy::Function);
  auto fnTy = fn.getFunctionType();

  // Coerce argument types for those cases where the IR type required
  // by the ABI differs from the type used within the function body.
  assert(fnTy->getNumParams() == Args.size());
  for (int i = 0, e = fnTy->getNumParams(); i != e; ++i) {
    auto *paramTy = fnTy->getParamType(i);
    auto *argTy = Args[i]->getType();
    if (paramTy != argTy)
      Args[i] = IGF.coerceValue(Args[i], paramTy, IGF.IGM.DataLayout);
  }

  // TODO: exceptions!
  auto call = createCall(fn, Args);

  // Make coroutines calls opaque to LLVM analysis.
  if (IsCoroutine) {
    // Go back and insert some instructions right before the call.
    // It's easier to do this than to mess around with copying and
    // modifying the FunctionPointer above.
    IGF.Builder.SetInsertPoint(call);

    // Insert a call to @llvm.coro.prepare.retcon, then bitcast to the right
    // function type.
    auto origCallee = call->getCalledOperand();
    llvm::Value *opaqueCallee = origCallee;
    opaqueCallee =
      IGF.Builder.CreateBitCast(opaqueCallee, IGF.IGM.Int8PtrTy);
    opaqueCallee = IGF.Builder.CreateIntrinsicCall(
        llvm::Intrinsic::coro_prepare_retcon, {opaqueCallee});
    opaqueCallee =
      IGF.Builder.CreateBitCast(opaqueCallee, origCallee->getType());
    call->setCalledFunction(fn.getFunctionType(), opaqueCallee);

    // Reset the insert point to after the call.
    IGF.Builder.SetInsertPoint(call->getParent());
  }

  Args.clear();

  // Destroy any temporaries we needed.
  // We don't do this for coroutines because we need to wait until the
  // coroutine is complete.
  if (!IsCoroutine) {
    Temporaries.destroyAll(IGF);

    // Clear the temporary set so that we can assert that there are no
    // temporaries later.
    Temporaries.clear();
  }

  // Return.
  return call;
}

llvm::CallInst *IRBuilder::CreateCall(const FunctionPointer &fn,
                                      ArrayRef<llvm::Value*> args) {
  assert(fn.getKind() == FunctionPointer::KindTy::Function);
  SmallVector<llvm::OperandBundleDef, 1> bundles;

  // Add a pointer-auth bundle if necessary.
  if (const auto &authInfo = fn.getAuthInfo()) {
    auto key = getInt32(authInfo.getKey());
    auto discriminator = authInfo.getDiscriminator();
    llvm::Value *bundleArgs[] = { key, discriminator };
    bundles.emplace_back("ptrauth", bundleArgs);
  }

  assert(!isTrapIntrinsic(fn.getRawPointer()) && "Use CreateNonMergeableTrap");
  llvm::CallInst *call = IRBuilderBase::CreateCall(
      cast<llvm::FunctionType>(
          fn.getRawPointer()->getType()->getPointerElementType()),
      fn.getRawPointer(), args, bundles);
  call->setAttributes(fn.getAttributes());
  call->setCallingConv(fn.getCallingConv());
  return call;
}

/// Emit the result of this call to memory.
void CallEmission::emitToMemory(Address addr,
                                const LoadableTypeInfo &indirectedResultTI,
                                bool isOutlined) {
  assert(state == State::Emitting);
  assert(LastArgWritten <= 1);

  // If the call is naturally to an explosion, emit it that way and
  // then initialize the temporary.
  if (LastArgWritten == 0) {
    Explosion result;
    emitToExplosion(result, isOutlined);
    indirectedResultTI.initialize(IGF, result, addr, isOutlined);
    return;
  }

  // Okay, we're naturally emitting to memory.
  Address origAddr = addr;

  auto origFnType = CurCallee.getOrigFunctionType();
  auto substFnType = CurCallee.getSubstFunctionType();

  // We're never being asked to do anything with *formal*
  // indirect results here, just the possibility of a direct-in-SIL
  // result that's actually being passed indirectly.
  //
  // TODO: SIL address lowering should be able to handle such cases earlier.
  auto origResultType =
      origFnType
          ->getDirectFormalResultsType(IGF.IGM.getSILModule(),
                                       IGF.IGM.getMaximalTypeExpansionContext())
          .getASTType();
  auto substResultType =
      substFnType
          ->getDirectFormalResultsType(IGF.IGM.getSILModule(),
                                       IGF.IGM.getMaximalTypeExpansionContext())
          .getASTType();

  if (origResultType->hasTypeParameter())
    origResultType = IGF.IGM.getGenericEnvironment()
      ->mapTypeIntoContext(origResultType)
      ->getCanonicalType();

  if (origResultType != substResultType) {
    auto origTy = IGF.IGM.getStoragePointerTypeForLowered(origResultType);
    origAddr = IGF.Builder.CreateBitCast(origAddr, origTy);
  }

  emitToUnmappedMemory(origAddr);
}

static void emitCastToSubstSchema(IRGenFunction &IGF, Explosion &in,
                                  const ExplosionSchema &schema,
                                  Explosion &out) {
  assert(in.size() == schema.size());
  for (unsigned i = 0, e = schema.size(); i != e; ++i) {
    llvm::Type *expectedType = schema.begin()[i].getScalarType();
    llvm::Value *value = in.claimNext();
    if (value->getType() != expectedType)
      value = IGF.Builder.CreateBitCast(value, expectedType,
                                        value->getName() + ".asSubstituted");
    out.add(value);
  }
}

void CallEmission::emitYieldsToExplosion(Explosion &out) {
  assert(state == State::Emitting);
  // Emit the call site.
  auto call = emitCallSite();

  // Pull the raw return values out.
  Explosion rawReturnValues;
  extractScalarResults(IGF, call->getType(), call, rawReturnValues);

  auto coroInfo = getCallee().getSignature().getCoroutineInfo();

  // Go ahead and forward the continuation pointer as an opaque pointer.
  auto continuation = rawReturnValues.claimNext();
  out.add(continuation);

  // Collect the raw value components.
  Explosion rawYieldComponents;

  // Add all the direct yield components.
  rawYieldComponents.add(
    rawReturnValues.claim(coroInfo.NumDirectYieldComponents));

  // Add all the indirect yield components.
  assert(rawReturnValues.size() <= 1);
  if (!rawReturnValues.empty()) {
    // Extract the indirect yield buffer.
    auto indirectPointer = rawReturnValues.claimNext();
    auto indirectStructTy = cast<llvm::StructType>(
      indirectPointer->getType()->getPointerElementType());
    auto layout = IGF.IGM.DataLayout.getStructLayout(indirectStructTy);
    Address indirectBuffer(indirectPointer,
                           Alignment(layout->getAlignment().value()));

    for (auto i : indices(indirectStructTy->elements())) {
      // Skip padding.
      if (indirectStructTy->getElementType(i)->isArrayTy())
        continue;

      auto eltAddr = IGF.Builder.CreateStructGEP(indirectBuffer, i, layout);
      rawYieldComponents.add(IGF.Builder.CreateLoad(eltAddr));
    }
  }

  auto substCoroType = getCallee().getSubstFunctionType();
  SILFunctionConventions fnConv(substCoroType, IGF.getSILModule());
  for (auto yield : fnConv.getYields()) {
    YieldSchema schema(IGF.IGM, fnConv, yield);

    // If the schema says it's indirect, then we expect a pointer.
    if (schema.isIndirect()) {
      auto pointer = IGF.Builder.CreateBitCast(rawYieldComponents.claimNext(),
                                               schema.getIndirectPointerType());

      // If it's formally indirect, then we should just add that pointer
      // to the output.
      if (schema.isFormalIndirect()) {
        out.add(pointer);
        continue;
      }

      // Otherwise, we need to load.
      auto &yieldTI = cast<LoadableTypeInfo>(schema.getTypeInfo());
      yieldTI.loadAsTake(IGF, yieldTI.getAddressForPointer(pointer), out);
      continue;
    }

    // Otherwise, it's direct.  Remap.
    const auto &directSchema = schema.getDirectSchema();
    Explosion eltValues;
    rawYieldComponents.transferInto(eltValues, directSchema.size());
    auto temp = directSchema.mapFromNative(IGF.IGM, IGF, eltValues,
                                           schema.getSILType());

    auto &yieldTI = cast<LoadableTypeInfo>(schema.getTypeInfo());
    emitCastToSubstSchema(IGF, temp, yieldTI.getSchema(), out);
  }
}

/// Emit the result of this call to an explosion.
void CallEmission::emitToExplosion(Explosion &out, bool isOutlined) {
  assert(state == State::Emitting);
  assert(LastArgWritten <= 1);

  // For coroutine calls, we need to collect the yields, not the results;
  // this looks very different.
  if (IsCoroutine) {
    assert(LastArgWritten == 0 && "coroutine with indirect result?");
    emitYieldsToExplosion(out);
    return;
  }

  SILFunctionConventions fnConv(getCallee().getSubstFunctionType(),
                                IGF.getSILModule());
  SILType substResultType =
      fnConv.getSILResultType(IGF.IGM.getMaximalTypeExpansionContext());

  auto &substResultTI =
    cast<LoadableTypeInfo>(IGF.getTypeInfo(substResultType));

  auto origFnType = getCallee().getOrigFunctionType();
  auto isNoReturnCFunction =
      origFnType->getLanguage() == SILFunctionLanguage::C &&
      origFnType->isNoReturnFunction(IGF.getSILModule(),
                                     IGF.IGM.getMaximalTypeExpansionContext());

  // If the call is naturally to memory, emit it that way and then
  // explode that temporary.
  if (LastArgWritten == 1) {
    if (isNoReturnCFunction) {
      auto fnType = getCallee().getFunctionPointer().getFunctionType();
      assert(fnType->getNumParams() > 0);
      auto resultTy = fnType->getParamType(0)->getPointerElementType();
      auto temp = IGF.createAlloca(resultTy, Alignment(), "indirect.result");
      emitToMemory(temp, substResultTI, isOutlined);
      return;
    }

    StackAddress ctemp = substResultTI.allocateStack(IGF, substResultType,
                                                     "call.aggresult");
    Address temp = ctemp.getAddress();
    emitToMemory(temp, substResultTI, isOutlined);

    // We can use a take.
    substResultTI.loadAsTake(IGF, temp, out);

    substResultTI.deallocateStack(IGF, ctemp, substResultType);
    return;
  }

  // Okay, we're naturally emitting to an explosion.
  Explosion temp;
  emitToUnmappedExplosion(temp);

  // Specially handle noreturn c function which would return a 'Never' SIL result
  // type: there is no need to cast the result.
  if (isNoReturnCFunction) {
    temp.transferInto(out, temp.size());
    return;
  }

  // We might need to bitcast the results.
  emitCastToSubstSchema(IGF, temp, substResultTI.getSchema(), out);
}

CallEmission::CallEmission(CallEmission &&other)
  : IGF(other.IGF),
    Args(std::move(other.Args)),
    CurCallee(std::move(other.CurCallee)),
    LastArgWritten(other.LastArgWritten),
    EmittedCall(other.EmittedCall) {
  // Prevent other's destructor from asserting.
  LastArgWritten = 0;
  EmittedCall = true;
  state = State::Finished;
}

CallEmission::~CallEmission() {
  assert(LastArgWritten == 0);
  assert(EmittedCall);
  assert(Temporaries.hasBeenCleared());
  assert(state == State::Finished);
}

void CallEmission::begin() {}

void CallEmission::end() {
  assert(state == State::Emitting);
  state = State::Finished;
}

Callee::Callee(CalleeInfo &&info, const FunctionPointer &fn,
               llvm::Value *firstData, llvm::Value *secondData)
    : Info(std::move(info)), Fn(fn),
      FirstData(firstData), SecondData(secondData) {

#ifndef NDEBUG
  // We should have foreign info if it's a foreign call.
  assert((Fn.getForeignInfo().ClangInfo != nullptr) ==
         (Info.OrigFnType->getLanguage() == SILFunctionLanguage::C));

  // We should have the right data values for the representation.
  switch (Info.OrigFnType->getRepresentation()) {
  case SILFunctionTypeRepresentation::ObjCMethod:
    assert(FirstData && SecondData);
    break;
  case SILFunctionTypeRepresentation::Method:
  case SILFunctionTypeRepresentation::WitnessMethod:
    assert((FirstData != nullptr) ==
           hasSelfContextParameter(Info.OrigFnType));
    assert(!SecondData);
    break;
  case SILFunctionTypeRepresentation::Thick:
  case SILFunctionTypeRepresentation::Block:
    assert(FirstData && !SecondData);
    break;
  case SILFunctionTypeRepresentation::Thin:
  case SILFunctionTypeRepresentation::Closure:
  case SILFunctionTypeRepresentation::CFunctionPointer:
    assert(!FirstData && !SecondData);
    break;
  case SILFunctionTypeRepresentation::CXXMethod:
    assert(FirstData && !SecondData);
    break;
  }
#endif

}

llvm::Value *Callee::getSwiftContext() const {
  switch (Info.OrigFnType->getRepresentation()) {
  case SILFunctionTypeRepresentation::Block:
  case SILFunctionTypeRepresentation::ObjCMethod:
  case SILFunctionTypeRepresentation::CFunctionPointer:
  case SILFunctionTypeRepresentation::Thin:
  case SILFunctionTypeRepresentation::Closure:
  case SILFunctionTypeRepresentation::CXXMethod:
    return nullptr;

  case SILFunctionTypeRepresentation::WitnessMethod:
  case SILFunctionTypeRepresentation::Method:
    // This may or may not be null.
    return FirstData;

  case SILFunctionTypeRepresentation::Thick:
    assert(FirstData && "no context value set on callee");
    return FirstData;
  }
  llvm_unreachable("bad representation");
}

llvm::Value *Callee::getBlockObject() const {
  assert(Info.OrigFnType->getRepresentation() ==
           SILFunctionTypeRepresentation::Block &&
         "not a block");
  assert(FirstData && "no block object set on callee");
  return FirstData;
}

llvm::Value *Callee::getCXXMethodSelf() const {
  assert(Info.OrigFnType->getRepresentation() ==
             SILFunctionTypeRepresentation::CXXMethod &&
         "not a C++ method");
  assert(FirstData && "no self object set on callee");
  return FirstData;
}

llvm::Value *Callee::getObjCMethodReceiver() const {
  assert(Info.OrigFnType->getRepresentation() ==
           SILFunctionTypeRepresentation::ObjCMethod &&
         "not a method");
  assert(FirstData && "no receiver set on callee");
  return FirstData;
}

llvm::Value *Callee::getObjCMethodSelector() const {
  assert(Info.OrigFnType->getRepresentation() ==
           SILFunctionTypeRepresentation::ObjCMethod &&
         "not a method");
  assert(SecondData && "no selector set on callee");
  return SecondData;
}

/// Set up this emitter afresh from the current callee specs.
void CallEmission::setFromCallee() {
  assert(state == State::Emitting);
  IsCoroutine = CurCallee.getSubstFunctionType()->isCoroutine();
  EmittedCall = false;

  unsigned numArgs = CurCallee.getLLVMFunctionType()->getNumParams();

  // Set up the args array.
  assert(Args.empty());
  Args.reserve(numArgs);
  Args.set_size(numArgs);
  LastArgWritten = numArgs;
}

bool irgen::canCoerceToSchema(IRGenModule &IGM,
                              ArrayRef<llvm::Type*> expandedTys,
                              const ExplosionSchema &schema) {
  // If the schemas don't even match in number, we have to go
  // through memory.
  if (expandedTys.size() != schema.size())
    return false;

  // If there's just one element, we can always coerce as a scalar.
  if (expandedTys.size() == 1) return true;

  // If there are multiple elements, the pairs of types need to
  // match in size for the coercion to work.
  for (size_t i = 0, e = expandedTys.size(); i != e; ++i) {
    llvm::Type *inputTy = schema[i].getScalarType();
    llvm::Type *outputTy = expandedTys[i];
    if (inputTy != outputTy &&
        IGM.DataLayout.getTypeSizeInBits(inputTy) !=
        IGM.DataLayout.getTypeSizeInBits(outputTy))
      return false;
  }

  // Okay, everything is fine.
  return true;
}

static llvm::Type *getOutputType(TranslationDirection direction, unsigned index,
                                 const ExplosionSchema &nativeSchema,
                                 ArrayRef<llvm::Type*> expandedForeignTys) {
  assert(nativeSchema.size() == expandedForeignTys.size());
  return (direction == TranslationDirection::ToForeign
            ? expandedForeignTys[index]
            : nativeSchema[index].getScalarType());
}

static void emitCoerceAndExpand(IRGenFunction &IGF, Explosion &in,
                                Explosion &out, SILType paramTy,
                                const LoadableTypeInfo &paramTI,
                                llvm::StructType *coercionTy,
                                ArrayRef<llvm::Type *> expandedTys,
                                TranslationDirection direction,
                                bool isOutlined) {
  // If we can directly coerce the scalar values, avoid going through memory.
  auto schema = paramTI.getSchema();
  if (canCoerceToSchema(IGF.IGM, expandedTys, schema)) {
    for (auto index : indices(expandedTys)) {
      llvm::Value *arg = in.claimNext();
      assert(arg->getType() ==
               getOutputType(reverse(direction), index, schema, expandedTys));
      auto outputTy = getOutputType(direction, index, schema, expandedTys);

      if (arg->getType() != outputTy)
        arg = IGF.coerceValue(arg, outputTy, IGF.IGM.DataLayout);
      out.add(arg);
    }
    return;
  }

  // Otherwise, materialize to a temporary.
  auto temporaryAlloc =
    paramTI.allocateStack(IGF, paramTy, "coerce-and-expand.temp");
  Address temporary = temporaryAlloc.getAddress();

  auto coercionTyLayout = IGF.IGM.DataLayout.getStructLayout(coercionTy);

  // Make the alloca at least as aligned as the coercion struct, just
  // so that the element accesses we make don't end up under-aligned.
  Alignment coercionTyAlignment =
      Alignment(coercionTyLayout->getAlignment().value());
  auto alloca = cast<llvm::AllocaInst>(temporary.getAddress());
  if (alloca->getAlignment() < coercionTyAlignment.getValue()) {
    alloca->setAlignment(
        llvm::MaybeAlign(coercionTyAlignment.getValue()).valueOrOne());
    temporary = Address(temporary.getAddress(), coercionTyAlignment);
  }

  // If we're translating *to* the foreign expansion, do an ordinary
  // initialization from the input explosion.
  if (direction == TranslationDirection::ToForeign) {
    paramTI.initialize(IGF, in, temporary, isOutlined);
  }

  Address coercedTemporary =
    IGF.Builder.CreateElementBitCast(temporary, coercionTy);

#ifndef NDEBUG
  size_t expandedTyIndex = 0;
#endif

  for (auto eltIndex : indices(coercionTy->elements())) {
    auto eltTy = coercionTy->getElementType(eltIndex);

    // Skip padding fields.
    if (eltTy->isArrayTy()) continue;
    assert(expandedTys[expandedTyIndex++] == eltTy);

    // Project down to the field.
    Address eltAddr =
      IGF.Builder.CreateStructGEP(coercedTemporary, eltIndex, coercionTyLayout);

    // If we're translating *to* the foreign expansion, pull the value out
    // of the field and add it to the output.
    if (direction == TranslationDirection::ToForeign) {
      llvm::Value *value = IGF.Builder.CreateLoad(eltAddr);
      out.add(value);

    // Otherwise, claim the next value from the input and store that
    // in the field.
    } else {
      llvm::Value *value = in.claimNext();
      IGF.Builder.CreateStore(value, eltAddr);
    }
  }

  assert(expandedTyIndex == expandedTys.size());

  // If we're translating *from* the foreign expansion, do an ordinary
  // load into the output explosion.
  if (direction == TranslationDirection::ToNative) {
    paramTI.loadAsTake(IGF, temporary, out);
  }

  paramTI.deallocateStack(IGF, temporaryAlloc, paramTy);
}

static void emitDirectExternalArgument(IRGenFunction &IGF, SILType argType,
                                       const clang::CodeGen::ABIArgInfo &AI,
                                       Explosion &in, Explosion &out,
                                       bool isOutlined) {
  bool IsDirectFlattened = AI.isDirect() && AI.getCanBeFlattened();
  bool IsIndirect = !AI.isDirect();

  // If we're supposed to pass directly as a struct type, that
  // really means expanding out as multiple arguments.
  llvm::Type *coercedTy = AI.getCoerceToType();
  ArrayRef<llvm::Type *> expandedTys =
      expandScalarOrStructTypeToArray(coercedTy);

  auto &argTI = cast<LoadableTypeInfo>(IGF.getTypeInfo(argType));
  auto inputSchema = argTI.getSchema();

  // Check to see if we can pairwise coerce Swift's exploded scalars
  // to Clang's expanded elements.
  if ((IsDirectFlattened || IsIndirect) &&
      canCoerceToSchema(IGF.IGM, expandedTys, inputSchema)) {
    for (auto outputTy : expandedTys) {
      llvm::Value *arg = in.claimNext();
      if (arg->getType() != outputTy)
        arg = IGF.coerceValue(arg, outputTy, IGF.IGM.DataLayout);
      out.add(arg);
    }
    return;
  }

  // Otherwise, we need to coerce through memory.
  Address temporary;
  Size tempSize;
  std::tie(temporary, tempSize) =
      allocateForCoercion(IGF, argTI.getStorageType(), coercedTy, "coerced-arg");
  IGF.Builder.CreateLifetimeStart(temporary, tempSize);

  // Store to a temporary.
  Address tempOfArgTy = IGF.Builder.CreateBitCast(
      temporary, argTI.getStorageType()->getPointerTo());
  argTI.initializeFromParams(IGF, in, tempOfArgTy, argType, isOutlined);

  // Bitcast the temporary to the expected type.
  Address coercedAddr =
      IGF.Builder.CreateBitCast(temporary, coercedTy->getPointerTo());

  if (IsDirectFlattened && isa<llvm::StructType>(coercedTy)) {
    // Project out individual elements if necessary.
    auto *ST = cast<llvm::StructType>(coercedTy);
    const auto *layout = IGF.IGM.DataLayout.getStructLayout(ST);
    for (unsigned EI : range(ST->getNumElements())) {
      auto offset = Size(layout->getElementOffset(EI));
      auto address = IGF.Builder.CreateStructGEP(coercedAddr, EI, offset);
      out.add(IGF.Builder.CreateLoad(address));
    }
  } else {
    // Otherwise, collect the single scalar.
    out.add(IGF.Builder.CreateLoad(coercedAddr));
  }

  IGF.Builder.CreateLifetimeEnd(temporary, tempSize);
}

namespace {
  /// Load a clang argument expansion from a buffer.
  struct ClangExpandLoadEmitter :
    ClangExpandProjection<ClangExpandLoadEmitter> {

    Explosion &Out;
    ClangExpandLoadEmitter(IRGenFunction &IGF, Explosion &out)
      : ClangExpandProjection(IGF), Out(out) {}

    void visitScalar(llvm::Type *scalarTy, Address addr) {
      addr = IGF.Builder.CreateBitCast(addr, scalarTy->getPointerTo());
      auto value = IGF.Builder.CreateLoad(addr);
      Out.add(value);
    }
  };

  /// Store a clang argument expansion into a buffer.
  struct ClangExpandStoreEmitter :
    ClangExpandProjection<ClangExpandStoreEmitter> {

    Explosion &In;
    ClangExpandStoreEmitter(IRGenFunction &IGF, Explosion &in)
      : ClangExpandProjection(IGF), In(in) {}

    void visitScalar(llvm::Type *scalarTy, Address addr) {
      auto value = In.claimNext();

      addr = IGF.Builder.CreateBitCast(addr, scalarTy->getPointerTo());
      IGF.Builder.CreateStore(value, addr);
    }
  };
} // end anonymous namespace

/// Given a Swift value explosion in 'in', produce a Clang expansion
/// (according to ABIArgInfo::Expand) in 'out'.
static void
emitClangExpandedArgument(IRGenFunction &IGF, Explosion &in, Explosion &out,
                          clang::CanQualType clangType, SILType swiftType,
                          const LoadableTypeInfo &swiftTI, bool isOutlined) {
  // If Clang's expansion schema matches Swift's, great.
  auto swiftSchema = swiftTI.getSchema();
  if (doesClangExpansionMatchSchema(IGF.IGM, clangType, swiftSchema)) {
    return in.transferInto(out, swiftSchema.size());
  }

  // Otherwise, materialize to a temporary.
  auto ctemp = swiftTI.allocateStack(IGF, swiftType, "clang-expand-arg.temp");
  Address temp = ctemp.getAddress();
  swiftTI.initialize(IGF, in, temp, isOutlined);

  Address castTemp = IGF.Builder.CreateBitCast(temp, IGF.IGM.Int8PtrTy);
  ClangExpandLoadEmitter(IGF, out).visit(clangType, castTemp);

  swiftTI.deallocateStack(IGF, ctemp, swiftType);
}

/// Given a Clang-expanded (according to ABIArgInfo::Expand) parameter
/// in 'in', produce a Swift value explosion in 'out'.
void irgen::emitClangExpandedParameter(IRGenFunction &IGF,
                                       Explosion &in, Explosion &out,
                                       clang::CanQualType clangType,
                                       SILType swiftType,
                                       const LoadableTypeInfo &swiftTI) {
  // If Clang's expansion schema matches Swift's, great.
  auto swiftSchema = swiftTI.getSchema();
  if (doesClangExpansionMatchSchema(IGF.IGM, clangType, swiftSchema)) {
    return in.transferInto(out, swiftSchema.size());
  }

  // Otherwise, materialize to a temporary.
  auto tempAlloc = swiftTI.allocateStack(IGF, swiftType,
                                         "clang-expand-param.temp");
  Address temp = tempAlloc.getAddress();
  Address castTemp = IGF.Builder.CreateBitCast(temp, IGF.IGM.Int8PtrTy);
  ClangExpandStoreEmitter(IGF, in).visit(clangType, castTemp);

  // Then load out.
  swiftTI.loadAsTake(IGF, temp, out);

  swiftTI.deallocateStack(IGF, tempAlloc, swiftType);
}

static void externalizeArguments(IRGenFunction &IGF, const Callee &callee,
                                 Explosion &in, Explosion &out,
                                 TemporarySet &temporaries,
                                 bool isOutlined) {
  auto fnType = callee.getOrigFunctionType();
  auto silConv = SILFunctionConventions(fnType, IGF.IGM.silConv);
  auto params = fnType->getParameters();

  assert(callee.getForeignInfo().ClangInfo);
  auto &FI = *callee.getForeignInfo().ClangInfo;

  // The index of the first "physical" parameter from paramTys/FI that
  // corresponds to a logical parameter from params.
  unsigned firstParam = 0;
  unsigned paramEnd = FI.arg_size();

  // Handle the ObjC prefix.
  if (callee.getRepresentation() == SILFunctionTypeRepresentation::ObjCMethod) {
    // Ignore both the logical and the physical parameters associated
    // with self and _cmd.
    firstParam += 2;
    params = params.drop_back();

  // Or the block prefix.
  } else if (fnType->getRepresentation()
                == SILFunctionTypeRepresentation::Block) {
    // Ignore the physical block-object parameter.
    firstParam += 1;
  } else if (callee.getRepresentation() ==
             SILFunctionTypeRepresentation::CXXMethod) {
    // Skip the "self" param.
    paramEnd--;
  } else if (fnType->getNumResults() > 0 &&
             fnType->getSingleResult().isFormalIndirect()) {
    // Ignore the indirect result parameter.
    firstParam += 1;
  }

  for (unsigned i = firstParam; i != paramEnd; ++i) {
    auto clangParamTy = FI.arg_begin()[i].type;
    auto &AI = FI.arg_begin()[i].info;

    // We don't need to do anything to handle the Swift parameter-ABI
    // attributes here because we shouldn't be trying to round-trip
    // swiftcall function pointers through SIL as C functions anyway.
    assert(FI.getExtParameterInfo(i).getABI() == clang::ParameterABI::Ordinary);

    // Add a padding argument if required.
    if (auto *padType = AI.getPaddingType())
      out.add(llvm::UndefValue::get(padType));

    SILType paramType = silConv.getSILType(
        params[i - firstParam], IGF.IGM.getMaximalTypeExpansionContext());
    switch (AI.getKind()) {
    case clang::CodeGen::ABIArgInfo::Extend: {
      bool signExt = clangParamTy->hasSignedIntegerRepresentation();
      assert((signExt || clangParamTy->hasUnsignedIntegerRepresentation()) &&
             "Invalid attempt to add extension attribute to argument!");
      (void) signExt;
      LLVM_FALLTHROUGH;
    }
    case clang::CodeGen::ABIArgInfo::Direct: {
      auto toTy = AI.getCoerceToType();

      // Indirect parameters are bridged as Clang pointer types.
      if (silConv.isSILIndirect(params[i - firstParam])) {
        assert(paramType.isAddress() && "SIL type is not an address?");

        auto addr = in.claimNext();
        if (addr->getType() != toTy)
          addr = IGF.coerceValue(addr, toTy, IGF.IGM.DataLayout);
        out.add(addr);
        break;
      }

      emitDirectExternalArgument(IGF, paramType, AI, in, out, isOutlined);
      break;
    }
    case clang::CodeGen::ABIArgInfo::Indirect: {
      auto &ti = cast<LoadableTypeInfo>(IGF.getTypeInfo(paramType));

      auto temp = ti.allocateStack(IGF, paramType, "indirect-temporary");
      temporaries.add({temp, paramType});

      Address addr = temp.getAddress();
      // Set at least the alignment the ABI expects.
      if (AI.getIndirectByVal()) {
        auto ABIAlign = AI.getIndirectAlign();
        if (ABIAlign > addr.getAlignment()) {
          auto *AS = cast<llvm::AllocaInst>(addr.getAddress());
          AS->setAlignment(
              llvm::MaybeAlign(ABIAlign.getQuantity()).valueOrOne());
          addr = Address(addr.getAddress(), Alignment(ABIAlign.getQuantity()));
        }
      }

      ti.initialize(IGF, in, addr, isOutlined);

      out.add(addr.getAddress());
      break;
    }
    case clang::CodeGen::ABIArgInfo::CoerceAndExpand: {
      auto &paramTI = cast<LoadableTypeInfo>(IGF.getTypeInfo(paramType));
      emitCoerceAndExpand(IGF, in, out, paramType, paramTI,
                          AI.getCoerceAndExpandType(),
                          AI.getCoerceAndExpandTypeSequence(),
                          TranslationDirection::ToForeign, isOutlined);
      break;
    }
    case clang::CodeGen::ABIArgInfo::Expand:
      emitClangExpandedArgument(
          IGF, in, out, clangParamTy, paramType,
          cast<LoadableTypeInfo>(IGF.getTypeInfo(paramType)), isOutlined);
      break;
    case clang::CodeGen::ABIArgInfo::Ignore:
      break;
    case clang::CodeGen::ABIArgInfo::InAlloca:
      llvm_unreachable("Need to handle InAlloca when externalizing arguments");
      break;
    }
  }
}

/// Returns whether allocas are needed.
bool irgen::addNativeArgument(IRGenFunction &IGF,
                              Explosion &in,
                              CanSILFunctionType fnTy,
                              SILParameterInfo origParamInfo, Explosion &out,
                              bool isOutlined) {
  // Addresses consist of a single pointer argument.
  if (IGF.IGM.silConv.isSILIndirect(origParamInfo)) {
    out.add(in.claimNext());
    return false;
  }
  auto paramType = IGF.IGM.silConv.getSILType(
      origParamInfo, fnTy, IGF.IGM.getMaximalTypeExpansionContext());
  auto &ti = cast<LoadableTypeInfo>(IGF.getTypeInfo(paramType));
  auto schema = ti.getSchema();
  auto &nativeSchema = ti.nativeParameterValueSchema(IGF.IGM);
  if (nativeSchema.requiresIndirect()) {
    // Pass the argument indirectly.
    auto buf = IGF.createAlloca(ti.getStorageType(),
                                ti.getFixedAlignment(), "");
    ti.initialize(IGF, in, buf, isOutlined);
    out.add(buf.getAddress());
    return true;
  } else {
    if (schema.empty()) {
      assert(nativeSchema.empty());
      return false;
    }
    assert(!nativeSchema.empty());

    // Pass the argument explosion directly, mapping into the native swift
    // calling convention.
    Explosion nonNativeParam;
    ti.reexplode(IGF, in, nonNativeParam);
    Explosion nativeParam = nativeSchema.mapIntoNative(
        IGF.IGM, IGF, nonNativeParam, paramType, isOutlined);
    nativeParam.transferInto(out, nativeParam.size());
    return false;
  }
}

/// Emit a direct parameter that was passed under a C-based CC.
static void emitDirectForeignParameter(IRGenFunction &IGF, Explosion &in,
                                       const clang::CodeGen::ABIArgInfo &AI,
                                       Explosion &out, SILType paramType,
                                       const LoadableTypeInfo &paramTI) {
  // The ABI IR types for the entrypoint might differ from the
  // Swift IR types for the body of the function.

  llvm::Type *coercionTy = AI.getCoerceToType();

  ArrayRef<llvm::Type*> expandedTys;
  if (AI.isDirect() && AI.getCanBeFlattened() &&
      isa<llvm::StructType>(coercionTy)) {
    const auto *ST = cast<llvm::StructType>(coercionTy);
    expandedTys = makeArrayRef(ST->element_begin(), ST->getNumElements());
  } else if (coercionTy == paramTI.getStorageType()) {
    // Fast-path a really common case.  This check assumes that either
    // the storage type of a type is an llvm::StructType or it has a
    // single-element explosion.
    out.add(in.claimNext());
    return;
  } else {
    expandedTys = coercionTy;
  }

  auto outputSchema = paramTI.getSchema();

  // Check to see if we can pairwise-coerce Swift's exploded scalars
  // to Clang's expanded elements.
  if (canCoerceToSchema(IGF.IGM, expandedTys, outputSchema)) {
    for (auto &outputElt : outputSchema) {
      llvm::Value *param = in.claimNext();
      llvm::Type *outputTy = outputElt.getScalarType();
      if (param->getType() != outputTy)
        param = IGF.coerceValue(param, outputTy, IGF.IGM.DataLayout);
      out.add(param);
    }
    return;
  }

  // Otherwise, we need to traffic through memory.
  // Create a temporary.
  Address temporary; Size tempSize;
  std::tie(temporary, tempSize) = allocateForCoercion(IGF,
                                          coercionTy,
                                          paramTI.getStorageType(),
                                          "");
  IGF.Builder.CreateLifetimeStart(temporary, tempSize);

  // Write the input parameters into the temporary:
  Address coercedAddr =
    IGF.Builder.CreateBitCast(temporary, coercionTy->getPointerTo());

  // Break down a struct expansion if necessary.
  if (auto expansionTy = dyn_cast<llvm::StructType>(coercionTy)) {
    auto layout = IGF.IGM.DataLayout.getStructLayout(expansionTy);
    for (unsigned i = 0, e = expansionTy->getNumElements(); i != e; ++i) {
      auto fieldOffset = Size(layout->getElementOffset(i));
      auto fieldAddr = IGF.Builder.CreateStructGEP(coercedAddr, i, fieldOffset);
      IGF.Builder.CreateStore(in.claimNext(), fieldAddr);
    }

  // Otherwise, store the single scalar.
  } else {
    IGF.Builder.CreateStore(in.claimNext(), coercedAddr);
  }

  // Pull out the elements.
  temporary = IGF.Builder.CreateBitCast(temporary,
                                      paramTI.getStorageType()->getPointerTo());
  paramTI.loadAsTake(IGF, temporary, out);

  // Deallocate the temporary.
  // `deallocateStack` emits the lifetime.end marker for us.
  paramTI.deallocateStack(IGF, StackAddress(temporary), paramType);
}

void irgen::emitForeignParameter(IRGenFunction &IGF, Explosion &params,
                                 ForeignFunctionInfo foreignInfo,
                                 unsigned foreignParamIndex, SILType paramTy,
                                 const LoadableTypeInfo &paramTI,
                                 Explosion &paramExplosion, bool isOutlined) {
  assert(foreignInfo.ClangInfo);
  auto &FI = *foreignInfo.ClangInfo;

  auto clangArgTy = FI.arg_begin()[foreignParamIndex].type;
  auto AI = FI.arg_begin()[foreignParamIndex].info;

  // We don't need to do anything to handle the Swift parameter-ABI
  // attributes here because we shouldn't be trying to round-trip
  // swiftcall function pointers through SIL as C functions anyway.
  assert(FI.getExtParameterInfo(foreignParamIndex).getABI()
           == clang::ParameterABI::Ordinary);

  // Drop padding arguments.
  if (AI.getPaddingType())
    params.claimNext();

  switch (AI.getKind()) {
  case clang::CodeGen::ABIArgInfo::Extend:
  case clang::CodeGen::ABIArgInfo::Direct:
    emitDirectForeignParameter(IGF, params, AI, paramExplosion, paramTy,
                               paramTI);
    return;
  case clang::CodeGen::ABIArgInfo::Indirect: {
    Address address = paramTI.getAddressForPointer(params.claimNext());
    paramTI.loadAsTake(IGF, address, paramExplosion);
    return;
  }
  case clang::CodeGen::ABIArgInfo::Expand: {
    emitClangExpandedParameter(IGF, params, paramExplosion, clangArgTy,
                               paramTy, paramTI);
    return;
  }
  case clang::CodeGen::ABIArgInfo::CoerceAndExpand: {
    auto &paramTI = cast<LoadableTypeInfo>(IGF.getTypeInfo(paramTy));
    emitCoerceAndExpand(IGF, params, paramExplosion, paramTy, paramTI,
                        AI.getCoerceAndExpandType(),
                        AI.getCoerceAndExpandTypeSequence(),
                        TranslationDirection::ToNative, isOutlined);
    break;
  }

  case clang::CodeGen::ABIArgInfo::Ignore:
    return;

  case clang::CodeGen::ABIArgInfo::InAlloca:
    llvm_unreachable("Need to handle InAlloca during signature expansion");
  }
}

std::pair<PointerAuthSchema, PointerAuthEntity>
irgen::getCoroutineResumeFunctionPointerAuth(IRGenModule &IGM,
                                             CanSILFunctionType fnType) {
  switch (fnType->getCoroutineKind()) {
  case SILCoroutineKind::None:
    llvm_unreachable("not a coroutine");
  case SILCoroutineKind::YieldMany:
    return { IGM.getOptions().PointerAuth.YieldManyResumeFunctions,
             PointerAuthEntity::forYieldTypes(fnType) };
  case SILCoroutineKind::YieldOnce:
    return { IGM.getOptions().PointerAuth.YieldOnceResumeFunctions,
             PointerAuthEntity::forYieldTypes(fnType) };
  }
  llvm_unreachable("bad coroutine kind");
}

static void
emitRetconCoroutineEntry(IRGenFunction &IGF, CanSILFunctionType fnType,
                         NativeCCEntryPointArgumentEmission &emission,
                         llvm::Intrinsic::ID idIntrinsic, Size bufferSize,
                         Alignment bufferAlignment) {
  auto prototype =
    IGF.IGM.getOpaquePtr(IGF.IGM.getAddrOfContinuationPrototype(fnType));

  // Use malloc and free as our allocator.
  auto allocFn = IGF.IGM.getOpaquePtr(IGF.IGM.getMallocFn());
  auto deallocFn = IGF.IGM.getOpaquePtr(IGF.IGM.getFreeFn());

  // Call the right 'llvm.coro.id.retcon' variant.
  llvm::Value *buffer = emission.getCoroutineBuffer();
  llvm::Value *id = IGF.Builder.CreateIntrinsicCall(idIntrinsic, {
    llvm::ConstantInt::get(IGF.IGM.Int32Ty, bufferSize.getValue()),
    llvm::ConstantInt::get(IGF.IGM.Int32Ty, bufferAlignment.getValue()),
    buffer,
    prototype,
    allocFn,
    deallocFn
  });

  // Call 'llvm.coro.begin', just for consistency with the normal pattern.
  // This serves as a handle that we can pass around to other intrinsics.
  auto hdl = IGF.Builder.CreateIntrinsicCall(
      llvm::Intrinsic::coro_begin,
      {id, llvm::ConstantPointerNull::get(IGF.IGM.Int8PtrTy)});

  // Set the coroutine handle; this also flags that is a coroutine so that
  // e.g. dynamic allocas use the right code generation.
  IGF.setCoroutineHandle(hdl);
}

void irgen::emitAsyncFunctionEntry(IRGenFunction &IGF,
                                   SILFunction *asyncFunction) {
  auto &IGM = IGF.IGM;
  auto size = getAsyncContextLayout(IGM, asyncFunction).getSize();
  auto asyncFuncPointer = IGF.Builder.CreateBitOrPointerCast(
      IGM.getAddrOfAsyncFunctionPointer(asyncFunction), IGM.Int8PtrTy);
  auto *id = IGF.Builder.CreateIntrinsicCall(
      llvm::Intrinsic::coro_id_async,
      {llvm::ConstantInt::get(IGM.Int32Ty, size.getValue()),
       llvm::ConstantInt::get(IGM.Int32Ty, 16),
       llvm::ConstantInt::get(IGM.Int32Ty, 2), asyncFuncPointer});
  // Call 'llvm.coro.begin', just for consistency with the normal pattern.
  // This serves as a handle that we can pass around to other intrinsics.
  auto hdl = IGF.Builder.CreateIntrinsicCall(
      llvm::Intrinsic::coro_begin,
      {id, llvm::ConstantPointerNull::get(IGM.Int8PtrTy)});

  // Set the coroutine handle; this also flags that is a coroutine so that
  // e.g. dynamic allocas use the right code generation.
  IGF.setCoroutineHandle(hdl);
}

void irgen::emitYieldOnceCoroutineEntry(
    IRGenFunction &IGF, CanSILFunctionType fnType,
    NativeCCEntryPointArgumentEmission &emission) {
  emitRetconCoroutineEntry(IGF, fnType, emission,
                           llvm::Intrinsic::coro_id_retcon_once,
                           getYieldOnceCoroutineBufferSize(IGF.IGM),
                           getYieldOnceCoroutineBufferAlignment(IGF.IGM));
}

void irgen::emitYieldManyCoroutineEntry(
    IRGenFunction &IGF, CanSILFunctionType fnType,
    NativeCCEntryPointArgumentEmission &emission) {
  emitRetconCoroutineEntry(IGF, fnType, emission,
                           llvm::Intrinsic::coro_id_retcon,
                           getYieldManyCoroutineBufferSize(IGF.IGM),
                           getYieldManyCoroutineBufferAlignment(IGF.IGM));
}

static Address createOpaqueBufferAlloca(IRGenFunction &IGF,
                                        Size size, Alignment align) {
  auto ty = llvm::ArrayType::get(IGF.IGM.Int8Ty, size.getValue());
  auto addr = IGF.createAlloca(ty, align);
  addr = IGF.Builder.CreateStructGEP(addr, 0, Size(0));
  IGF.Builder.CreateLifetimeStart(addr, size);
  return addr;
}

Address irgen::emitAllocYieldOnceCoroutineBuffer(IRGenFunction &IGF) {
  return createOpaqueBufferAlloca(IGF, getYieldOnceCoroutineBufferSize(IGF.IGM),
                                 getYieldOnceCoroutineBufferAlignment(IGF.IGM));
}

Address irgen::emitAllocYieldManyCoroutineBuffer(IRGenFunction &IGF) {
  return createOpaqueBufferAlloca(IGF, getYieldManyCoroutineBufferSize(IGF.IGM),
                                 getYieldManyCoroutineBufferAlignment(IGF.IGM));
}

void irgen::emitDeallocYieldOnceCoroutineBuffer(IRGenFunction &IGF,
                                                Address buffer) {
  auto bufferSize = getYieldOnceCoroutineBufferSize(IGF.IGM);
  IGF.Builder.CreateLifetimeEnd(buffer, bufferSize);
}

void irgen::emitDeallocYieldManyCoroutineBuffer(IRGenFunction &IGF,
                                                Address buffer) {
  auto bufferSize = getYieldManyCoroutineBufferSize(IGF.IGM);
  IGF.Builder.CreateLifetimeEnd(buffer, bufferSize);
}

void irgen::emitTaskCancel(IRGenFunction &IGF, llvm::Value *task) {
  if (task->getType() != IGF.IGM.SwiftTaskPtrTy) {
    task = IGF.Builder.CreateBitCast(task, IGF.IGM.SwiftTaskPtrTy);
  }

  auto *call = IGF.Builder.CreateCall(IGF.IGM.getTaskCancelFn(), {task});
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);
}

llvm::Value *irgen::emitTaskCreate(
    IRGenFunction &IGF, llvm::Value *flags, llvm::Value *parentTask,
    llvm::Value *futureResultType,
    llvm::Value *taskFunction, llvm::Value *localContextInfo,
    SubstitutionMap subs) {
  parentTask = IGF.Builder.CreateBitOrPointerCast(
      parentTask, IGF.IGM.SwiftTaskPtrTy);

  // Determine the size of the async context for the closure.
  ASTContext &ctx = IGF.IGM.IRGen.SIL.getASTContext();
  auto extInfo = ASTExtInfoBuilder().withAsync().withThrows().build();
  AnyFunctionType *taskFunctionType;
  if (futureResultType) {
    auto genericParam = GenericTypeParamType::get(0, 0, ctx);
    auto genericSig = GenericSignature::get({genericParam}, {});
    taskFunctionType = GenericFunctionType::get(
        genericSig, { }, genericParam, extInfo);

    taskFunctionType = Type(taskFunctionType).subst(subs)->castTo<FunctionType>();
  } else {
    taskFunctionType = FunctionType::get(
        { }, ctx.TheEmptyTupleType, extInfo);
  }
  CanSILFunctionType taskFunctionCanSILType =
      IGF.IGM.getLoweredType(taskFunctionType).castTo<SILFunctionType>();
  auto layout = getAsyncContextLayout(
      IGF.IGM, taskFunctionCanSILType, taskFunctionCanSILType, subs);

  CanSILFunctionType taskContinuationFunctionTy = [&]() {
    ASTContext &ctx = IGF.IGM.IRGen.SIL.getASTContext();
    auto extInfo =
        ASTExtInfoBuilder()
            .withRepresentation(FunctionTypeRepresentation::CFunctionPointer)
            .build();
    // FIXME: Use the appropriate signature for TaskContinuationFunction:
    //
    //            using TaskContinuationFunction =
    //              SWIFT_CC(swift)
    //              void (AsyncTask *, ExecutorRef, AsyncContext *);
    auto ty = FunctionType::get({}, ctx.TheEmptyTupleType, extInfo);
    return IGF.IGM.getLoweredType(ty).castTo<SILFunctionType>();
  }();

  // Call the function.
  llvm::CallInst *result;
  llvm::Value *theSize, *theFunction;
  auto taskFunctionPointer = FunctionPointer::forExplosionValue(
      IGF, taskFunction, taskFunctionCanSILType);
  std::tie(theFunction, theSize) =
      getAsyncFunctionAndSize(IGF, SILFunctionTypeRepresentation::Thick,
                              taskFunctionPointer, localContextInfo);
  if (auto authInfo = PointerAuthInfo::forFunctionPointer(
          IGF.IGM, taskContinuationFunctionTy)) {
    theFunction = emitPointerAuthResign(
        IGF, theFunction, taskFunctionPointer.getAuthInfo(), authInfo);
  }
  theFunction = IGF.Builder.CreateBitOrPointerCast(
      theFunction, IGF.IGM.TaskContinuationFunctionPtrTy);
  theSize = IGF.Builder.CreateZExtOrBitCast(theSize, IGF.IGM.SizeTy);
  if (futureResultType) {
    result = IGF.Builder.CreateCall(
      IGF.IGM.getTaskCreateFutureFuncFn(),
      { flags, parentTask, futureResultType, theFunction, theSize });
  } else {
    result = IGF.Builder.CreateCall(IGF.IGM.getTaskCreateFuncFn(),
                                    {flags, parentTask, theFunction, theSize});
  }
  result->setDoesNotThrow();
  result->setCallingConv(IGF.IGM.SwiftCC);

  // Write the local context information into the initial context for the task.
  assert(layout.hasLocalContext());
  // Dig out the initial context returned from task creation.
  auto initialContext = IGF.Builder.CreateExtractValue(result, {1});
  Address initialContextAddr = layout.emitCastTo(IGF, initialContext);

  auto localContextLayout = layout.getLocalContextLayout();
  auto localContextAddr =
      localContextLayout.project(IGF, initialContextAddr, llvm::None);
  IGF.Builder.CreateStore(localContextInfo, localContextAddr);

  return result;
}

Address irgen::emitAllocAsyncContext(IRGenFunction &IGF,
                                     llvm::Value *sizeValue) {
  auto alignment = getAsyncContextAlignment(IGF.IGM);
  auto address = IGF.emitTaskAlloc(sizeValue, alignment);
  IGF.Builder.CreateLifetimeStart(address, Size(-1) /*dynamic size*/);
  return address;
}

void irgen::emitDeallocAsyncContext(IRGenFunction &IGF, Address context) {
  IGF.emitTaskDealloc(context);
  IGF.Builder.CreateLifetimeEnd(context, Size(-1) /*dynamic size*/);
}

llvm::Value *irgen::emitYield(IRGenFunction &IGF,
                              CanSILFunctionType coroutineType,
                              Explosion &substValues) {
  // TODO: Handle async!
  auto coroSignature = IGF.IGM.getSignature(coroutineType);
  auto coroInfo = coroSignature.getCoroutineInfo();

  // Translate the arguments to an unsubstituted form.
  Explosion allComponents;
  for (auto yield : coroutineType->getYields())
    addNativeArgument(IGF, substValues, coroutineType,
                      yield, allComponents, false);

  // Figure out which arguments need to be yielded directly.
  SmallVector<llvm::Value*, 8> yieldArgs;

  // Add the direct yield components.
  auto directComponents =
    allComponents.claim(coroInfo.NumDirectYieldComponents);
  yieldArgs.append(directComponents.begin(), directComponents.end());

  // The rest need to go into an indirect buffer.
  auto indirectComponents = allComponents.claimAll();

  auto resultStructTy =
    dyn_cast<llvm::StructType>(coroSignature.getType()->getReturnType());
  assert((!resultStructTy
             && directComponents.empty()
             && indirectComponents.empty())
         || (resultStructTy
             && resultStructTy->getNumElements() ==
                  (1 + directComponents.size()
                     + unsigned(!indirectComponents.empty()))));

  // Fill in the indirect buffer if necessary.
  Optional<Address> indirectBuffer;
  Size indirectBufferSize;
  if (!indirectComponents.empty()) {
    auto bufferStructTy = cast<llvm::StructType>(
      resultStructTy->getElementType(resultStructTy->getNumElements() - 1)
                    ->getPointerElementType());
    auto layout = IGF.IGM.DataLayout.getStructLayout(bufferStructTy);
    indirectBuffer = IGF.createAlloca(
        bufferStructTy, Alignment(layout->getAlignment().value()));
    indirectBufferSize = Size(layout->getSizeInBytes());
    IGF.Builder.CreateLifetimeStart(*indirectBuffer, indirectBufferSize);

    for (size_t i : indices(bufferStructTy->elements())) {
      // Skip padding elements.
      if (bufferStructTy->getElementType(i)->isArrayTy())
        continue;

      assert(!indirectComponents.empty() &&
             "insufficient number of indirect yield components");

      auto addr = IGF.Builder.CreateStructGEP(*indirectBuffer, i, layout);
      IGF.Builder.CreateStore(indirectComponents.front(), addr);
      indirectComponents = indirectComponents.drop_front();
    }

    assert(indirectComponents.empty() && "too many indirect yield components");

    // Remember to yield the indirect buffer.
    yieldArgs.push_back(indirectBuffer->getAddress());
  }

  // Perform the yield.
  auto isUnwind = IGF.Builder.CreateIntrinsicCall(
      llvm::Intrinsic::coro_suspend_retcon, {IGF.IGM.Int1Ty}, yieldArgs);

  // We're done with the indirect buffer.
  if (indirectBuffer) {
    IGF.Builder.CreateLifetimeEnd(*indirectBuffer, indirectBufferSize);
  }

  return isUnwind;
}

/// Add a new set of arguments to the function.
void CallEmission::setArgs(Explosion &adjusted, bool isOutlined,
                           WitnessMetadata *witnessMetadata) {
  assert(state == State::Emitting);
  // Add the given number of arguments.
  assert(LastArgWritten >= adjusted.size());

  size_t targetIndex = LastArgWritten - adjusted.size();
  assert(targetIndex <= 1);
  LastArgWritten = targetIndex;

  auto argIterator = Args.begin() + targetIndex;
  for (auto value : adjusted.claimAll()) {
    *argIterator++ = value;
  }
}

void CallEmission::addAttribute(unsigned index,
                                llvm::Attribute::AttrKind attr) {
  assert(state == State::Emitting);
  auto &attrs = CurCallee.getMutableAttributes();
  attrs = attrs.addAttribute(IGF.IGM.getLLVMContext(), index, attr);
}

/// Initialize an Explosion with the parameters of the current
/// function.  All of the objects will be added unmanaged.  This is
/// really only useful when writing prologue code.
Explosion IRGenFunction::collectParameters() {
  Explosion params;
  for (auto i = CurFn->arg_begin(), e = CurFn->arg_end(); i != e; ++i)
    params.add(&*i);
  return params;
}

/// Fetch the error result slot.
Address IRGenFunction::getCalleeErrorResultSlot(SILType errorType) {
  if (!CalleeErrorResultSlot) {
    auto &errorTI = cast<FixedTypeInfo>(getTypeInfo(errorType));

    IRBuilder builder(IGM.getLLVMContext(), IGM.DebugInfo != nullptr);
    builder.SetInsertPoint(AllocaIP->getParent(), AllocaIP->getIterator());

    // Create the alloca.  We don't use allocateStack because we're
    // not allocating this in stack order.
    auto addr = createAlloca(errorTI.getStorageType(),
                             errorTI.getFixedAlignment(),
                             "swifterror");

    // Only add the swifterror attribute on ABIs that pass it in a register.
    // We create a shadow stack location of the swifterror parameter for the
    // debugger on platforms that pass swifterror by reference and so we can't
    // mark the parameter with a swifterror attribute for these.
    if (IGM.IsSwiftErrorInRegister)
      cast<llvm::AllocaInst>(addr.getAddress())->setSwiftError(true);

    // Initialize at the alloca point.
    auto nullError = llvm::ConstantPointerNull::get(
                            cast<llvm::PointerType>(errorTI.getStorageType()));
    builder.CreateStore(nullError, addr);

    CalleeErrorResultSlot = addr.getAddress();
  }
  return Address(CalleeErrorResultSlot, IGM.getPointerAlignment());
}

/// Fetch the error result slot received from the caller.
Address IRGenFunction::getCallerErrorResultSlot() {
  assert(CallerErrorResultSlot && "no error result slot!");
  assert(isa<llvm::Argument>(CallerErrorResultSlot) && !isAsync() ||
         isa<llvm::GetElementPtrInst>(CallerErrorResultSlot) && isAsync() &&
             "error result slot is local!");
  return Address(CallerErrorResultSlot, IGM.getPointerAlignment());
}

// Set the error result slot.  This should only be done in the prologue.
void IRGenFunction::setCallerErrorResultSlot(llvm::Value *address) {
  assert(!CallerErrorResultSlot && "already have a caller error result slot!");
  assert(isa<llvm::PointerType>(address->getType()));
  CallerErrorResultSlot = address;
  if (!isAsync()) {
    CalleeErrorResultSlot = address;
  }
}

/// Emit the basic block that 'return' should branch to and insert it into
/// the current function. This creates a second
/// insertion point that most blocks should be inserted before.
void IRGenFunction::emitBBForReturn() {
  ReturnBB = createBasicBlock("return");
  CurFn->getBasicBlockList().push_back(ReturnBB);
}

/// Emit the prologue for the function.
void IRGenFunction::emitPrologue() {
  // Set up the IRBuilder.
  llvm::BasicBlock *EntryBB = createBasicBlock("entry");
  assert(CurFn->getBasicBlockList().empty() && "prologue already emitted?");
  CurFn->getBasicBlockList().push_back(EntryBB);
  Builder.SetInsertPoint(EntryBB);

  // Set up the alloca insertion point.
  AllocaIP = Builder.IRBuilderBase::CreateAlloca(IGM.Int1Ty,
                                                 /*array size*/ nullptr,
                                                 "alloca point");
}

/// Emit a branch to the return block and set the insert point there.
/// Returns true if the return block is reachable, false otherwise.
bool IRGenFunction::emitBranchToReturnBB() {
  // If there are no edges to the return block, we never want to emit it.
  if (ReturnBB->use_empty()) {
    ReturnBB->eraseFromParent();
    
    // Normally this means that we'll just insert the epilogue in the
    // current block, but if the current IP is unreachable then so is
    // the entire epilogue.
    if (!Builder.hasValidIP())
      return false;
    
    // Otherwise, branch to it if the current IP is reachable.
  } else if (Builder.hasValidIP()) {
    Builder.CreateBr(ReturnBB);
    Builder.SetInsertPoint(ReturnBB);
    
    // Otherwise, if there is exactly one use of the return block, merge
    // it into its predecessor.
  } else if (ReturnBB->hasOneUse()) {
    // return statements are never emitted as conditional branches.
    llvm::BranchInst *Br = cast<llvm::BranchInst>(*ReturnBB->use_begin());
    assert(Br->isUnconditional());
    Builder.SetInsertPoint(Br->getParent());
    Br->eraseFromParent();
    ReturnBB->eraseFromParent();
    
    // Otherwise, just move the IP to the return block.
  } else {
    Builder.SetInsertPoint(ReturnBB);
  }
  return true;
}

/// Emit the epilogue for the function.
void IRGenFunction::emitEpilogue() {
  // Destroy the alloca insertion point.
  AllocaIP->eraseFromParent();
}

std::pair<Address, Size>
irgen::allocateForCoercion(IRGenFunction &IGF,
                           llvm::Type *fromTy,
                           llvm::Type *toTy,
                           const llvm::Twine &basename) {
  auto &DL = IGF.IGM.DataLayout;
  
  auto fromSize = DL.getTypeSizeInBits(fromTy);
  auto toSize = DL.getTypeSizeInBits(toTy);
  auto bufferTy = fromSize >= toSize
    ? fromTy
    : toTy;

  auto alignment = std::max(DL.getABITypeAlignment(fromTy),
                            DL.getABITypeAlignment(toTy));

  auto buffer = IGF.createAlloca(bufferTy, Alignment(alignment),
                                 basename + ".coerced");
  
  Size size(std::max(fromSize, toSize));
  return {buffer, size};
}

llvm::Value* IRGenFunction::coerceValue(llvm::Value *value, llvm::Type *toTy,
                                        const llvm::DataLayout &DL)
{
  llvm::Type *fromTy = value->getType();
  assert(fromTy != toTy && "Unexpected same types in type coercion!");
  assert(!fromTy->isVoidTy()
         && "Unexpected void source type in type coercion!");
  assert(!toTy->isVoidTy()
         && "Unexpected void destination type in type coercion!");

  // Use the pointer/pointer and pointer/int casts if we can.
  if (toTy->isPointerTy()) {
    if (fromTy->isPointerTy())
      return Builder.CreateBitCast(value, toTy);
    if (fromTy == IGM.IntPtrTy)
      return Builder.CreateIntToPtr(value, toTy);
  } else if (fromTy->isPointerTy()) {
    if (toTy == IGM.IntPtrTy) {
      return Builder.CreatePtrToInt(value, toTy);
    }
  }

  // Otherwise we need to store, bitcast, and load.
  Address address; Size size;
  std::tie(address, size) = allocateForCoercion(*this, fromTy, toTy,
                                                value->getName() + ".coercion");
  Builder.CreateLifetimeStart(address, size);
  auto orig = Builder.CreateBitCast(address, fromTy->getPointerTo());
  Builder.CreateStore(value, orig);
  auto coerced = Builder.CreateBitCast(address, toTy->getPointerTo());
  auto loaded = Builder.CreateLoad(coerced);
  Builder.CreateLifetimeEnd(address, size);
  return loaded;
}

void IRGenFunction::emitScalarReturn(llvm::Type *resultType,
                                     Explosion &result) {
  if (result.empty()) {
    Builder.CreateRetVoid();
    return;
  }

  auto *ABIType = CurFn->getReturnType();

  if (result.size() == 1) {
    auto *returned = result.claimNext();
    if (ABIType != returned->getType())
      returned = coerceValue(returned, ABIType, IGM.DataLayout);

    Builder.CreateRet(returned);
    return;
  }

  // Multiple return values are returned as a struct.
  assert(cast<llvm::StructType>(resultType)->getNumElements() == result.size());
  llvm::Value *resultAgg = llvm::UndefValue::get(resultType);
  for (unsigned i = 0, e = result.size(); i != e; ++i) {
    llvm::Value *elt = result.claimNext();
    resultAgg = Builder.CreateInsertValue(resultAgg, elt, i);
  }

  if (ABIType != resultType)
    resultAgg = coerceValue(resultAgg, ABIType, IGM.DataLayout);

  Builder.CreateRet(resultAgg);
}

/// Adjust the alignment of the alloca pointed to by \p allocaAddr to the
/// required alignment of the struct \p type.
static void adjustAllocaAlignment(const llvm::DataLayout &DL,
                                  Address allocaAddr, llvm::StructType *type) {
  auto layout = DL.getStructLayout(type);
  Alignment layoutAlignment = Alignment(layout->getAlignment().value());
  auto alloca = cast<llvm::AllocaInst>(allocaAddr.getAddress());
  if (alloca->getAlignment() < layoutAlignment.getValue()) {
    alloca->setAlignment(
        llvm::MaybeAlign(layoutAlignment.getValue()).valueOrOne());
    allocaAddr = Address(allocaAddr.getAddress(), layoutAlignment);
  }
}

unsigned NativeConventionSchema::size() const {
  if (empty())
    return 0;
  unsigned size = 0;
  Lowering.enumerateComponents([&](clang::CharUnits offset,
                                   clang::CharUnits end,
                                   llvm::Type *type) { ++size; });
  return size;
}

static bool canMatchByTruncation(IRGenModule &IGM,
                                 ArrayRef<llvm::Type*> expandedTys,
                                 const ExplosionSchema &schema) {
  // If the schemas don't even match in number, we have to go
  // through memory.
  if (expandedTys.size() != schema.size() || expandedTys.empty())
    return false;

  if (expandedTys.size() == 1) return false;

  // If there are multiple elements, the pairs of types need to
  // match in size upto the penultimate for the truncation to work.
  size_t e = expandedTys.size();
  for (size_t i = 0; i != e - 1; ++i) {
    // Check that we can truncate the last element.
    llvm::Type *outputTy = schema[i].getScalarType();
    llvm::Type *inputTy = expandedTys[i];
    if (inputTy != outputTy &&
        IGM.DataLayout.getTypeSizeInBits(inputTy) !=
        IGM.DataLayout.getTypeSizeInBits(outputTy))
      return false;
  }
  llvm::Type *outputTy = schema[e-1].getScalarType();
  llvm::Type *inputTy = expandedTys[e-1];
  return inputTy == outputTy || (IGM.DataLayout.getTypeSizeInBits(inputTy) ==
                                 IGM.DataLayout.getTypeSizeInBits(outputTy)) ||
         (IGM.DataLayout.getTypeSizeInBits(inputTy) >
              IGM.DataLayout.getTypeSizeInBits(outputTy) &&
          isa<llvm::IntegerType>(inputTy) && isa<llvm::IntegerType>(outputTy));
}

Explosion NativeConventionSchema::mapFromNative(IRGenModule &IGM,
                                                IRGenFunction &IGF,
                                                Explosion &native,
                                                SILType type) const {
  if (native.empty()) {
    assert(empty() && "Empty explosion must match the native convention");
    return Explosion();
  }

  assert(!empty());

  auto *nativeTy = getExpandedType(IGM);
  auto expandedTys = expandScalarOrStructTypeToArray(nativeTy);
  auto &TI = IGM.getTypeInfo(type);
  auto schema = TI.getSchema();
  // The expected explosion type.
  auto *explosionTy = schema.getScalarResultType(IGM);

  // Check whether we can coerce the explosion to the expected type convention.
  auto &DataLayout = IGM.DataLayout;
  Explosion nonNativeExplosion;
  if (canCoerceToSchema(IGM, expandedTys, schema)) {
    if (native.size() == 1) {
      auto *elt = native.claimNext();
      if (explosionTy != elt->getType()) {
        if (isa<llvm::IntegerType>(explosionTy) &&
            isa<llvm::IntegerType>(elt->getType())) {
          elt = IGF.Builder.CreateTrunc(elt, explosionTy);
        } else {
          elt = IGF.coerceValue(elt, explosionTy, DataLayout);
        }
      }
      nonNativeExplosion.add(elt);
      return nonNativeExplosion;
    } else if (nativeTy == explosionTy) {
      native.transferInto(nonNativeExplosion, native.size());
      return nonNativeExplosion;
    }
    // Otherwise, we have to go through memory if we can match by truncation.
  } else if (canMatchByTruncation(IGM, expandedTys, schema)) {
    assert(expandedTys.size() == schema.size());
    for (size_t i = 0, e = expandedTys.size(); i != e; ++i) {
      auto *elt = native.claimNext();
      auto *schemaTy = schema[i].getScalarType();
      auto *nativeTy = elt->getType();
      assert(nativeTy == expandedTys[i]);
      if (schemaTy == nativeTy) {
        // elt = elt
      } else if (DataLayout.getTypeSizeInBits(schemaTy) ==
                 DataLayout.getTypeSizeInBits(nativeTy))
        elt = IGF.coerceValue(elt, schemaTy, DataLayout);
      else {
        assert(DataLayout.getTypeSizeInBits(schemaTy) <
               DataLayout.getTypeSizeInBits(nativeTy));
        elt = IGF.Builder.CreateTrunc(elt, schemaTy);
      }
      nonNativeExplosion.add(elt);
    }
    return nonNativeExplosion;
  }

  // If not, go through memory.
  auto &loadableTI = cast<LoadableTypeInfo>(TI);

  // We can get two layouts if there are overlapping ranges in the legal type
  // sequence.
  llvm::StructType *coercionTy, *overlappedCoercionTy;
  SmallVector<unsigned, 8> expandedTyIndicesMap;
  std::tie(coercionTy, overlappedCoercionTy) =
      getCoercionTypes(IGM, expandedTyIndicesMap);

  // Get the larger layout out of those two.
  auto coercionSize = DataLayout.getTypeSizeInBits(coercionTy);
  auto overlappedCoercionSize =
      DataLayout.getTypeSizeInBits(overlappedCoercionTy);
  llvm::StructType *largerCoercion = coercionSize >= overlappedCoercionSize
                                         ? coercionTy
                                         : overlappedCoercionTy;

  // Allocate a temporary for the coercion.
  Address temporary;
  Size tempSize;
  std::tie(temporary, tempSize) = allocateForCoercion(
      IGF, largerCoercion, loadableTI.getStorageType(), "temp-coercion");

  // Make sure we have sufficiently large alignment.
  adjustAllocaAlignment(DataLayout, temporary, coercionTy);
  adjustAllocaAlignment(DataLayout, temporary, overlappedCoercionTy);

  auto &Builder = IGF.Builder;
  Builder.CreateLifetimeStart(temporary, tempSize);

  // Store the expanded type elements.
  auto coercionAddr = Builder.CreateElementBitCast(temporary, coercionTy);
  unsigned expandedMapIdx = 0;

  auto eltsArray = native.claimAll();
  SmallVector<llvm::Value *, 8> nativeElts(eltsArray.begin(), eltsArray.end());
  auto storeToFn = [&](llvm::StructType *ty, Address structAddr) {
    for (auto eltIndex : indices(ty->elements())) {
      auto layout = DataLayout.getStructLayout(ty);
      auto eltTy = ty->getElementType(eltIndex);
      // Skip padding fields.
      if (eltTy->isArrayTy())
        continue;
      Address eltAddr = Builder.CreateStructGEP(structAddr, eltIndex, layout);
      auto index = expandedTyIndicesMap[expandedMapIdx];
      assert(index < nativeElts.size() && nativeElts[index] != nullptr);
      auto nativeElt = nativeElts[index];
      Builder.CreateStore(nativeElt, eltAddr);
      nativeElts[index] = nullptr;
      ++expandedMapIdx;
    }
  };

  storeToFn(coercionTy, coercionAddr);
  if (!overlappedCoercionTy->isEmptyTy()) {
    auto overlappedCoercionAddr =
        Builder.CreateElementBitCast(temporary, overlappedCoercionTy);
    storeToFn(overlappedCoercionTy, overlappedCoercionAddr);
  }

  // Reload according to the types schema.
  Address storageAddr = Builder.CreateBitCast(
      temporary, loadableTI.getStorageType()->getPointerTo());
  loadableTI.loadAsTake(IGF, storageAddr, nonNativeExplosion);

  Builder.CreateLifetimeEnd(temporary, tempSize);

  return nonNativeExplosion;
}

Explosion NativeConventionSchema::mapIntoNative(IRGenModule &IGM,
                                                IRGenFunction &IGF,
                                                Explosion &fromNonNative,
                                                SILType type,
                                                bool isOutlined) const {
  if (fromNonNative.empty()) {
    assert(empty() && "Empty explosion must match the native convention");
    return Explosion();
  }

  assert(!requiresIndirect() && "Expected direct convention");
  assert(!empty());

  auto *nativeTy = getExpandedType(IGM);
  auto expandedTys = expandScalarOrStructTypeToArray(nativeTy);
  auto &TI = IGM.getTypeInfo(type);
  auto schema = TI.getSchema();
  auto *explosionTy = schema.getScalarResultType(IGM);

  // Check whether we can coerce the explosion to the expected type convention.
  auto &DataLayout = IGM.DataLayout;
  Explosion nativeExplosion;
  if (canCoerceToSchema(IGM, expandedTys, schema)) {
    if (fromNonNative.size() == 1) {
      auto *elt = fromNonNative.claimNext();
      if (nativeTy != elt->getType()) {
        if (isa<llvm::IntegerType>(nativeTy) &&
            isa<llvm::IntegerType>(elt->getType()))
          elt = IGF.Builder.CreateZExt(elt, nativeTy);
        else
          elt = IGF.coerceValue(elt, nativeTy, DataLayout);
      }
      nativeExplosion.add(elt);
      return nativeExplosion;
    } else if (nativeTy == explosionTy) {
      fromNonNative.transferInto(nativeExplosion, fromNonNative.size());
      return nativeExplosion;
    }
    // Otherwise, we have to go through memory if we can't match by truncation.
  } else if (canMatchByTruncation(IGM, expandedTys, schema)) {
    assert(expandedTys.size() == schema.size());
    for (size_t i = 0, e = expandedTys.size(); i != e; ++i) {
      auto *elt = fromNonNative.claimNext();
      auto *schemaTy = elt->getType();
      auto *nativeTy = expandedTys[i];
      assert(schema[i].getScalarType() == schemaTy);
      if (schemaTy == nativeTy) {
        // elt = elt
      } else if (DataLayout.getTypeSizeInBits(schemaTy) ==
                 DataLayout.getTypeSizeInBits(nativeTy))
        elt = IGF.coerceValue(elt, nativeTy, DataLayout);
      else {
        assert(DataLayout.getTypeSizeInBits(schemaTy) <
               DataLayout.getTypeSizeInBits(nativeTy));
        elt = IGF.Builder.CreateZExt(elt, nativeTy);
      }
      nativeExplosion.add(elt);
    }
    return nativeExplosion;
  }

  // If not, go through memory.
  auto &loadableTI = cast<LoadableTypeInfo>(TI);

  // We can get two layouts if there are overlapping ranges in the legal type
  // sequence.
  llvm::StructType *coercionTy, *overlappedCoercionTy;
  SmallVector<unsigned, 8> expandedTyIndicesMap;
  std::tie(coercionTy, overlappedCoercionTy) =
      getCoercionTypes(IGM, expandedTyIndicesMap);

  // Get the larger layout out of those two.
  auto coercionSize = DataLayout.getTypeSizeInBits(coercionTy);
  auto overlappedCoercionSize =
      DataLayout.getTypeSizeInBits(overlappedCoercionTy);
  llvm::StructType *largerCoercion = coercionSize >= overlappedCoercionSize
                                         ? coercionTy
                                         : overlappedCoercionTy;

  // Allocate a temporary for the coercion.
  Address temporary;
  Size tempSize;
  std::tie(temporary, tempSize) = allocateForCoercion(
      IGF, largerCoercion, loadableTI.getStorageType(), "temp-coercion");

  // Make sure we have sufficiently large alignment.
  adjustAllocaAlignment(DataLayout, temporary, coercionTy);
  adjustAllocaAlignment(DataLayout, temporary, overlappedCoercionTy);

  auto &Builder = IGF.Builder;
  Builder.CreateLifetimeStart(temporary, tempSize);

  // Initialize the memory of the temporary.
  Address storageAddr = Builder.CreateBitCast(
      temporary, loadableTI.getStorageType()->getPointerTo());
  loadableTI.initialize(IGF, fromNonNative, storageAddr, isOutlined);

  // Load the expanded type elements from memory.
  auto coercionAddr = Builder.CreateElementBitCast(temporary, coercionTy);

  unsigned expandedMapIdx = 0;
  SmallVector<llvm::Value *, 8> expandedElts(expandedTys.size(), nullptr);

  auto loadFromFn = [&](llvm::StructType *ty, Address structAddr) {
    for (auto eltIndex : indices(ty->elements())) {
      auto layout = DataLayout.getStructLayout(ty);
      auto eltTy = ty->getElementType(eltIndex);
      // Skip padding fields.
      if (eltTy->isArrayTy())
        continue;
      Address eltAddr = Builder.CreateStructGEP(structAddr, eltIndex, layout);
      llvm::Value *elt = Builder.CreateLoad(eltAddr);
      auto index = expandedTyIndicesMap[expandedMapIdx];
      assert(expandedElts[index] == nullptr);
      expandedElts[index] = elt;
      ++expandedMapIdx;
    }
  };

  loadFromFn(coercionTy, coercionAddr);
  if (!overlappedCoercionTy->isEmptyTy()) {
    auto overlappedCoercionAddr =
        Builder.CreateElementBitCast(temporary, overlappedCoercionTy);
    loadFromFn(overlappedCoercionTy, overlappedCoercionAddr);
  }

  Builder.CreateLifetimeEnd(temporary, tempSize);

  // Add the values to the explosion.
  for (auto *val : expandedElts)
    nativeExplosion.add(val);

  assert(expandedTys.size() == nativeExplosion.size());
  return nativeExplosion;
}

Explosion IRGenFunction::coerceValueTo(SILType fromTy, Explosion &from,
                                       SILType toTy) {
  if (fromTy == toTy)
    return std::move(from);

  auto &fromTI = cast<LoadableTypeInfo>(IGM.getTypeInfo(fromTy));
  auto &toTI = cast<LoadableTypeInfo>(IGM.getTypeInfo(toTy));

  Explosion result;
  if (fromTI.getStorageType()->isPointerTy() &&
      toTI.getStorageType()->isPointerTy()) {
    auto ptr = from.claimNext();
    ptr = Builder.CreateBitCast(ptr, toTI.getStorageType());
    result.add(ptr);
    return result;
  }

  auto temporary = toTI.allocateStack(*this, toTy, "coerce.temp");

  auto addr =
      Address(Builder.CreateBitCast(temporary.getAddressPointer(),
                                    fromTI.getStorageType()->getPointerTo()),
              temporary.getAlignment());
  fromTI.initialize(*this, from, addr, false);

  toTI.loadAsTake(*this, temporary.getAddress(), result);
  toTI.deallocateStack(*this, temporary, toTy);
  return result;
}

void IRGenFunction::emitScalarReturn(SILType returnResultType,
                                     SILType funcResultType, Explosion &result,
                                     bool isSwiftCCReturn, bool isOutlined) {
  if (result.empty()) {
    assert(IGM.getTypeInfo(returnResultType)
               .nativeReturnValueSchema(IGM)
               .empty() &&
           "Empty explosion must match the native calling convention");

    Builder.CreateRetVoid();
    return;
  }

  // In the native case no coercion is needed.
  if (isSwiftCCReturn) {
    result = coerceValueTo(returnResultType, result, funcResultType);
    auto &nativeSchema =
        IGM.getTypeInfo(funcResultType).nativeReturnValueSchema(IGM);
    assert(!nativeSchema.requiresIndirect());

    Explosion native = nativeSchema.mapIntoNative(IGM, *this, result,
                                                  funcResultType, isOutlined);
    if (native.size() == 1) {
      Builder.CreateRet(native.claimNext());
      return;
    }
    llvm::Value *nativeAgg =
        llvm::UndefValue::get(nativeSchema.getExpandedType(IGM));
    for (unsigned i = 0, e = native.size(); i != e; ++i) {
      llvm::Value *elt = native.claimNext();
      nativeAgg = Builder.CreateInsertValue(nativeAgg, elt, i);
    }
    Builder.CreateRet(nativeAgg);
    return;
  }

  // Otherwise we potentially need to coerce the type. We don't need to go
  // through the mapping to the native calling convention.
  auto *ABIType = CurFn->getReturnType();
  if (result.size() == 1) {
    auto *returned = result.claimNext();
    if (ABIType != returned->getType())
      returned = coerceValue(returned, ABIType, IGM.DataLayout);

    Builder.CreateRet(returned);
    return;
  }

  auto &resultTI = IGM.getTypeInfo(returnResultType);
  auto schema = resultTI.getSchema();
  auto *bodyType = schema.getScalarResultType(IGM);

  // Multiple return values are returned as a struct.
  assert(cast<llvm::StructType>(bodyType)->getNumElements() == result.size());
  llvm::Value *resultAgg = llvm::UndefValue::get(bodyType);
  for (unsigned i = 0, e = result.size(); i != e; ++i) {
    llvm::Value *elt = result.claimNext();
    resultAgg = Builder.CreateInsertValue(resultAgg, elt, i);
  }

  if (ABIType != bodyType)
    resultAgg = coerceValue(resultAgg, ABIType, IGM.DataLayout);

  Builder.CreateRet(resultAgg);
}

/// Modify the given variable to hold a pointer whose type is the
/// LLVM lowering of the given function type, and return the signature
/// for the type.
Signature irgen::emitCastOfFunctionPointer(IRGenFunction &IGF,
                                           llvm::Value *&fnPtr,
                                           CanSILFunctionType fnType) {
  // Figure out the function type.
  auto sig = IGF.IGM.getSignature(fnType);

  // Emit the cast.
  fnPtr = IGF.Builder.CreateBitCast(fnPtr, sig.getType()->getPointerTo());

  // Return the information.
  return sig;
}

Callee irgen::getBlockPointerCallee(IRGenFunction &IGF,
                                    llvm::Value *blockPtr,
                                    CalleeInfo &&info) {
  // Grab the block pointer and make it the first physical argument.
  llvm::PointerType *blockPtrTy = IGF.IGM.ObjCBlockPtrTy;
  auto castBlockPtr = IGF.Builder.CreateBitCast(blockPtr, blockPtrTy);

  // Extract the invocation pointer for blocks.
  auto blockStructTy = blockPtrTy->getElementType();
  llvm::Value *invokeFnPtrPtr =
    IGF.Builder.CreateStructGEP(blockStructTy, castBlockPtr, 3);
  Address invokeFnPtrAddr(invokeFnPtrPtr, IGF.IGM.getPointerAlignment());
  llvm::Value *invokeFnPtr = IGF.Builder.CreateLoad(invokeFnPtrAddr);

  auto sig = emitCastOfFunctionPointer(IGF, invokeFnPtr, info.OrigFnType);

  auto &schema = IGF.getOptions().PointerAuth.BlockInvocationFunctionPointers;
  auto authInfo = PointerAuthInfo::emit(IGF, schema,
                                        invokeFnPtrAddr.getAddress(),
                                        info.OrigFnType);

  FunctionPointer fn(FunctionPointer::KindTy::Function, invokeFnPtr, authInfo,
                     sig);

  return Callee(std::move(info), fn, blockPtr);
}

Callee irgen::getSwiftFunctionPointerCallee(
    IRGenFunction &IGF, llvm::Value *fnPtr, llvm::Value *dataPtr,
    CalleeInfo &&calleeInfo, bool castOpaqueToRefcountedContext) {
  auto sig = emitCastOfFunctionPointer(IGF, fnPtr, calleeInfo.OrigFnType);
  auto authInfo =
    PointerAuthInfo::forFunctionPointer(IGF.IGM, calleeInfo.OrigFnType);

  FunctionPointer fn(calleeInfo.OrigFnType, fnPtr, authInfo, sig);
  if (castOpaqueToRefcountedContext) {
    assert(dataPtr && dataPtr->getType() == IGF.IGM.OpaquePtrTy &&
           "Expecting trivial closure context");
    dataPtr = IGF.Builder.CreateBitCast(dataPtr, IGF.IGM.RefCountedPtrTy);
  }
  return Callee(std::move(calleeInfo), fn, dataPtr);
}

Callee irgen::getCFunctionPointerCallee(IRGenFunction &IGF,
                                        llvm::Value *fnPtr,
                                        CalleeInfo &&calleeInfo) {
  auto sig = emitCastOfFunctionPointer(IGF, fnPtr, calleeInfo.OrigFnType);
  auto authInfo =
    PointerAuthInfo::forFunctionPointer(IGF.IGM, calleeInfo.OrigFnType);

  FunctionPointer fn(FunctionPointer::KindTy::Function, fnPtr, authInfo, sig);

  return Callee(std::move(calleeInfo), fn);
}

FunctionPointer FunctionPointer::forDirect(IRGenModule &IGM,
                                           llvm::Constant *fnPtr,
                                           CanSILFunctionType fnType) {
  return forDirect(fnType, fnPtr, IGM.getSignature(fnType));
}

StringRef FunctionPointer::getName(IRGenModule &IGM) const {
  assert(isConstant());
  switch (Kind.value) {
  case KindTy::Value::Function:
    return getRawPointer()->getName();
  case KindTy::Value::AsyncFunctionPointer:
    return IGM
        .getSILFunctionForAsyncFunctionPointer(
            cast<llvm::Constant>(getDirectPointer()->getOperand(0)))
        ->getName();
  }
}

llvm::Value *FunctionPointer::getPointer(IRGenFunction &IGF) const {
  switch (Kind.value) {
  case KindTy::Value::Function:
    return Value;
  case KindTy::Value::AsyncFunctionPointer: {
    if (!isFunctionPointerWithoutContext) {
      auto *fnPtr = Value;
      if (auto authInfo = AuthInfo) {
        fnPtr = emitPointerAuthAuth(IGF, fnPtr, authInfo);
      }
      auto *descriptorPtr =
          IGF.Builder.CreateBitCast(fnPtr, IGF.IGM.AsyncFunctionPointerPtrTy);
      auto *addrPtr = IGF.Builder.CreateStructGEP(descriptorPtr, 0);
      auto *result = IGF.emitLoadOfRelativePointer(
          Address(addrPtr, IGF.IGM.getPointerAlignment()), /*isFar*/ false,
          /*expectedType*/ getFunctionType()->getPointerTo());
      if (auto authInfo = AuthInfo) {
        result = emitPointerAuthSign(IGF, result, authInfo);
      }
      return result;
    } else {
      return IGF.Builder.CreateBitOrPointerCast(
          Value, getFunctionType()->getPointerTo());
    }
  }
  }
}

FunctionPointer FunctionPointer::forExplosionValue(IRGenFunction &IGF,
                                                   llvm::Value *fnPtr,
                                                   CanSILFunctionType fnType) {
  // Bitcast out of an opaque pointer type.
  assert(fnPtr->getType() == IGF.IGM.Int8PtrTy);
  auto sig = emitCastOfFunctionPointer(IGF, fnPtr, fnType);
  auto authInfo = PointerAuthInfo::forFunctionPointer(IGF.IGM, fnType);

  return FunctionPointer(fnType, fnPtr, authInfo, sig);
}

llvm::Value *
FunctionPointer::getExplosionValue(IRGenFunction &IGF,
                                   CanSILFunctionType fnType) const {
  llvm::Value *fnPtr = getRawPointer();

  // Re-sign to the appropriate schema for this function pointer type.
  auto resultAuthInfo = PointerAuthInfo::forFunctionPointer(IGF.IGM, fnType);
  if (getAuthInfo() != resultAuthInfo) {
    fnPtr = emitPointerAuthResign(IGF, fnPtr, getAuthInfo(), resultAuthInfo);
  }

  // Bitcast to an opaque pointer type.
  fnPtr = IGF.Builder.CreateBitCast(fnPtr, IGF.IGM.Int8PtrTy);

  return fnPtr;
}

FunctionPointer FunctionPointer::getAsFunction(IRGenFunction &IGF) const {
  return FunctionPointer(KindTy::Function, getPointer(IGF), AuthInfo, Sig);
}

void irgen::emitAsyncReturn(IRGenFunction &IGF, AsyncContextLayout &asyncLayout,
                            CanSILFunctionType fnType) {
  auto contextAddr = asyncLayout.emitCastTo(IGF, IGF.getAsyncContext());
  auto returnToCallerLayout = asyncLayout.getResumeParentLayout();
  auto returnToCallerAddr =
      returnToCallerLayout.project(IGF, contextAddr, llvm::None);
  Explosion fn;
  cast<LoadableTypeInfo>(returnToCallerLayout.getType())
      .loadAsCopy(IGF, returnToCallerAddr, fn);
  llvm::Value *fnVal = fn.claimNext();

  if (auto schema = IGF.IGM.getOptions().PointerAuth.AsyncContextResume) {
    Address fieldAddr =
        returnToCallerLayout.project(IGF, contextAddr, /*offsets*/ llvm::None);
    auto authInfo = PointerAuthInfo::emit(IGF, schema, fieldAddr.getAddress(),
                                          PointerAuthEntity());
    fnVal = emitPointerAuthAuth(IGF, fnVal, authInfo);
  }

  auto sig = emitCastOfFunctionPointer(IGF, fnVal, fnType);
  FunctionPointer fnPtr(FunctionPointer::KindTy::Function, fnVal,
                        PointerAuthInfo(), sig);

  SmallVector<llvm::Value*, 4> Args;
  // Get the current task, executor, and async context.
  Args.push_back(IGF.getAsyncTask());
  Args.push_back(IGF.getAsyncExecutor());
  Args.push_back(IGF.getAsyncContext());
  auto call = IGF.Builder.CreateCall(fnPtr, Args);
  call->setTailCall();
}

FunctionPointer
IRGenFunction::getFunctionPointerForResumeIntrinsic(llvm::Value *resume) {
  auto *fnTy = llvm::FunctionType::get(
      IGM.VoidTy, {IGM.Int8PtrTy, IGM.Int8PtrTy, IGM.Int8PtrTy},
      false /*vaargs*/);
  auto signature =
      Signature(fnTy, IGM.constructInitialAttributes(), IGM.SwiftCC);
  auto fnPtr = FunctionPointer(
      FunctionPointer::KindTy::Function,
      Builder.CreateBitOrPointerCast(resume, fnTy->getPointerTo()),
      PointerAuthInfo(), signature);
  return fnPtr;
}

Address irgen::emitAutoDiffCreateLinearMapContext(
    IRGenFunction &IGF, llvm::Value *topLevelSubcontextSize) {
  auto *call = IGF.Builder.CreateCall(
      IGF.IGM.getAutoDiffCreateLinearMapContextFn(), {topLevelSubcontextSize});
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);
  return Address(call, IGF.IGM.getPointerAlignment());
}

Address irgen::emitAutoDiffProjectTopLevelSubcontext(
    IRGenFunction &IGF, Address context) {
  auto *call = IGF.Builder.CreateCall(
      IGF.IGM.getAutoDiffProjectTopLevelSubcontextFn(),
      {context.getAddress()});
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);
  return Address(call, IGF.IGM.getPointerAlignment());
}

Address irgen::emitAutoDiffAllocateSubcontext(
    IRGenFunction &IGF, Address context, llvm::Value *size) {
  auto *call = IGF.Builder.CreateCall(
      IGF.IGM.getAutoDiffAllocateSubcontextFn(),
      {context.getAddress(), size});
  call->setDoesNotThrow();
  call->setCallingConv(IGF.IGM.SwiftCC);
  return Address(call, IGF.IGM.getPointerAlignment());
}
