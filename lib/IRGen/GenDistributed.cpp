//===--- GenDistributed.cpp - IRGen for distributed features --------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements IR generation for distributed features.
//
//===----------------------------------------------------------------------===//

#include "GenDistributed.h"

#include "BitPatternBuilder.h"
#include "CallEmission.h"
#include "Callee.h"
#include "ClassTypeInfo.h"
#include "ExtraInhabitants.h"
#include "GenCall.h"
#include "GenClass.h"
#include "GenDecl.h"
#include "GenMeta.h"
#include "GenOpaque.h"
#include "GenProto.h"
#include "GenType.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "LoadableTypeInfo.h"
#include "ScalarPairTypeInfo.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/AST/ExtInfo.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/SILFunction.h"

using namespace swift;
using namespace irgen;

llvm::Value *irgen::emitDistributedActorInitializeRemote(
    IRGenFunction &IGF, SILType selfType, llvm::Value *actorMetatype, Explosion &out) {
  auto &classTI = IGF.getTypeInfo(selfType).as<ClassTypeInfo>();
  auto &classLayout = classTI.getClassLayout(IGF.IGM, selfType,
                                             /*forBackwardDeployment=*/false);
  llvm::Type *destType = classLayout.getType()->getPointerTo();

  auto fn = IGF.IGM.getDistributedActorInitializeRemoteFunctionPointer();
  actorMetatype =
      IGF.Builder.CreateBitCast(actorMetatype, IGF.IGM.TypeMetadataPtrTy);

  auto call = IGF.Builder.CreateCall(fn, {actorMetatype});
  call->setCallingConv(IGF.IGM.SwiftCC);
  call->setDoesNotThrow();

  auto result = IGF.Builder.CreateBitCast(call, destType);

  out.add(result);

  return result;
}

namespace {

struct ArgumentDecoderInfo {
  /// The instance of the decoder this information belongs to.
  llvm::Value *Decoder;

  /// The pointer to `decodeNextArgument` method which
  /// could be used to form a call to it.
  FunctionPointer MethodPtr;

  /// The type of `decodeNextArgument` method.
  CanSILFunctionType MethodType;

  /// Protocol requirements associated with the generic
  /// parameter `Argument` of this decode method.
  GenericSignature::RequiredProtocols ProtocolRequirements;

  // Witness metadata for conformance to DistributedTargetInvocationDecoder
  // protocol.
  WitnessMetadata Witness;

  ArgumentDecoderInfo(llvm::Value *decoder, llvm::Value *decoderType,
                      llvm::Value *decoderWitnessTable,
                      FunctionPointer decodeNextArgumentPtr,
                      CanSILFunctionType decodeNextArgumentTy)
      : Decoder(decoder), MethodPtr(decodeNextArgumentPtr),
        MethodType(decodeNextArgumentTy),
        ProtocolRequirements(findProtocolRequirements(decodeNextArgumentTy)) {
    Witness.SelfMetadata = decoderType;
    Witness.SelfWitnessTable = decoderWitnessTable;
  }

  CanSILFunctionType getMethodType() const { return MethodType; }

  ArrayRef<ProtocolDecl *> getProtocolRequirements() const {
    return ProtocolRequirements;
  }

  /// Form a callee to a decode method - `decodeNextArgument`.
  Callee getCallee() const;

private:
  static GenericSignature::RequiredProtocols
  findProtocolRequirements(CanSILFunctionType decodeMethodTy) {
    auto signature = decodeMethodTy->getInvocationGenericSignature();
    auto genericParams = signature.getGenericParams();

    // func decodeNextArgument<Arg : #SerializationRequirement#>() throws -> Arg
    assert(genericParams.size() == 1);
    return signature->getRequiredProtocols(genericParams.front());
  }
};

class DistributedAccessor {
  IRGenModule &IGM;
  IRGenFunction &IGF;

  /// Underlying distributed method for this accessor.
  SILFunction *Target;

  /// The interface type of this accessor function.
  CanSILFunctionType AccessorType;
  /// The asynchronous context associated with this accessor.
  AsyncContextLayout AsyncLayout;

  /// The list of all arguments that were allocated on the stack.
  SmallVector<StackAddress, 4> AllocatedArguments;

  /// The list of all the arguments that were loaded.
  SmallVector<std::pair<Address, /*type=*/llvm::Value *>, 4> LoadedArguments;

public:
  DistributedAccessor(IRGenFunction &IGF, SILFunction *target,
                      CanSILFunctionType accessorTy);

  void emit();

private:
  void decodeArguments(const ArgumentDecoderInfo &decoder,
                       llvm::Value *argumentTypes, Explosion &arguments);

  /// Load an argument value from the given decoder \c decoder
  /// to the given explosion \c arguments. Information describing
  /// the type of argument comes from runtime metadata.
  void decodeArgument(unsigned argumentIdx, const ArgumentDecoderInfo &decoder,
                      llvm::Value *argumentType, const SILParameterInfo &param,
                      Explosion &arguments);

  void lookupWitnessTables(llvm::Value *value,
                           ArrayRef<ProtocolDecl *> protocols,
                           Explosion &witnessTables);

  /// Load witness table addresses (if any) from the given buffer
  /// into the given argument explosion.
  ///
  /// Number of witnesses to load is provided by \c numTables but
  /// it's checked against the number of \c expectedWitnessTables.
  void emitLoadOfWitnessTables(llvm::Value *witnessTables,
                               llvm::Value *numTables,
                               unsigned expectedWitnessTables,
                               Explosion &arguments);

  /// Emit an async return from accessor which does cleanup of
  /// all the argument allocations.
  void emitReturn(llvm::Value *errorValue);

  FunctionPointer getPointerToTarget() const;

  Callee getCalleeForDistributedTarget(llvm::Value *self) const;

  /// Given an instance of invocation decoder, its type metadata,
  /// and protocol witness table, find `decodeNextArgument`.
  ArgumentDecoderInfo findArgumentDecoder(llvm::Value *decoder,
                                          llvm::Value *decoderTy,
                                          llvm::Value *witnessTable);

  /// The result type of the accessor.
  SILType getResultType() const;

  /// The error type of this accessor.
  SILType getErrorType() const;
};

} // end namespace

static NominalTypeDecl *getDistributedActorOf(SILFunction *thunk) {
  assert(thunk->isDistributed() && thunk->isThunk());
  return thunk->getDeclContext()
      ->getInnermostTypeContext()
      ->getSelfNominalTypeDecl();
}

/// Compute a type of a distributed method accessor function based
/// on the provided distributed target.
static CanSILFunctionType getAccessorType(IRGenModule &IGM,
                                          SILFunction *Target) {
  auto &Context = IGM.Context;

  // func __accessor__<D: DistributedTargetInvocationDecoder>(
  //   inout D, <- invocation decoder
  //   UnsafeRawPointer,  <- argument types
  //   UnsafeRawPointer,  <- result buffer
  //   UnsafeRawPointer?, <- generic parameter substitutions
  //   UnsafeRawPointer?, <- witness tables
  //   UInt,              <- number of witness tables
  //   <actor>
  // ) async throws

  SmallVector<GenericFunctionType::Param, 8> parameters;

  // A generic parameter that represents instance of invocation decoder.
  auto *decoderType =
      GenericTypeParamType::get(/*isParameterPack=*/false,
                                /*depth=*/1, /*index=*/0, Context);

  // decoder
  parameters.push_back(GenericFunctionType::Param(
      decoderType,
      /*label=*/Identifier(),
      /*flags=*/ParameterTypeFlags().withInOut(true)));

  // argument type buffer
  parameters.push_back(
      GenericFunctionType::Param(Context.getUnsafeRawPointerType()));

  // result buffer
  parameters.push_back(
      GenericFunctionType::Param(Context.getUnsafeRawPointerType()));

  // generic parameter substitutions
  parameters.push_back(
      GenericFunctionType::Param(Context.getUnsafeRawPointerType()));

  // witness tables
  parameters.push_back(
      GenericFunctionType::Param(Context.getUnsafeRawPointerType()));

  // number of witness tables
  parameters.push_back(GenericFunctionType::Param(Context.getUIntType()));

  // actor
  {
    auto targetTy = Target->getLoweredFunctionType();
    auto actorLoc = targetTy->getParameters().back();

    parameters.push_back(
        GenericFunctionType::Param(actorLoc.getInterfaceType()));
  }

  auto decoderProtocolTy =
      Context
          .getProtocol(KnownProtocolKind::DistributedTargetInvocationDecoder)
          ->getDeclaredInterfaceType();

  // Build generic signature that includes all contextual generic parameters.
  GenericSignature signature;
  {
    SmallVector<GenericTypeParamType *, 4> genericParams;
    SmallVector<Requirement, 4> genericRequirements;

    auto *actor = getDistributedActorOf(Target);
    assert(actor);

    for (auto *genericParam : actor->getInnermostGenericParamTypes())
      genericParams.push_back(genericParam);

    // Add a generic parameter `D` which stands for decoder type in the
    // accessor signature - `inout D`.
    genericParams.push_back(decoderType);
    // Add a requirement that decoder conforms to the expected protocol.
    genericRequirements.push_back(
        {RequirementKind::Conformance, decoderType, decoderProtocolTy});

    signature = GenericSignature::get(genericParams, genericRequirements);
  }

  auto accessorTy = GenericFunctionType::get(
      signature, parameters, Context.TheEmptyTupleType,
      ASTExtInfoBuilder()
          .withRepresentation(FunctionTypeRepresentation::Thin)
          .withAsync()
          .withThrows()
          .build());

  return IGM.getLoweredType(accessorTy).castTo<SILFunctionType>();
}

llvm::Function *
IRGenModule::getAddrOfDistributedTargetAccessor(SILFunction *F,
                                                ForDefinition_t forDefinition) {
  auto entity = LinkEntity::forDistributedTargetAccessor(F);

  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition)
      updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  Signature signature = getSignature(getAccessorType(*this, F));
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);

  return createFunction(*this, link, signature);
}

void IRGenModule::emitDistributedTargetAccessor(SILFunction *target) {
  assert(target->isDistributed());

  auto *f = getAddrOfDistributedTargetAccessor(target, ForDefinition);
  if (!f->isDeclaration())
    return;

  IRGenFunction IGF(*this, f);
  DistributedAccessor(IGF, target, getAccessorType(*this, target)).emit();
}

DistributedAccessor::DistributedAccessor(IRGenFunction &IGF,
                                         SILFunction *target,
                                         CanSILFunctionType accessorTy)
    : IGM(IGF.IGM), IGF(IGF), Target(target), AccessorType(accessorTy),
      AsyncLayout(getAsyncContextLayout(
          IGM, AccessorType, AccessorType, SubstitutionMap())) {
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(IGF, IGF.CurFn);
}

void DistributedAccessor::decodeArguments(const ArgumentDecoderInfo &decoder,
                                          llvm::Value *argumentTypes,
                                          Explosion &arguments) {
  auto fnType = Target->getLoweredFunctionType();

  // Cover all of the arguments except to `self` of the actor.
  auto parameters = fnType->getParameters().drop_back();

  // If there are no parameters to extract, we are done.
  if (parameters.empty())
    return;

  // Cast type buffer to `swift.type**`
  argumentTypes =
      IGF.Builder.CreateBitCast(argumentTypes, IGM.TypeMetadataPtrPtrTy);

  for (unsigned i = 0, n = parameters.size(); i != n; ++i) {
    const auto &param = parameters[i];
    auto paramTy = param.getSILStorageInterfaceType();

    // Check whether the native representation is empty e.g.
    // this happens for empty enums, and if so - continue to
    // the next argument.
    if (paramTy.isObject()) {
      auto &typeInfo = IGM.getTypeInfo(paramTy);
      auto &nativeSchema = typeInfo.nativeParameterValueSchema(IGM);

      if (nativeSchema.empty())
        continue;
    }

    auto offset =
        Size(i * IGM.DataLayout.getTypeAllocSize(IGM.TypeMetadataPtrTy));
    auto alignment = IGM.DataLayout.getABITypeAlignment(IGM.TypeMetadataPtrTy);

    // Load metadata describing argument value from argument types buffer.
    auto typeLoc = IGF.emitAddressAtOffset(
        argumentTypes, Offset(offset), IGM.TypeMetadataPtrTy,
        Alignment(alignment), "arg_type_loc");

    auto *argumentTy = IGF.Builder.CreateLoad(typeLoc, "arg_type");

    // Decode and load argument value using loaded type metadata.
    decodeArgument(i, decoder, argumentTy, param, arguments);
  }
}

void DistributedAccessor::decodeArgument(unsigned argumentIdx,
                                         const ArgumentDecoderInfo &decoder,
                                         llvm::Value *argumentType,
                                         const SILParameterInfo &param,
                                         Explosion &arguments) {
  auto &paramInfo = IGM.getTypeInfo(param.getSILStorageInterfaceType());
  // TODO: `emitLoad*` would actually load value witness table every
  // time it's called, which is sub-optimal but all of the APIs that
  // deal with value witness tables are currently hidden in GenOpaque.cpp
  llvm::Value *valueSize = emitLoadOfSize(IGF, argumentType);

  Callee callee = decoder.getCallee();

  std::unique_ptr<CallEmission> emission =
      getCallEmission(IGF, callee.getSwiftContext(), std::move(callee));

  StackAddress resultValue = IGF.emitDynamicAlloca(
      IGM.Int8Ty, valueSize, paramInfo.getBestKnownAlignment());

  llvm::Value *resultAddr = resultValue.getAddress().getAddress();

  resultAddr = IGF.Builder.CreateBitCast(resultAddr, IGM.OpaquePtrTy);

  Explosion decodeArgs;
  // indirect result buffer as `swift.opaque*`
  decodeArgs.add(resultAddr);
  // substitution Argument -> <argument metadata>
  decodeArgs.add(argumentType);

  // Lookup witness tables for the requirement on the argument type.
  lookupWitnessTables(argumentType, decoder.getProtocolRequirements(),
                      decodeArgs);

  Address calleeErrorSlot;
  llvm::Value *decodeError = nullptr;

  emission->begin();
  {
    emission->setArgs(decodeArgs, /*isOutlined=*/false,
                      /*witnessMetadata=*/nullptr);

    Explosion result;
    emission->emitToExplosion(result, /*isOutlined=*/false);
    assert(result.empty());

    // Load error from the slot to emit an early return if necessary.
    {
      SILFunctionConventions conv(decoder.getMethodType(), IGM.getSILModule());
      SILType errorType =
          conv.getSILErrorType(IGM.getMaximalTypeExpansionContext());

      calleeErrorSlot =
          emission->getCalleeErrorSlot(errorType, /*isCalleeAsync=*/true);
      decodeError = IGF.Builder.CreateLoad(calleeErrorSlot);
    }
  }
  emission->end();

  // Remember to deallocate later.
  AllocatedArguments.push_back(resultValue);

  // Check whether the error slot has been set and if so
  // emit an early return from accessor.
  {
    auto contBB = IGF.createBasicBlock("");
    auto errorBB = IGF.createBasicBlock("on-error");

    auto nullError = llvm::Constant::getNullValue(decodeError->getType());
    auto hasError = IGF.Builder.CreateICmpNE(decodeError, nullError);

    IGF.Builder.CreateCondBr(hasError, errorBB, contBB);
    {
      IGF.Builder.emitBlock(errorBB);
      // Emit an early return if argument decoding failed.
      emitReturn(decodeError);
    }

    IGF.Builder.emitBlock(contBB);
    // Reset value of the slot back to `null`
    IGF.Builder.CreateStore(nullError, calleeErrorSlot);
  }

  switch (param.getConvention()) {
  case ParameterConvention::Indirect_In: {
    // The only way to load opaque type is to allocate a temporary
    // variable on the stack for it and initialize from the given address
    // either at +0 or +1 depending on convention.

    auto stackAddr =
        IGF.emitDynamicAlloca(IGM.Int8Ty, valueSize, Alignment(16));

    emitInitializeWithCopyCall(IGF, argumentType, stackAddr.getAddress(),
                               resultValue.getAddress());

    // Remember to deallocate a copy.
    AllocatedArguments.push_back(stackAddr);
    break;
  }

  case ParameterConvention::Indirect_In_Guaranteed: {
    // The argument is +0, so we can use the address of the param in
    // the context directly.
    arguments.add(resultAddr);
    break;
  }

  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
    llvm_unreachable("indirect 'inout' parameters are not supported");

  case ParameterConvention::Pack_Guaranteed:
  case ParameterConvention::Pack_Owned:
  case ParameterConvention::Pack_Inout:
    llvm_unreachable("pack parameters are not supported");

  case ParameterConvention::Direct_Guaranteed:
  case ParameterConvention::Direct_Unowned: {
    auto paramTy = param.getSILStorageInterfaceType();
    Address eltPtr = IGF.Builder.CreateElementBitCast(
        resultValue.getAddress(), IGM.getStorageType(paramTy));

    cast<LoadableTypeInfo>(paramInfo).loadAsTake(IGF, eltPtr, arguments);
    LoadedArguments.push_back(std::make_pair(eltPtr, argumentType));
    break;
  }

  case ParameterConvention::Direct_Owned: {
    // Copy the value out at +1.
    cast<LoadableTypeInfo>(paramInfo).loadAsCopy(IGF, resultValue.getAddress(),
                                                 arguments);
    LoadedArguments.push_back(
        std::make_pair(resultValue.getAddress(), argumentType));
    break;
  }
  }
}

void DistributedAccessor::lookupWitnessTables(
    llvm::Value *value, ArrayRef<ProtocolDecl *> protocols,
    Explosion &witnessTables) {
  auto conformsToProtocol = IGM.getConformsToProtocolFunctionPointer();

  for (auto *protocol : protocols) {
    auto *protocolDescriptor = IGM.getAddrOfProtocolDescriptor(protocol);
    auto *witnessTable =
        IGF.Builder.CreateCall(conformsToProtocol, {value, protocolDescriptor});

    auto failBB = IGF.createBasicBlock("missing-witness");
    auto contBB = IGF.createBasicBlock("");

    auto isNull = IGF.Builder.CreateICmpEQ(
        witnessTable, llvm::ConstantPointerNull::get(IGM.WitnessTablePtrTy));
    IGF.Builder.CreateCondBr(isNull, failBB, contBB);

    // This operation shouldn't fail because runtime should have checked that
    // a particular argument type conforms to `SerializationRequirement`
    // of the distributed actor the decoder is used for. If it does fail
    // then accessor should trap.
    {
      IGF.Builder.emitBlock(failBB);
      IGF.emitTrap("missing witness table", /*EmitUnreachable=*/true);
    }

    IGF.Builder.emitBlock(contBB);
    witnessTables.add(witnessTable);
  }
}

void DistributedAccessor::emitLoadOfWitnessTables(llvm::Value *witnessTables,
                                                  llvm::Value *numTables,
                                                  unsigned expectedWitnessTables,
                                                  Explosion &arguments) {
  auto contBB = IGF.createBasicBlock("");
  auto unreachableBB = IGF.createBasicBlock("incorrect-witness-tables");

  auto incorrectNum = IGF.Builder.CreateICmpNE(
      numTables, llvm::ConstantInt::get(IGM.SizeTy, expectedWitnessTables));

  // Make sure that we have a correct number of witness tables provided to us.
  IGF.Builder.CreateCondBr(incorrectNum, unreachableBB, contBB);
  {
    IGF.Builder.emitBlock(unreachableBB);
    IGF.Builder.CreateUnreachable();
  }

  IGF.Builder.emitBlock(contBB);

  witnessTables = IGF.Builder.CreateBitCast(witnessTables,
                                            IGM.Int8PtrPtrTy->getPointerTo());

  for (unsigned i = 0, n = expectedWitnessTables; i != n; ++i) {
    auto offset = Size(i * IGM.getPointerSize());
    auto alignment = IGM.getPointerAlignment();

    auto witnessTableAddr = IGF.emitAddressAtOffset(
        witnessTables, Offset(offset), IGM.Int8PtrPtrTy, Alignment(alignment));

    arguments.add(IGF.Builder.CreateLoad(witnessTableAddr));
  }
}

void DistributedAccessor::emitReturn(llvm::Value *errorValue) {
  // Destroy loaded arguments.
  // This MUST be done before deallocating, as otherwise we'd try to
  // swift_release freed memory, which will be a no-op, however that also would
  // mean we never drop retain counts to 0 and miss to run deinitializers of
  // classes!
  llvm::for_each(LoadedArguments, [&](const auto &argInfo) {
    emitDestroyCall(IGF, argInfo.second, argInfo.first);
  });

  // Deallocate all of the copied arguments. Since allocations happened
  // on stack they have to be deallocated in reverse order.
  {
    for (auto alloca = AllocatedArguments.rbegin();
         alloca != AllocatedArguments.rend(); ++alloca) {
      IGF.emitDeallocateDynamicAlloca(*alloca);
    }
  }

  Explosion voidResult;

  Explosion error;
  error.add(errorValue);

  emitAsyncReturn(IGF, AsyncLayout, getResultType(), AccessorType, voidResult,
                  error);
}

void DistributedAccessor::emit() {
  auto *actor = getDistributedActorOf(Target);
  auto targetTy = Target->getLoweredFunctionType();
  SILFunctionConventions targetConv(targetTy, IGF.getSILModule());
  TypeExpansionContext expansionContext = IGM.getMaximalTypeExpansionContext();

  auto params = IGF.collectParameters();

  auto directResultTy = targetConv.getSILResultType(expansionContext);
  const auto &directResultTI = IGM.getTypeInfo(directResultTy);

  Explosion arguments;

  unsigned numAsyncContextParams =
      (unsigned)AsyncFunctionArgumentIndex::Context + 1;
  (void)params.claim(numAsyncContextParams);

  // A container that produces argument values based on the given set of
  // argument types (supplied as a next argument).
  auto *argDecoder = params.claimNext();
  // `swift.type**` that holds the argument types that correspond to values.
  auto *argTypes = params.claimNext();
  // UnsafeRawPointer that is used to store the result.
  auto *resultBuffer = params.claimNext();
  // UnsafeRawPointer that represents a list of substitutions
  auto *substitutions = params.claimNext();
  // UnsafeRawPointer that represents a list of witness tables
  auto *witnessTables = params.claimNext();
  // Integer that represented the number of witness tables
  auto *numWitnessTables = params.claimNext();
  // Reference to a `self` of the actor to be called.
  auto *actorSelf = params.claimNext();
  // Metadata that represents passed in the invocation decoder.
  auto *decoderType = params.claimNext();

  // If the distributed thunk is declared in a protocol that conforms
  // to `DistributedActor` protocol, there is an extract parameter that
  // represents a type of protocol witness.
  if (isa<ProtocolDecl>(actor))
    (void)params.claimNext();

  // Witness table for decoder conformance to DistributedTargetInvocationDecoder
  auto *decoderProtocolWitness = params.claimNext();

  GenericContextScope scope(IGM, targetTy->getInvocationGenericSignature());

  // Preliminary: Setup async context for this accessor.
  {
    auto fpKind = FunctionPointerKind::defaultAsync();
    auto asyncContextIdx =
        Signature::forAsyncEntry(IGM, AccessorType, fpKind)
            .getAsyncContextIndex();

    auto entity = LinkEntity::forDistributedTargetAccessor(Target);
    emitAsyncFunctionEntry(IGF, AsyncLayout, entity, asyncContextIdx);
    emitAsyncFunctionPointer(IGM, IGF.CurFn, entity, AsyncLayout.getSize());
  }

  auto *typedResultBuffer = IGF.Builder.CreateBitCast(
    resultBuffer, IGM.getStoragePointerType(directResultTy));

  if (targetConv.getNumIndirectSILResults()) {
    // Since tuples are not allowed as valid result types (because they cannot
    // conform to protocols), there could be only a single indirect result type
    // associated with distributed method.
    assert(targetConv.getNumIndirectSILResults() == 1);
    arguments.add(typedResultBuffer);
  }

  // There is always at least one parameter associated with accessor - `self`
  // of the distributed actor.
  if (targetTy->getNumParameters() > 1) {
    /// The argument decoder associated with the distributed actor
    /// this accessor belong to.
    ArgumentDecoderInfo decoder =
        findArgumentDecoder(argDecoder, decoderType, decoderProtocolWitness);

    // Step one is to load all of the data from argument buffer,
    // so it could be forwarded to the distributed method.
    decodeArguments(decoder, argTypes, arguments);
  }

  // Add all of the substitutions to the explosion
  if (auto *genericEnvironment = Target->getGenericEnvironment()) {
    // swift.type **
    llvm::Value *substitutionBuffer =
        IGF.Builder.CreateBitCast(substitutions, IGM.TypeMetadataPtrPtrTy);

    // Collect the generic arguments expected by the distributed thunk.
    // We need this to determine the expected number of witness tables
    // to load from the buffer provided by the caller.
    llvm::SmallVector<llvm::Type *, 4> targetGenericArguments;
    auto expandedSignature =
        expandPolymorphicSignature(IGM, targetTy, targetGenericArguments);
    assert(expandedSignature.numShapes == 0 &&
           "Distributed actors don't support variadic generics");

    // Generic arguments associated with the distributed thunk directly
    // e.g. `distributed func echo<T, U>(...)`
    assert(
        !IGM.getLLVMContext().supportsTypedPointers() ||
        expandedSignature.numTypeMetadataPtrs ==
            llvm::count_if(targetGenericArguments, [&](const llvm::Type *type) {
              return type == IGM.TypeMetadataPtrTy;
            }));

    for (unsigned index = 0; index < expandedSignature.numTypeMetadataPtrs; ++index) {
      auto offset =
          Size(index * IGM.DataLayout.getTypeAllocSize(IGM.TypeMetadataPtrTy));
      auto alignment =
          IGM.DataLayout.getABITypeAlignment(IGM.TypeMetadataPtrTy);

      auto substitution =
          IGF.emitAddressAtOffset(substitutionBuffer, Offset(offset),
                                  IGM.TypeMetadataPtrTy, Alignment(alignment));
      arguments.add(IGF.Builder.CreateLoad(substitution, "substitution"));
    }

    emitLoadOfWitnessTables(witnessTables, numWitnessTables,
                            expandedSignature.numWitnessTablePtrs, arguments);
  }

  // Step two, let's form and emit a call to the distributed method
  // using computed argument explosion.
  {
    Explosion result;
    llvm::Value *targetError = nullptr;

    auto callee = getCalleeForDistributedTarget(actorSelf);
    auto emission =
        getCallEmission(IGF, callee.getSwiftContext(), std::move(callee));

    emission->begin();
    emission->setArgs(arguments, /*isOutlined=*/false,
                      /*witnessMetadata=*/nullptr);

    // Load result of the thunk into the location provided by the caller.
    // This would only generate code for direct results, if thunk has an
    // indirect result (e.g. large struct) it result buffer would be passed
    // as an argument.
    {
      Address resultAddr(typedResultBuffer, directResultTI.getStorageType(),
                         directResultTI.getBestKnownAlignment());
      emission->emitToMemory(resultAddr, cast<LoadableTypeInfo>(directResultTI),
                             /*isOutlined=*/false);
    }

    // Both accessor and distributed method are always `async throws`
    // so we need to load error value (if any) from the slot.
    {
      assert(targetTy->hasErrorResult());

      Address calleeErrorSlot =
          emission->getCalleeErrorSlot(getErrorType(), /*isCalleeAsync=*/true);
      targetError = IGF.Builder.CreateLoad(calleeErrorSlot);
    }

    emission->end();

    // Emit an async return that does allocation cleanup and propagates error
    // (if any) back to the caller.
    emitReturn(targetError);
  }
}

FunctionPointer DistributedAccessor::getPointerToTarget() const {
  auto fnType = Target->getLoweredFunctionType();
  auto fpKind = classifyFunctionPointerKind(Target);
  auto signature = IGM.getSignature(fnType, fpKind);

  auto *fnPtr =
    llvm::ConstantExpr::getBitCast(IGM.getAddrOfAsyncFunctionPointer(Target),
                                   signature.getType()->getPointerTo());

  return FunctionPointer::forDirect(
      FunctionPointer::Kind(fnType), fnPtr,
      IGM.getAddrOfSILFunction(Target, NotForDefinition), signature);
}

Callee
DistributedAccessor::getCalleeForDistributedTarget(llvm::Value *self) const {
  auto fnType = Target->getLoweredFunctionType();
  CalleeInfo info{fnType, fnType, SubstitutionMap()};
  return {std::move(info), getPointerToTarget(), self};
}

ArgumentDecoderInfo DistributedAccessor::findArgumentDecoder(
    llvm::Value *decoder, llvm::Value *decoderTy, llvm::Value *witnessTable) {
  auto *actor = getDistributedActorOf(Target);
  auto expansionContext = IGM.getMaximalTypeExpansionContext();

  auto *decodeFn = IGM.Context.getDistributedActorArgumentDecodingMethod(actor);
  assert(decodeFn && "no suitable decoder?");

  auto methodTy = IGM.getSILTypes().getConstantFunctionType(
      expansionContext, SILDeclRef(decodeFn));

  auto fpKind = FunctionPointerKind::defaultAsync();
  auto signature = IGM.getSignature(methodTy, fpKind);

  // If the decoder class is `final`, let's emit a direct reference.
  auto *decoderDecl = decodeFn->getDeclContext()->getSelfNominalTypeDecl();

  // If decoder is a class, need to load it first because generic parameter
  // is passed indirectly. This is good for structs and enums because
  // `decodeNextArgument` is a mutating method, but not for classes because
  // in that case heap object is mutated directly.
  if (isa<ClassDecl>(decoderDecl)) {
    auto selfTy = methodTy->getSelfParameter().getSILStorageType(
        IGM.getSILModule(), methodTy, expansionContext);

    auto &classTI = IGM.getTypeInfo(selfTy).as<ClassTypeInfo>();
    auto &classLayout = classTI.getClassLayout(IGM, selfTy,
                                               /*forBackwardDeployment=*/false);

    llvm::Value *typedDecoderPtr = IGF.Builder.CreateBitCast(
        decoder, classLayout.getType()->getPointerTo()->getPointerTo());

    Explosion instance;

    classTI.loadAsTake(IGF,
                       {typedDecoderPtr, classTI.getStorageType(),
                        classTI.getBestKnownAlignment()},
                       instance);

    decoder = instance.claimNext();
  }

  auto *decodeSIL = IGM.getSILModule().lookUpFunction(SILDeclRef(decodeFn));
  auto *fnPtr = IGM.getAddrOfSILFunction(decodeSIL, NotForDefinition,
                                         /*isDynamicallyReplaceable=*/false);

  auto methodPtr = FunctionPointer::forDirect(
    classifyFunctionPointerKind(decodeSIL), fnPtr,
    /*secondaryValue=*/nullptr, signature);

  return {decoder, decoderTy, witnessTable, methodPtr, methodTy};
}

SILType DistributedAccessor::getResultType() const {
  SILFunctionConventions conv(AccessorType, IGF.getSILModule());
  return conv.getSILResultType(IGM.getMaximalTypeExpansionContext());
}

SILType DistributedAccessor::getErrorType() const {
  SILFunctionConventions conv(AccessorType, IGF.getSILModule());
  return conv.getSILErrorType(IGM.getMaximalTypeExpansionContext());
}

Callee ArgumentDecoderInfo::getCallee() const {
  CalleeInfo info(MethodType, MethodType, SubstitutionMap());
  return {std::move(info), MethodPtr, Decoder};
}
