//===--- Thunk.cpp - Automatic differentiation thunks ---------*- C++ -*---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Automatic differentiation thunk generation utilities.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "differentiation"

#include "swift/SILOptimizer/Differentiation/Thunk.h"
#include "swift/SILOptimizer/Differentiation/Common.h"

#include "swift/AST/AnyFunctionRef.h"
#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/Requirement.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"

namespace swift {
namespace autodiff {

//===----------------------------------------------------------------------===//
// Thunk helpers
//===----------------------------------------------------------------------===//
// These helpers are copied/adapted from SILGen. They should be refactored and
// moved to a shared location.
//===----------------------------------------------------------------------===//

CanGenericSignature buildThunkSignature(SILFunction *fn, bool inheritGenericSig,
                                        OpenedArchetypeType *openedExistential,
                                        GenericEnvironment *&genericEnv,
                                        SubstitutionMap &contextSubs,
                                        SubstitutionMap &interfaceSubs,
                                        ArchetypeType *&newArchetype) {
  // If there's no opened existential, we just inherit the generic environment
  // from the parent function.
  if (openedExistential == nullptr) {
    auto genericSig = fn->getLoweredFunctionType()->getSubstGenericSignature();
    genericEnv = fn->getGenericEnvironment();
    interfaceSubs = fn->getForwardingSubstitutionMap();
    contextSubs = interfaceSubs;
    return genericSig;
  }

  auto &ctx = fn->getASTContext();
  GenericSignatureBuilder builder(ctx);

  // Add the existing generic signature.
  int depth = 0;
  if (inheritGenericSig) {
    if (auto genericSig =
            fn->getLoweredFunctionType()->getSubstGenericSignature()) {
      builder.addGenericSignature(genericSig);
      depth = genericSig->getGenericParams().back()->getDepth() + 1;
    }
  }

  // Add a new generic parameter to replace the opened existential.
  auto *newGenericParam = GenericTypeParamType::get(depth, 0, ctx);

  builder.addGenericParameter(newGenericParam);
  Requirement newRequirement(RequirementKind::Conformance, newGenericParam,
                             openedExistential->getOpenedExistentialType());
  auto source =
      GenericSignatureBuilder::FloatingRequirementSource::forAbstract();
  builder.addRequirement(newRequirement, source, nullptr);

  auto genericSig = std::move(builder).computeGenericSignature(
      SourceLoc(), /*allowConcreteGenericParams=*/true);
  genericEnv = genericSig->getGenericEnvironment();

  newArchetype =
      genericEnv->mapTypeIntoContext(newGenericParam)->castTo<ArchetypeType>();

  // Calculate substitutions to map the caller's archetypes to the thunk's
  // archetypes.
  if (auto calleeGenericSig =
          fn->getLoweredFunctionType()->getSubstGenericSignature()) {
    contextSubs = SubstitutionMap::get(
        calleeGenericSig,
        [&](SubstitutableType *type) -> Type {
          return genericEnv->mapTypeIntoContext(type);
        },
        MakeAbstractConformanceForGenericType());
  }

  // Calculate substitutions to map interface types to the caller's archetypes.
  interfaceSubs = SubstitutionMap::get(
      genericSig,
      [&](SubstitutableType *type) -> Type {
        if (type->isEqual(newGenericParam))
          return openedExistential;
        return fn->mapTypeIntoContext(type);
      },
      MakeAbstractConformanceForGenericType());

  return genericSig->getCanonicalSignature();
}

CanSILFunctionType buildThunkType(SILFunction *fn,
                                  CanSILFunctionType &sourceType,
                                  CanSILFunctionType &expectedType,
                                  GenericEnvironment *&genericEnv,
                                  SubstitutionMap &interfaceSubs,
                                  bool withoutActuallyEscaping,
                                  DifferentiationThunkKind thunkKind) {
  assert(!expectedType->isPolymorphic() &&
         !expectedType->getCombinedSubstitutions());
  assert(!sourceType->isPolymorphic() &&
         !sourceType->getCombinedSubstitutions());

  // Cannot build a reabstraction thunk without context. Ownership semantics
  // on the result type are required.
  if (thunkKind == DifferentiationThunkKind::Reabstraction)
    assert(expectedType->getExtInfo().hasContext());

  // This may inherit @noescape from the expected type. The `@noescape`
  // attribute is only stripped when using this type to materialize a new decl.
  // Use `@convention(thin)` if:
  // - Building a reabstraction thunk type.
  // - Building an index subset thunk type, where the expected type has context
  //   (i.e. is `@convention(thick)`).
  auto extInfoBuilder = expectedType->getExtInfo().intoBuilder();
  if (thunkKind == DifferentiationThunkKind::Reabstraction ||
      extInfoBuilder.hasContext()) {
    extInfoBuilder = extInfoBuilder.withRepresentation(
        SILFunctionType::Representation::Thin);
  }
  if (withoutActuallyEscaping)
    extInfoBuilder = extInfoBuilder.withNoEscape(false);

  // Does the thunk type involve archetypes other than opened existentials?
  bool hasArchetypes = false;
  // Does the thunk type involve an open existential type?
  CanOpenedArchetypeType openedExistential;
  auto archetypeVisitor = [&](CanType t) {
    if (auto archetypeTy = dyn_cast<OpenedArchetypeType>(t)) {
      if (auto opened = dyn_cast<OpenedArchetypeType>(archetypeTy)) {
        assert((openedExistential == CanArchetypeType() ||
                openedExistential == opened) &&
               "one too many open existentials");
        openedExistential = opened;
      } else {
        hasArchetypes = true;
      }
    }
  };

  // Use the generic signature from the context if the thunk involves
  // generic parameters.
  CanGenericSignature genericSig;
  SubstitutionMap contextSubs;
  ArchetypeType *newArchetype = nullptr;

  if (expectedType->hasArchetype() || sourceType->hasArchetype()) {
    expectedType.visit(archetypeVisitor);
    sourceType.visit(archetypeVisitor);
    genericSig =
        buildThunkSignature(fn, hasArchetypes, openedExistential, genericEnv,
                            contextSubs, interfaceSubs, newArchetype);
  }

  auto substTypeHelper = [&](SubstitutableType *type) -> Type {
    if (CanType(type) == openedExistential)
      return newArchetype;
    return Type(type).subst(contextSubs);
  };
  auto substConformanceHelper = LookUpConformanceInSubstitutionMap(contextSubs);

  // Utility function to apply contextSubs, and also replace the
  // opened existential with the new archetype.
  auto substLoweredTypeIntoThunkContext =
      [&](CanSILFunctionType t) -> CanSILFunctionType {
    return SILType::getPrimitiveObjectType(t)
        .subst(fn->getModule(), substTypeHelper, substConformanceHelper)
        .castTo<SILFunctionType>();
  };

  sourceType = substLoweredTypeIntoThunkContext(sourceType);
  expectedType = substLoweredTypeIntoThunkContext(expectedType);

  // If our parent function was pseudogeneric, this thunk must also be
  // pseudogeneric, since we have no way to pass generic parameters.
  if (genericSig)
    if (fn->getLoweredFunctionType()->isPseudogeneric())
      extInfoBuilder = extInfoBuilder.withIsPseudogeneric();

  ParameterConvention contextConvention;
  if (SILType::getPrimitiveObjectType(sourceType).isTrivial(*fn)) {
    contextConvention = ParameterConvention::Direct_Unowned;
  } else {
    contextConvention = sourceType->isCalleeConsumed()
        ? ParameterConvention::Direct_Owned
        : ParameterConvention::Direct_Guaranteed;
  }

  // Add the function type as the parameter.
  SmallVector<SILParameterInfo, 4> params;
  params.append(expectedType->getParameters().begin(),
                expectedType->getParameters().end());
  // Add reabstraction function parameter only if building a reabstraction thunk
  // type.
  if (thunkKind == DifferentiationThunkKind::Reabstraction)
    params.push_back({sourceType, sourceType->getExtInfo().hasContext()
                                      ? contextConvention
                                      : ParameterConvention::Direct_Unowned});

  auto mapTypeOutOfContext = [&](CanType type) -> CanType {
    return type->mapTypeOutOfContext()->getCanonicalType(genericSig);
  };

  // Map the parameter and expected types out of context to get the interface
  // type of the thunk.
  SmallVector<SILParameterInfo, 4> interfaceParams;
  interfaceParams.reserve(params.size());
  for (auto &param : params) {
    auto interfaceParam = param.map(mapTypeOutOfContext);
    interfaceParams.push_back(interfaceParam);
  }

  SmallVector<SILYieldInfo, 4> interfaceYields;
  for (auto &yield : expectedType->getYields()) {
    auto interfaceYield = yield.map(mapTypeOutOfContext);
    interfaceYields.push_back(interfaceYield);
  }

  SmallVector<SILResultInfo, 4> interfaceResults;
  for (auto &result : expectedType->getResults()) {
    auto interfaceResult = result.map(mapTypeOutOfContext);
    interfaceResults.push_back(interfaceResult);
  }

  Optional<SILResultInfo> interfaceErrorResult;
  if (expectedType->hasErrorResult()) {
    auto errorResult = expectedType->getErrorResult();
    interfaceErrorResult = errorResult.map(mapTypeOutOfContext);
  }

  // The type of the thunk function.
  return SILFunctionType::get(
      genericSig, extInfoBuilder.build(), expectedType->getCoroutineKind(),
      ParameterConvention::Direct_Unowned, interfaceParams, interfaceYields,
      interfaceResults, interfaceErrorResult,
      expectedType->getPatternSubstitutions(), SubstitutionMap(),
      fn->getASTContext());
}

/// Forward function arguments, handling ownership convention mismatches.
/// Adapted from `forwardFunctionArguments` in SILGenPoly.cpp.
///
/// Forwarded arguments are appended to `forwardedArgs`.
///
/// Local allocations are appended to `localAllocations`. They need to be
/// deallocated via `dealloc_stack`.
///
/// Local values requiring cleanup are appended to `valuesToCleanup`.
static void forwardFunctionArgumentsConvertingOwnership(
    SILBuilder &builder, SILLocation loc, CanSILFunctionType fromTy,
    CanSILFunctionType toTy, ArrayRef<SILArgument *> originalArgs,
    SmallVectorImpl<SILValue> &forwardedArgs,
    SmallVectorImpl<AllocStackInst *> &localAllocations,
    SmallVectorImpl<SILValue> &valuesToCleanup) {
  auto fromParameters = fromTy->getParameters();
  auto toParameters = toTy->getParameters();
  assert(fromParameters.size() == toParameters.size());
  assert(fromParameters.size() == originalArgs.size());
  for (auto index : indices(originalArgs)) {
    auto &arg = originalArgs[index];
    auto fromParam = fromParameters[index];
    auto toParam = toParameters[index];
    // To convert guaranteed argument to be owned, create a copy.
    if (fromParam.isConsumed() && !toParam.isConsumed()) {
      // If the argument has an object type, create a `copy_value`.
      if (arg->getType().isObject()) {
        auto argCopy = builder.emitCopyValueOperation(loc, arg);
        forwardedArgs.push_back(argCopy);
        continue;
      }
      // If the argument has an address type, create a local allocation and
      // `copy_addr` its contents to the local allocation.
      auto *alloc = builder.createAllocStack(loc, arg->getType());
      builder.createCopyAddr(loc, arg, alloc, IsNotTake, IsInitialization);
      localAllocations.push_back(alloc);
      forwardedArgs.push_back(alloc);
      continue;
    }
    // To convert owned argument to be guaranteed, borrow the argument.
    if (fromParam.isGuaranteed() && !toParam.isGuaranteed()) {
      auto bbi = builder.emitBeginBorrowOperation(loc, arg);
      forwardedArgs.push_back(bbi);
      valuesToCleanup.push_back(bbi);
      valuesToCleanup.push_back(arg);
      continue;
    }
    // Otherwise, simply forward the argument.
    forwardedArgs.push_back(arg);
  }
}

SILFunction *getOrCreateReabstractionThunk(SILOptFunctionBuilder &fb,
                                           SILModule &module, SILLocation loc,
                                           SILFunction *caller,
                                           CanSILFunctionType fromType,
                                           CanSILFunctionType toType) {
  assert(!fromType->getCombinedSubstitutions());
  assert(!toType->getCombinedSubstitutions());

  SubstitutionMap interfaceSubs;
  GenericEnvironment *genericEnv = nullptr;
  auto thunkType =
      buildThunkType(caller, fromType, toType, genericEnv, interfaceSubs,
                     /*withoutActuallyEscaping*/ false,
                     DifferentiationThunkKind::Reabstraction);
  auto thunkDeclType =
      thunkType->getWithExtInfo(thunkType->getExtInfo().withNoEscape(false));

  auto fromInterfaceType = fromType->mapTypeOutOfContext()->getCanonicalType();
  auto toInterfaceType = toType->mapTypeOutOfContext()->getCanonicalType();

  Mangle::ASTMangler mangler;
  std::string name = mangler.mangleReabstractionThunkHelper(
      thunkType, fromInterfaceType, toInterfaceType, Type(),
      module.getSwiftModule());

  auto *thunk = fb.getOrCreateSharedFunction(
      loc, name, thunkDeclType, IsBare, IsTransparent, IsSerialized,
      ProfileCounter(), IsReabstractionThunk, IsNotDynamic);
  if (!thunk->empty())
    return thunk;

  thunk->setGenericEnvironment(genericEnv);
  auto *entry = thunk->createBasicBlock();
  SILBuilder builder(entry);
  createEntryArguments(thunk);

  SILFunctionConventions fromConv(fromType, module);
  SILFunctionConventions toConv(toType, module);
  assert(toConv.useLoweredAddresses());

  // Forward thunk arguments, handling ownership convention mismatches.
  SmallVector<SILValue, 4> forwardedArgs;
  for (auto indRes : thunk->getIndirectResults())
    forwardedArgs.push_back(indRes);
  SmallVector<AllocStackInst *, 4> localAllocations;
  SmallVector<SILValue, 4> valuesToCleanup;
  forwardFunctionArgumentsConvertingOwnership(
      builder, loc, fromType, toType,
      thunk->getArgumentsWithoutIndirectResults().drop_back(), forwardedArgs,
      localAllocations, valuesToCleanup);

  SmallVector<SILValue, 4> arguments;
  auto toArgIter = forwardedArgs.begin();
  auto useNextArgument = [&]() { arguments.push_back(*toArgIter++); };

  auto createAllocStack = [&](SILType type) {
    auto *alloc = builder.createAllocStack(loc, type);
    localAllocations.push_back(alloc);
    return alloc;
  };

  // Handle indirect results.
  assert(fromType->getNumResults() == toType->getNumResults());
  for (unsigned resIdx : range(toType->getNumResults())) {
    auto fromRes = fromConv.getResults()[resIdx];
    auto toRes = toConv.getResults()[resIdx];
    // No abstraction mismatch.
    if (fromRes.isFormalIndirect() == toRes.isFormalIndirect()) {
      // If result types are indirect, directly pass as next argument.
      if (toRes.isFormalIndirect())
        useNextArgument();
      continue;
    }
    // Convert indirect result to direct result.
    if (fromRes.isFormalIndirect()) {
      SILType resultTy =
          fromConv.getSILType(fromRes, builder.getTypeExpansionContext());
      assert(resultTy.isAddress());
      auto *indRes = createAllocStack(resultTy);
      arguments.push_back(indRes);
      continue;
    }
    // Convert direct result to indirect result.
    // Increment thunk argument iterator; reabstraction handled later.
    ++toArgIter;
  }

  // Reabstract parameters.
  assert(toType->getNumParameters() == fromType->getNumParameters());
  for (unsigned paramIdx : range(toType->getNumParameters())) {
    auto fromParam = fromConv.getParameters()[paramIdx];
    auto toParam = toConv.getParameters()[paramIdx];
    // No abstraction mismatch. Directly use next argument.
    if (fromParam.isFormalIndirect() == toParam.isFormalIndirect()) {
      useNextArgument();
      continue;
    }
    // Convert indirect parameter to direct parameter.
    if (fromParam.isFormalIndirect()) {
      auto paramTy = fromConv.getSILType(fromType->getParameters()[paramIdx],
                                         builder.getTypeExpansionContext());
      if (!paramTy.hasArchetype())
        paramTy = thunk->mapTypeIntoContext(paramTy);
      assert(paramTy.isAddress());
      auto toArg = *toArgIter++;
      auto *buf = createAllocStack(toArg->getType());
      toArg = builder.emitCopyValueOperation(loc, toArg);
      builder.emitStoreValueOperation(loc, toArg, buf,
                                      StoreOwnershipQualifier::Init);
      valuesToCleanup.push_back(buf);
      arguments.push_back(buf);
      continue;
    }
    // Convert direct parameter to indirect parameter.
    assert(toParam.isFormalIndirect());
    auto toArg = *toArgIter++;
    auto load = builder.emitLoadBorrowOperation(loc, toArg);
    if (isa<LoadBorrowInst>(load))
      valuesToCleanup.push_back(load);
    arguments.push_back(load);
  }

  auto *fnArg = thunk->getArgumentsWithoutIndirectResults().back();
  auto *apply = builder.createApply(loc, fnArg, SubstitutionMap(), arguments,
                                    /*isNonThrowing*/ false);

  // Get return elements.
  SmallVector<SILValue, 4> results;
  // Extract all direct results.
  SmallVector<SILValue, 4> directResults;
  extractAllElements(apply, builder, directResults);

  auto fromDirResultsIter = directResults.begin();
  auto fromIndResultsIter = apply->getIndirectSILResults().begin();
  auto toIndResultsIter = thunk->getIndirectResults().begin();
  // Reabstract results.
  for (unsigned resIdx : range(toType->getNumResults())) {
    auto fromRes = fromConv.getResults()[resIdx];
    auto toRes = toConv.getResults()[resIdx];
    // Check function-typed results.
    if (isa<SILFunctionType>(fromRes.getInterfaceType()) &&
        isa<SILFunctionType>(toRes.getInterfaceType())) {
      auto fromFnType = cast<SILFunctionType>(fromRes.getInterfaceType());
      auto toFnType = cast<SILFunctionType>(toRes.getInterfaceType());
      auto fromUnsubstFnType = fromFnType->getUnsubstitutedType(module);
      auto toUnsubstFnType = toFnType->getUnsubstitutedType(module);
      // If unsubstituted function types are not equal, perform reabstraction.
      if (fromUnsubstFnType != toUnsubstFnType) {
        auto fromFn = *fromDirResultsIter++;
        auto newFromFn = reabstractFunction(
            builder, fb, loc, fromFn, toFnType,
            [](SubstitutionMap substMap) { return substMap; });
        results.push_back(newFromFn);
        continue;
      }
    }
    // No abstraction mismatch.
    if (fromRes.isFormalIndirect() == toRes.isFormalIndirect()) {
      // If result types are direct, add call result as direct thunk result.
      if (toRes.isFormalDirect())
        results.push_back(*fromDirResultsIter++);
      // If result types are indirect, increment indirect result iterators.
      else {
        ++fromIndResultsIter;
        ++toIndResultsIter;
      }
      continue;
    }
    // Load direct results from indirect results.
    if (fromRes.isFormalIndirect()) {
      auto indRes = *fromIndResultsIter++;
      auto load = builder.emitLoadValueOperation(loc, indRes,
                                                 LoadOwnershipQualifier::Take);
      results.push_back(load);
      continue;
    }
    // Store direct results to indirect results.
    assert(toRes.isFormalIndirect());
#ifndef NDEBUG
    SILType resultTy =
        toConv.getSILType(toRes, builder.getTypeExpansionContext());
    assert(resultTy.isAddress());
#endif
    auto indRes = *toIndResultsIter++;
    auto dirRes = *fromDirResultsIter++;
    builder.emitStoreValueOperation(loc, dirRes, indRes,
                                    StoreOwnershipQualifier::Init);
  }
  auto retVal = joinElements(results, builder, loc);

  // Clean up local values.
  // Guaranteed values need an `end_borrow`.
  // Owned values need to be destroyed.
  for (auto arg : valuesToCleanup) {
    switch (arg.getOwnershipKind()) {
    case OwnershipKind::Any:
      llvm_unreachable("value with any ownership kind?!");
    case OwnershipKind::Guaranteed:
      builder.emitEndBorrowOperation(loc, arg);
      break;
    case OwnershipKind::Owned:
    case OwnershipKind::Unowned:
    case OwnershipKind::None:
      builder.emitDestroyOperation(loc, arg);
      break;
    }
  }

  // Deallocate local allocations.
  for (auto *alloc : llvm::reverse(localAllocations))
    builder.createDeallocStack(loc, alloc);

  // Create return.
  builder.createReturn(loc, retVal);

  LLVM_DEBUG(auto &s = getADDebugStream() << "Created reabstraction thunk.\n";
             s << "  From type: " << fromType << '\n';
             s << "  To type: " << toType << '\n'; s << '\n'
                                                     << *thunk);

  return thunk;
}

SILValue reabstractFunction(
    SILBuilder &builder, SILOptFunctionBuilder &fb, SILLocation loc,
    SILValue fn, CanSILFunctionType toType,
    std::function<SubstitutionMap(SubstitutionMap)> remapSubstitutions) {
  auto &module = *fn->getModule();
  auto fromType = fn->getType().getAs<SILFunctionType>();
  auto unsubstFromType = fromType->getUnsubstitutedType(module);
  auto unsubstToType = toType->getUnsubstitutedType(module);

  auto *thunk = getOrCreateReabstractionThunk(fb, module, loc,
                                              /*caller*/ fn->getFunction(),
                                              unsubstFromType, unsubstToType);
  auto *thunkRef = builder.createFunctionRef(loc, thunk);

  if (fromType != unsubstFromType)
    fn = builder.createConvertFunction(
        loc, fn, SILType::getPrimitiveObjectType(unsubstFromType),
        /*withoutActuallyEscaping*/ false);

  fn = builder.createPartialApply(
      loc, thunkRef, remapSubstitutions(thunk->getForwardingSubstitutionMap()),
      {fn}, toType->getCalleeConvention());

  if (toType != unsubstToType)
    fn = builder.createConvertFunction(loc, fn,
                                       SILType::getPrimitiveObjectType(toType),
                                       /*withoutActuallyEscaping*/ false);

  return fn;
}

std::pair<SILFunction *, SubstitutionMap>
getOrCreateSubsetParametersThunkForLinearMap(
    SILOptFunctionBuilder &fb, SILFunction *parentThunk,
    CanSILFunctionType origFnType, CanSILFunctionType linearMapType,
    CanSILFunctionType targetType, AutoDiffDerivativeFunctionKind kind,
    AutoDiffConfig desiredConfig, AutoDiffConfig actualConfig) {
  LLVM_DEBUG(getADDebugStream()
             << "Getting a subset parameters thunk for " << linearMapType
             << " from " << actualConfig << " to " << desiredConfig << '\n');

  assert(!linearMapType->getCombinedSubstitutions());
  assert(!targetType->getCombinedSubstitutions());
  SubstitutionMap interfaceSubs;
  GenericEnvironment *genericEnv = nullptr;
  auto thunkType = buildThunkType(parentThunk, linearMapType, targetType,
                                  genericEnv, interfaceSubs,
                                  /*withoutActuallyEscaping*/ true,
                                  DifferentiationThunkKind::Reabstraction);

  // TODO(SR-13508): Use more principled mangling for thunks.
  std::string thunkName;
  switch (kind) {
  case AutoDiffDerivativeFunctionKind::JVP:
    thunkName = "differential";
    break;
  case AutoDiffDerivativeFunctionKind::VJP:
    thunkName = "pullback";
  }
  Mangle::ASTMangler mangler;
  auto fromInterfaceType =
      linearMapType->mapTypeOutOfContext()->getCanonicalType();
  auto toInterfaceType = targetType->mapTypeOutOfContext()->getCanonicalType();
  CanType dynamicSelfType;
  thunkName = "AD__" +
              mangler.mangleReabstractionThunkHelper(
                  thunkType, fromInterfaceType, toInterfaceType,
                  dynamicSelfType, parentThunk->getModule().getSwiftModule()) +
              "_" + desiredConfig.mangle() + "_" + thunkName;
  thunkName += "_index_subset_thunk";

  auto loc = parentThunk->getLocation();
  auto *thunk = fb.getOrCreateSharedFunction(
      loc, thunkName, thunkType, IsBare, IsTransparent, IsSerialized,
      ProfileCounter(), IsThunk, IsNotDynamic);

  if (!thunk->empty())
    return {thunk, interfaceSubs};

  // TODO(SR-13929): Enable ownership in all differentiation thunks.
  thunk->setOwnershipEliminated();
  thunk->setGenericEnvironment(genericEnv);
  auto *entry = thunk->createBasicBlock();
  SILBuilder builder(entry);
  createEntryArguments(thunk);

  // Get arguments.
  SmallVector<SILValue, 4> arguments;
  SmallVector<AllocStackInst *, 4> localAllocations;

  // Build a `.zero` argument for the given `Differentiable`-conforming type.
  auto buildZeroArgument = [&](SILType zeroSILType) {
    auto zeroSILObjType = zeroSILType.getObjectType();
    auto zeroType = zeroSILType.getASTType();
    auto *swiftMod = parentThunk->getModule().getSwiftModule();
    auto tangentSpace =
        zeroType->getAutoDiffTangentSpace(LookUpConformanceInModule(swiftMod));
    assert(tangentSpace && "No tangent space for this type");
    switch (tangentSpace->getKind()) {
    case TangentSpace::Kind::TangentVector: {
      auto *buf = builder.createAllocStack(loc, zeroSILObjType);
      localAllocations.push_back(buf);
      emitZeroIntoBuffer(builder, zeroType, buf, loc);
      if (zeroSILType.isAddress()) {
        arguments.push_back(buf);
      } else {
        auto arg = builder.emitLoadValueOperation(loc, buf,
                                                  LoadOwnershipQualifier::Take);
        arguments.push_back(arg);
      }
      break;
    }
    case TangentSpace::Kind::Tuple: {
      llvm_unreachable("Unimplemented: Handle zero initialization for tuples");
    }
    }
  };

  // The indices in `actualConfig` and `desiredConfig` are with respect to the
  // original function. However, the differential parameters and pullback
  // results may already be w.r.t. a subset. We create a map between the
  // original function's actual parameter indices and the linear map's actual
  // indices.
  // Example:
  //   Original: (T0, T1, T2) -> R
  //   Actual indices: 0, 2
  //   Original differential: (T0, T2) -> R
  //   Original pullback: R -> (T0, T2)
  //   Desired indices w.r.t. original: 2
  //   Desired indices w.r.t. linear map: 1
  SmallVector<unsigned, 4> actualParamIndicesMap(
      actualConfig.parameterIndices->getCapacity(), UINT_MAX);
  {
    unsigned indexInBitVec = 0;
    for (auto index : actualConfig.parameterIndices->getIndices()) {
      actualParamIndicesMap[index] = indexInBitVec;
      ++indexInBitVec;
    }
  }
  auto mapOriginalParameterIndex = [&](unsigned index) -> unsigned {
    auto mappedIndex = actualParamIndicesMap[index];
    assert(mappedIndex < actualConfig.parameterIndices->getCapacity());
    return mappedIndex;
  };

  switch (kind) {
  // Differential arguments are:
  // - All indirect results, followed by:
  // - An interleaving of:
  //   - Thunk arguments (when parameter index is in both desired and actual
  //     indices).
  //   - Zeros (when parameter is not in desired indices).
  case AutoDiffDerivativeFunctionKind::JVP: {
    // Forward all indirect results.
    arguments.append(thunk->getIndirectResults().begin(),
                     thunk->getIndirectResults().end());
    auto toArgIter = thunk->getArgumentsWithoutIndirectResults().begin();
    auto useNextArgument = [&]() { arguments.push_back(*toArgIter++); };
    // Iterate over actual indices.
    for (unsigned i : actualConfig.parameterIndices->getIndices()) {
      // If index is desired, use next argument.
      if (desiredConfig.isWrtParameter(i)) {
        useNextArgument();
      }
      // Otherwise, construct and use a zero argument.
      else {
        auto zeroSILType =
            linearMapType->getParameters()[mapOriginalParameterIndex(i)]
                .getSILStorageInterfaceType();
        buildZeroArgument(zeroSILType);
      }
    }
    break;
  }
  // Pullback arguments are:
  // - An interleaving of:
  //   - Thunk indirect results (when parameter index is in both desired and
  //     actual indices).
  //   - Zeros (when parameter is not in desired indices).
  // - All actual arguments.
  case AutoDiffDerivativeFunctionKind::VJP: {
    auto toIndirectResultsIter = thunk->getIndirectResults().begin();
    auto useNextIndirectResult = [&]() {
      arguments.push_back(*toIndirectResultsIter++);
    };
    // Collect pullback arguments.
    unsigned pullbackResultIndex = 0;
    for (unsigned i : actualConfig.parameterIndices->getIndices()) {
      auto origParamInfo = origFnType->getParameters()[i];
      // Skip original `inout` parameters. All non-indirect-result pullback
      // arguments (including `inout` arguments) are appended to `arguments`
      // later.
      if (origParamInfo.isIndirectMutating())
        continue;
      auto resultInfo = linearMapType->getResults()[pullbackResultIndex];
      assert(pullbackResultIndex < linearMapType->getNumResults());
      ++pullbackResultIndex;
      // Skip pullback direct results. Only indirect results are relevant as
      // arguments.
      if (resultInfo.isFormalDirect())
        continue;
      // If index is desired, use next pullback indirect result.
      if (desiredConfig.isWrtParameter(i)) {
        useNextIndirectResult();
        continue;
      }
      // Otherwise, allocate and use an uninitialized pullback indirect result.
      auto *indirectResult = builder.createAllocStack(
          loc, resultInfo.getSILStorageInterfaceType());
      localAllocations.push_back(indirectResult);
      arguments.push_back(indirectResult);
    }
    // Forward all actual non-indirect-result arguments.
    arguments.append(thunk->getArgumentsWithoutIndirectResults().begin(),
                     thunk->getArgumentsWithoutIndirectResults().end() - 1);
    break;
  }
  }

  // Get the linear map value (the thunk's last argument) and apply it.
  auto *linearMap = thunk->getArguments().back();
  auto *ai = builder.createApply(loc, linearMap, SubstitutionMap(), arguments,
                                 /*isNonThrowing*/ false);

  // If differential thunk, deallocate local allocations and directly return
  // `apply` result.
  if (kind == AutoDiffDerivativeFunctionKind::JVP) {
    for (auto *alloc : llvm::reverse(localAllocations))
      builder.createDeallocStack(loc, alloc);
    builder.createReturn(loc, ai);
    return {thunk, interfaceSubs};
  }

  // If pullback thunk, return only the desired results and clean up the
  // undesired results.
  SmallVector<SILValue, 8> pullbackDirectResults;
  extractAllElements(ai, builder, pullbackDirectResults);
  SmallVector<SILValue, 8> allResults;
  collectAllActualResultsInTypeOrder(ai, pullbackDirectResults, allResults);
  // Collect pullback `inout` arguments in type order.
  unsigned inoutArgIdx = 0;
  SILFunctionConventions origConv(origFnType, thunk->getModule());
  for (auto paramIdx : actualConfig.parameterIndices->getIndices()) {
    auto paramInfo = origConv.getParameters()[paramIdx];
    if (!paramInfo.isIndirectMutating())
      continue;
    auto inoutArg = *std::next(ai->getInoutArguments().begin(), inoutArgIdx++);
    unsigned mappedParamIdx = mapOriginalParameterIndex(paramIdx);
    allResults.insert(allResults.begin() + mappedParamIdx, inoutArg);
  }
  assert(allResults.size() == actualConfig.parameterIndices->getNumIndices() &&
         "Number of pullback results should match number of differentiability "
         "parameters");

  SmallVector<SILValue, 8> results;
  for (unsigned i : actualConfig.parameterIndices->getIndices()) {
    unsigned mappedIndex = mapOriginalParameterIndex(i);
    // If result is desired:
    // - Do nothing if result is indirect.
    //   (It was already forwarded to the `apply` instruction).
    // - Push it to `results` if result is direct.
    auto result = allResults[mappedIndex];
    if (desiredConfig.isWrtParameter(i)) {
      if (result->getType().isObject())
        results.push_back(result);
    }
    // Otherwise, cleanup the unused results.
    else {
      if (result->getType().isAddress())
        builder.emitDestroyAddrAndFold(loc, result);
      else
        builder.emitDestroyValueOperation(loc, result);
    }
  }
  // Deallocate local allocations and return final direct result.
  for (auto *alloc : llvm::reverse(localAllocations))
    builder.createDeallocStack(loc, alloc);
  auto result = joinElements(results, builder, loc);
  builder.createReturn(loc, result);

  return {thunk, interfaceSubs};
}

std::pair<SILFunction *, SubstitutionMap>
getOrCreateSubsetParametersThunkForDerivativeFunction(
    SILOptFunctionBuilder &fb, SILValue origFnOperand, SILValue derivativeFn,
    AutoDiffDerivativeFunctionKind kind, AutoDiffConfig desiredConfig,
    AutoDiffConfig actualConfig) {
  LLVM_DEBUG(getADDebugStream()
             << "Getting a subset parameters thunk for derivative function "
             << derivativeFn << " of the original function " << origFnOperand
             << " from " << actualConfig << " to " << desiredConfig << '\n');

  auto origFnType = origFnOperand->getType().castTo<SILFunctionType>();
  auto &module = fb.getModule();
  auto lookupConformance = LookUpConformanceInModule(module.getSwiftModule());

  // Compute target type for thunking.
  auto derivativeFnType = derivativeFn->getType().castTo<SILFunctionType>();
  auto targetType = origFnType->getAutoDiffDerivativeFunctionType(
      desiredConfig.parameterIndices, desiredConfig.resultIndices, kind,
      module.Types, lookupConformance);
  auto *caller = derivativeFn->getFunction();
  if (targetType->hasArchetype()) {
    auto substTargetType =
        caller->mapTypeIntoContext(targetType->mapTypeOutOfContext())
            ->getCanonicalType();
    targetType = SILType::getPrimitiveObjectType(substTargetType)
                     .castTo<SILFunctionType>();
  }
  assert(derivativeFnType->getNumParameters() ==
         targetType->getNumParameters());
  assert(derivativeFnType->getNumResults() == targetType->getNumResults());

  // Build thunk type.
  SubstitutionMap interfaceSubs;
  GenericEnvironment *genericEnv = nullptr;
  auto thunkType = buildThunkType(derivativeFn->getFunction(), derivativeFnType,
                                  targetType, genericEnv, interfaceSubs,
                                  /*withoutActuallyEscaping*/ false,
                                  DifferentiationThunkKind::IndexSubset);

  // FIXME: The logic for resolving `assocRef` does not reapply function
  // conversions, which is problematic if `derivativeFn` is a `partial_apply`
  // instruction.
  StringRef origName;
  if (auto *origFnRef =
          peerThroughFunctionConversions<FunctionRefInst>(origFnOperand)) {
    origName = origFnRef->getInitiallyReferencedFunction()->getName();
  } else if (auto *origMethodInst =
                 peerThroughFunctionConversions<MethodInst>(origFnOperand)) {
    origName = origMethodInst->getMember()
                   .getAnyFunctionRef()
                   ->getAbstractFunctionDecl()
                   ->getNameStr();
  }
  assert(!origName.empty() && "Original function name could not be resolved");
  // TODO(SR-13508): Use more principled mangling for thunks.
  std::string thunkName;
  switch (kind) {
  case AutoDiffDerivativeFunctionKind::JVP:
    thunkName = "jvp";
    break;
  case AutoDiffDerivativeFunctionKind::VJP:
    thunkName = "vjp";
  }
  Mangle::ASTMangler mangler;
  auto fromInterfaceType =
      derivativeFnType->mapTypeOutOfContext()->getCanonicalType();
  auto toInterfaceType = targetType->mapTypeOutOfContext()->getCanonicalType();
  CanType dynamicSelfType;
  thunkName = "AD__orig_" + origName.str() + "_" +
              mangler.mangleReabstractionThunkHelper(
                  thunkType, fromInterfaceType, toInterfaceType,
                  dynamicSelfType, module.getSwiftModule()) +
              "_" + desiredConfig.mangle() + "_" + thunkName;
  thunkName += "_subset_parameters_thunk";

  auto loc = origFnOperand.getLoc();
  auto *thunk = fb.getOrCreateSharedFunction(
      loc, thunkName, thunkType, IsBare, IsTransparent, caller->isSerialized(),
      ProfileCounter(), IsThunk, IsNotDynamic);
  thunk->setInlineStrategy(AlwaysInline);

  if (!thunk->empty())
    return {thunk, interfaceSubs};

  thunk->setGenericEnvironment(genericEnv);
  auto *entry = thunk->createBasicBlock();
  SILBuilder builder(entry);
  createEntryArguments(thunk);

  SubstitutionMap assocSubstMap;
  if (auto *partialApply = dyn_cast<PartialApplyInst>(derivativeFn))
    assocSubstMap = partialApply->getSubstitutionMap();

  // FIXME: The logic for resolving `assocRef` does not reapply function
  // conversions, which is problematic if `derivativeFn` is a `partial_apply`
  // instruction.
  SILValue assocRef;
  if (auto *derivativeFnRef =
          peerThroughFunctionConversions<FunctionRefInst>(derivativeFn)) {
    auto *assoc = derivativeFnRef->getReferencedFunctionOrNull();
    assocRef = builder.createFunctionRef(loc, assoc);
  } else if (auto *assocMethodInst =
                 peerThroughFunctionConversions<WitnessMethodInst>(
                     derivativeFn)) {
    assocRef = builder.createWitnessMethod(
        loc, assocMethodInst->getLookupType(),
        assocMethodInst->getConformance(), assocMethodInst->getMember(),
        thunk->mapTypeIntoContext(assocMethodInst->getType()));
  } else if (auto *assocMethodInst =
                 peerThroughFunctionConversions<ClassMethodInst>(
                     derivativeFn)) {
    auto classOperand = thunk->getArgumentsWithoutIndirectResults().back();
#ifndef NDEBUG
    auto classOperandType = assocMethodInst->getOperand()->getType();
    assert(classOperand->getType() == classOperandType);
#endif
    assocRef = builder.createClassMethod(
        loc, classOperand, assocMethodInst->getMember(),
        thunk->mapTypeIntoContext(assocMethodInst->getType()));
  } else if (auto *diffWitFn = peerThroughFunctionConversions<
                 DifferentiabilityWitnessFunctionInst>(derivativeFn)) {
    assocRef = builder.createDifferentiabilityWitnessFunction(
        loc, diffWitFn->getWitnessKind(), diffWitFn->getWitness());
  }
  assert(assocRef && "Expected derivative function to be resolved");

  assocSubstMap = assocSubstMap.subst(thunk->getForwardingSubstitutionMap());
  derivativeFnType = assocRef->getType().castTo<SILFunctionType>();

  SmallVector<SILValue, 4> arguments;
  arguments.append(thunk->getArguments().begin(), thunk->getArguments().end());
  assert(arguments.size() ==
         derivativeFnType->getNumParameters() +
             derivativeFnType->getNumIndirectFormalResults());
  auto *apply = builder.createApply(loc, assocRef, assocSubstMap, arguments,
                                    /*isNonThrowing*/ false);

  // Extract all direct results.
  SmallVector<SILValue, 8> directResults;
  extractAllElements(apply, builder, directResults);
  auto originalDirectResults = ArrayRef<SILValue>(directResults).drop_back(1);
  auto originalDirectResult =
      joinElements(originalDirectResults, builder, apply->getLoc());
  auto linearMap = directResults.back();

  auto linearMapType = linearMap->getType().castTo<SILFunctionType>();
  auto linearMapTargetType = targetType->getResults()
                                 .back()
                                 .getSILStorageInterfaceType()
                                 .castTo<SILFunctionType>();
  auto unsubstLinearMapType = linearMapType->getUnsubstitutedType(module);
  auto unsubstLinearMapTargetType =
      linearMapTargetType->getUnsubstitutedType(module);

  SILFunction *linearMapThunk;
  SubstitutionMap linearMapSubs;
  std::tie(linearMapThunk, linearMapSubs) =
      getOrCreateSubsetParametersThunkForLinearMap(
          fb, thunk, origFnType, unsubstLinearMapType,
          unsubstLinearMapTargetType, kind, desiredConfig, actualConfig);

  auto *linearMapThunkFRI = builder.createFunctionRef(loc, linearMapThunk);
  SILValue thunkedLinearMap = linearMap;
  if (linearMapType != unsubstLinearMapType) {
    thunkedLinearMap = builder.createConvertFunction(
        loc, thunkedLinearMap,
        SILType::getPrimitiveObjectType(unsubstLinearMapType),
        /*withoutActuallyEscaping*/ false);
  }
  thunkedLinearMap = builder.createPartialApply(
      loc, linearMapThunkFRI, linearMapSubs, {thunkedLinearMap},
      linearMapTargetType->isCalleeConsumed()
          ? ParameterConvention::Direct_Owned
          : ParameterConvention::Direct_Guaranteed);
  if (linearMapTargetType != unsubstLinearMapTargetType) {
    thunkedLinearMap = builder.createConvertFunction(
        loc, thunkedLinearMap,
        SILType::getPrimitiveObjectType(linearMapTargetType),
        /*withoutActuallyEscaping*/ false);
  }
  assert(origFnType->getNumResults() +
             origFnType->getNumIndirectMutatingParameters() ==
         1);
  if (origFnType->getNumResults() > 0 &&
      origFnType->getResults().front().isFormalDirect()) {
    auto result =
        joinElements({originalDirectResult, thunkedLinearMap}, builder, loc);
    builder.createReturn(loc, result);
  } else {
    builder.createReturn(loc, thunkedLinearMap);
  }

  return {thunk, interfaceSubs};
}

} // end namespace autodiff
} // end namespace swift
