//===--- SILGenThunk.cpp - SILGen for thunks ------------------------------===//
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
// This file contains code for emitting various types of thunks that can be
// referenced from code, such as dynamic thunks, curry thunks, native to foreign
// thunks and foreign to native thunks.
//
// VTable thunks and witness thunks can be found in SILGenType.cpp, and the
// meat of the bridging thunk implementation is in SILGenBridging.cpp, and
// re-abstraction thunks are in SILGenPoly.cpp.
//
//===----------------------------------------------------------------------===//

#include "ManagedValue.h"
#include "SILGenFunction.h"
#include "SILGenFunctionBuilder.h"
#include "Scope.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/ForeignAsyncConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/TypeLowering.h"

#include "clang/AST/ASTContext.h"

using namespace swift;
using namespace Lowering;

SILValue SILGenFunction::emitClassMethodRef(SILLocation loc,
                                            SILValue selfPtr,
                                            SILDeclRef constant,
                                            CanSILFunctionType constantTy) {
  assert(!constant.isForeign);
  return B.createClassMethod(loc, selfPtr, constant,
                             SILType::getPrimitiveObjectType(constantTy));
}

SILFunction *SILGenModule::getDynamicThunk(SILDeclRef constant,
                                           CanSILFunctionType constantTy) {
  assert(constant.kind != SILDeclRef::Kind::Allocator &&
         "allocating entry point for constructor is never dynamic");
  // Mangle the constant with a TD suffix.
  auto nameTmp = constant.mangle(SILDeclRef::ManglingKind::DynamicThunk);
  auto name = M.allocateCopy(nameTmp);

  SILGenFunctionBuilder builder(*this);
  auto F = builder.getOrCreateFunction(
      constant.getDecl(), name, SILLinkage::Shared, constantTy, IsBare,
      IsTransparent, IsSerializable, IsNotDynamic, ProfileCounter(), IsThunk);

  if (F->empty()) {
    // Emit the thunk if we haven't yet.
    // Currently a dynamic thunk looks just like a foreign-to-native thunk around
    // an ObjC method. This would change if we introduced a native
    // runtime-hookable mechanism.
    SILGenFunction SGF(*this, *F, SwiftModule);
    SGF.emitForeignToNativeThunk(constant);
    emitLazyConformancesForFunction(F);
  }

  return F;
}

ManagedValue
SILGenFunction::emitDynamicMethodRef(SILLocation loc, SILDeclRef constant,
                                     CanSILFunctionType constantTy) {
  // If the method is foreign, its foreign thunk will handle the dynamic
  // dispatch for us.
  if (constant.isForeignToNativeThunk()) {
    if (!SGM.hasFunction(constant))
      SGM.emitForeignToNativeThunk(constant);
    return ManagedValue::forUnmanaged(B.createFunctionRefFor(
        loc, SGM.getFunction(constant, NotForDefinition)));
  }

  // Otherwise, we need a dynamic dispatch thunk.
  SILFunction *F = SGM.getDynamicThunk(constant, constantTy);

  return ManagedValue::forUnmanaged(B.createFunctionRefFor(loc, F));
}

void SILGenModule::emitForeignToNativeThunk(SILDeclRef thunk) {
  // Thunks are always emitted by need, so don't need delayed emission.
  assert(thunk.isForeignToNativeThunk() && "foreign-to-native thunks only");
  emitFunctionDefinition(thunk, getFunction(thunk, ForDefinition));
}

void SILGenModule::emitNativeToForeignThunk(SILDeclRef thunk) {
  // Thunks are always emitted by need, so don't need delayed emission.
  assert(thunk.isNativeToForeignThunk() && "native-to-foreign thunks only");
  emitFunctionDefinition(thunk, getFunction(thunk, ForDefinition));
}

SILValue
SILGenFunction::emitGlobalFunctionRef(SILLocation loc, SILDeclRef constant,
                                      SILConstantInfo constantInfo,
                                      bool callPreviousDynamicReplaceableImpl) {
  assert(constantInfo == getConstantInfo(getTypeExpansionContext(), constant));

  // Builtins must be fully applied at the point of reference.
  if (constant.hasDecl() &&
      isa<BuiltinUnit>(constant.getDecl()->getDeclContext())) {
    SGM.diagnose(loc.getSourceLoc(), diag::not_implemented,
                 "delayed application of builtin");
    return SILUndef::get(constantInfo.getSILType(), F);
  }
  
  // If the constant is a thunk we haven't emitted yet, emit it.
  if (!SGM.hasFunction(constant)) {
    if (constant.isForeignToNativeThunk()) {
      SGM.emitForeignToNativeThunk(constant);
    } else if (constant.isNativeToForeignThunk()) {
      SGM.emitNativeToForeignThunk(constant);
    }
  }

  auto f = SGM.getFunction(constant, NotForDefinition);
#ifndef NDEBUG
  auto constantFnTypeInContext =
    SGM.Types.getLoweredType(constantInfo.SILFnType,
                             B.getTypeExpansionContext())
             .castTo<SILFunctionType>();
  assert(f->getLoweredFunctionTypeInContext(B.getTypeExpansionContext())
          == constantFnTypeInContext);
#endif
  if (callPreviousDynamicReplaceableImpl)
    return B.createPreviousDynamicFunctionRef(loc, f);
  else
    return B.createFunctionRefFor(loc, f);
}

static const clang::Type *prependParameterType(
      ASTContext &ctx,
      const clang::Type *oldBlockPtrTy,
      const clang::Type *newParameterTy) {
  if (!oldBlockPtrTy)
    return nullptr;

  SmallVector<clang::QualType, 4> newParamTypes;
  newParamTypes.push_back(clang::QualType(newParameterTy, 0));
  clang::QualType returnType;
  clang::FunctionProtoType::ExtProtoInfo newExtProtoInfo{};
  using ExtParameterInfo = clang::FunctionProtoType::ExtParameterInfo;
  SmallVector<ExtParameterInfo, 4> newExtParamInfos;

  auto blockPtrTy = cast<clang::BlockPointerType>(oldBlockPtrTy);
  auto blockPointeeTy = blockPtrTy->getPointeeType().getTypePtr();
  if (auto fnNoProtoTy = dyn_cast<clang::FunctionNoProtoType>(blockPointeeTy)) {
    returnType = fnNoProtoTy->getReturnType();
    newExtProtoInfo.ExtInfo = fnNoProtoTy->getExtInfo();
  } else {
    auto fnProtoTy = cast<clang::FunctionProtoType>(blockPointeeTy);
    llvm::copy(fnProtoTy->getParamTypes(), std::back_inserter(newParamTypes));
    returnType = fnProtoTy->getReturnType();
    newExtProtoInfo = fnProtoTy->getExtProtoInfo();
    auto extParamInfos = fnProtoTy->getExtParameterInfosOrNull();
    if (extParamInfos) {
      auto oldExtParamInfos =
          ArrayRef<ExtParameterInfo>(extParamInfos, fnProtoTy->getNumParams());
      newExtParamInfos.push_back(clang::FunctionProtoType::ExtParameterInfo());
      llvm::copy(oldExtParamInfos, std::back_inserter(newExtParamInfos));
      newExtProtoInfo.ExtParameterInfos = newExtParamInfos.data();
    }
  }

  auto &clangCtx = ctx.getClangModuleLoader()->getClangASTContext();
  auto newFnTy =
      clangCtx.getFunctionType(returnType, newParamTypes, newExtProtoInfo);
  return clangCtx.getPointerType(newFnTy).getTypePtr();
}

SILFunction *
SILGenModule::getOrCreateForeignAsyncCompletionHandlerImplFunction(
                                         CanSILFunctionType blockType,
                                         CanType continuationTy,
                                         ForeignAsyncConvention convention) {
  // Extract the result type from the continuation type.
  auto resumeType = cast<BoundGenericType>(continuationTy).getGenericArgs()[0];
  
  // Build up the implementation function type, which matches the
  // block signature with an added block storage argument that points at the
  // block buffer. The block storage holds the continuation we feed the
  // result values into.
  SmallVector<SILParameterInfo, 4> implArgs;
  auto blockStorageTy = SILBlockStorageType::get(continuationTy);
  implArgs.push_back(SILParameterInfo(blockStorageTy,
                                ParameterConvention::Indirect_InoutAliasable));
  
  std::copy(blockType->getParameters().begin(),
            blockType->getParameters().end(),
            std::back_inserter(implArgs));

  auto newClangTy = prependParameterType(
      getASTContext(),
      blockType->getClangTypeInfo().getType(),
      getASTContext().getClangTypeForIRGen(blockStorageTy));
  
  auto implTy = SILFunctionType::get(GenericSignature(),
         blockType->getExtInfo().intoBuilder()
           .withRepresentation(SILFunctionTypeRepresentation::CFunctionPointer)
           .withClangFunctionType(newClangTy)
           .build(),
         SILCoroutineKind::None,
         ParameterConvention::Direct_Unowned,
         implArgs, {}, blockType->getResults(),
         None,
         SubstitutionMap(), SubstitutionMap(), getASTContext());

  auto loc = RegularLocation::getAutoGeneratedLocation();

  Mangle::ASTMangler Mangler;
  auto name = Mangler.mangleObjCAsyncCompletionHandlerImpl(blockType,
                                                           resumeType,
                                                           /*predefined*/ false);
  
  SILGenFunctionBuilder builder(*this);
  auto F = builder.getOrCreateSharedFunction(loc, name, implTy,
                                           IsBare, IsTransparent, IsSerializable,
                                           ProfileCounter(),
                                           IsThunk,
                                           IsNotDynamic);
  
  if (F->empty()) {
    // Emit the implementation.
    SILGenFunction SGF(*this, *F, SwiftModule);
    {
      Scope scope(SGF, loc);
      SmallVector<ManagedValue, 4> params;
      SGF.collectThunkParams(loc, params);

      // Get the continuation out of the block object.
      auto blockStorage = params[0].getValue();
      auto continuationAddr = SGF.B.createProjectBlockStorage(loc, blockStorage);
      auto continuationVal = SGF.B.createLoad(loc, continuationAddr,
                                           LoadOwnershipQualifier::Trivial);
      auto continuation = ManagedValue::forUnmanaged(continuationVal);
      
      // Check for an error if the convention includes one.
      auto errorIndex = convention.completionHandlerErrorParamIndex();
    
      FuncDecl *resumeIntrinsic, *errorIntrinsic;

      SILBasicBlock *returnBB = nullptr;
      if (errorIndex) {
        resumeIntrinsic = getResumeUnsafeThrowingContinuation();
        errorIntrinsic = getResumeUnsafeThrowingContinuationWithError();
        
        auto errorArgument = params[*errorIndex + 1];
        auto someErrorBB = SGF.createBasicBlock(FunctionSection::Postmatter);
        auto noneErrorBB = SGF.createBasicBlock();
        returnBB = SGF.createBasicBlockAfter(noneErrorBB);

        auto &C = SGF.getASTContext();
        std::pair<EnumElementDecl *, SILBasicBlock *> switchErrorBBs[] = {
          {C.getOptionalSomeDecl(), someErrorBB},
          {C.getOptionalNoneDecl(), noneErrorBB}
        };
        
        SGF.B.createSwitchEnum(loc, errorArgument.borrow(SGF, loc).getValue(),
                               /*default*/ nullptr,
                               switchErrorBBs);
        
        SGF.B.emitBlock(someErrorBB);
        
        auto matchedErrorTy = errorArgument.getType().getOptionalObjectType();
        auto matchedError = SGF.B
          .createGuaranteedTransformingTerminatorArgument(matchedErrorTy);
        
        // Resume the continuation as throwing the given error, bridged to a
        // native Swift error.
        auto nativeError = SGF.emitBridgedToNativeError(loc, matchedError);
        Type replacementTypes[] = {resumeType};
        auto subs = SubstitutionMap::get(errorIntrinsic->getGenericSignature(),
                                         replacementTypes,
                                         ArrayRef<ProtocolConformanceRef>{});
        SGF.emitApplyOfLibraryIntrinsic(loc, errorIntrinsic, subs,
                                        {continuation, nativeError},
                                        SGFContext());
        
        SGF.B.createBranch(loc, returnBB);
        SGF.B.emitBlock(noneErrorBB);
      } else {
        resumeIntrinsic = getResumeUnsafeContinuation();
      }
            
      auto loweredResumeTy = SGF.getLoweredType(AbstractionPattern::getOpaque(),
                                                resumeType);
      
      // Prepare the argument for the resume intrinsic, using the non-error
      // arguments to the callback.
      {
        Scope resumeScope(SGF, loc);
        unsigned errorIndexBoundary = errorIndex ? *errorIndex : ~0u;
        auto resumeArgBuf = SGF.emitTemporaryAllocation(loc,
                                              loweredResumeTy.getAddressType());

        auto prepareArgument = [&](SILValue destBuf, ManagedValue arg) {
          // Convert the ObjC argument to the bridged Swift representation we
          // want.
          ManagedValue bridgedArg = SGF.emitBridgedToNativeValue(loc,
                                       arg.copy(SGF, loc),
                                       arg.getType().getASTType(),
                                       // FIXME: pass down formal type
                                       destBuf->getType().getASTType(),
                                       destBuf->getType().getObjectType());
          // Force-unwrap an argument that comes to us as Optional if it's
          // formally non-optional in the return.
          if (bridgedArg.getType().getOptionalObjectType()
              && !destBuf->getType().getOptionalObjectType()) {
            bridgedArg = SGF.emitPreconditionOptionalHasValue(loc,
                                                             bridgedArg,
                                                             /*implicit*/ true);
          }
          bridgedArg.forwardInto(SGF, loc, destBuf);
        };

        if (auto resumeTuple = dyn_cast<TupleType>(resumeType)) {
          assert(params.size() == resumeTuple->getNumElements()
                                   + 1 + (bool)errorIndex);
          for (auto i : indices(resumeTuple.getElementTypes())) {
            auto resumeEltBuf = SGF.B.createTupleElementAddr(loc,
                                                             resumeArgBuf, i);
            auto arg = params[1 + i + (i >= errorIndexBoundary)];
            prepareArgument(resumeEltBuf, arg);
          }
        } else {
          assert(params.size() == 2 + (bool)errorIndex);
          prepareArgument(resumeArgBuf, params[1 + (errorIndexBoundary == 0)]);
        }
        
        
        // Resume the continuation with the composed bridged result.
        ManagedValue resumeArg = SGF.emitManagedBufferWithCleanup(resumeArgBuf);
        Type replacementTypes[] = {resumeType};
        auto subs = SubstitutionMap::get(resumeIntrinsic->getGenericSignature(),
                                         replacementTypes,
                                         ArrayRef<ProtocolConformanceRef>{});
        SGF.emitApplyOfLibraryIntrinsic(loc, resumeIntrinsic, subs,
                                        {continuation, resumeArg},
                                        SGFContext());
      }
      
      // Now we've resumed the continuation one way or another. Return from the
      // completion callback.
      if (returnBB) {
        SGF.B.createBranch(loc, returnBB);
        SGF.B.emitBlock(returnBB);
      }
    }

    SGF.B.createReturn(loc,
                       SILUndef::get(SGF.SGM.Types.getEmptyTupleType(), SGF.F));
  }
  
  return F;
}

SILFunction *SILGenModule::
getOrCreateReabstractionThunk(CanSILFunctionType thunkType,
                              CanSILFunctionType fromType,
                              CanSILFunctionType toType,
                              CanType dynamicSelfType) {
  // The reference to the thunk is likely @noescape, but declarations are always
  // escaping.
  auto thunkDeclType =
      thunkType->getWithExtInfo(thunkType->getExtInfo().withNoEscape(false));

  // Mangle the reabstraction thunk.
  // Substitute context parameters out of the "from" and "to" types.
  auto fromInterfaceType = fromType->mapTypeOutOfContext()
    ->getCanonicalType();
  auto toInterfaceType = toType->mapTypeOutOfContext()
    ->getCanonicalType();
  CanType dynamicSelfInterfaceType;
  if (dynamicSelfType)
    dynamicSelfInterfaceType = dynamicSelfType->mapTypeOutOfContext()
      ->getCanonicalType();

  Mangle::ASTMangler NewMangler;
  std::string name = NewMangler.mangleReabstractionThunkHelper(thunkType,
                       fromInterfaceType, toInterfaceType,
                       dynamicSelfInterfaceType,
                       M.getSwiftModule());
  
  auto loc = RegularLocation::getAutoGeneratedLocation();

  SILGenFunctionBuilder builder(*this);
  return builder.getOrCreateSharedFunction(
      loc, name, thunkDeclType, IsBare, IsTransparent, IsSerializable,
      ProfileCounter(), IsReabstractionThunk, IsNotDynamic);
}

SILFunction *SILGenModule::getOrCreateAutoDiffClassMethodThunk(
    SILDeclRef derivativeFnDeclRef, CanSILFunctionType constantTy) {
  auto *derivativeId = derivativeFnDeclRef.getDerivativeFunctionIdentifier();
  assert(derivativeId);
  auto *derivativeFnDecl = derivativeFnDeclRef.getDecl();

  SILGenFunctionBuilder builder(*this);
  auto originalFnDeclRef = derivativeFnDeclRef.asAutoDiffOriginalFunction();
  // TODO(SR-13508): Use principled thunk mangling.
  // Do not simply reuse reabstraction thunk mangling.
  auto name = "AD__" + derivativeFnDeclRef.mangle() + "_vtable_entry_thunk";
  auto *thunk = builder.getOrCreateFunction(
      derivativeFnDecl, name, originalFnDeclRef.getLinkage(ForDefinition),
      constantTy, IsBare, IsTransparent, derivativeFnDeclRef.isSerialized(),
      IsNotDynamic, ProfileCounter(), IsThunk);
  if (!thunk->empty())
    return thunk;

  if (auto genSig = constantTy->getSubstGenericSignature())
    thunk->setGenericEnvironment(genSig->getGenericEnvironment());
  SILGenFunction SGF(*this, *thunk, SwiftModule);
  SmallVector<ManagedValue, 4> params;
  auto loc = derivativeFnDeclRef.getAsRegularLocation();
  SGF.collectThunkParams(loc, params);

  auto originalFn = SGF.emitGlobalFunctionRef(loc, originalFnDeclRef);
  auto *loweredParamIndices = autodiff::getLoweredParameterIndices(
      derivativeId->getParameterIndices(),
      derivativeFnDecl->getInterfaceType()->castTo<AnyFunctionType>());
  auto *loweredResultIndices = IndexSubset::get(getASTContext(), 1, {0});
  auto diffFn = SGF.B.createDifferentiableFunction(
      loc, loweredParamIndices, loweredResultIndices, originalFn);
  auto derivativeFn = SGF.B.createDifferentiableFunctionExtract(
      loc, NormalDifferentiableFunctionTypeComponent(derivativeId->getKind()),
      diffFn);
  auto derivativeFnSILTy = SILType::getPrimitiveObjectType(constantTy);
  SmallVector<SILValue, 4> args(thunk->getArguments().begin(),
                                thunk->getArguments().end());
  auto apply =
      SGF.emitApplyWithRethrow(loc, derivativeFn, derivativeFnSILTy,
                               SGF.getForwardingSubstitutionMap(), args);
  SGF.B.createReturn(loc, apply);

  return thunk;
}
