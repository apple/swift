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
#include "IRGenMangler.h"
#include "IRGenModule.h"
#include "LoadableTypeInfo.h"
#include "ScalarPairTypeInfo.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/AST/DistributedDecl.h"
#include "swift/AST/ExtInfo.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/Basic/Assertions.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/SILFunction.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/Alignment.h"

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

using ThunkOrRequirement = llvm::PointerUnion<SILFunction *, AbstractFunctionDecl *>;

static LinkEntity
getAccessorLinking(ThunkOrRequirement accessorFor) {
  if (auto *method = accessorFor.dyn_cast<SILFunction *>()) {
    assert(method->isDistributed());
    return LinkEntity::forDistributedTargetAccessor(method);
  }

  auto *requirement = accessorFor.get<AbstractFunctionDecl *>();
  return LinkEntity::forDistributedTargetAccessor(requirement);
}

struct ArgumentDecoderInfo {
  /// The instance of the decoder this information belongs to.
  llvm::Value *Decoder;

  /// The pointer to `decodeNextArgument` method which
  /// could be used to form a call to it.
  FunctionPointer MethodPtr;

  /// The type of `decodeNextArgument` method.
  CanSILFunctionType MethodType;

  /// Witness metadata for conformance to DistributedTargetInvocationDecoder
  /// protocol.
  WitnessMetadata Witness;

  /// Indicates whether `decodeNextArgument` is referenced through
  /// a protocol witness thunk.
  bool UsesWitnessDispatch;

  ArgumentDecoderInfo(llvm::Value *decoder, llvm::Value *decoderType,
                      llvm::Value *decoderWitnessTable,
                      FunctionPointer decodeNextArgumentPtr,
                      CanSILFunctionType decodeNextArgumentTy,
                      bool usesWitnessDispatch)
      : Decoder(decoder), MethodPtr(decodeNextArgumentPtr),
        MethodType(decodeNextArgumentTy),
        UsesWitnessDispatch(usesWitnessDispatch) {
    Witness.SelfMetadata = decoderType;
    Witness.SelfWitnessTable = decoderWitnessTable;
  }

  CanSILFunctionType getMethodType() const { return MethodType; }

  WitnessMetadata *getWitnessMetadata() const {
    return const_cast<WitnessMetadata *>(&Witness);
  }

  /// Protocol requirements associated with the generic
  /// parameter `Argument` of this decode method.
  GenericSignature::RequiredProtocols getProtocolRequirements() const {
    if (UsesWitnessDispatch)
      return {};

    auto signature = MethodType->getInvocationGenericSignature();
    auto genericParams = signature.getGenericParams();

    // func decodeNextArgument<Arg : #SerializationRequirement#>() throws -> Arg
    assert(genericParams.size() == 1);
    return signature->getRequiredProtocols(genericParams.front());
  }

  /// Form a callee to a decode method - `decodeNextArgument`.
  Callee getCallee() const;
};

struct AccessorTarget {
private:
  IRGenFunction &IGF;
  ThunkOrRequirement Target;

  CanSILFunctionType Type;

  mutable std::optional<WitnessMetadata> Witness;

public:
  AccessorTarget(IRGenFunction &IGF, ThunkOrRequirement target)
      : IGF(IGF), Target(target) {
    if (auto *thunk = target.dyn_cast<SILFunction *>()) {
      Type = thunk->getLoweredFunctionType();
    } else {
      auto *requirement = target.get<AbstractFunctionDecl *>();
      Type = IGF.IGM.getSILTypes().getConstantFunctionType(
          IGF.IGM.getMaximalTypeExpansionContext(),
          SILDeclRef(requirement).asDistributed());
    }
  }

  DeclContext *getDeclContext() const {
    if (auto *thunk = Target.dyn_cast<SILFunction *>())
      return thunk->getDeclContext();
    return Target.get<AbstractFunctionDecl *>();
  }

  CanSILFunctionType getType() const { return Type; }

  bool isGeneric() const {
    auto sig = Type->getInvocationGenericSignature();
    return sig && !sig->areAllParamsConcrete();
  }

  Callee getCallee(llvm::Value *actorSelf);

  LinkEntity getLinking() const { return getAccessorLinking(Target); }

  /// Witness metadata is computed lazily upon the first request.
  WitnessMetadata *getWitnessMetadata(llvm::Value *actorSelf);

private:
  FunctionPointer getPointerToTarget(llvm::Value *actorSelf);

  llvm::Value *emitMetadataRef(llvm::Value *actorSelf) const;
};

class DistributedAccessor {
  IRGenModule &IGM;
  IRGenFunction &IGF;

  /// Underlying distributed method for this accessor.
  AccessorTarget Target;

  /// The interface type of this accessor function.
  CanSILFunctionType AccessorType;
  /// The asynchronous context associated with this accessor.
  AsyncContextLayout AsyncLayout;

  /// The list of all arguments that were allocated on the stack.
  SmallVector<StackAddress, 4> AllocatedArguments;

  /// The list of all the arguments that were loaded.
  SmallVector<std::pair<Address, /*type=*/llvm::Value *>, 4> LoadedArguments;

public:
  DistributedAccessor(IRGenFunction &IGF, ThunkOrRequirement target,
                      CanSILFunctionType accessorTy);

  CanSILFunctionType getTargetType() const { return Target.getType(); }

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

/// Compute a type of a distributed method accessor function based
/// on the provided distributed target.
static CanSILFunctionType getAccessorType(IRGenModule &IGM) {
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
                                /*depth=*/0, /*index=*/0, Context);

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

  auto actorTypeParam = Context.getAnyObjectType();
    parameters.push_back(
        GenericFunctionType::Param(actorTypeParam));

  auto decoderProtocolTy =
      Context
          .getProtocol(KnownProtocolKind::DistributedTargetInvocationDecoder)
          ->getDeclaredInterfaceType();

  // Build generic signature that includes all contextual generic parameters.
  GenericSignature signature;
  {
    SmallVector<GenericTypeParamType *, 4> genericParams;
    SmallVector<Requirement, 4> genericRequirements;

    // Add a generic parameter `D` which stands for decoder type in the
    // accessor signature - `inout D`.
    genericParams.push_back(decoderType);
    // Add a requirement that decoder conforms to the expected protocol.
    genericRequirements.push_back(
        {RequirementKind::Conformance, decoderType, decoderProtocolTy});

    signature = buildGenericSignature(Context, GenericSignature(),
                                      std::move(genericParams),
                                      std::move(genericRequirements),
                                      /*allowInverses=*/true);
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
IRGenModule::getAddrOfDistributedTargetAccessor(LinkEntity accessor,
                                                ForDefinition_t forDefinition) {
  llvm::Function *&entry = GlobalFuncs[accessor];
  if (entry) {
    if (forDefinition)
      updateLinkageForDefinition(*this, entry, accessor);
    return entry;
  }

  Signature signature = getSignature(getAccessorType(*this));
  LinkInfo link = LinkInfo::get(*this, accessor, forDefinition);

  return createFunction(*this, link, signature);
}

void IRGenModule::emitDistributedTargetAccessor(ThunkOrRequirement target) {
  LinkEntity accessorRef = getAccessorLinking(target);
  auto *f = getAddrOfDistributedTargetAccessor(accessorRef,
                                               ForDefinition);

  if (!f->isDeclaration())
    return;

  IRGenFunction IGF(*this, f);
  auto accessor = DistributedAccessor(IGF, target, getAccessorType(*this));
  accessor.emit();

  auto targetDecl = cast<AbstractFunctionDecl>(accessorRef.getDecl());

  IRGenMangler mangler;

  addAccessibleFunction(AccessibleFunction::forDistributed(
      mangler.mangleDistributedThunkRecord(targetDecl),
      mangler.mangleDistributedThunk(targetDecl),
      accessor.getTargetType(),
      getAddrOfAsyncFunctionPointer(accessorRef)));
}

DistributedAccessor::DistributedAccessor(IRGenFunction &IGF,
                                         ThunkOrRequirement target,
                                         CanSILFunctionType accessorTy)
    : IGM(IGF.IGM), IGF(IGF), Target(IGF, target), AccessorType(accessorTy),
      AsyncLayout(getAsyncContextLayout(IGM, AccessorType, AccessorType,
                                        SubstitutionMap())) {
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(IGF, IGF.CurFn);
}

void DistributedAccessor::decodeArguments(const ArgumentDecoderInfo &decoder,
                                          llvm::Value *argumentTypes,
                                          Explosion &arguments) {
  auto fnType = Target.getType();

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

    Size offset =
        Size(i * IGM.DataLayout.getTypeAllocSize(IGM.TypeMetadataPtrTy));
    llvm::Align alignment = IGM.DataLayout.getABITypeAlign(IGM.TypeMetadataPtrTy);

    // Load metadata describing argument value from argument types buffer.
    auto typeLoc = IGF.emitAddressAtOffset(
        argumentTypes, Offset(offset), IGM.TypeMetadataPtrTy,
        Alignment(alignment.value()), "arg_type_loc");

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
                      decoder.UsesWitnessDispatch ? decoder.getWitnessMetadata()
                                                  : nullptr);

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
  case ParameterConvention::Indirect_In_CXX:
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
    // Don't forget to actually store the argument
    arguments.add(stackAddr.getAddressPointer());
    break;
  }

  case ParameterConvention::Indirect_In_Guaranteed: {
    // The argument is +0, so we can use the address of the param in
    // the context directly.
    arguments.add(resultAddr);
    LoadedArguments.push_back(std::make_pair(resultValue.getAddress(), argumentType));
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

static llvm::Value *lookupWitnessTable(IRGenFunction &IGF, llvm::Value *witness,
                                       ProtocolDecl *protocol) {
  assert(Lowering::TypeConverter::protocolRequiresWitnessTable(protocol));

  auto &IGM = IGF.IGM;
  auto *protocolDescriptor = IGM.getAddrOfProtocolDescriptor(protocol);
  auto *witnessTable = IGF.Builder.CreateCall(
      IGM.getConformsToProtocolFunctionPointer(), {witness, protocolDescriptor});

  auto failBB = IGF.createBasicBlock("missing-witness");
  auto contBB = IGF.createBasicBlock("");

  auto isNull = IGF.Builder.CreateICmpEQ(
    witnessTable, llvm::ConstantPointerNull::get(IGM.WitnessTablePtrTy));
  IGF.Builder.CreateCondBr(isNull, failBB, contBB);

  // This operation shouldn't fail because the compiler should have
  // checked that the given witness conforms to the protocol. If it
  // does fail then accessor should trap.
  {
    IGF.Builder.emitBlock(failBB);
    IGF.emitTrap("missing witness table", /*EmitUnreachable=*/true);
  }

  IGF.Builder.emitBlock(contBB);

  return witnessTable;
}

void DistributedAccessor::lookupWitnessTables(
    llvm::Value *value, ArrayRef<ProtocolDecl *> protocols,
    Explosion &witnessTables) {
  if (protocols.empty())
    return;

  auto conformsToProtocol = IGM.getConformsToProtocolFunctionPointer();

  for (auto *protocol : protocols) {
    if (!Lowering::TypeConverter::protocolRequiresWitnessTable(protocol))
      continue;

    witnessTables.add(lookupWitnessTable(IGF, value, protocol));
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
  auto targetTy = Target.getType();
  SILFunctionConventions targetConv(targetTy, IGF.getSILModule());
  TypeExpansionContext expansionContext = IGM.getMaximalTypeExpansionContext();

  auto params = IGF.collectParameters();

  GenericContextScope scope(IGM, targetTy->getInvocationGenericSignature());

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

  // Witness table for decoder conformance to DistributedTargetInvocationDecoder
  auto *decoderProtocolWitness = params.claimNext();

  // Preliminary: Setup async context for this accessor.
  {
    auto fpKind = FunctionPointerKind::defaultAsync();
    auto asyncContextIdx =
        Signature::forAsyncEntry(IGM, AccessorType, fpKind)
            .getAsyncContextIndex();

    auto entity = Target.getLinking();
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
  if (Target.isGeneric()) {
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

    for (unsigned index = 0; index < expandedSignature.numTypeMetadataPtrs; ++index) {
      auto offset =
          Size(index * IGM.DataLayout.getTypeAllocSize(IGM.TypeMetadataPtrTy));
      llvm::Align alignment =
          IGM.DataLayout.getABITypeAlign(IGM.TypeMetadataPtrTy);

      auto substitution = IGF.emitAddressAtOffset(
          substitutionBuffer, Offset(offset), IGM.TypeMetadataPtrTy,
          Alignment(alignment.value()));
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

    auto callee = Target.getCallee(actorSelf);
    auto emission =
        getCallEmission(IGF, callee.getSwiftContext(), std::move(callee));

    emission->begin();
    emission->setArgs(arguments, /*isOutlined=*/false,
                      Target.getWitnessMetadata(actorSelf));

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

FunctionPointer AccessorTarget::getPointerToTarget(llvm::Value *actorSelf) {
  auto &IGM = IGF.IGM;

  if (auto *thunk = Target.dyn_cast<SILFunction *>()) {
    auto fpKind = classifyFunctionPointerKind(thunk);
    auto signature = IGM.getSignature(Type, fpKind);

    auto *fnPtr =
        llvm::ConstantExpr::getBitCast(IGM.getAddrOfAsyncFunctionPointer(thunk),
                                       signature.getType()->getPointerTo());

    return FunctionPointer::forDirect(
        FunctionPointer::Kind(Type), fnPtr,
        IGM.getAddrOfSILFunction(thunk, NotForDefinition), signature);
  }

  auto *requirementDecl = Target.get<AbstractFunctionDecl *>();
  auto *protocol = requirementDecl->getDeclContext()->getSelfProtocolDecl();
  SILDeclRef requirementRef = SILDeclRef(requirementDecl).asDistributed();

  if (!IGM.isResilient(protocol, ResilienceExpansion::Maximal)) {
    auto *witness = getWitnessMetadata(actorSelf);
    return emitWitnessMethodValue(IGF, witness->SelfWitnessTable,
                                  requirementRef);
  }

  auto fnPtr = IGM.getAddrOfDispatchThunk(requirementRef, NotForDefinition);
  auto sig = IGM.getSignature(Type);
  return FunctionPointer::forDirect(Type, fnPtr,
                                    /*secondaryValue=*/nullptr, sig, true);
}

llvm::Value *AccessorTarget::emitMetadataRef(llvm::Value *actorSelf) const {
  auto &IGM = IGF.IGM;

  if (!IGM.ObjCInterop) {
    llvm::Value *slot =
      IGF.Builder.CreateBitCast(actorSelf, IGM.TypeMetadataPtrPtrTy);
    return IGF.Builder.CreateLoad(
      Address(slot, IGM.TypeMetadataPtrTy, IGM.getPointerAlignment()));
  }

  return emitHeapMetadataRefForUnknownHeapObject(IGF, actorSelf);
}

Callee AccessorTarget::getCallee(llvm::Value *actorSelf) {
  CalleeInfo info{Type, Type, SubstitutionMap()};
  return {std::move(info), getPointerToTarget(actorSelf), actorSelf};
}

WitnessMetadata *AccessorTarget::getWitnessMetadata(llvm::Value *actorSelf) {
  if (Target.is<SILFunction *>())
    return nullptr;

  if (!Witness) {
    WitnessMetadata witness;

    auto *requirement = Target.get<AbstractFunctionDecl *>();
    auto *protocol = requirement->getDeclContext()->getSelfProtocolDecl();
    assert(protocol);

    witness.SelfMetadata = actorSelf;
    witness.SelfWitnessTable =
        lookupWitnessTable(IGF, emitMetadataRef(actorSelf), protocol);

    Witness = witness;
  }

  return &(*Witness);
}

ArgumentDecoderInfo DistributedAccessor::findArgumentDecoder(
    llvm::Value *decoder, llvm::Value *decoderTy, llvm::Value *witnessTable) {
  auto &C = IGM.Context;
  auto *thunk = cast<AbstractFunctionDecl>(Target.getDeclContext());
  auto expansionContext = IGM.getMaximalTypeExpansionContext();

  /// If the context was a function, unwrap it and look for the decode method
  /// based off a concrete class; If we're not in a concrete class, we'll be
  /// using a witness for the decoder so returning null is okey.
  FuncDecl *decodeFn = getDistributedActorArgumentDecodingMethod(
      thunk->getDeclContext()->getSelfNominalTypeDecl());

  // If distributed actor is generic over actor system, we have to
  // use witness to reference `decodeNextArgument`.
  if (!decodeFn) {
    auto decoderProtocol = C.getDistributedTargetInvocationDecoderDecl();
    auto decodeNextArgRequirement =
        decoderProtocol->getSingleRequirement(C.Id_decodeNextArgument);
    assert(decodeNextArgRequirement);
    SILDeclRef decodeNextArgumentRef(decodeNextArgRequirement);

    llvm::Constant *fnPtr =
        IGM.getAddrOfDispatchThunk(decodeNextArgumentRef, NotForDefinition);
    auto fnType = IGM.getSILTypes().getConstantFunctionType(
        IGM.getMaximalTypeExpansionContext(), decodeNextArgumentRef);

    auto sig = IGM.getSignature(fnType);
    auto fn = FunctionPointer::forDirect(fnType, fnPtr,
                                         /*secondaryValue=*/nullptr, sig, true);
    return {decoder, decoderTy, witnessTable,
            fn,      fnType,    /*usesWitnessDispatch=*/true};
  }

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
  bool usesDispatchThunk = false;

  if (auto classDecl = dyn_cast<ClassDecl>(decoderDecl)) {
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

    /// When using library evolution functions have another "dispatch thunk"
    /// so we must use this instead of the decodeFn directly.
    usesDispatchThunk =
        getMethodDispatch(decodeFn) == swift::MethodDispatch::Class &&
        classDecl->hasResilientMetadata();
  }

  FunctionPointer methodPtr;

  if (usesDispatchThunk) {
    auto fnPtr = IGM.getAddrOfDispatchThunk(SILDeclRef(decodeFn), NotForDefinition);
    methodPtr = FunctionPointer::createUnsigned(
        methodTy, fnPtr, signature, /*useSignature=*/true);
  } else {
    SILFunction *decodeSILFn = IGM.getSILModule().lookUpFunction(SILDeclRef(decodeFn));
    auto fnPtr = IGM.getAddrOfSILFunction(decodeSILFn, NotForDefinition,
        /*isDynamicallyReplaceable=*/false);
    methodPtr = FunctionPointer::forDirect(
        classifyFunctionPointerKind(decodeSILFn), fnPtr,
        /*secondaryValue=*/nullptr, signature);
  }

  return {decoder,   decoderTy, witnessTable,
          methodPtr, methodTy,  /*usesWitnessDispatch=*/false};
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
