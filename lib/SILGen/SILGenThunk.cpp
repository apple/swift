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
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/SIL/FormalLinkage.h"
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
      IsTransparent, IsSerialized, IsNotDynamic, IsNotDistributed,
      IsNotRuntimeAccessible, ProfileCounter(), IsThunk);

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
    return ManagedValue::forObjectRValueWithoutOwnership(B.createFunctionRefFor(
        loc, SGM.getFunction(constant, NotForDefinition)));
  }

  // Otherwise, we need a dynamic dispatch thunk.
  SILFunction *F = SGM.getDynamicThunk(constant, constantTy);

  return ManagedValue::forObjectRValueWithoutOwnership(
      B.createFunctionRefFor(loc, F));
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

void SILGenModule::emitDistributedThunk(SILDeclRef thunk) {
  // Thunks are always emitted by need, so don't need delayed emission.
  assert(thunk.isDistributedThunk() && "distributed thunks only");
  emitFunctionDefinition(thunk, getFunction(thunk, ForDefinition));
}

void SILGenModule::emitBackDeploymentThunk(SILDeclRef thunk) {
  // Thunks are always emitted by need, so don't need delayed emission.
  assert(thunk.isBackDeploymentThunk() && "back deployment thunks only");
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

SILFunction *SILGenModule::getOrCreateForeignAsyncCompletionHandlerImplFunction(
    CanSILFunctionType blockType, CanType continuationTy,
    AbstractionPattern origFormalType, CanGenericSignature sig,
    ForeignAsyncConvention convention,
    llvm::Optional<ForeignErrorConvention> foreignError) {
  // Extract the result and error types from the continuation type.
  auto resumeType = cast<BoundGenericType>(continuationTy).getGenericArgs()[0];

  CanAnyFunctionType completionHandlerOrigTy = [&]() {
    auto completionHandlerOrigTy =
        origFormalType.getObjCMethodAsyncCompletionHandlerForeignType(convention, Types);
    llvm::Optional<CanAnyFunctionType> maybeCompletionHandlerOrigTy;
    if (auto fnTy =
            dyn_cast<AnyFunctionType>(completionHandlerOrigTy)) {
      maybeCompletionHandlerOrigTy = fnTy;
    } else {
      maybeCompletionHandlerOrigTy = cast<AnyFunctionType>(
          completionHandlerOrigTy.getOptionalObjectType());
    }
    return maybeCompletionHandlerOrigTy.value();
  }();
  
  // Bridge the block type, so that if it is formally expressed in terms of
  // bridged Swift types, we still lower the parameters to their ultimate
  // ObjC types.
  completionHandlerOrigTy = Types
    .getBridgedFunctionType(AbstractionPattern(origFormalType.getGenericSignatureOrNull(),
                                               completionHandlerOrigTy),
                            completionHandlerOrigTy,
                            Bridgeability::Full,
                            SILFunctionTypeRepresentation::Block);

  auto blockParams = completionHandlerOrigTy.getParams();

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

  auto implTy = SILFunctionType::get(
      sig,
      blockType->getExtInfo()
          .intoBuilder()
          .withRepresentation(SILFunctionTypeRepresentation::CFunctionPointer)
          .withClangFunctionType(newClangTy)
          .withIsPseudogeneric((bool)sig)
          .build(),
      SILCoroutineKind::None, ParameterConvention::Direct_Unowned, implArgs, {},
      blockType->getResults(), llvm::None, SubstitutionMap(), SubstitutionMap(),
      getASTContext());

  auto loc = RegularLocation::getAutoGeneratedLocation();

  Mangle::ASTMangler Mangler;
  auto name = Mangler.mangleObjCAsyncCompletionHandlerImpl(
      blockType, resumeType, sig,
      convention.completionHandlerFlagParamIndex()
          ? llvm::Optional<bool>(
                convention.completionHandlerFlagIsErrorOnZero())
          : llvm::Optional<bool>(),
      /*predefined*/ false);

  SILGenFunctionBuilder builder(*this);
  auto F = builder.getOrCreateSharedFunction(loc, name, implTy,
                                           IsBare, IsTransparent, IsSerialized,
                                           ProfileCounter(),
                                           IsThunk,
                                           IsNotDynamic,
                                           IsNotDistributed,
                                           IsNotRuntimeAccessible);
  
  if (F->empty()) {
    // Emit the implementation.
    F->setGenericEnvironment(sig.getGenericEnvironment());

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
      auto continuation =
          ManagedValue::forObjectRValueWithoutOwnership(continuationVal);

      // Check for an error if the convention includes one.
      // Increment the error and flag indices if present.  They do not account
      // for the fact that they are preceded by the block_storage arguments.
      auto errorIndex = convention.completionHandlerErrorParamIndex().transform(
          [](auto original) { return original + 1; });
      auto flagIndex = convention.completionHandlerFlagParamIndex().transform(
          [](auto original) { return original + 1; });

      FuncDecl *resumeIntrinsic;

      SILBasicBlock *returnBB = nullptr;
      if (errorIndex) {
        resumeIntrinsic = getResumeUnsafeThrowingContinuation();
        auto errorIntrinsic = getResumeUnsafeThrowingContinuationWithError();

        auto errorArgument = params[*errorIndex];
        auto someErrorBB = SGF.createBasicBlock(FunctionSection::Postmatter);
        auto noneErrorBB = SGF.createBasicBlock();
        returnBB = SGF.createBasicBlockAfter(noneErrorBB);
        auto &C = SGF.getASTContext();
        SwitchEnumInst *switchEnum = nullptr;

        // Check whether there's an error, based on the presence of a flag
        // parameter. If there is a flag parameter, test it against zero.
        if (flagIndex) {
          auto flagArgument = params[*flagIndex];

          // The flag must be an integer type. Get the underlying builtin
          // integer field from it.
          auto builtinFlagArg = SGF.emitUnwrapIntegerResult(loc, flagArgument.getValue());
          
          auto zero = SGF.B.createIntegerLiteral(loc, builtinFlagArg->getType(), 0);
          auto zeroOnError = convention.completionHandlerFlagIsErrorOnZero();
          
          auto zeroBB = zeroOnError ? someErrorBB : noneErrorBB;
          auto nonzeroBB = zeroOnError ? noneErrorBB : someErrorBB;
          
          std::pair<SILValue, SILBasicBlock*> switchFlagBBs[] = {
            {zero, zeroBB}
          };
          
          SGF.B.createSwitchValue(loc, builtinFlagArg,
                                  /*default*/ nonzeroBB,
                                  switchFlagBBs);
        } else {
          // If there is no flag parameter, the presence of a nonnull error
          // parameter indicates an error.
          std::pair<EnumElementDecl *, SILBasicBlock *> switchErrorBBs[] = {
            {C.getOptionalSomeDecl(), someErrorBB},
            {C.getOptionalNoneDecl(), noneErrorBB}
          };

          switchEnum = SGF.B.createSwitchEnum(
              loc, errorArgument.borrow(SGF, loc).getValue(),
              /*default*/ nullptr, switchErrorBBs);
        }
        
        SGF.B.emitBlock(someErrorBB);
        
        Scope errorScope(SGF, loc);
        ManagedValue matchedError;
        if (flagIndex) {
          // Force-unwrap the error argument, since the flag condition should
          // guarantee that an error did occur.
          matchedError = SGF.emitPreconditionOptionalHasValue(loc,
                                                 errorArgument.borrow(SGF, loc),
                                                 /*implicit*/ true);
        } else {
          matchedError = SGF.B.createOptionalSomeResult(switchEnum);
        }
        
        // Resume the continuation as throwing the given error, bridged to a
        // native Swift error.
        auto nativeError = SGF.emitBridgedToNativeError(loc, matchedError);
        Type replacementTypes[]
          = {F->mapTypeIntoContext(resumeType)->getCanonicalType()};
        auto subs = SubstitutionMap::get(errorIntrinsic->getGenericSignature(),
                                         replacementTypes,
                                         ArrayRef<ProtocolConformanceRef>{});
        SGF.emitApplyOfLibraryIntrinsic(loc, errorIntrinsic, subs,
                                        {continuation, nativeError},
                                        SGFContext());
        errorScope.pop();
        SGF.B.createBranch(loc, returnBB);
        SGF.B.emitBlock(noneErrorBB);
      } else if (foreignError) {
        resumeIntrinsic = getResumeUnsafeThrowingContinuation();
      } else {
        resumeIntrinsic = getResumeUnsafeContinuation();
      }

      auto loweredResumeTy = SGF.getLoweredType(AbstractionPattern::getOpaque(),
                                            F->mapTypeIntoContext(resumeType));
      
      // Prepare the argument for the resume intrinsic, using the non-error
      // arguments to the callback.
      {
        Scope resumeScope(SGF, loc);
        auto resumeArgBuf = SGF.emitTemporaryAllocation(loc,
                                              loweredResumeTy.getAddressType());

        auto prepareArgument = [&](SILValue destBuf, CanType destFormalType,
                                   ManagedValue arg, CanType argFormalType) {
          // Convert the ObjC argument to the bridged Swift representation we
          // want.
          ManagedValue bridgedArg = SGF.emitBridgedToNativeValue(
              loc, arg.copy(SGF, loc), argFormalType, destFormalType,
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

        // Collect the indices which correspond to the values to be returned.
        SmallVector<unsigned long, 4> paramIndices;
        for (auto index : indices(params)) {
          // The first index is the block_storage parameter.
          if (index == 0)
            continue;
          if (errorIndex && index == *errorIndex)
            continue;
          if (flagIndex && index == *flagIndex)
            continue;
          paramIndices.push_back(index);
        }
        auto blockParamIndex = [paramIndices](unsigned long i) {
          // The non-error, non-flag block parameter (formal types of the
          // completion handler's arguments) indices are the same as the the
          // parameter (lowered types of the completion handler's arguments)
          // indices but shifted by 1 corresponding to the fact that the lowered
          // completion handler has a block_storage argument but the formal type
          // does not.
          return paramIndices[i] - 1;
        };
        if (auto resumeTuple = dyn_cast<TupleType>(resumeType)) {
          assert(paramIndices.size() == resumeTuple->getNumElements());
          assert(params.size() == resumeTuple->getNumElements()
                                   + 1 + (bool)errorIndex + (bool)flagIndex);
          for (unsigned i : indices(resumeTuple.getElementTypes())) {
            auto resumeEltBuf = SGF.B.createTupleElementAddr(loc,
                                                             resumeArgBuf, i);
            prepareArgument(
                /*destBuf*/ resumeEltBuf,
                /*destFormalType*/
                F->mapTypeIntoContext(resumeTuple.getElementTypes()[i])
                    ->getCanonicalType(),
                /*arg*/ params[paramIndices[i]],
                /*argFormalType*/
                blockParams[blockParamIndex(i)].getParameterType());
          }
        } else {
          assert(paramIndices.size() == 1);
          assert(params.size() == 2 + (bool)errorIndex + (bool)flagIndex);
          prepareArgument(/*destBuf*/ resumeArgBuf,
                          /*destFormalType*/
                          F->mapTypeIntoContext(resumeType)->getCanonicalType(),
                          /*arg*/ params[paramIndices[0]],
                          /*argFormalType*/
                          blockParams[blockParamIndex(0)].getParameterType());
        }
        
        // Resume the continuation with the composed bridged result.
        ManagedValue resumeArg = SGF.emitManagedBufferWithCleanup(resumeArgBuf);
        Type replacementTypes[]
          = {F->mapTypeIntoContext(resumeType)->getCanonicalType()};
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
                              CanType dynamicSelfType,
                              CanType fromGlobalActorBound) {
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
  if (fromGlobalActorBound)
    fromGlobalActorBound = fromGlobalActorBound->mapTypeOutOfContext()
      ->getCanonicalType();

  Mangle::ASTMangler NewMangler;
  std::string name = NewMangler.mangleReabstractionThunkHelper(thunkType,
                       fromInterfaceType, toInterfaceType,
                       dynamicSelfInterfaceType,
                       fromGlobalActorBound,
                       M.getSwiftModule());
  
  auto loc = RegularLocation::getAutoGeneratedLocation();
  
  // The thunk that converts an actor-constrained, non-async function to an
  // async function is not serializable if the actor's visibility precludes it.
  auto serializable = IsSerialized;
  if (fromGlobalActorBound) {
    auto globalActorLinkage = getTypeLinkage(fromGlobalActorBound);
    serializable = globalActorLinkage >= FormalLinkage::PublicNonUnique
      ? IsSerialized : IsNotSerialized;
  }

  SILGenFunctionBuilder builder(*this);
  return builder.getOrCreateSharedFunction(
      loc, name, thunkDeclType, IsBare, IsTransparent, serializable,
      ProfileCounter(), IsReabstractionThunk, IsNotDynamic, IsNotDistributed,
      IsNotRuntimeAccessible);
}

SILFunction *SILGenModule::getOrCreateDerivativeVTableThunk(
    SILDeclRef derivativeFnDeclRef, CanSILFunctionType constantTy) {
  auto *derivativeId = derivativeFnDeclRef.getDerivativeFunctionIdentifier();
  assert(derivativeId);
  auto *derivativeFnDecl = derivativeFnDeclRef.getDecl();

  SILGenFunctionBuilder builder(*this);
  auto originalFnDeclRef = derivativeFnDeclRef.asAutoDiffOriginalFunction();
  Mangle::ASTMangler mangler;
  auto *resultIndices = autodiff::getFunctionSemanticResultIndices(
    originalFnDeclRef.getAbstractFunctionDecl(),
    derivativeId->getParameterIndices());
  auto name = mangler.mangleAutoDiffDerivativeFunction(
      originalFnDeclRef.getAbstractFunctionDecl(),
      derivativeId->getKind(),
      AutoDiffConfig(derivativeId->getParameterIndices(),
                     resultIndices,
                     derivativeId->getDerivativeGenericSignature()),
      /*isVTableThunk*/ true);
  auto *thunk = builder.getOrCreateFunction(
      derivativeFnDecl, name, SILLinkage::Private, constantTy, IsBare,
      IsTransparent, derivativeFnDeclRef.isSerialized(), IsNotDynamic,
      IsNotDistributed, IsNotRuntimeAccessible, ProfileCounter(), IsThunk);
  if (!thunk->empty())
    return thunk;

  thunk->setGenericEnvironment(constantTy->getSubstGenericSignature().getGenericEnvironment());
  SILGenFunction SGF(*this, *thunk, SwiftModule);
  SmallVector<ManagedValue, 4> params;
  auto loc = derivativeFnDeclRef.getAsRegularLocation();
  SGF.collectThunkParams(loc, params);

  auto originalFn = SGF.emitGlobalFunctionRef(loc, originalFnDeclRef);
  auto *loweredParamIndices = autodiff::getLoweredParameterIndices(
      derivativeId->getParameterIndices(),
      derivativeFnDecl->getInterfaceType()->castTo<AnyFunctionType>());
  // FIXME: Do we need to lower the result indices? Likely yes.
  auto *loweredResultIndices =
    autodiff::getFunctionSemanticResultIndices(
      originalFnDeclRef.getAbstractFunctionDecl(),
      derivativeId->getParameterIndices()
    );
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
