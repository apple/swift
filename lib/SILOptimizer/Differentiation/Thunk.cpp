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
#include "swift/AST/Requirement.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/DifferentiationMangler.h"

namespace swift {
namespace autodiff {

//===----------------------------------------------------------------------===//
// Thunk helpers
//===----------------------------------------------------------------------===//
// These helpers are copied/adapted from SILGen. They should be refactored and
// moved to a shared location.
//===----------------------------------------------------------------------===//

CanSILFunctionType buildThunkType(SILFunction *fn,
                                  CanSILFunctionType &sourceType,
                                  CanSILFunctionType &expectedType,
                                  GenericEnvironment *&genericEnv,
                                  SubstitutionMap &interfaceSubs,
                                  bool withoutActuallyEscaping,
                                  DifferentiationThunkKind thunkKind) {
  CanType inputSubstType;
  CanType outputSubstType;
  CanType dynamicSelfType;
  return buildSILFunctionThunkType(
      fn, sourceType, expectedType, inputSubstType, outputSubstType, genericEnv,
      interfaceSubs, dynamicSelfType, withoutActuallyEscaping, thunkKind);
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
      thunkType, fromInterfaceType, toInterfaceType, Type(), Type(),
      module.getSwiftModule());

  auto *thunk = fb.getOrCreateSharedFunction(
      loc, name, thunkDeclType, IsBare, IsTransparent, IsSerialized,
      ProfileCounter(), IsReabstractionThunk, IsNotDynamic, IsNotDistributed,
      IsNotRuntimeAccessible);
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
  auto *apply = builder.createApply(loc, fnArg, SubstitutionMap(), arguments);

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
    switch (arg->getOwnershipKind()) {
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
      {fn}, fromType->getCalleeConvention());

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
    const AutoDiffConfig &desiredConfig, const AutoDiffConfig &actualConfig,
    ADContext &adContext) {
  LLVM_DEBUG(getADDebugStream()
             << "Getting a subset parameters thunk for "
             << (kind == AutoDiffDerivativeFunctionKind::JVP ? "jvp" : "vjp")
             << " linear map " << linearMapType
             << " from " << actualConfig << " to " << desiredConfig << '\n');

  assert(!linearMapType->getCombinedSubstitutions());
  assert(!targetType->getCombinedSubstitutions());
  SubstitutionMap interfaceSubs;
  GenericEnvironment *genericEnv = nullptr;
  auto thunkType = buildThunkType(parentThunk, linearMapType, targetType,
                                  genericEnv, interfaceSubs,
                                  /*withoutActuallyEscaping*/ true,
                                  DifferentiationThunkKind::Reabstraction);

  Mangle::DifferentiationMangler mangler;
  auto fromInterfaceType =
      linearMapType->mapTypeOutOfContext()->getCanonicalType();

  auto thunkName = mangler.mangleLinearMapSubsetParametersThunk(
      fromInterfaceType, kind.getLinearMapKind(),
      actualConfig.parameterIndices, actualConfig.resultIndices,
      desiredConfig.parameterIndices);

  auto loc = parentThunk->getLocation();
  auto *thunk = fb.getOrCreateSharedFunction(
      loc, thunkName, thunkType, IsBare, IsTransparent, IsSerialized,
      ProfileCounter(), IsThunk, IsNotDynamic, IsNotDistributed,
      IsNotRuntimeAccessible);

  if (!thunk->empty())
    return {thunk, interfaceSubs};

  thunk->setGenericEnvironment(genericEnv);
  auto *entry = thunk->createBasicBlock();
  TangentBuilder builder(entry, adContext);
  createEntryArguments(thunk);

  // Get arguments.
  SmallVector<SILValue, 4> arguments;
  SmallVector<AllocStackInst *, 4> localAllocations;
  SmallVector<SILValue, 4> valuesToCleanup;
  auto cleanupValues = [&]() {
    for (auto value : llvm::reverse(valuesToCleanup))
      builder.emitDestroyOperation(loc, value);

    for (auto *alloc : llvm::reverse(localAllocations))
      builder.createDeallocStack(loc, alloc);
  };

  // Build a `.zero` argument for the given `Differentiable`-conforming type.
  auto buildZeroArgument = [&](SILParameterInfo zeroSILParameter) {
    auto zeroSILType = zeroSILParameter.getSILStorageInterfaceType();
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
      builder.emitZeroIntoBuffer(loc, buf, IsInitialization);
      if (zeroSILType.isAddress()) {
        arguments.push_back(buf);
        if (zeroSILParameter.isGuaranteed()) {
          valuesToCleanup.push_back(buf);
        }
      } else {
        auto arg = builder.emitLoadValueOperation(loc, buf,
                                                  LoadOwnershipQualifier::Take);
        arguments.push_back(arg);
        if (zeroSILParameter.isGuaranteed()) {
          valuesToCleanup.push_back(arg);
        }
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

  auto toIndirectResultsIter = thunk->getIndirectResults().begin();
  auto useNextIndirectResult = [&]() {
      assert(toIndirectResultsIter != thunk->getIndirectResults().end());
      arguments.push_back(*toIndirectResultsIter++);
  };

  switch (kind) {
  // Differential arguments are:
  // - All indirect results, followed by:
  // - An interleaving of:
  //   - Thunk arguments (when parameter index is in both desired and actual
  //     indices).
  //   - Zeros (when parameter is not in desired indices).
  case AutoDiffDerivativeFunctionKind::JVP: {
    unsigned numIndirectResults = linearMapType->getNumIndirectFormalResults();
    // Forward desired indirect results
    for (unsigned idx : *actualConfig.resultIndices) {
      if (idx >= numIndirectResults)
        break;

      auto resultInfo = linearMapType->getResults()[idx];
      assert(idx < linearMapType->getNumResults());

      // Forward result argument in case we do not need to thunk it away
      if (desiredConfig.resultIndices->contains(idx)) {
        useNextIndirectResult();
        continue;
      }

      // Otherwise, allocate and use an uninitialized indirect result
      auto *indirectResult = builder.createAllocStack(
        loc, resultInfo.getSILStorageInterfaceType());
      localAllocations.push_back(indirectResult);
      arguments.push_back(indirectResult);
    }
    assert(toIndirectResultsIter == thunk->getIndirectResults().end());

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
        auto zeroSILParameter =
            linearMapType->getParameters()[mapOriginalParameterIndex(i)];
        buildZeroArgument(zeroSILParameter);
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
    // Collect pullback arguments.
    unsigned pullbackResultIndex = 0;
    for (unsigned i : actualConfig.parameterIndices->getIndices()) {
      auto origParamInfo = origFnType->getParameters()[i];
      // Skip original semantic result parameters. All non-indirect-result pullback
      // arguments (including semantic result` arguments) are appended to `arguments`
      // later.
      if (origParamInfo.isAutoDiffSemanticResult())
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
    auto thunkArgs = thunk->getArgumentsWithoutIndirectResults();
    // Slice out the function to be called
    thunkArgs = thunkArgs.slice(0, thunkArgs.size() - 1);
    unsigned thunkArg = 0;
    for (unsigned idx : *actualConfig.resultIndices) {
      // Forward result argument in case we do not need to thunk it away
      if (desiredConfig.resultIndices->contains(idx))
        arguments.push_back(thunkArgs[thunkArg++]);
      else // otherwise, zero it out
        buildZeroArgument(linearMapType->getParameters()[arguments.size()]);
    }
    break;
  }
  }

  // Get the linear map thunk argument and apply it.
  auto *linearMap = thunk->getArguments().back();
  auto *ai = builder.createApply(loc, linearMap, SubstitutionMap(), arguments);

  // If differential thunk, deallocate local allocations and directly return
  // `apply` result (if it is desired).
  if (kind == AutoDiffDerivativeFunctionKind::JVP) {
    SmallVector<SILValue, 8> differentialDirectResults;
    extractAllElements(ai, builder, differentialDirectResults);
    SmallVector<SILValue, 8> allResults;
    collectAllActualResultsInTypeOrder(ai, differentialDirectResults, allResults);
    unsigned numResults = thunk->getConventions().getNumDirectSILResults() +
     thunk->getConventions().getNumDirectSILResults();
    SmallVector<SILValue, 8> results;
    for (unsigned idx : *actualConfig.resultIndices) {
      if (idx >= numResults)
        break;

      auto result = allResults[idx];
      if (desiredConfig.isWrtResult(idx))
        results.push_back(result);
      else {
        if (result->getType().isAddress())
          builder.emitDestroyAddrAndFold(loc, result);
        else
          builder.emitDestroyValueOperation(loc, result);
      }
    }

    cleanupValues();
    auto result = joinElements(results, builder, loc);
    builder.createReturn(loc, result);
    return {thunk, interfaceSubs};
  }

  // If pullback thunk, return only the desired results and clean up the
  // undesired results.
  SmallVector<SILValue, 8> pullbackDirectResults;
  extractAllElements(ai, builder, pullbackDirectResults);
  SmallVector<SILValue, 8> allResults;
  collectAllActualResultsInTypeOrder(ai, pullbackDirectResults, allResults);
  // Collect pullback semantic result arguments in type order.
  unsigned semanticResultArgIdx = 0;
  SILFunctionConventions origConv(origFnType, thunk->getModule());
  for (auto paramIdx : actualConfig.parameterIndices->getIndices()) {
    auto paramInfo = origConv.getParameters()[paramIdx];
    if (!paramInfo.isAutoDiffSemanticResult())
      continue;
    auto semanticResultArg =
      *std::next(ai->getAutoDiffSemanticResultArguments().begin(),
                 semanticResultArgIdx++);
    unsigned mappedParamIdx = mapOriginalParameterIndex(paramIdx);
    allResults.insert(allResults.begin() + mappedParamIdx, semanticResultArg);
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
  cleanupValues();
  auto result = joinElements(results, builder, loc);
  builder.createReturn(loc, result);

  return {thunk, interfaceSubs};
}

std::pair<SILFunction *, SubstitutionMap>
getOrCreateSubsetParametersThunkForDerivativeFunction(
    SILOptFunctionBuilder &fb, SILValue origFnOperand, SILValue derivativeFn,
    AutoDiffDerivativeFunctionKind kind, const AutoDiffConfig &desiredConfig,
    const AutoDiffConfig &actualConfig, ADContext &adContext) {
  LLVM_DEBUG(getADDebugStream()
             << "Getting a subset parameters thunk for derivative "
             << (kind == AutoDiffDerivativeFunctionKind::JVP ? "jvp" : "vjp")
             << " function " << derivativeFn
             << " of the original function " << origFnOperand
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
    origName = origFnRef->getReferencedFunction()->getName();
  } else if (auto *origMethodInst =
                 peerThroughFunctionConversions<MethodInst>(origFnOperand)) {
    origName = origMethodInst->getMember()
                   .getAnyFunctionRef()
                   ->getAbstractFunctionDecl()
                   ->getNameStr();
  }
  assert(!origName.empty() && "Original function name could not be resolved");
  Mangle::DifferentiationMangler mangler;
  auto thunkName = mangler.mangleDerivativeFunctionSubsetParametersThunk(
      origName, targetType->mapTypeOutOfContext()->getCanonicalType(),
      kind, actualConfig.parameterIndices, actualConfig.resultIndices,
      desiredConfig.parameterIndices);

  auto loc = origFnOperand.getLoc();
  auto *thunk = fb.getOrCreateSharedFunction(
      loc, thunkName, thunkType, IsBare, IsTransparent, caller->isSerialized(),
      ProfileCounter(), IsThunk, IsNotDynamic, IsNotDistributed,
      IsNotRuntimeAccessible);

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
    auto *assoc = derivativeFnRef->getReferencedFunction();
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
  auto *apply = builder.createApply(loc, assocRef, assocSubstMap, arguments);

  // Extract all direct results.
  SmallVector<SILValue, 8> directResults;
  extractAllElements(apply, builder, directResults);
  auto linearMap = directResults.back();
  directResults.pop_back();

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
          unsubstLinearMapTargetType, kind, desiredConfig, actualConfig,
          adContext);

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
      ParameterConvention::Direct_Guaranteed);
  if (linearMapTargetType != unsubstLinearMapTargetType) {
    thunkedLinearMap = builder.createConvertFunction(
        loc, thunkedLinearMap,
        SILType::getPrimitiveObjectType(linearMapTargetType),
        /*withoutActuallyEscaping*/ false);
  }
  assert(origFnType->getNumAutoDiffSemanticResults() > 0);
  if (origFnType->getNumResults() > 0 &&
      origFnType->getResults().front().isFormalDirect()) {
    directResults.push_back(thunkedLinearMap);
    auto result = joinElements(directResults, builder, loc);
    builder.createReturn(loc, result);
  } else {
    builder.createReturn(loc, thunkedLinearMap);
  }

  return {thunk, interfaceSubs};
}

} // end namespace autodiff
} // end namespace swift
