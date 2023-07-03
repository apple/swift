//===--- SILGenBridging.cpp - SILGen for bridging to Clang ASTs -----------===//
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

#include "ArgumentScope.h"
#include "Callee.h"
#include "ExecutorBreadcrumb.h"
#include "RValue.h"
#include "ResultPlan.h"
#include "SILGenFunction.h"
#include "SILGenFunctionBuilder.h"
#include "Scope.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/TypeLowering.h"
#include "clang/AST/DeclObjC.h"

using namespace swift;
using namespace Lowering;

/// Convert to the given formal type, assuming that the lowered type of
/// the source type is the same as its formal type.  This is a reasonable
/// assumption for a wide variety of types.
static ManagedValue emitUnabstractedCast(SILGenFunction &SGF, SILLocation loc,
                                         ManagedValue value,
                                         CanType sourceFormalType,
                                         CanType targetFormalType) {
  SILType loweredResultTy = SGF.getLoweredType(targetFormalType);
  if (value.getType() == loweredResultTy)
    return value;

  return SGF.emitTransformedValue(loc, value,
                                  AbstractionPattern(sourceFormalType),
                                  sourceFormalType,
                                  AbstractionPattern(targetFormalType),
                                  targetFormalType,
                                  loweredResultTy);
}

static bool shouldBridgeThroughError(SILGenModule &SGM, CanType type,
                                     CanType targetType) {
  // Never use this logic if the target type is AnyObject.
  if (targetType->isEqual(SGM.getASTContext().getAnyObjectType()))
    return false;

  auto errorProtocol = SGM.getASTContext().getErrorDecl();
  if (!errorProtocol) return false;

  // Existential types are convertible to Error if they are, or imply, Error.
  if (type.isExistentialType()) {
    auto layout = type->getExistentialLayout();
    for (auto proto : layout.getProtocols()) {
      if (proto == errorProtocol ||
          proto->inheritsFrom(errorProtocol)) {
        return true;
      }
    }

    // They're also convertible to Error if they have a class bound that
    // conforms to Error.
    if (auto superclass = layout.getSuperclass()) {
      type = superclass->getCanonicalType();

    // Otherwise, they are not convertible to Error.
    } else {
      return false;
    }
  }

  return (bool)SGM.SwiftModule->lookupConformance(type, errorProtocol);
}

/// Bridge the given Swift value to its corresponding Objective-C
/// object, using the appropriate witness for the
/// _ObjectiveCBridgeable._bridgeToObjectiveC requirement.
static llvm::Optional<ManagedValue>
emitBridgeNativeToObjectiveC(SILGenFunction &SGF, SILLocation loc,
                             ManagedValue swiftValue, CanType swiftValueType,
                             CanType bridgedType,
                             ProtocolConformance *conformance) {
  // Find the _bridgeToObjectiveC requirement.
  auto requirement = SGF.SGM.getBridgeToObjectiveCRequirement(loc);
  if (!requirement)
    return llvm::None;

  // Retrieve the _bridgeToObjectiveC witness.
  auto witness = conformance->getWitnessDecl(requirement);
  assert(witness);

  // Determine the type we're bridging to.
  auto objcTypeReq = SGF.SGM.getBridgedObjectiveCTypeRequirement(loc);
  if (!objcTypeReq)
    return llvm::None;

  Type objcType = conformance->getTypeWitness(objcTypeReq);

  // Create a reference to the witness.
  SILDeclRef witnessConstant(witness);
  auto witnessRef = SGF.emitGlobalFunctionRef(loc, witnessConstant);

  // Determine the substitutions.
  auto witnessFnTy = witnessRef->getType();

  // Compute the substitutions.

  // FIXME: Figure out the right SubstitutionMap stuff if the witness
  // has generic parameters of its own.
  assert(!cast<FuncDecl>(witness)->isGeneric() &&
         "Generic witnesses not supported");

  auto *dc = cast<FuncDecl>(witness)->getDeclContext();
  auto typeSubMap = swiftValueType->getContextSubstitutionMap(
      SGF.SGM.SwiftModule, dc);

  // Substitute into the witness function type.
  witnessFnTy = witnessFnTy.substGenericArgs(SGF.SGM.M, typeSubMap,
                                             SGF.getTypeExpansionContext());

  // We might have to re-abstract the 'self' value if it is an
  // Optional.
  AbstractionPattern origSelfType(witness->getInterfaceType());
  origSelfType = origSelfType.getFunctionParamType(0);

  ArgumentScope scope(SGF, loc);

  swiftValue = SGF.emitSubstToOrigValue(loc, swiftValue,
                                        origSelfType,
                                        swiftValueType,
                                        SGFContext());

  // The witness may be more abstract than the concrete value we're bridging,
  // for instance, if the value is a concrete instantiation of a generic type.
  //
  // Note that we assume that we don't ever have to reabstract the parameter.
  // This is safe for now, since only nominal types currently can conform to
  // protocols.
  SILFunctionConventions witnessConv(witnessFnTy.castTo<SILFunctionType>(),
                                     SGF.SGM.M);
  if (witnessConv.isSILIndirect(witnessConv.getParameters()[0])
      && !swiftValue.getType().isAddress()) {
    auto tmp = SGF.emitTemporaryAllocation(loc, swiftValue.getType());
    swiftValue = SGF.emitManagedStoreBorrow(
        loc, swiftValue.borrow(SGF, loc).getValue(), tmp);
  }

  // Call the witness.
  SILValue bridgedValue =
      SGF.B.createApply(loc, witnessRef, typeSubMap,
                        swiftValue.borrow(SGF, loc).getValue());

  auto bridgedMV = SGF.emitManagedRValueWithCleanup(bridgedValue);
  bridgedMV = scope.popPreservingValue(bridgedMV);

  // The Objective-C value doesn't necessarily match the desired type.
  bridgedMV = emitUnabstractedCast(SGF, loc, bridgedMV,
                                   objcType->getCanonicalType(), bridgedType);

  return bridgedMV;
}

/// Bridge the given Objective-C object to its corresponding Swift
/// value, using the appropriate witness for the
/// _ObjectiveCBridgeable._unconditionallyBridgeFromObjectiveC requirement.
static llvm::Optional<ManagedValue>
emitBridgeObjectiveCToNative(SILGenFunction &SGF, SILLocation loc,
                             ManagedValue objcValue, CanType bridgedType,
                             ProtocolConformance *conformance) {
  // Find the _unconditionallyBridgeFromObjectiveC requirement.
  auto requirement =
    SGF.SGM.getUnconditionallyBridgeFromObjectiveCRequirement(loc);
  if (!requirement)
    return llvm::None;

  // Find the _ObjectiveCType requirement.
  auto objcTypeRequirement = SGF.SGM.getBridgedObjectiveCTypeRequirement(loc);
  if (!objcTypeRequirement)
    return llvm::None;

  // Retrieve the _unconditionallyBridgeFromObjectiveC witness.
  auto witness = conformance->getWitnessDeclRef(requirement);
  assert(witness);

  // Retrieve the _ObjectiveCType witness.
  auto objcType = conformance->getTypeWitness(objcTypeRequirement);

  // Create a reference to the witness.
  SILDeclRef witnessConstant(witness.getDecl());
  auto witnessRef = SGF.emitGlobalFunctionRef(loc, witnessConstant);

  // Determine the substitutions.
  auto witnessFnTy = witnessRef->getType().castTo<SILFunctionType>();

  CanType swiftValueType = conformance->getType()->getCanonicalType();
  auto genericSig = witnessFnTy->getInvocationGenericSignature();
  SubstitutionMap typeSubMap = witness.getSubstitutions();

  // Substitute into the witness function type.
  witnessFnTy = witnessFnTy->substGenericArgs(SGF.SGM.M, typeSubMap,
                                              SGF.getTypeExpansionContext());

  // The witness takes an _ObjectiveCType?, so convert to that type.
  CanType desiredValueType = OptionalType::get(objcType)->getCanonicalType();
  objcValue = emitUnabstractedCast(SGF, loc, objcValue, bridgedType,
                                   desiredValueType);

  // Call the witness.
  auto metatypeParam = witnessFnTy->getParameters()[1];
  assert(isa<MetatypeType>(metatypeParam.getInterfaceType()) &&
         cast<MetatypeType>(metatypeParam.getInterfaceType()).getInstanceType()
           == swiftValueType);
  SILValue metatypeValue = SGF.B.createMetatype(
      loc, metatypeParam.getSILStorageType(SGF.SGM.M, witnessFnTy,
                                           SGF.getTypeExpansionContext()));

  auto witnessCI =
      SGF.getConstantInfo(SGF.getTypeExpansionContext(), witnessConstant);
  CanType formalResultTy = witnessCI.LoweredType.getResult();

  auto subs = witness.getSubstitutions();

  // Set up the generic signature, since formalResultTy is an interface type.
  CalleeTypeInfo calleeTypeInfo(
      witnessFnTy,
      AbstractionPattern(genericSig, formalResultTy),
      swiftValueType);
  SGFContext context;
  ResultPlanPtr resultPlan =
      ResultPlanBuilder::computeResultPlan(SGF, calleeTypeInfo, loc, context);
  ArgumentScope argScope(SGF, loc);
  RValue result =
      SGF.emitApply(std::move(resultPlan), std::move(argScope), loc,
                    ManagedValue::forUnmanaged(witnessRef), subs,
                    {objcValue, ManagedValue::forUnmanaged(metatypeValue)},
                    calleeTypeInfo, ApplyOptions(), context, llvm::None);
  return std::move(result).getAsSingleValue(SGF, loc);
}

static ManagedValue emitBridgeBoolToObjCBool(SILGenFunction &SGF,
                                             SILLocation loc,
                                             ManagedValue swiftBool) {
  // func _convertBoolToObjCBool(Bool) -> ObjCBool
  SILValue boolToObjCBoolFn
    = SGF.emitGlobalFunctionRef(loc, SGF.SGM.getBoolToObjCBoolFn());

  SILValue result = SGF.B.createApply(loc, boolToObjCBoolFn,
                                      {}, swiftBool.forward(SGF));
  return SGF.emitManagedRValueWithCleanup(result);
}

static ManagedValue emitBridgeBoolToDarwinBoolean(SILGenFunction &SGF,
                                                  SILLocation loc,
                                                  ManagedValue swiftBool) {
  // func _convertBoolToDarwinBoolean(Bool) -> DarwinBoolean
  SILValue boolToDarwinBooleanFn
    = SGF.emitGlobalFunctionRef(loc, SGF.SGM.getBoolToDarwinBooleanFn());

  SILValue result = SGF.B.createApply(loc, boolToDarwinBooleanFn,
                                      {}, swiftBool.forward(SGF));
  return SGF.emitManagedRValueWithCleanup(result);
}

static ManagedValue emitBridgeBoolToWindowsBool(SILGenFunction &SGF,
                                                SILLocation L, ManagedValue b) {
  // func _convertToWindowsBool(Bool) -> WindowsBool
  SILValue F = SGF.emitGlobalFunctionRef(L, SGF.SGM.getBoolToWindowsBoolFn());
  SILValue R = SGF.B.createApply(L, F, {}, b.forward(SGF));
  return SGF.emitManagedRValueWithCleanup(R);
}

static ManagedValue emitBridgeForeignBoolToBool(SILGenFunction &SGF,
                                                SILLocation loc,
                                                ManagedValue foreignBool,
                                                SILDeclRef bridgingFnRef) {
  // func _convertObjCBoolToBool(ObjCBool) -> Bool
  SILValue bridgingFn = SGF.emitGlobalFunctionRef(loc, bridgingFnRef);

  SILValue result = SGF.B.createApply(loc, bridgingFn, {},
                                      foreignBool.forward(SGF));
  return SGF.emitManagedRValueWithCleanup(result);
}

static ManagedValue emitManagedParameter(SILGenFunction &SGF, SILLocation loc,
                                         SILParameterInfo param,
                                         SILValue value) {
  const TypeLowering &valueTL = SGF.getTypeLowering(value->getType());

  switch (param.getConvention()) {
  case ParameterConvention::Direct_Owned:
    // Consume owned parameters at +1.
    return SGF.emitManagedRValueWithCleanup(value, valueTL);

  case ParameterConvention::Direct_Guaranteed:
    // If we have a guaranteed parameter, the object should not need to be
    // retained or have a cleanup.
    return ManagedValue::forUnmanaged(value);

  case ParameterConvention::Direct_Unowned:
    // We need to independently retain the value.
    return SGF.emitManagedRetain(loc, value, valueTL);

  case ParameterConvention::Indirect_Inout:
    return ManagedValue::forLValue(value);

  case ParameterConvention::Indirect_In_Guaranteed:
    if (valueTL.isLoadable()) {
      return SGF.B.createLoadBorrow(loc, ManagedValue::forUnmanaged(value));
    } else {
      return ManagedValue::forUnmanaged(value);
    }

  case ParameterConvention::Indirect_In:
    if (valueTL.isLoadable()) {
      return SGF.emitLoad(loc, value, valueTL, SGFContext(), IsTake);
    } else {
      return SGF.emitManagedRValueWithCleanup(value, valueTL);
    }

  case ParameterConvention::Indirect_InoutAliasable:
  case ParameterConvention::Pack_Guaranteed:
  case ParameterConvention::Pack_Owned:
  case ParameterConvention::Pack_Inout:
    llvm_unreachable("unexpected convention");
  }
  llvm_unreachable("bad convention");
}

/// Get the type of each parameter, filtering out empty tuples.
static SmallVector<CanType, 8>
getParameterTypes(AnyFunctionType::CanParamArrayRef params,
                  bool hasSelfParam=false) {
  SmallVector<CanType, 8> results;
  for (auto n : indices(params)) {
    bool isSelf = (hasSelfParam ? n == params.size() - 1 : false);

    const auto &param = params[n];
    assert(isSelf || !param.isInOut() &&
           "Only the 'self' parameter can be inout in a bridging thunk");
    assert(!param.isVariadic());

    if (param.getPlainType()->isVoid())
      continue;
    results.push_back(param.getPlainType());
  }
  return results;
}

static CanAnyFunctionType
getBridgedBlockType(SILGenModule &SGM, CanAnyFunctionType blockType,
                    SILFunctionTypeRepresentation silRep) {
  return SGM.Types.getBridgedFunctionType(
      AbstractionPattern(blockType), blockType, Bridgeability::Full, silRep);
}

static void buildFuncToBlockInvokeBody(SILGenFunction &SGF,
                                       SILLocation loc,
                                       CanAnyFunctionType formalFuncType,
                                       CanAnyFunctionType formalBlockType,
                                       CanSILFunctionType funcTy,
                                       CanSILFunctionType blockTy,
                                       CanSILBlockStorageType blockStorageTy,
                                       bool isUnretainedClosureSafe) {
  Scope scope(SGF.Cleanups, CleanupLocation(loc));
  SILBasicBlock *entry = &*SGF.F.begin();
  SILFunctionConventions blockConv(blockTy, SGF.SGM.M);
  SILFunctionConventions funcConv(funcTy, SGF.SGM.M);

  // Make sure we lower the component types of the formal block type.
  formalBlockType = getBridgedBlockType(SGF.SGM, formalBlockType,
                                        blockTy->getRepresentation());

  // Set up the indirect result.
  SILType blockResultTy =
      blockTy->getAllResultsSubstType(SGF.SGM.M, SGF.getTypeExpansionContext());
  SILValue indirectResult;
  if (blockTy->getNumResults() != 0) {
    auto result = blockTy->getSingleResult();
    if (result.getConvention() == ResultConvention::Indirect) {
      indirectResult = entry->createFunctionArgument(blockResultTy);
    }
  }

  // Get the captured native function value out of the block.
  auto storageAddrTy = SILType::getPrimitiveAddressType(blockStorageTy);
  auto storage = entry->createFunctionArgument(storageAddrTy);
  auto capture = SGF.B.createProjectBlockStorage(loc, storage);
  auto &funcTL = SGF.getTypeLowering(funcTy);
  auto fn = isUnretainedClosureSafe
                ? SGF.emitManagedLoadBorrow(loc, capture)
                : SGF.emitLoad(loc, capture, funcTL, SGFContext(), IsNotTake);

  // Collect the block arguments, which may have nonstandard conventions.
  assert(blockTy->getParameters().size() == funcTy->getParameters().size()
         && "block and function types don't match");

  auto nativeParamTypes = getParameterTypes(formalFuncType.getParams());
  auto bridgedParamTypes = getParameterTypes(formalBlockType.getParams());

  SmallVector<ManagedValue, 4> args;
  for (unsigned i : indices(funcTy->getParameters())) {
    auto &param = blockTy->getParameters()[i];
    SILType paramTy =
        blockConv.getSILType(param, SGF.getTypeExpansionContext());
    SILValue v = entry->createFunctionArgument(paramTy);
    ManagedValue mv;
    
    // If the parameter is a block, we need to copy it to ensure it lives on
    // the heap. The adapted closure value might outlive the block's original
    // scope.
    if (SGF.getSILType(param, blockTy).isBlockPointerCompatible()) {
      // We still need to consume the original block if it was owned.
      switch (param.getConvention()) {
      case ParameterConvention::Direct_Owned:
        SGF.emitManagedRValueWithCleanup(v);
        break;

      case ParameterConvention::Direct_Guaranteed:
      case ParameterConvention::Direct_Unowned:
        break;
        
      case ParameterConvention::Indirect_In:
      case ParameterConvention::Indirect_In_Guaranteed:
      case ParameterConvention::Indirect_Inout:
      case ParameterConvention::Indirect_InoutAliasable:
      case ParameterConvention::Pack_Guaranteed:
      case ParameterConvention::Pack_Owned:
      case ParameterConvention::Pack_Inout:
        llvm_unreachable("indirect params to blocks not supported");
      }
      
      SILValue blockCopy = SGF.B.createCopyBlock(loc, v);
      mv = SGF.emitManagedRValueWithCleanup(blockCopy);
    } else {
      mv = emitManagedParameter(SGF, loc, param, v);
    }

    CanType formalBridgedType = bridgedParamTypes[i];
    CanType formalNativeType = nativeParamTypes[i];
    SILType loweredNativeTy = funcTy->getParameters()[i].getSILStorageType(
        SGF.SGM.M, funcTy, SGF.getTypeExpansionContext());

    args.push_back(SGF.emitBridgedToNativeValue(loc, mv, formalBridgedType,
                                                formalNativeType,
                                                loweredNativeTy));
  }

  auto init = indirectResult
                ? SGF.useBufferAsTemporary(indirectResult,
                                SGF.getTypeLowering(indirectResult->getType()))
                : nullptr;

  CanType formalNativeResultType = formalFuncType.getResult();
  CanType formalBridgedResultType = formalBlockType.getResult();

  bool canEmitIntoInit =
    (indirectResult &&
     indirectResult->getType()
       == SGF.getLoweredType(formalNativeResultType).getAddressType());

  // Call the native function.
  SGFContext C(canEmitIntoInit ? init.get() : nullptr);
  ManagedValue result =
      SGF.emitMonomorphicApply(loc, fn, args, formalNativeResultType,
                               formalNativeResultType, ApplyOptions(),
                               llvm::None, llvm::None, C)
          .getAsSingleValue(SGF, loc);

  // Bridge the result back to ObjC.
  if (!canEmitIntoInit) {
    result = SGF.emitNativeToBridgedValue(loc, result,
                                          formalNativeResultType,
                                          formalBridgedResultType,
                                          blockResultTy,
                                          SGFContext(init.get()));
  }

  SILValue resultVal;

  // If we have an indirect result, make sure the result is there.
  if (indirectResult) {
    if (!result.isInContext()) {
      init->copyOrInitValueInto(SGF, loc, result, /*isInit*/ true);
      init->finishInitialization(SGF);
    }
    init->getManagedAddress().forward(SGF);
    resultVal = SGF.B.createTuple(loc, {});
  } else {
    // Otherwise, return the result at +1.
    resultVal = result.forward(SGF);
  }

  scope.pop();

  SGF.B.createReturn(loc, resultVal);
}

/// Bridge a native function to a block with a thunk.
ManagedValue SILGenFunction::emitFuncToBlock(SILLocation loc,
                                             ManagedValue fn,
                                             CanAnyFunctionType funcType,
                                             CanAnyFunctionType blockType,
                                             CanSILFunctionType loweredBlockTy){
  auto loweredFuncTy = fn.getType().castTo<SILFunctionType>();

  // If we store a @noescape closure in a block verify that the block has not
  // escaped by storing a withoutActuallyEscaping closure in the block and after
  // the block is ultimately destroyed checking that the closure is uniquely
  // referenced.
  bool useWithoutEscapingVerification = false;
  ManagedValue escaping;
  if (loweredFuncTy->isNoEscape()) {
    auto escapingTy = loweredFuncTy->getWithExtInfo(
        loweredFuncTy->getExtInfo().withNoEscape(false));

    escaping = createWithoutActuallyEscapingClosure(
        loc, fn, SILType::getPrimitiveObjectType(escapingTy));
    loweredFuncTy = escapingTy;
    auto escapingAnyTy =
        funcType.withExtInfo(funcType->getExtInfo().withNoEscape(false));
    funcType = escapingAnyTy;
    fn = escaping.copy(*this, loc);
    useWithoutEscapingVerification = true;
  } else {
    // Since we are going to be storing this into memory, we need fn at +1.
    fn = fn.ensurePlusOne(*this, loc);
  }
  
  // All different substitutions of a function type can share a thunk.
  auto loweredFuncUnsubstTy = loweredFuncTy->getUnsubstitutedType(SGM.M);
  if (loweredFuncUnsubstTy != loweredFuncTy) {
    fn = B.createConvertFunction(loc, fn,
                         SILType::getPrimitiveObjectType(loweredFuncUnsubstTy));
  }

  // Build the invoke function signature. The block will capture the original
  // function value.
  auto fnInterfaceTy = cast<SILFunctionType>(
    loweredFuncUnsubstTy->mapTypeOutOfContext()->getCanonicalType());
  auto blockInterfaceTy = cast<SILFunctionType>(
    loweredBlockTy->mapTypeOutOfContext()->getCanonicalType());

  assert(!blockInterfaceTy->isCoroutine());

  auto storageTy = SILBlockStorageType::get(loweredFuncUnsubstTy);
  auto storageInterfaceTy = SILBlockStorageType::get(fnInterfaceTy);

  // Build the invoke function type.
  SmallVector<SILParameterInfo, 4> params;
  params.push_back(SILParameterInfo(storageInterfaceTy,
                                 ParameterConvention::Indirect_InoutAliasable));
  std::copy(blockInterfaceTy->getParameters().begin(),
            blockInterfaceTy->getParameters().end(),
            std::back_inserter(params));

  auto results = blockInterfaceTy->getResults();
  auto representation = SILFunctionType::Representation::CFunctionPointer;
  auto *clangFnType = getASTContext().getCanonicalClangFunctionType(
      params, results.empty() ? llvm::Optional<SILResultInfo>() : results[0],
      representation);

  auto extInfo = SILFunctionType::ExtInfoBuilder()
                     .withRepresentation(representation)
                     .withClangFunctionType(clangFnType)
                     .build();

  CanGenericSignature genericSig;
  GenericEnvironment *genericEnv = nullptr;
  SubstitutionMap subs;
  if (funcType->hasArchetype() || blockType->hasArchetype()) {
    genericSig = F.getLoweredFunctionType()->getInvocationGenericSignature();
    genericEnv = F.getGenericEnvironment();

    subs = F.getForwardingSubstitutionMap();

    // The block invoke function must be pseudogeneric. This should be OK for now
    // since a bridgeable function's parameters and returns should all be
    // trivially representable in ObjC so not need to exercise the type metadata.
    //
    // Ultimately we may need to capture generic parameters in block storage, but
    // that will require a redesign of the interface to support dependent-layout
    // context. Currently we don't capture anything directly into a block but a
    // Swift closure, but that's totally dumb.
    if (genericSig)
      extInfo = extInfo.intoBuilder().withIsPseudogeneric().build();
  }

  auto invokeTy = SILFunctionType::get(
      genericSig, extInfo, SILCoroutineKind::None,
      ParameterConvention::Direct_Unowned, params, 
      /*yields*/ {}, blockInterfaceTy->getResults(),
      blockInterfaceTy->getOptionalErrorResult(),
      SubstitutionMap(), SubstitutionMap(),
      getASTContext());

  // Create the invoke function. Borrow the mangling scheme from reabstraction
  // thunks, which is what we are in spirit.
  auto thunk = SGM.getOrCreateReabstractionThunk(invokeTy,
                                                 loweredFuncUnsubstTy,
                                                 loweredBlockTy,
                                                 /*dynamicSelfType=*/CanType(),
                                                 /*global actor=*/CanType());

  // Build it if necessary.
  if (thunk->empty()) {
    thunk->setGenericEnvironment(genericEnv);
    SILGenFunction thunkSGF(SGM, *thunk, FunctionDC);
    auto loc = RegularLocation::getAutoGeneratedLocation();
    // Not retaining the closure in the reabstraction thunk is safe if we hold
    // another reference for the is_escaping sentinel.
    buildFuncToBlockInvokeBody(thunkSGF, loc, funcType, blockType,
                               loweredFuncUnsubstTy, loweredBlockTy, storageTy,
                               useWithoutEscapingVerification);
    SGM.emitLazyConformancesForFunction(thunk);
  }

  // Form the block on the stack.
  auto storageAddrTy = SILType::getPrimitiveAddressType(storageTy);
  auto storage = emitTemporaryAllocation(loc, storageAddrTy);
  auto capture = B.createProjectBlockStorage(loc, storage);
  B.createStore(loc, fn, capture, StoreOwnershipQualifier::Init);
  auto invokeFn = B.createFunctionRefFor(loc, thunk);

  auto stackBlock = B.createInitBlockStorageHeader(loc, storage, invokeFn,
                              SILType::getPrimitiveObjectType(loweredBlockTy),
                                                   subs);

  // Copy the block so we have an independent heap object we can hand off.

  // If withoutActuallyEscaping verification is requested we emit a
  // copy_block_without_escaping %block withoutEscaping %closure instruction.
  // A mandatory SIL pass will replace this instruction by the required
  // verification instruction sequence.
  auto heapBlock = useWithoutEscapingVerification
                       ? SILValue(B.createCopyBlockWithoutEscaping(
                             loc, stackBlock, escaping.forward(*this)))
                       : SILValue(B.createCopyBlock(loc, stackBlock));
  return emitManagedRValueWithCleanup(heapBlock);
}

static ManagedValue emitNativeToCBridgedNonoptionalValue(SILGenFunction &SGF,
                                                         SILLocation loc,
                                                         ManagedValue v,
                                                         CanType nativeType,
                                                         CanType bridgedType,
                                                      SILType loweredBridgedTy,
                                                         SGFContext C) {
  assert(loweredBridgedTy.isObject());
  if (v.getType().getObjectType() == loweredBridgedTy)
    return v;

  // If the input is a native type with a bridged mapping, convert it.
#define BRIDGE_TYPE(BridgedModule,BridgedType, NativeModule,NativeType,Opt) \
  if (nativeType == SGF.SGM.Types.get##NativeType##Type()                   \
      && bridgedType == SGF.SGM.Types.get##BridgedType##Type()) {           \
    return emitBridge##NativeType##To##BridgedType(SGF, loc, v);            \
  }
#include "swift/SIL/BridgedTypes.def"

  // Bridge thick to Objective-C metatypes.
  if (auto bridgedMetaTy = dyn_cast<AnyMetatypeType>(bridgedType)) {
    if (bridgedMetaTy->hasRepresentation() &&
        bridgedMetaTy->getRepresentation() == MetatypeRepresentation::ObjC) {
      SILValue native = SGF.B.emitThickToObjCMetatype(loc, v.getValue(),
                                                      loweredBridgedTy);
      // *NOTE*: ObjCMetatypes are trivial types. They only gain ARC semantics
      // when they are converted to an object via objc_metatype_to_object.
      assert(!v.hasCleanup() &&
             "Metatypes are trivial and thus should not have cleanups");
      return ManagedValue::forUnmanaged(native);
    }
  }

  // Bridge native functions to blocks.
  auto bridgedFTy = dyn_cast<AnyFunctionType>(bridgedType);
  if (bridgedFTy && bridgedFTy->getRepresentation()
                      == AnyFunctionType::Representation::Block) {
    auto nativeFTy = cast<AnyFunctionType>(nativeType);

    if (nativeFTy->getRepresentation()
          != AnyFunctionType::Representation::Block)
      return SGF.emitFuncToBlock(loc, v, nativeFTy, bridgedFTy,
                                 loweredBridgedTy.castTo<SILFunctionType>());
  }

  // If the native type conforms to _ObjectiveCBridgeable, use its
  // _bridgeToObjectiveC witness.
  if (auto conformance =
          SGF.SGM.getConformanceToObjectiveCBridgeable(loc, nativeType)) {
    if (auto result = emitBridgeNativeToObjectiveC(SGF, loc, v, nativeType,
                                                   bridgedType, conformance))
      return *result;

    assert(SGF.SGM.getASTContext().Diags.hadAnyError() &&
           "Bridging code should have complained");
    return SGF.emitUndef(bridgedType);
  }

  // Bridge Error, or types that conform to it, to NSError.
  if (shouldBridgeThroughError(SGF.SGM, nativeType, bridgedType)) {
    auto errorTy = SGF.SGM.Types.getNSErrorType();
    auto error = SGF.emitNativeToBridgedError(loc, v, nativeType, errorTy);
    if (errorTy != bridgedType) {
      error = emitUnabstractedCast(SGF, loc, error, errorTy, bridgedType);
    }
    return error;
  }

  // Fall back to dynamic Any-to-id bridging.
  // The destination type should be AnyObject in this case.
  assert(bridgedType->isEqual(SGF.getASTContext().getAnyObjectType()));

  // Blocks bridge to id with a cast under ObjCInterop.
  if (auto nativeFnType = dyn_cast<AnyFunctionType>(nativeType)) {
    if (nativeFnType->getRepresentation() ==
          FunctionTypeRepresentation::Block &&
        SGF.getASTContext().LangOpts.EnableObjCInterop) {
      return SGF.B.createBlockToAnyObject(loc, v, loweredBridgedTy);
    }
  }

  // If the input argument is known to be an existential, save the runtime
  // some work by opening it.
  if (nativeType->isExistentialType()) {
    auto openedType = OpenedArchetypeType::get(nativeType,
                                               SGF.F.getGenericSignature());

    FormalEvaluationScope scope(SGF);

    v = SGF.emitOpenExistential(
        loc, v, SGF.getLoweredType(openedType),
        AccessKind::Read);
    v = v.ensurePlusOne(SGF, loc);

    nativeType = openedType;
  }

  // Call into the stdlib intrinsic.
  if (auto bridgeAnything =
        SGF.getASTContext().getBridgeAnythingToObjectiveC()) {
    auto genericSig = bridgeAnything->getGenericSignature();
    auto subMap = SubstitutionMap::get(
      genericSig,
      [&](SubstitutableType *t) -> Type {
        return nativeType;
      },
      MakeAbstractConformanceForGenericType());

    // The intrinsic takes a T; reabstract to the generic abstraction
    // pattern.
    v = SGF.emitSubstToOrigValue(loc, v, AbstractionPattern::getOpaque(),
                                 nativeType);

    // Put the value into memory if necessary.
    assert(v.getOwnershipKind() == OwnershipKind::None || v.hasCleanup());
    SILModuleConventions silConv(SGF.SGM.M);
    // bridgeAnything always takes an indirect argument as @in.
    // Since we don't have the SIL type here, check the current SIL stage/mode
    // to determine the convention.
    if (v.getType().isObject() && silConv.useLoweredAddresses()) {
      auto tmp = SGF.emitTemporaryAllocation(loc, v.getType());
      v.forwardInto(SGF, loc, tmp);
      v = SGF.emitManagedBufferWithCleanup(tmp);
    }
    return SGF.emitApplyOfLibraryIntrinsic(loc, bridgeAnything, subMap, v, C)
              .getAsSingleValue(SGF, loc);
  }
  
  // Shouldn't get here unless the standard library is busted.
  return SGF.emitUndef(loweredBridgedTy);
}

static ManagedValue emitNativeToCBridgedValue(SILGenFunction &SGF,
                                              SILLocation loc,
                                              ManagedValue v,
                                              CanType nativeType,
                                              CanType bridgedType,
                                              SILType loweredBridgedTy,
                                              SGFContext C = SGFContext()) {
  SILType loweredNativeTy = v.getType();
  if (loweredNativeTy.getObjectType() == loweredBridgedTy.getObjectType())
    return v;

  CanType bridgedObjectType = bridgedType.getOptionalObjectType();
  CanType nativeObjectType = nativeType.getOptionalObjectType();

  // Check for optional-to-optional conversions.
  if (bridgedObjectType && nativeObjectType) {
    auto helper = [&](SILGenFunction &SGF, SILLocation loc,
                      ManagedValue v, SILType loweredBridgedObjectTy,
                      SGFContext C) {
      return emitNativeToCBridgedValue(SGF, loc, v, nativeObjectType,
                                       bridgedObjectType,
                                       loweredBridgedObjectTy, C);
    };
    return SGF.emitOptionalToOptional(loc, v, loweredBridgedTy, helper, C);
  }
  
  // Check if we need to wrap the bridged result in an optional.
  if (bridgedObjectType) {
    auto helper = [&](SILGenFunction &SGF, SILLocation loc, SGFContext C) {
      auto loweredBridgedObjectTy = loweredBridgedTy.getOptionalObjectType();
      return emitNativeToCBridgedValue(SGF, loc, v, nativeType,
                                       bridgedObjectType,
                                       loweredBridgedObjectTy, C);
    };
    return SGF.emitOptionalSome(loc, loweredBridgedTy, helper, C);
  }
  
  return emitNativeToCBridgedNonoptionalValue(SGF, loc, v, nativeType,
                                              bridgedType, loweredBridgedTy, C);
}

ManagedValue SILGenFunction::emitNativeToBridgedValue(SILLocation loc,
                                                      ManagedValue v,
                                                      CanType nativeTy,
                                                      CanType bridgedTy,
                                                      SILType loweredBridgedTy,
                                                      SGFContext C) {
  loweredBridgedTy = loweredBridgedTy.getObjectType();
  return emitNativeToCBridgedValue(*this, loc, v, nativeTy, bridgedTy,
                                   loweredBridgedTy, C);
}

static void buildBlockToFuncThunkBody(SILGenFunction &SGF,
                                      SILLocation loc,
                                      CanAnyFunctionType formalBlockTy,
                                      CanAnyFunctionType formalFuncTy,
                                      CanSILFunctionType blockTy,
                                      CanSILFunctionType funcTy) {
  // Collect the native arguments, which should all be +1.
  Scope scope(SGF.Cleanups, CleanupLocation(loc));

  // Make sure we lower the component types of the formal block type.
  formalBlockTy =
      getBridgedBlockType(SGF.SGM, formalBlockTy, blockTy->getRepresentation());

  assert(blockTy->getNumParameters() == funcTy->getNumParameters()
         && "block and function types don't match");

  SmallVector<ManagedValue, 4> args;
  SILBasicBlock *entry = &*SGF.F.begin();

  SILFunctionConventions fnConv(funcTy, SGF.SGM.M);

  // Set up the indirect result slot.
  SILValue indirectResult;
  if (funcTy->getNumResults() != 0) {
    auto result = funcTy->getSingleResult();
    if (result.getConvention() == ResultConvention::Indirect) {
      SILType resultTy =
          fnConv.getSILType(result, SGF.getTypeExpansionContext());
      indirectResult = entry->createFunctionArgument(resultTy);
    }
  }

  auto formalBlockParams = getParameterTypes(formalBlockTy.getParams());
  auto formalFuncParams = getParameterTypes(formalFuncTy.getParams());
  assert(formalBlockParams.size() == blockTy->getNumParameters());
  assert(formalFuncParams.size() == funcTy->getNumParameters());

  // Create the arguments for the call.
  for (unsigned i : indices(funcTy->getParameters())) {
    auto &param = funcTy->getParameters()[i];
    CanType formalBlockParamTy = formalBlockParams[i];
    CanType formalFuncParamTy = formalFuncParams[i];

    auto paramTy = fnConv.getSILType(param, SGF.getTypeExpansionContext());
    SILValue v = entry->createFunctionArgument(paramTy);

    // First get the managed parameter for this function.
    auto mv = emitManagedParameter(SGF, loc, param, v);

    SILType loweredBlockArgTy = blockTy->getParameters()[i].getSILStorageType(
        SGF.SGM.M, blockTy, SGF.getTypeExpansionContext());

    // Then bridge the native value to its bridged variant.
    mv = SGF.emitNativeToBridgedValue(loc, mv, formalFuncParamTy,
                                      formalBlockParamTy, loweredBlockArgTy);

    // Finally change ownership if we need to. We do not need to care about the
    // case of a +1 parameter being passed to a +0 function since +1 parameters
    // can be "instantaneously" borrowed at the call site.
    if (blockTy->getParameters()[i].isConsumed()) {
      mv = mv.ensurePlusOne(SGF, loc);
    }
    args.push_back(mv);
  }

  // Add the block argument.
  SILValue blockV =
      entry->createFunctionArgument(SILType::getPrimitiveObjectType(blockTy));
  ManagedValue block = ManagedValue::forUnmanaged(blockV);

  CanType formalResultType = formalFuncTy.getResult();

  auto init = indirectResult
                ? SGF.useBufferAsTemporary(indirectResult,
                                SGF.getTypeLowering(indirectResult->getType()))
                : nullptr;

  // Call the block.
  ManagedValue result =
      SGF.emitMonomorphicApply(
             loc, block, args, formalBlockTy.getResult(), formalResultType,
             ApplyOptions(),
             /*override CC*/ SILFunctionTypeRepresentation::Block,
             /*foreign error*/ llvm::None, SGFContext(init.get()))
          .getAsSingleValue(SGF, loc);

  SILValue r;

  // If we have an indirect result, make sure the result is there.
  if (indirectResult) {
    if (!result.isInContext()) {
      init->copyOrInitValueInto(SGF, loc, result, /*isInit*/ true);
      init->finishInitialization(SGF);
    }
    init->getManagedAddress().forward(SGF);
    r = SGF.B.createTuple(
        loc, fnConv.getSILResultType(SGF.getTypeExpansionContext()),
        ArrayRef<SILValue>());

    // Otherwise, return the result at +1.
  } else {
    r = result.forward(SGF);
  }

  scope.pop();

  SGF.B.createReturn(loc, r);

  // Finally, verify the thunk for SIL invariants.
  SGF.F.verifyIncompleteOSSA();
}

/// Bridge a native function to a block with a thunk.
ManagedValue
SILGenFunction::emitBlockToFunc(SILLocation loc,
                                ManagedValue block,
                                CanAnyFunctionType blockType,
                                CanAnyFunctionType funcType,
                                CanSILFunctionType loweredFuncTy) {
  // Declare the thunk.
  auto loweredBlockTy = block.getType().castTo<SILFunctionType>();

  SubstitutionMap contextSubs, interfaceSubs;
  GenericEnvironment *genericEnv = nullptr;

  // These two are not used here -- but really, bridging thunks
  // should be emitted using the formal AST type, not the lowered
  // type
  CanType inputSubstType, outputSubstType;

  auto loweredFuncTyWithoutNoEscape = adjustFunctionType(
      loweredFuncTy, loweredFuncTy->getExtInfo().withNoEscape(false),
      loweredFuncTy->getWitnessMethodConformanceOrInvalid());
  
  auto loweredFuncUnsubstTy =
    loweredFuncTyWithoutNoEscape->getUnsubstitutedType(SGM.M);

  CanType dynamicSelfType;
  auto thunkTy = buildThunkType(loweredBlockTy, loweredFuncUnsubstTy,
                                inputSubstType, outputSubstType,
                                genericEnv, interfaceSubs, dynamicSelfType);
  assert(!dynamicSelfType && "Not implemented");

  auto thunk = SGM.getOrCreateReabstractionThunk(thunkTy,
                                                 loweredBlockTy,
                                                 loweredFuncUnsubstTy,
                                                 /*dynamicSelfType=*/CanType(),
                                                 /*global actor=*/CanType());

  // Build it if necessary.
  if (thunk->empty()) {
    SILGenFunction thunkSGF(SGM, *thunk, FunctionDC);
    thunk->setGenericEnvironment(genericEnv);
    auto loc = RegularLocation::getAutoGeneratedLocation();
    buildBlockToFuncThunkBody(thunkSGF, loc, blockType, funcType,
                              loweredBlockTy, loweredFuncUnsubstTy);
    SGM.emitLazyConformancesForFunction(thunk);
  }

  CanSILFunctionType substFnTy = thunkTy;

  if (thunkTy->getInvocationGenericSignature()) {
    substFnTy = thunkTy->substGenericArgs(F.getModule(),
                                          interfaceSubs,
                                          getTypeExpansionContext());
  }

  // Create it in the current function.
  auto thunkValue = B.createFunctionRefFor(loc, thunk);
  ManagedValue thunkedFn = B.createPartialApply(
      loc, thunkValue, interfaceSubs, block,
      loweredFuncTy->getCalleeConvention());

  if (loweredFuncUnsubstTy != loweredFuncTyWithoutNoEscape) {
    thunkedFn = B.createConvertFunction(loc, thunkedFn,
                SILType::getPrimitiveObjectType(loweredFuncTyWithoutNoEscape));
  }
  
  if (!loweredFuncTy->isNoEscape()) {
    return thunkedFn;
  }

  // Handle the escaping to noescape conversion.
  assert(loweredFuncTy->isNoEscape());
  return B.createConvertEscapeToNoEscape(
      loc, thunkedFn, SILType::getPrimitiveObjectType(loweredFuncTy));
}

static ManagedValue emitCBridgedToNativeValue(
    SILGenFunction &SGF, SILLocation loc, ManagedValue v, CanType bridgedType,
    SILType loweredBridgedTy, CanType nativeType, SILType loweredNativeTy,
    int bridgedOptionalsToUnwrap, bool isCallResult, SGFContext C) {
  assert(loweredNativeTy.isObject());
  if (loweredNativeTy == loweredBridgedTy.getObjectType())
    return v;

  if (auto nativeObjectType = nativeType.getOptionalObjectType()) {
    auto bridgedObjectType = bridgedType.getOptionalObjectType();

    // Optional injection.
    if (!bridgedObjectType) {
      auto helper = [&](SILGenFunction &SGF, SILLocation loc, SGFContext C) {
        auto loweredNativeObjectTy = loweredNativeTy.getOptionalObjectType();
        return emitCBridgedToNativeValue(
            SGF, loc, v, bridgedType, loweredBridgedTy, nativeObjectType,
            loweredNativeObjectTy, bridgedOptionalsToUnwrap, isCallResult, C);
      };
      return SGF.emitOptionalSome(loc, loweredNativeTy, helper, C);
    }

    // Optional-to-optional.
    auto helper = [=](SILGenFunction &SGF, SILLocation loc, ManagedValue v,
                      SILType loweredNativeObjectTy, SGFContext C) {
      return emitCBridgedToNativeValue(
          SGF, loc, v, bridgedObjectType,
          loweredBridgedTy.getOptionalObjectType(), nativeObjectType,
          loweredNativeObjectTy, bridgedOptionalsToUnwrap, isCallResult, C);
    };
    return SGF.emitOptionalToOptional(loc, v, loweredNativeTy, helper, C);
  }
  if (auto bridgedObjectType = bridgedType.getOptionalObjectType()) {
    return emitCBridgedToNativeValue(
        SGF, loc, v, bridgedObjectType,
        loweredBridgedTy.getOptionalObjectType(), nativeType, loweredNativeTy,
        bridgedOptionalsToUnwrap + 1, isCallResult, C);
  }

  auto unwrapBridgedOptionals = [&](ManagedValue v) {
    for (int i = 0; i < bridgedOptionalsToUnwrap; ++i) {
      v = SGF.emitPreconditionOptionalHasValue(loc, v,
                                               /*implicit*/ true);
    };
    return v;
  };

  // Bridge ObjCBool, DarwinBoolean, WindowsBool to Bool when requested.
  if (nativeType == SGF.SGM.Types.getBoolType()) {
    if (bridgedType == SGF.SGM.Types.getObjCBoolType()) {
      return emitBridgeForeignBoolToBool(SGF, loc, unwrapBridgedOptionals(v),
                                         SGF.SGM.getObjCBoolToBoolFn());
    }
    if (bridgedType == SGF.SGM.Types.getDarwinBooleanType()) {
      return emitBridgeForeignBoolToBool(SGF, loc, unwrapBridgedOptionals(v),
                                         SGF.SGM.getDarwinBooleanToBoolFn());
    }
    if (bridgedType == SGF.SGM.Types.getWindowsBoolType()) {
      return emitBridgeForeignBoolToBool(SGF, loc, unwrapBridgedOptionals(v),
                                         SGF.SGM.getWindowsBoolToBoolFn());
    }
  }

  // Bridge Objective-C to thick metatypes.
  if (isa<AnyMetatypeType>(nativeType)) {
    auto bridgedMetaTy = cast<AnyMetatypeType>(bridgedType);
    if (bridgedMetaTy->hasRepresentation() &&
        bridgedMetaTy->getRepresentation() == MetatypeRepresentation::ObjC) {
      SILValue native = SGF.B.emitObjCToThickMetatype(
          loc, unwrapBridgedOptionals(v).getValue(), loweredNativeTy);
      // *NOTE*: ObjCMetatypes are trivial types. They only gain ARC semantics
      // when they are converted to an object via objc_metatype_to_object.
      assert(!v.hasCleanup() && "Metatypes are trivial and should not have "
                                "cleanups");
      return ManagedValue::forUnmanaged(native);
    }
  }

  // Bridge blocks back into native function types.
  if (auto nativeFTy = dyn_cast<AnyFunctionType>(nativeType)) {
    auto bridgedFTy = cast<AnyFunctionType>(bridgedType);
    if (bridgedFTy->getRepresentation()
          == AnyFunctionType::Representation::Block
        && nativeFTy->getRepresentation()
          != AnyFunctionType::Representation::Block) {
      return SGF.emitBlockToFunc(loc, unwrapBridgedOptionals(v), bridgedFTy,
                                 nativeFTy,
                                 loweredNativeTy.castTo<SILFunctionType>());
    }
  }

  // Bridge via _ObjectiveCBridgeable.
  if (auto conformance =
        SGF.SGM.getConformanceToObjectiveCBridgeable(loc, nativeType)) {
    if (auto result = emitBridgeObjectiveCToNative(SGF, loc, v, bridgedType,
                                                   conformance)) {
      --bridgedOptionalsToUnwrap;
      return unwrapBridgedOptionals(*result);
    }

    assert(SGF.SGM.getASTContext().Diags.hadAnyError() &&
           "Bridging code should have complained");
    return SGF.emitUndef(nativeType);
  }

  // id-to-Any bridging.
  if (nativeType->isAny()) {
    // If this is not a call result, use the normal erasure logic.
    if (!isCallResult) {
      return SGF.emitTransformedValue(loc, unwrapBridgedOptionals(v),
                                      bridgedType, nativeType, C);
    }

    // Otherwise, we use more complicated logic that handles results that
    // were unexpectedly null.

    assert(bridgedType.isAnyClassReferenceType());

    // Convert to AnyObject if necessary.
    CanType anyObjectTy =
      SGF.getASTContext().getAnyObjectType()->getCanonicalType();
    if (bridgedType != anyObjectTy) {
      v = SGF.emitTransformedValue(loc, unwrapBridgedOptionals(v), bridgedType,
                                   anyObjectTy);
    }

    // TODO: Ever need to handle +0 values here?
    assert(v.hasCleanup());

    // Use a runtime call to bridge the AnyObject to Any. We do this instead of
    // a simple AnyObject-to-Any upcast because the ObjC API may have returned
    // a null object in spite of its annotation.
    
    // Bitcast to Optional. This provides a barrier to the optimizer to prevent
    // it from attempting to eliminate null checks.
    auto optionalBridgedTy = SILType::getOptionalType(loweredBridgedTy);
    auto optionalMV = SGF.B.createUncheckedBitCast(
        loc, unwrapBridgedOptionals(v), optionalBridgedTy);
    return SGF.emitApplyOfLibraryIntrinsic(loc,
                           SGF.getASTContext().getBridgeAnyObjectToAny(),
                           SubstitutionMap(), optionalMV, C)
              .getAsSingleValue(SGF, loc);
  }

  // Bridge NSError to Error.
  if (bridgedType == SGF.SGM.Types.getNSErrorType())
    return SGF.emitBridgedToNativeError(loc, unwrapBridgedOptionals(v));

  return unwrapBridgedOptionals(v);
}

ManagedValue SILGenFunction::emitBridgedToNativeValue(SILLocation loc,
                                                      ManagedValue v,
                                                      CanType bridgedType,
                                                      CanType nativeType,
                                                      SILType loweredNativeTy,
                                                      SGFContext C,
                                                      bool isCallResult) {
  loweredNativeTy = loweredNativeTy.getObjectType();
  SILType loweredBridgedTy = v.getType();
  return emitCBridgedToNativeValue(
      *this, loc, v, bridgedType, loweredBridgedTy, nativeType, loweredNativeTy,
      /*bridgedOptionalsToUnwrap=*/0, isCallResult, C);
}

/// Bridge a possibly-optional foreign error type to Error.
ManagedValue SILGenFunction::emitBridgedToNativeError(SILLocation loc,
                                                  ManagedValue bridgedError) {
  // If the incoming error is non-optional, just do an existential erasure.
  auto bridgedErrorTy = bridgedError.getType().getASTType();
  if (!bridgedErrorTy.getOptionalObjectType()) {
    auto nativeErrorTy = SILType::getExceptionType(getASTContext());

    auto conformance = SGM.getNSErrorConformanceToError();
    if (!conformance)
      return emitUndef(nativeErrorTy);
    ProtocolConformanceRef conformanceArray[] = {
      ProtocolConformanceRef(conformance)
    };
    auto conformances = getASTContext().AllocateCopy(conformanceArray);

    return B.createInitExistentialRef(loc, nativeErrorTy, bridgedErrorTy,
                                      bridgedError, conformances);
  }

  // Otherwise, we need to call a runtime function to potential substitute
  // a standard error for a nil NSError.
  auto bridgeFn = emitGlobalFunctionRef(loc, SGM.getNSErrorToErrorFn());
  auto bridgeFnType = bridgeFn->getType().castTo<SILFunctionType>();
  assert(bridgeFnType->getNumResults() == 1);
  assert(bridgeFnType->getResults()[0].getConvention()
         == ResultConvention::Owned);

  assert(bridgeFnType->getParameters()[0].getConvention()
	       == ParameterConvention::Direct_Guaranteed);
  (void) bridgeFnType;

  SILValue arg = bridgedError.getValue();

  SILValue nativeError = B.createApply(loc, bridgeFn, {}, arg);
  return emitManagedRValueWithCleanup(nativeError);
}

/// Bridge Error to a foreign error type.
ManagedValue SILGenFunction::emitNativeToBridgedError(SILLocation loc,
                                                      ManagedValue nativeError,
                                                      CanType nativeType,
                                                      CanType bridgedErrorType){
  // Handle injections into optional.
  if (auto bridgedObjectType = bridgedErrorType.getOptionalObjectType()) {
    auto loweredBridgedOptionalTy =
      SILType::getPrimitiveObjectType(bridgedErrorType);
    return emitOptionalSome(
        loc, loweredBridgedOptionalTy,
        [&](SILGenFunction &SGF, SILLocation loc, SGFContext C) {
          SILType loweredBridgedObjectTy =
              loweredBridgedOptionalTy.getOptionalObjectType();
          return emitNativeToBridgedValue(loc, nativeError, nativeType,
                                          bridgedObjectType,
                                          loweredBridgedObjectTy);
        });
  }

  assert(bridgedErrorType == SGM.Types.getNSErrorType() &&
         "only handling NSError for now");

  // The native error might just be a value of a type that conforms to
  // Error.  This should be a subtyping or erasure conversion of the sort
  // that we can do automatically.
  // FIXME: maybe we should use a different entrypoint for this case, to
  // avoid the code size and performance overhead of forming the box?
  nativeError = emitUnabstractedCast(*this, loc, nativeError, nativeType,
                                     getASTContext().getErrorExistentialType());

  auto bridgeFn = emitGlobalFunctionRef(loc, SGM.getErrorToNSErrorFn());
  auto bridgeFnType = bridgeFn->getType().castTo<SILFunctionType>();
  assert(bridgeFnType->getNumResults() == 1);
  assert(bridgeFnType->getResults()[0].getConvention()
         == ResultConvention::Owned);
  assert(bridgeFnType->getParameters()[0].getConvention()
         == ParameterConvention::Direct_Guaranteed);
  (void) bridgeFnType;

  SILValue arg = nativeError.getValue();

  SILValue bridgedError = B.createApply(loc, bridgeFn, {}, arg);
  return emitManagedRValueWithCleanup(bridgedError);
}

//===----------------------------------------------------------------------===//
// ObjC method thunks
//===----------------------------------------------------------------------===//

static SILValue emitBridgeReturnValue(SILGenFunction &SGF,
                                      SILLocation loc,
                                      SILValue result,
                                      CanType formalNativeTy,
                                      CanType formalBridgedTy,
                                      SILType loweredBridgedTy) {
  Scope scope(SGF.Cleanups, CleanupLocation(loc));

  ManagedValue native = SGF.emitManagedRValueWithCleanup(result);
  ManagedValue bridged =
    SGF.emitNativeToBridgedValue(loc, native, formalNativeTy, formalBridgedTy,
                                 loweredBridgedTy);
  return bridged.forward(SGF);
}

/// Take an argument at +0 and bring it to +1.
static SILValue emitObjCUnconsumedArgument(SILGenFunction &SGF,
                                           SILLocation loc,
                                           SILValue arg) {
  auto &lowering = SGF.getTypeLowering(arg->getType());
  // If address-only, make a +1 copy and operate on that.
  if (lowering.isAddressOnly()) {
    auto tmp = SGF.emitTemporaryAllocation(loc, arg->getType().getObjectType());
    SGF.B.createCopyAddr(loc, arg, tmp, IsNotTake, IsInitialization);
    return tmp;
  }

  return lowering.emitCopyValue(SGF.B, loc, arg);
}

static CanAnyFunctionType substGenericArgs(CanAnyFunctionType fnType,
                                           SubstitutionMap subs) {
  if (auto genericFnType = dyn_cast<GenericFunctionType>(fnType)) {
    return cast<FunctionType>(genericFnType->substGenericArgs(subs)
                                           ->getCanonicalType());
  }
  return fnType;
}

/// Bridge argument types and adjust retain count conventions for an ObjC thunk.
static SILFunctionType *
emitObjCThunkArguments(SILGenFunction &SGF, SILLocation loc, SILDeclRef thunk,
                       SmallVectorImpl<SILValue> &args,
                       SILValue &foreignErrorSlot, SILValue &foreignAsyncSlot,
                       llvm::Optional<ForeignErrorConvention> &foreignError,
                       llvm::Optional<ForeignAsyncConvention> &foreignAsync,
                       CanType &nativeFormalResultTy,
                       CanType &bridgedFormalResultTy) {
  SILDeclRef native = thunk.asForeign(false);

  auto subs = SGF.F.getForwardingSubstitutionMap();

  auto objcInfo =
      SGF.SGM.Types.getConstantInfo(SGF.getTypeExpansionContext(), thunk);
  auto objcFnTy = objcInfo.SILFnType->substGenericArgs(
      SGF.SGM.M, subs, SGF.getTypeExpansionContext());
  auto objcFormalFnTy = substGenericArgs(objcInfo.LoweredType, subs);

  auto swiftInfo =
      SGF.SGM.Types.getConstantInfo(SGF.getTypeExpansionContext(), native);
  auto swiftFnTy = swiftInfo.SILFnType->substGenericArgs(
      SGF.SGM.M, subs, SGF.getTypeExpansionContext());
  auto swiftFormalFnTy = substGenericArgs(swiftInfo.LoweredType, subs);
  SILFunctionConventions swiftConv(swiftFnTy, SGF.SGM.M);

  SmallVector<ManagedValue, 8> bridgedArgs;
  bridgedArgs.reserve(objcFnTy->getParameters().size());

  // Find the foreign error and async conventions if we have one.
  if (thunk.hasDecl()) {
    if (auto func = dyn_cast<AbstractFunctionDecl>(thunk.getDecl())) {
      foreignError = func->getForeignErrorConvention();
      foreignAsync = func->getForeignAsyncConvention();
    }
  }

  // We don't know what to do with indirect results from the Objective-C side.
  assert(objcFnTy->getNumIndirectFormalResults() == 0
         && "Objective-C methods cannot have indirect results");

  auto bridgedFormalTypes = getParameterTypes(objcFormalFnTy.getParams());
  bridgedFormalResultTy = objcFormalFnTy.getResult();

  auto nativeFormalTypes = getParameterTypes(swiftFormalFnTy.getParams());
  nativeFormalResultTy = swiftFormalFnTy.getResult();

  // Emit the other arguments, taking ownership of arguments if necessary.
  auto inputs = objcFnTy->getParameters();
  auto nativeInputs = swiftFnTy->getParameters();
  auto fnConv = SGF.silConv.getFunctionConventions(swiftFnTy);
  assert(nativeInputs.size() == bridgedFormalTypes.size());
  assert(nativeInputs.size() == nativeFormalTypes.size());
  assert(inputs.size() ==
           nativeInputs.size() + unsigned(foreignError.has_value())
                               + unsigned(foreignAsync.has_value()));
  for (unsigned i = 0, e = inputs.size(); i < e; ++i) {
    SILType argTy = SGF.getSILType(inputs[i], objcFnTy);
    SILValue arg = SGF.F.begin()->createFunctionArgument(argTy);

    // If this parameter is the foreign error or completion slot, pull it out.
    // It does not correspond to a native argument.
    if (foreignError && i == foreignError->getErrorParameterIndex()) {
      foreignErrorSlot = arg;
      continue;
    }

    if (foreignAsync && i == foreignAsync->completionHandlerParamIndex()) {
      // Copy the block.
      foreignAsyncSlot = SGF.B.createCopyBlock(loc, arg);
      // If the argument is consumed, we're still responsible for releasing the
      // original.
      if (inputs[i].isConsumed())
        SGF.emitManagedRValueWithCleanup(arg);
      continue;
    }

    // If the argument is a block, copy it.
    if (argTy.isBlockPointerCompatible()) {
      auto copy = SGF.B.createCopyBlock(loc, arg);
      // If the argument is consumed, we're still responsible for releasing the
      // original.
      if (inputs[i].isConsumed())
        SGF.emitManagedRValueWithCleanup(arg);
      arg = copy;
    }
    // Convert the argument to +1 if necessary.
    else if (!inputs[i].isConsumed()) {
      arg = emitObjCUnconsumedArgument(SGF, loc, arg);
    }

    auto managedArg = SGF.emitManagedRValueWithCleanup(arg);

    bridgedArgs.push_back(managedArg);
  }

  assert(bridgedArgs.size()
           + unsigned(foreignError.has_value())
           + unsigned(foreignAsync.has_value())
        == objcFnTy->getParameters().size() &&
         "objc inputs don't match number of arguments?!");
  assert(bridgedArgs.size() == swiftFnTy->getParameters().size() &&
         "swift inputs don't match number of arguments?!");
  assert((foreignErrorSlot || !foreignError) &&
         "didn't find foreign error slot");

  // Bridge the input types.
  assert(bridgedArgs.size() == nativeInputs.size());
  for (unsigned i = 0, size = bridgedArgs.size(); i < size; ++i) {
    // Consider the bridged values to be "call results" since they're coming
    // from potentially nil-unsound ObjC callers.
    ManagedValue native = SGF.emitBridgedToNativeValue(
        loc, bridgedArgs[i], bridgedFormalTypes[i], nativeFormalTypes[i],
        swiftFnTy->getParameters()[i].getSILStorageType(
            SGF.SGM.M, swiftFnTy, SGF.getTypeExpansionContext()),
        SGFContext(),
        /*isCallResult*/ true);
    SILValue argValue;

    // This can happen if the value is resilient in the calling convention
    // but not resilient locally.
    if (fnConv.isSILIndirect(nativeInputs[i]) &&
        !native.getType().isAddress()) {
      auto buf = SGF.emitTemporaryAllocation(loc, native.getType());
      native.forwardInto(SGF, loc, buf);
      native = SGF.emitManagedBufferWithCleanup(buf);
    }

    if (nativeInputs[i].isConsumed()) {
      argValue = native.forward(SGF);
    } else if (nativeInputs[i].isGuaranteed()) {
      argValue = native.borrow(SGF, loc).getUnmanagedValue();
    } else {
      argValue = native.getValue();
    }

    args.push_back(argValue);
  }

  return objcFnTy;
}

SILFunction *SILGenFunction::emitNativeAsyncToForeignThunk(SILDeclRef thunk) {
  assert(thunk.isForeign);
  assert(thunk.hasAsync());
  SILDeclRef native = thunk.asForeign(false);

  // Use the same generic environment as the native entry point.
  F.setGenericEnvironment(SGM.Types.getConstantGenericEnvironment(native));

  // Collect the arguments and make copies of them we can absorb into the
  // closure.
  auto subs = F.getForwardingSubstitutionMap();
  SmallVector<SILValue, 4> closureArgs;
  auto objcInfo =
      SGM.Types.getConstantInfo(getTypeExpansionContext(), thunk);
  auto objcFnTy = objcInfo.SILFnType->substGenericArgs(
      SGM.M, subs, getTypeExpansionContext());
  auto loc = thunk.getAsRegularLocation();
  loc.markAutoGenerated();
  
  Scope scope(*this, loc);

  for (auto input : objcFnTy->getParameters()) {
    SILType argTy = getSILType(input, objcFnTy);
    SILValue arg = F.begin()->createFunctionArgument(argTy);
    // Copy block arguments.
    if (argTy.isBlockPointerCompatible()) {
      auto argCopy = B.createCopyBlock(loc, arg);
      // If the argument is consumed, we're still responsible for releasing the
      // original.
      if (input.isConsumed())
        emitManagedRValueWithCleanup(arg);
      arg = argCopy;
    } else if (!input.isConsumed()) {
      arg = emitObjCUnconsumedArgument(*this, loc, arg);
    }
    auto managedArg = emitManagedRValueWithCleanup(arg);
    closureArgs.push_back(managedArg.forward(*this));
  }
  
  // Create the closure implementation function. It has the same signature,
  // but is swiftcc and async.
  auto closureExtInfo = objcFnTy->getExtInfo().intoBuilder()
    .withRepresentation(SILFunctionTypeRepresentation::Thin)
    .withAsync()
    .withConcurrent()
    .build();
  auto closureTy = objcFnTy->getWithExtInfo(closureExtInfo);
  
  SmallString<64> closureName(F.getName().begin(), F.getName().end());
  // Trim off the thunk suffix and mangle this like a closure nested inside the
  // thunk (which it sorta is)
  char thunkSuffix[2] = {closureName.pop_back_val(),
                         closureName.pop_back_val()};
  assert(thunkSuffix[1] == 'T'
         && thunkSuffix[0] == 'o'
         && "not an objc thunk?");
  closureName += "yyYacfU_"; // closure with type () async -> ()
  closureName.push_back(thunkSuffix[1]);
  closureName.push_back(thunkSuffix[0]);

  SILGenFunctionBuilder fb(SGM);
  auto closure = fb.getOrCreateSharedFunction(loc, closureName,
                                              closureTy,
                                              IsBare,
                                              IsNotTransparent,
                                              F.isSerialized(),
                                              ProfileCounter(),
                                              IsThunk,
                                              IsNotDynamic,
                                              IsNotDistributed,
                                              IsNotRuntimeAccessible);
  
  auto closureRef = B.createFunctionRef(loc, closure);
  
  auto closureVal = B.createPartialApply(loc, closureRef, subs,
                                      closureArgs,
                                      ParameterConvention::Direct_Guaranteed);
  auto closureMV = emitManagedRValueWithCleanup(closureVal);
  // Pass the closure on to the intrinsic to spawn it on a task.
  auto spawnTask = SGM.getRunTaskForBridgedAsyncMethod();
  emitApplyOfLibraryIntrinsic(loc, spawnTask, {}, closureMV, SGFContext());
  
  scope.pop();
  
  // Return void to the immediate caller.
  B.createReturn(loc, SILUndef::get(SGM.Types.getEmptyTupleType(), F));
  
  return closure;
}

void SILGenFunction::emitNativeToForeignThunk(SILDeclRef thunk) {
  assert(thunk.isForeign);
  SILDeclRef native = thunk.asForeign(false);

  if (thunk.hasDecl()) {
    if (shouldLowerToUnavailableCodeStub(thunk.getDecl()))
      emitApplyOfUnavailableCodeReached();
  }

  // If we're calling a native non-designated class initializer, we have to
  // discard the `self` object we were given, since
  // Swift convenience initializers only have allocating entry points that
  // create whole new objects.
  bool isInitializingToAllocatingInitThunk = false;
  if (native.kind == SILDeclRef::Kind::Initializer) {
    if (auto ctor = dyn_cast<ConstructorDecl>(native.getDecl())) {
      if (!ctor->isDesignatedInit() && !ctor->isObjC()) {
        isInitializingToAllocatingInitThunk = true;
        native = SILDeclRef(ctor, SILDeclRef::Kind::Allocator);
      }
    }
  }

  auto nativeInfo = getConstantInfo(getTypeExpansionContext(), native);
  auto subs = F.getForwardingSubstitutionMap();
  auto substTy = nativeInfo.SILFnType->substGenericArgs(
      SGM.M, subs, getTypeExpansionContext());
  SILFunctionConventions substConv(substTy, SGM.M);

  // Use the same generic environment as the native entry point.
  F.setGenericEnvironment(SGM.Types.getConstantGenericEnvironment(native));

  auto loc = thunk.getAsRegularLocation();
  loc.markAutoGenerated();
  Scope scope(Cleanups, CleanupLocation(loc));
  
  // Hop to the actor for the method's actor constraint, if any.
  // Note that, since an async native-to-foreign thunk only ever runs in a
  // task purpose-built for running the Swift async code triggering the
  // completion handler, there is no need for us to hop back to the existing
  // executor, since the task will end after we invoke the completion handler.
  if (F.isAsync()) {
    llvm::Optional<ActorIsolation> isolation;
    if (thunk.hasDecl()) {
      isolation = getActorIsolation(thunk.getDecl());
    }

    // A hop is only needed in the thunk if it is global-actor isolated.
    // Native, instance-isolated async methods will hop in the prologue.
    if (isolation && isolation->isGlobalActor()) {
      emitPrologGlobalActorHop(loc, isolation->getGlobalActor());
    }
  }

  // If we are bridging a Swift method with Any return value(s), create a
  // stack allocation to hold the result(s), since Any is address-only.
  SmallVector<SILValue, 4> args;
  if (substConv.hasIndirectSILResults()) {
    for (auto result : substConv.getResults()) {
      if (!substConv.isSILIndirect(result)) {
        continue;
      }
      args.push_back(emitTemporaryAllocation(
                loc, substConv.getSILType(result, getTypeExpansionContext())));
    }
  }

  // If the '@objc' was inferred due to deprecated rules,
  // emit a Builtin.swift3ImplicitObjCEntrypoint().
  //
  // However, don't do so for 'dynamic' members, which must use Objective-C
  // dispatch and therefore create many false positives.
  if (thunk.hasDecl()) {
    auto decl = thunk.getDecl();

    // For an accessor, look at the storage declaration's attributes.
    if (auto accessor = dyn_cast<AccessorDecl>(decl)) {
      decl = accessor->getStorage();
    }

    if (auto attr = decl->getAttrs().getAttribute<ObjCAttr>()) {
      // If @objc was inferred based on the Swift 3 @objc inference rules, emit
      // a call to Builtin.swift3ImplicitObjCEntrypoint() to enable runtime
      // logging of the uses of such entrypoints.
      if (attr->isSwift3Inferred() && !decl->shouldUseObjCDispatch()) {
        // Get the starting source location of the declaration so we can say
        // exactly where to stick '@objc'.
        SourceLoc objcInsertionLoc =
          decl->getAttributeInsertionLoc(/*modifier*/ false);

        auto objcInsertionLocArgs
          = emitSourceLocationArgs(objcInsertionLoc, loc);
        
        B.createBuiltin(loc,
          getASTContext().getIdentifier("swift3ImplicitObjCEntrypoint"),
          getModule().Types.getEmptyTupleType(), { }, {
            objcInsertionLocArgs.filenameStartPointer.forward(*this),
            objcInsertionLocArgs.filenameLength.forward(*this),
            objcInsertionLocArgs.line.forward(*this),
            objcInsertionLocArgs.column.forward(*this)
          });
      }
    }
  }

  // Now, enter a cleanup used for bridging the arguments. Note that if we
  // have an indirect result, it must be outside of this scope, otherwise
  // we will deallocate it too early.
  Scope argScope(Cleanups, CleanupLocation(loc));

  // Bridge the arguments.
  llvm::Optional<ForeignErrorConvention> foreignError;
  llvm::Optional<ForeignAsyncConvention> foreignAsync;
  SILValue foreignErrorSlot;
  SILValue foreignAsyncSlot;
  CanType nativeFormalResultType, bridgedFormalResultType;
  auto objcFnTy = emitObjCThunkArguments(*this, loc, thunk, args,
                                         foreignErrorSlot, foreignAsyncSlot,
                                         foreignError, foreignAsync,
                                         nativeFormalResultType,
                                         bridgedFormalResultType);

  // Throw away the partially-initialized `self` value we were given if we're
  // bridging from an initializing to allocating entry point.
  if (isInitializingToAllocatingInitThunk) {
    auto oldSelf = args.pop_back_val();
    auto oldSelfTy = B.createValueMetatype(loc,
         SILType::getPrimitiveObjectType(
           CanMetatypeType::get(oldSelf->getType().getASTType(),
                                MetatypeRepresentation::Thick)),
         oldSelf);
    
    B.createDeallocPartialRef(loc, oldSelf, oldSelfTy);
    
    // Pass the dynamic type on to the native allocating initializer.
    args.push_back(oldSelfTy);
    native = SILDeclRef(native.getDecl(), SILDeclRef::Kind::Allocator);
  }

  SILFunctionConventions objcConv(CanSILFunctionType(objcFnTy), SGM.M);
  SILFunctionConventions nativeConv(CanSILFunctionType(nativeInfo.SILFnType),
                                    SGM.M);
  auto swiftResultTy = F.mapTypeIntoContext(
      nativeConv.getSILResultType(getTypeExpansionContext()));
  auto objcResultTy = objcConv.getSILResultType(getTypeExpansionContext());

  // Call the native entry point.
  SILValue nativeFn = emitGlobalFunctionRef(loc, native, nativeInfo);

  SILValue result;
  
  CanSILFunctionType completionTy;
  bool completionIsOptional = false;
  if (foreignAsyncSlot) {
    completionTy = foreignAsyncSlot->getType().getAs<SILFunctionType>();
    if (!completionTy) {
      completionTy = foreignAsyncSlot->getType().getOptionalObjectType()
        .castTo<SILFunctionType>();
      completionIsOptional = true;
    }
  }
  
  // Helper function to take ownership of the completion handler from the
  // foreign async slot, and unwrap it if it's in an optional.
  auto consumeAndUnwrapCompletionBlock = [&](SILValue &completionBlock,
                                             SILBasicBlock *&doneBBOrNull) {
    auto completionBlockMV = emitManagedRValueWithCleanup(foreignAsyncSlot);
    
    // If the completion handler argument is nullable, and the caller gave us
    // no completion handler, discard the result.
    completionBlock = completionBlockMV.borrow(*this, loc).getValue();
    doneBBOrNull = nullptr;
    if (completionIsOptional) {
      doneBBOrNull = createBasicBlock();
      auto hasCompletionBB = createBasicBlock();
      auto noCompletionBB = createBasicBlock();

      std::pair<EnumElementDecl *, SILBasicBlock *> dests[] = {
        {getASTContext().getOptionalSomeDecl(), hasCompletionBB},
        {getASTContext().getOptionalNoneDecl(), noCompletionBB},
      };

      auto *switchEnum =
          B.createSwitchEnum(loc, completionBlock, nullptr, dests);

      B.emitBlock(noCompletionBB);
      B.createBranch(loc, doneBBOrNull);

      B.emitBlock(hasCompletionBB);
      completionBlock = switchEnum->createOptionalSomeResult();
    }
  };
  
  auto pushErrorFlag = [&](bool hasError,
                           SmallVectorImpl<SILValue> &completionHandlerArgs) {
    bool errorFlagIsZeroOnError = foreignAsync->completionHandlerFlagIsErrorOnZero();
    auto errorFlagIndex = foreignAsync->completionHandlerFlagParamIndex();
    auto errorFlagTy = completionTy->getParameters()[*errorFlagIndex]
      .getSILStorageInterfaceType();
    
    auto errorFlag = emitWrapIntegerLiteral(loc, errorFlagTy,
                                            hasError ^ errorFlagIsZeroOnError);
    
    completionHandlerArgs.push_back(errorFlag);
  };
  
  // Helper function to pass a native async function's result as arguments to
  // the ObjC completion handler block.
  auto passResultToCompletionHandler = [&](SILValue result) -> SILValue {
    Scope completionArgScope(*this, loc);
    
    SmallVector<SILValue, 2> completionHandlerArgs;
    
    auto asyncResult = emitManagedRValueWithCleanup(result);
    
    SILValue completionBlock;
    SILBasicBlock *doneBB;
    consumeAndUnwrapCompletionBlock(completionBlock, doneBB);
    
    auto pushArg = [&](ManagedValue arg,
                       CanType nativeFormalTy,
                       SILParameterInfo param) {
      auto bridgedTy = param.getInterfaceType();
      auto bridgedArg = emitNativeToBridgedValue(loc,
                                 arg, nativeFormalTy,
                                 bridgedTy,
                                 SILType::getPrimitiveObjectType(bridgedTy));
      completionHandlerArgs.push_back(bridgedArg.borrow(*this, loc).getValue());
    };
 
    Scope completionArgDestructureScope(*this, loc);

    auto errorParamIndex = foreignAsync->completionHandlerErrorParamIndex();
    auto errorFlagIndex = foreignAsync->completionHandlerFlagParamIndex();
    auto pushErrorPlaceholder = [&]{
      auto errorArgTy = completionTy->getParameters()[*errorParamIndex]
        .getSILStorageInterfaceType();
      
      // Error type must be optional. We pass nil for a successful return
      auto none = B.createOptionalNone(loc, errorArgTy);
      completionHandlerArgs.push_back(none);
    };
    
    unsigned numResults
      = completionTy->getParameters().size() - errorParamIndex.has_value()
                                             - errorFlagIndex.has_value();
    
    if (numResults == 1) {
      for (unsigned i = 0; i < completionTy->getNumParameters(); ++i) {
        if (errorParamIndex && *errorParamIndex == i) {
          pushErrorPlaceholder();
          continue;
        }
        if (errorFlagIndex && *errorFlagIndex == i) {
          pushErrorFlag(/*has error*/ false, completionHandlerArgs);
          continue;
        }
        
        // Use the indirect return argument if the result is indirect.
        if (substConv.hasIndirectSILResults()) {
          pushArg(emitManagedRValueWithCleanup(args[0]),
                  nativeFormalResultType,
                  completionTy->getParameters()[i]);
        } else {
          pushArg(asyncResult,
                  nativeFormalResultType,
                  completionTy->getParameters()[i]);
        }
      }
    } else {
      // A tuple return maps to multiple completion handler parameters.
      auto formalTuple = cast<TupleType>(nativeFormalResultType);
      
      unsigned indirectResultI = 0;
      unsigned directResultI = 0;
      
      auto directResults = substConv.getDirectSILResults();
      auto hasMultipleDirectResults
        = !directResults.empty() &&
          std::next(directResults.begin()) != directResults.end();
      
      for (unsigned paramI : indices(completionTy->getParameters())) {
        if (errorParamIndex && paramI == *errorParamIndex) {
          pushErrorPlaceholder();
          continue;
        }
        if (errorFlagIndex && paramI == *errorFlagIndex) {
          pushErrorFlag(/*has error*/ false, completionHandlerArgs);
          continue;
        }
        auto elementI = paramI - (errorParamIndex && paramI > *errorParamIndex)
                               - (errorFlagIndex && paramI > *errorFlagIndex);
        auto param = completionTy->getParameters()[paramI];
        auto formalTy = formalTuple.getElementType(elementI);
        ManagedValue argPiece;
        
        auto result = substConv.getResults()[elementI];
        if (substConv.isSILIndirect(result)) {
          // Take the arg piece from the indirect return arguments.
          argPiece = emitManagedRValueWithCleanup(args[indirectResultI++]);
        } else if (hasMultipleDirectResults) {
          // Take the arg piece from one of the tuple elements of the direct
          // result tuple from the apply.
          argPiece = B.createTupleExtract(loc, asyncResult, directResultI++);
        } else {
          // Take the entire direct result from the apply as the arg piece.
          argPiece = asyncResult;
        }
        
        pushArg(argPiece, formalTy, param);
      }
    }
    // Pass the bridged results on to the completion handler.
    B.createApply(loc, completionBlock, {}, completionHandlerArgs);
    completionArgDestructureScope.pop();
    
    if (doneBB) {
      B.createBranch(loc, doneBB);
      B.emitBlock(doneBB);
    }
    
    // The immediate function result is an empty tuple.
    return SILUndef::get(SGM.Types.getEmptyTupleType(), F);
  };
    
  if (!substTy->hasErrorResult()) {
    // Create the apply.
    result = B.createApply(loc, nativeFn, subs, args);
  
    // Leave the argument cleanup scope immediately.  This isn't really
    // necessary; it just limits lifetimes a little bit more.
    argScope.pop();

    // Now bridge the return value.
    // If this is an async method, we forward the results of the async call to
    // the completion handler.
    if (foreignAsync) {
      result = passResultToCompletionHandler(result);
    } else {
      if (substConv.hasIndirectSILResults()) {
        assert(substTy->getNumResults() == 1);
        result = args[0];
      }
      result = emitBridgeReturnValue(*this, loc, result, nativeFormalResultType,
                                     bridgedFormalResultType, objcResultTy);
    }
  } else {
    SILBasicBlock *contBB = createBasicBlock();
    SILBasicBlock *errorBB = createBasicBlock();
    SILBasicBlock *normalBB = createBasicBlock();
    B.createTryApply(loc, nativeFn, subs, args, normalBB, errorBB);

    // Emit the non-error destination.
    {
      B.emitBlock(normalBB);
      SILValue nativeResult =
          normalBB->createPhiArgument(swiftResultTy, OwnershipKind::Owned);

      if (foreignAsync) {
        // If the function is async, pass the results as the success argument(s)
        // to the completion handler, with a nil error.
        passResultToCompletionHandler(nativeResult);
        B.createBranch(loc, contBB);
      } else {
        if (substConv.hasIndirectSILResults()) {
          assert(substTy->getNumResults() == 1);
          nativeResult = args[0];
        }
        // In this branch, the eventual return value is mostly created
        // by bridging the native return value, but we may need to
        // adjust it slightly.
        SILValue bridgedResult =
          emitBridgeReturnValueForForeignError(loc, nativeResult,
                                               nativeFormalResultType,
                                               bridgedFormalResultType,
                                               objcResultTy,
                                               foreignErrorSlot, *foreignError);
        B.createBranch(loc, contBB, bridgedResult);
      }
    }

    // Emit the error destination.
    {
      B.emitBlock(errorBB);
      
      SILValue nativeError = errorBB->createPhiArgument(
          substConv.getSILErrorType(getTypeExpansionContext()),
          OwnershipKind::Owned);

      if (foreignAsync) {
        // If the function is async, pass the bridged error along to the
        // completion handler, with dummy values for the other argument(s).
        Scope completionArgScope(*this, loc);
        
        auto nativeErrorMV = emitManagedRValueWithCleanup(nativeError);
        
        SILValue completionBlock;
        SILBasicBlock *doneBB;
        consumeAndUnwrapCompletionBlock(completionBlock, doneBB);

        Scope completionErrorScope(*this, loc);

        SmallVector<SILValue, 2> completionHandlerArgs;
        auto completionTy = completionBlock->getType().castTo<SILFunctionType>();
        auto errorParamIndex = *foreignAsync->completionHandlerErrorParamIndex();
        auto errorFlagIndex = foreignAsync->completionHandlerFlagParamIndex();
        auto completionErrorTy = completionTy->getParameters()[errorParamIndex]
          .getInterfaceType();
        auto bridgedError = emitNativeToBridgedError(loc,
                                           nativeErrorMV,
                                           nativeError->getType().getASTType(),
                                           completionErrorTy);
        
        // Fill in placeholder arguments, and put the bridged error in its
        // rightful place.
        for (unsigned i : indices(completionTy->getParameters())) {
          if (i == errorParamIndex) {
            completionHandlerArgs.push_back(bridgedError.borrow(*this, loc).getValue());
            continue;
          }
          
          if (errorFlagIndex && i == *errorFlagIndex) {
            pushErrorFlag(/*has error*/ true, completionHandlerArgs);
            continue;
          }
          
          // For non-error arguments, pass a placeholder.
          // If the argument type is non-trivial, it must be Optional, and
          // we pass nil.
          auto param = completionTy->getParameters()[i];
          auto paramTy = param.getSILStorageInterfaceType();
          if (paramTy.isTrivial(F)) {
            // If it's trivial, the value passed doesn't matter.
            completionHandlerArgs.push_back(SILUndef::get(paramTy, F.getModule()));
          } else {
            // If it's not trivial, it must be a nullable class type. Pass
            // nil.
            auto none = B.createOptionalNone(loc, paramTy);
            completionHandlerArgs.push_back(none);
          }
        }
        // Pass the bridged error on to the completion handler.
        B.createApply(loc, completionBlock, {}, completionHandlerArgs);

        completionErrorScope.pop();
        
        if (doneBB) {
          B.createBranch(loc, doneBB);
          B.emitBlock(doneBB);
        }
        completionArgScope.pop();

        B.createBranch(loc, contBB);
      } else {
        // In this branch, the eventual return value is mostly invented.
        // Store the native error in the appropriate location and return.
        SILValue bridgedResult =
          emitBridgeErrorForForeignError(loc, nativeError, objcResultTy,
                                         foreignErrorSlot, *foreignError);
        B.createBranch(loc, contBB, bridgedResult);
      }
    }

    // Emit the join block.
    B.emitBlock(contBB);
    
    if (foreignAsync) {
      // After invoking the completion handler, our immediate return value is
      // void.
      result = SILUndef::get(SGM.Types.getEmptyTupleType(), F);
    } else {
      result = contBB->createPhiArgument(objcResultTy, OwnershipKind::Owned);
    }

    // Leave the scope now.
    argScope.pop();
  }

  scope.pop();
  B.createReturn(loc, result);
}

static SILValue getThunkedForeignFunctionRef(SILGenFunction &SGF,
                                             AbstractFunctionDecl *fd,
                                             SILDeclRef foreign,
                                             ArrayRef<ManagedValue> args,
                                             const SILConstantInfo &foreignCI) {
  assert(foreign.isForeign);

  // Produce an objc_method when thunking ObjC methods.
  if (foreignCI.SILFnType->getRepresentation() ==
      SILFunctionTypeRepresentation::ObjCMethod) {
    auto *objcDecl =
        dyn_cast_or_null<clang::ObjCMethodDecl>(fd->getClangDecl());
    const bool isObjCDirect = objcDecl && objcDecl->isDirectMethod();
    if (isObjCDirect) {
      auto *fn = SGF.SGM.getFunction(foreign, NotForDefinition);
      return SGF.B.createFunctionRef(fd, fn);
    }

    SILValue thisArg = args.back().getValue();
    return SGF.B.createObjCMethod(fd, thisArg, foreign, foreignCI.getSILType());
  }

  // Otherwise, emit a function_ref.
  return SGF.emitGlobalFunctionRef(fd, foreign);
}

/// Generate code to emit a thunk with native conventions that calls a
/// function with foreign conventions.
void SILGenFunction::emitForeignToNativeThunk(SILDeclRef thunk) {
  assert(!thunk.isForeign && "foreign-to-native thunks only");

  // Wrap the function in its original form.

  auto fd = cast<AbstractFunctionDecl>(thunk.getDecl());
  auto nativeCI = getConstantInfo(getTypeExpansionContext(), thunk);
  auto nativeFnTy = F.getLoweredFunctionType();
  assert(nativeFnTy == nativeCI.SILFnType);

  if (shouldLowerToUnavailableCodeStub(fd))
    emitApplyOfUnavailableCodeReached();

  // Use the same generic environment as the native entry point.
  F.setGenericEnvironment(SGM.Types.getConstantGenericEnvironment(thunk));
  
  SILDeclRef foreignDeclRef = thunk.asForeign(true);
  SILConstantInfo foreignCI =
      getConstantInfo(getTypeExpansionContext(), foreignDeclRef);
  auto foreignFnTy = foreignCI.SILFnType;

  // Find the foreign error/async convention and 'self' parameter index.
  bool hasError = false;
  llvm::Optional<ForeignAsyncConvention> foreignAsync;
  if (nativeFnTy->isAsync()) {
    foreignAsync = fd->getForeignAsyncConvention();
    assert(foreignAsync && "couldn't find foreign async convention?!");
  }
  llvm::Optional<ForeignErrorConvention> foreignError;
  if (nativeFnTy->hasErrorResult()) {
    hasError = true;
    foreignError = fd->getForeignErrorConvention();
    assert((foreignError || foreignAsync)
           && "couldn't find foreign error or async convention for foreign error!");
  }
  ImportAsMemberStatus memberStatus = fd->getImportAsMemberStatus();

  // Introduce indirect returns if necessary.
  // TODO: Handle exploded results? We don't currently need to since the only
  // bridged indirect type is Any.
  SILValue indirectResult;
  SILFunctionConventions nativeConv(nativeFnTy, SGM.M);
  if (nativeConv.hasIndirectSILResults()) {
    assert(nativeConv.getNumIndirectSILResults() == 1
           && "bridged exploded result?!");
    indirectResult = F.begin()->createFunctionArgument(
        nativeConv.getSingleSILResultType(F.getTypeExpansionContext()));
  }
  
  // Forward the arguments.
  SmallVector<SILValue, 8> params;

  bindParametersForForwarding(fd->getParameters(), params);
  if (thunk.kind != SILDeclRef::Kind::Allocator)
    if (auto *selfDecl = fd->getImplicitSelfDecl())
      bindParameterForForwarding(selfDecl, params);

  // For allocating constructors, 'self' is a metatype, not the 'self' value
  // formally present in the constructor body.
  Type allocatorSelfType;
  if (thunk.kind == SILDeclRef::Kind::Allocator) {
    auto *selfDecl = fd->getImplicitSelfDecl();
    allocatorSelfType = F.mapTypeIntoContext(
      fd->getDeclContext()->getSelfInterfaceType());

    auto selfMetatype =
      CanMetatypeType::get(allocatorSelfType->getCanonicalType());
    auto selfArg = F.begin()->createFunctionArgument(
        getLoweredLoadableType(selfMetatype), selfDecl);
    params.push_back(selfArg);
  }

  // Set up the throw destination if necessary.
  CleanupLocation cleanupLoc(fd);
  if (hasError) {
    prepareRethrowEpilog(cleanupLoc);
  }

  SILValue result;
  {
    Scope scope(Cleanups, fd);

    // Bridge all the arguments.
    SmallVector<ManagedValue, 8> args;
    unsigned foreignArgIndex = 0;

    // A helper function to add a placeholder for a foreign argument in the
    // appropriate position.
    auto maybeAddForeignArg = [&]() -> bool {
      if ((foreignError
           && foreignArgIndex == foreignError->getErrorParameterIndex())
          || (foreignAsync
              && foreignArgIndex == foreignAsync->completionHandlerParamIndex()))
      {
        args.push_back(ManagedValue());
        ++foreignArgIndex;
        return true;
      }
      
      return false;
    };

    {
      bool hasSelfParam = fd->hasImplicitSelfDecl();
      auto foreignFormalParams =
        getParameterTypes(foreignCI.LoweredType.getParams(), hasSelfParam);
      auto nativeFormalParams =
        getParameterTypes(nativeCI.LoweredType.getParams(), hasSelfParam);

      for (unsigned nativeParamIndex : indices(params)) {
        // Bring the parameter to +1.
        auto paramValue = params[nativeParamIndex];
        auto thunkParam = nativeFnTy->getParameters()[nativeParamIndex];
        // TODO: Could avoid a retain if the bridged parameter is also +0 and
        // doesn't require a bridging conversion.
        ManagedValue param;
        switch (thunkParam.getConvention()) {
        case ParameterConvention::Direct_Owned:
          param = emitManagedRValueWithCleanup(paramValue);
          break;
        case ParameterConvention::Direct_Guaranteed:
        case ParameterConvention::Direct_Unowned:
          param = emitManagedRetain(fd, paramValue);
          break;
        case ParameterConvention::Indirect_Inout:
        case ParameterConvention::Indirect_InoutAliasable:
          param = ManagedValue::forLValue(paramValue);
          break;
        case ParameterConvention::Indirect_In:
          param = emitManagedRValueWithCleanup(paramValue);
          break;
        case ParameterConvention::Indirect_In_Guaranteed: {
          auto tmp = emitTemporaryAllocation(fd, paramValue->getType());
          B.createCopyAddr(fd, paramValue, tmp, IsNotTake, IsInitialization);
          param = emitManagedRValueWithCleanup(tmp);
          break;
        }
        case ParameterConvention::Pack_Guaranteed:
        case ParameterConvention::Pack_Owned:
        case ParameterConvention::Pack_Inout:
          llvm_unreachable("bridging a parameter pack?");
        }

        while (maybeAddForeignArg());

        bool isSelf = (hasSelfParam && nativeParamIndex == params.size() - 1);

        if (memberStatus.isInstance()) {
          // Leave space for `self` to be filled in later.
          if (foreignArgIndex == memberStatus.getSelfIndex()) {
            args.push_back({});
            ++foreignArgIndex;
          }
          
          // Use the `self` space we skipped earlier if it's time.
          if (isSelf) {
            foreignArgIndex = memberStatus.getSelfIndex();
          }
        } else if (memberStatus.isStatic() && isSelf) {
          // Lose a static `self` parameter.
          break;
        }

        CanType nativeFormalType =
          F.mapTypeIntoContext(nativeFormalParams[nativeParamIndex])
            ->getCanonicalType();
        CanType foreignFormalType =
          F.mapTypeIntoContext(foreignFormalParams[nativeParamIndex])
            ->getCanonicalType();

        if (isSelf) {
          assert(!nativeCI.LoweredType.getParams()[nativeParamIndex].isInOut() ||
                 nativeFormalType == foreignFormalType &&
                 "Cannot bridge 'self' parameter if passed inout");
        }

        auto foreignParam = foreignFnTy->getParameters()[foreignArgIndex++];
        SILType foreignLoweredTy =
            F.mapTypeIntoContext(foreignParam.getSILStorageType(
                F.getModule(), foreignFnTy, F.getTypeExpansionContext()));

        auto bridged = emitNativeToBridgedValue(fd, param, nativeFormalType,
                                                foreignFormalType,
                                                foreignLoweredTy);
        if (foreignParam.getConvention() == ParameterConvention::Indirect_In ||
            foreignParam.getConvention() == ParameterConvention::Indirect_In_Guaranteed) {
          auto temp = emitTemporaryAllocation(fd, bridged.getType());
          bridged.forwardInto(*this, fd, temp);
          bridged = emitManagedBufferWithCleanup(temp);
        }
        
        if (memberStatus.isInstance() && isSelf) {
          // Fill in the `self` space.
          args[memberStatus.getSelfIndex()] = bridged;
        } else {
          args.push_back(bridged);
        }
      }
    }

    while (maybeAddForeignArg());
    
    // Call the original.
    auto subs = getForwardingSubstitutionMap();
    auto fn = getThunkedForeignFunctionRef(*this, fd, foreignDeclRef, args,
                                           foreignCI);

    auto fnType = fn->getType().castTo<SILFunctionType>();
    fnType = fnType->substGenericArgs(SGM.M, subs, getTypeExpansionContext());

    CanType nativeFormalResultType =
        fd->mapTypeIntoContext(nativeCI.LoweredType.getResult())
            ->getCanonicalType();
    CanType bridgedFormalResultType =
        fd->mapTypeIntoContext(foreignCI.LoweredType.getResult())
            ->getCanonicalType();
    CalleeTypeInfo calleeTypeInfo(
        fnType, AbstractionPattern(nativeFnTy->getInvocationGenericSignature(),
                                   bridgedFormalResultType),
        nativeFormalResultType,
        foreignError,
        foreignAsync,
        ImportAsMemberStatus());
    calleeTypeInfo.origFormalType =
        foreignCI.FormalPattern.getFunctionResultType();

    auto init = indirectResult
                ? useBufferAsTemporary(indirectResult,
                                    getTypeLowering(indirectResult->getType()))
                : nullptr;

    SGFContext context(init.get());
    ResultPlanPtr resultPlan = ResultPlanBuilder::computeResultPlan(
        *this, calleeTypeInfo, fd, context);
    ArgumentScope argScope(*this, fd);
    ManagedValue resultMV =
        emitApply(std::move(resultPlan), std::move(argScope), fd,
                  ManagedValue::forUnmanaged(fn), subs, args, calleeTypeInfo,
                  ApplyOptions(), context, llvm::None)
            .getAsSingleValue(*this, fd);

    if (indirectResult) {
      if (!resultMV.isInContext()) {
        init->copyOrInitValueInto(*this, fd, resultMV, /*isInit*/ true);
        init->finishInitialization(*this);
      }
      init->getManagedAddress().forward(*this);
      result = emitEmptyTuple(fd);
    } else {
      result = resultMV.forward(*this);
    }
  }
  B.createReturn(ImplicitReturnLocation(fd), result);
  // Emit the throw destination.
  emitRethrowEpilog(fd);
}
